// Copyright (C) 2026 Josué Oswaldo Hernández López (bayo.sh). Licencia GPL-3.0, ver LICENSE.
// bayo.sh v0.4: home-screen widgets. The payload gathers the data (weather from Open-Meteo, the
// PS5's temperature and free space, the controller battery the ShellUI module reports) and sends
// it to the module, which draws a "cristal neón" card on the home screen.
#include "bayosh.hpp"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/mount.h>
#include <unistd.h>

#include <ps5/kernel.h>

namespace bayosh {
namespace {

constexpr const char* kWidgetsFile = "/data/bayosh/widgets.txt";

std::mutex  g_wmu;
bool        g_on      = true;  // card shown
bool        g_weather = true;  // rows
bool        g_temp    = true;
bool        g_disk    = true;
bool        g_pad     = true;
std::string g_city;            // as typed by the user
std::string g_place;           // as Open-Meteo names it
std::string g_lang = "es";
double      g_lat = 0, g_lon = 0;
bool        g_geo = false;     // lat/lon known
std::string g_weather_text;    // "24° · Nublado"
std::string g_weather_err;
time_t      g_weather_at = 0;
int         g_wcode = -1;      // WMO weather code of the last reading
bool        g_day   = true;
bool        g_auto_place = false; // place found from the network (no city typed)
std::string g_style = "wave";  // outline: wave | neon | rainbow | follow | fixed | breathe | off
std::string g_color = "22b8ff"; // for fixed / breathe
std::string g_battery;         // from the module: "80" (percent) or "c80" while charging
std::atomic<int> g_kick {0};   // settings changed: refresh now

std::string read_file(const char* path) {
	FILE* f = fopen(path, "r");
	if (f == nullptr) return {};
	std::string s;
	char buf[1024];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
	fclose(f);
	return s;
}

std::string value_of(const std::string& text, const char* key) {
	const std::string k = std::string("\n") + key + "=";
	const std::string t = "\n" + text;
	const size_t      p = t.find(k);
	if (p == std::string::npos) return {};
	const size_t s = p + k.size();
	return t.substr(s, t.find('\n', s) - s);
}

void load() {
	const std::string t = read_file(kWidgetsFile);
	if (t.empty()) return;
	g_on      = value_of(t, "on") != "0";
	g_weather = value_of(t, "weather") != "0";
	g_temp    = value_of(t, "temp") != "0";
	g_disk    = value_of(t, "disk") != "0";
	g_pad     = value_of(t, "pad") != "0";
	g_city    = value_of(t, "city");
	g_place   = value_of(t, "place");
	const std::string st = value_of(t, "style"), co = value_of(t, "color");
	if (!st.empty()) g_style = st;
	if (co.size() == 6) g_color = co;
	const std::string l = value_of(t, "lang");
	if (!l.empty()) g_lang = l;
	const std::string la = value_of(t, "lat"), lo = value_of(t, "lon");
	if (!la.empty() && !lo.empty()) {
		g_lat = atof(la.c_str());
		g_lon = atof(lo.c_str());
		g_geo = true;
	}
}

void save_locked() {
	char geo[80] = {};
	if (g_geo) snprintf(geo, sizeof(geo), "lat=%.4f\nlon=%.4f\n", g_lat, g_lon);
	const std::string t = std::string("on=") + (g_on ? "1" : "0") + "\nweather=" + (g_weather ? "1" : "0") +
	                      "\ntemp=" + (g_temp ? "1" : "0") + "\ndisk=" + (g_disk ? "1" : "0") + "\npad=" + (g_pad ? "1" : "0") + "\ncity=" + g_city +
	                      "\nplace=" + g_place + "\nlang=" + g_lang + "\nstyle=" + g_style + "\ncolor=" + g_color + "\n" + geo;
	const std::string tmp = std::string(kWidgetsFile) + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (f != nullptr) {
		const bool ok = fwrite(t.data(), 1, t.size(), f) == t.size() && fflush(f) == 0 && fsync(fileno(f)) == 0;
		fclose(f);
		if (ok) rename(tmp.c_str(), kWidgetsFile);
	}
	const int dir = open("/data/bayosh", O_RDONLY);
	if (dir >= 0) {
		fsync(dir);
		close(dir);
	}
	sync();
}

bool en() {
	return g_lang != "es";
}

// WMO weather codes (Open-Meteo).
const char* sky(int code) {
	struct Row { int max; const char* es; const char* en; };
	static const Row rows[] = {{0, "Despejado", "Clear"}, {2, "Parcialmente nublado", "Partly cloudy"}, {3, "Nublado", "Cloudy"},
	                           {48, "Niebla", "Fog"}, {57, "Llovizna", "Drizzle"}, {67, "Lluvia", "Rain"}, {77, "Nieve", "Snow"},
	                           {82, "Chubascos", "Showers"}, {86, "Nieve", "Snow"}, {99, "Tormenta", "Storm"}};
	for (const Row& r : rows) {
		if (code <= r.max) return en() ? r.en : r.es;
	}
	return "";
}

double json_number(const std::string& j, const std::string& key, size_t from, bool& ok) {
	const size_t p = j.find("\"" + key + "\"", from);
	ok             = false;
	if (p == std::string::npos) return 0;
	const size_t c = j.find(':', p);
	if (c == std::string::npos) return 0;
	size_t at = c + 1;
	while (at < j.size() && (j[at] == ' ' || j[at] == '"')) ++at;
	char* end      = nullptr;
	const double v = strtod(j.c_str() + at, &end);
	ok             = end != j.c_str() + at;
	return v;
}

std::string json_string(const std::string& j, const std::string& key, size_t from) {
	const size_t p = j.find("\"" + key + "\"", from);
	if (p == std::string::npos) return {};
	const size_t q1 = j.find('"', j.find(':', p) + 1);
	const size_t q2 = q1 == std::string::npos ? q1 : j.find('"', q1 + 1);
	return q1 == std::string::npos || q2 == std::string::npos ? std::string() : j.substr(q1 + 1, q2 - q1 - 1);
}

std::string url_encode(const std::string& s) {
	std::string o;
	char        b[4];
	for (const unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.') {
			o += static_cast<char>(c);
		} else {
			snprintf(b, sizeof(b), "%%%02X", c);
			o += b;
		}
	}
	return o;
}

// Where the PS5 is, from its public IP (city level). Used when no city is typed, and to steer the
// address search. Two free HTTPS services, no key.
bool ip_location(double& lat, double& lon, std::string& place) {
	std::string body, error;
	if (http_get("https://get.geojs.io/v1/ip/geo.json", 16 * 1024, body, error)) {
		bool ok1, ok2;
		lat   = json_number(body, "latitude", 0, ok1);
		lon   = json_number(body, "longitude", 0, ok2);
		place = json_string(body, "city", 0);
		if (place.empty()) place = json_string(body, "region", 0);
		if (ok1 && ok2) return true;
	}
	if (http_get("https://ipwho.is/?fields=latitude,longitude,city,region", 16 * 1024, body, error)) {
		bool ok1, ok2;
		lat   = json_number(body, "latitude", 0, ok1);
		lon   = json_number(body, "longitude", 0, ok2);
		place = json_string(body, "city", 0);
		if (ok1 && ok2) return true;
	}
	return false;
}

// A typed place -> coordinates. Open-Meteo knows cities; for a colonia or a street address fall
// back to OpenStreetMap's Nominatim, biased to the area the IP points at.
bool find_place(const std::string& city, const std::string& lang, double& lat, double& lon, std::string& place, std::string& error) {
	std::string body;
	if (http_get("https://geocoding-api.open-meteo.com/v1/search?count=1&language=" + lang + "&name=" + url_encode(city), 64 * 1024,
	             body, error)) {
		const size_t r = body.find("\"results\"");
		bool         ok1 = false, ok2 = false;
		if (r != std::string::npos) {
			lat = json_number(body, "latitude", r, ok1);
			lon = json_number(body, "longitude", r, ok2);
		}
		if (ok1 && ok2) {
			place = json_string(body, "name", r);
			return true;
		}
	}
	double      ilat = 0, ilon = 0;
	std::string iplace;
	const bool  near = ip_location(ilat, ilon, iplace);
	std::string url  = "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=1&addressdetails=1&accept-language=" + lang +
	                  "&q=" + url_encode(city);
	if (near) {
		char vb[120];
		snprintf(vb, sizeof(vb), "&viewbox=%.3f,%.3f,%.3f,%.3f", ilon - 1.0, ilat + 1.0, ilon + 1.0, ilat - 1.0);
		url += vb;
	}
	error.clear();
	if (http_get(url, 64 * 1024, body, error, "bayo.sh/0.4 (PS5 wallpaper app; github.com/bayo-labs/bayo.sh)")) {
		bool ok1, ok2;
		lat = json_number(body, "lat", 0, ok1);
		lon = json_number(body, "lon", 0, ok2);
		if (ok1 && ok2) {
			const size_t a = body.find("\"address\"");
			place.clear();
			for (const char* k : {"borough", "city_district", "suburb", "city", "town", "village", "municipality", "county", "state"}) {
				if (a != std::string::npos) place = json_string(body, k, a);
				if (!place.empty()) break;
			}
			if (place.empty()) place = city;
			return true;
		}
	}
	error = "notfound";
	return false;
}

void refresh_weather() {
	std::string city, lang;
	bool        geo;
	double      lat, lon;
	{
		std::lock_guard<std::mutex> lock(g_wmu);
		city = g_city;
		lang = g_lang == "es" ? "es" : "en";
		geo  = g_geo;
		lat  = g_lat;
		lon  = g_lon;
	}
	std::string body, error;
	if (!geo) {
		std::string place;
		bool        found = false, automatic = city.empty();
		if (!automatic) {
			found = find_place(city, lang, lat, lon, place, error);
		} else {
			found = ip_location(lat, lon, place);
			if (!found) error = "noip";
		}
		if (!found) {
			std::lock_guard<std::mutex> lock(g_wmu);
			g_weather_err = error.empty() ? "notfound" : error;
			log("widgets: place '%s' not found (%s)", city.c_str(), g_weather_err.c_str());
			return;
		}
		std::lock_guard<std::mutex> lock(g_wmu);
		if (g_city != city) return; // changed meanwhile
		g_lat        = lat;
		g_lon        = lon;
		g_geo        = true;
		g_place      = place;
		g_auto_place = automatic;
		save_locked();
		log("widgets: %s -> %s %.3f %.3f", automatic ? "(auto)" : city.c_str(), g_place.c_str(), lat, lon);
	}
	char url[200];
	snprintf(url, sizeof(url),
	         "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,is_day&timezone=auto",
	         lat, lon);
	if (!http_get(url, 64 * 1024, body, error)) {
		std::lock_guard<std::mutex> lock(g_wmu);
		g_weather_err = error;
		return;
	}
	const size_t cur = body.find("\"current\"");
	bool         okt, okc;
	const double temp = json_number(body, "temperature_2m", cur == std::string::npos ? 0 : cur, okt);
	const double code = json_number(body, "weather_code", cur == std::string::npos ? 0 : cur, okc);
	bool         okd;
	const double day  = json_number(body, "is_day", cur == std::string::npos ? 0 : cur, okd);
	std::lock_guard<std::mutex> lock(g_wmu);
	if (cur == std::string::npos || !okt) {
		g_weather_err = "data";
		return;
	}
	char text[96];
	snprintf(text, sizeof(text), "%d° · %s", static_cast<int>(std::lround(temp)), okc ? sky(static_cast<int>(code)) : "");
	g_weather_text = text;
	g_wcode        = okc ? static_cast<int>(code) : -1;
	g_day          = !okd || day > 0.5;
	g_weather_err.clear();
	g_weather_at = time(nullptr);
}

int cpu_temperature() {
	static int (*fn)(int*) = nullptr;
	static bool looked     = false;
	if (!looked) {
		looked      = true;
		uint32_t kh = 0;
		for (const char* lib : {"libkernel_sys.sprx", "libkernel.sprx", "libkernel_web.sprx"}) {
			if (kernel_dynlib_handle(-1, lib, &kh) == 0 && kh != 0 && kh != 0xffffffffu) break;
			kh = 0;
		}
		fn = kh ? reinterpret_cast<int (*)(int*)>(kernel_dynlib_dlsym(-1, kh, "sceKernelGetCpuTemperature")) : nullptr;
	}
	int t = -1;
	return fn != nullptr && fn(&t) == 0 ? t : -1;
}

long long total_gb() {
	struct statfs sf {};
	return statfs("/user", &sf) == 0 ? static_cast<long long>(sf.f_blocks) * sf.f_bsize / (1000LL * 1000 * 1000) : -1;
}

long long free_gb() {
	struct statfs sf {};
	return statfs("/user", &sf) == 0 ? static_cast<long long>(sf.f_bavail) * sf.f_bsize / (1000LL * 1000 * 1000) : -1;
}

// Row colour from the weather: sun, night, clouds, fog, rain, snow, storm.
const char* weather_color(int code, bool day) {
	if (code < 0) return "22b8ff";
	if (code <= 1) return day ? "ffc93c" : "8f9bff";
	if (code <= 3) return day ? "9fc4ff" : "7d8bc4";
	if (code <= 48) return "a7b4c2";
	if (code <= 67 || (code >= 80 && code <= 82)) return "3da5ff";
	if (code <= 77 || code == 85 || code == 86) return "e6f3ff";
	return "b06bff";
}

// "W <on> <style> <c1> <c2>...\t<RRGGBB|title>\t<value>\t..." -> the module's card.
std::string message(const std::string& outline) {
	std::lock_guard<std::mutex> lock(g_wmu);
	std::string m = std::string("W ") + (g_on ? "1" : "0") + " " + g_style;
	if (!outline.empty()) m += " " + outline;
	else if (g_style == "fixed" || g_style == "breathe") m += " " + g_color;
	auto row = [&](const char* color, const char* title, const std::string& value) {
		m += std::string("\t") + color + "|" + title + "\t" + value;
	};
	if (g_weather) {
		std::string v = g_weather_text;
		if (v.empty()) {
			v = g_weather_err == "notfound" ? (en() ? "Place not found, check it in bayo.sh" : "No encontré tu ciudad, revísala en bayo.sh")
			    : g_weather_err.empty()      ? "…"
			                                 : (en() ? "No weather right now" : "Sin clima por ahora");
		}
		if (!g_weather_text.empty() && !g_place.empty()) v += "\x1f" + g_place; // the card alternates the two
		row(weather_color(g_weather_text.empty() ? -1 : g_wcode, g_day), en() ? "Weather" : "Clima", v);
	}
	if (g_temp) {
		const int t = cpu_temperature();
		if (t >= 0) {
			row(t >= 75 ? "ff4d4d" : t >= 62 ? "ff9a3c" : "2ee5b5", en() ? "Temperature" : "Temperatura", std::to_string(t) + "°C\x1f" +
			        (t >= 75 ? (en() ? "Very hot" : "Muy caliente") : t >= 62 ? (en() ? "Warm" : "Caliente") : (en() ? "All good" : "Todo bien")));
		}
	}
	if (g_disk) {
		const long long f = free_gb();
		if (f >= 0) {
			row(f < 50 ? "ff4d6d" : "a855f7", en() ? "Storage" : "Disco", std::to_string(f) + " GB " + (en() ? "free" : "libres") +
			    (total_gb() > 0 ? "\x1f" + std::string(en() ? "of " : "de ") + std::to_string(total_gb()) + " GB" : std::string()));
		}
	}
	if (g_pad && !g_battery.empty()) {
		const bool        charging = g_battery[0] == 'c';
		const std::string pct      = charging ? g_battery.substr(1) : g_battery;
		const int         n        = atoi(pct.c_str());
		row(charging ? "7cff00" : n <= 20 ? "ff4d4d" : n <= 40 ? "ffb020" : "34e89e", en() ? "Controller" : "Control",
		    pct + "%" + (charging ? (en() ? " · charging" : " · cargando") : n <= 20 ? (en() ? " · low" : " · baja") : ""));
	}
	return m;
}

void* widgets_thread(void*) {
	{
		std::lock_guard<std::mutex> lock(g_wmu);
		load();
	}
	time_t last_weather = 0;
	int    seen_kick    = -1;
	for (int tick = 0;; ++tick) {
		const int kick = g_kick.load();
		bool      want_weather, missing;
		{
			std::lock_guard<std::mutex> lock(g_wmu);
			want_weather = g_on && g_weather;
			missing      = g_weather_text.empty();
		}
		// refetch every 20 min, or when it is missing (new city / turned on) - at most once a minute
		if (want_weather && ((missing && time(nullptr) - last_weather > 60) || time(nullptr) - last_weather > 20 * 60 ||
		                     (missing && kick != seen_kick))) {
			refresh_weather();
			last_weather = time(nullptr);
		}
		seen_kick = kick;
		std::string style;
		{
			std::lock_guard<std::mutex> lock(g_wmu);
			style = g_style;
		}
		module_send(message(widget_colors(style)));
		for (int i = 0; i < 30 && g_kick.load() == seen_kick; ++i) {
			sleep(1); // every 30 s, or right away after a change
		}
	}
	return nullptr;
}

} // namespace

void start_widgets() {
	pthread_t      t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 512 * 1024); // HTTPS
	if (pthread_create(&t, &attr, widgets_thread, nullptr) == 0) {
		pthread_detach(t);
	}
	pthread_attr_destroy(&attr);
}

void widgets_battery(const std::string& value) {
	std::lock_guard<std::mutex> lock(g_wmu);
	const std::string v = value == "-" ? std::string() : value;
	if (v != g_battery) {
		g_battery = v;
		++g_kick;
	}
}

bool widgets_set(const std::string& key, const std::string& value) {
	std::lock_guard<std::mutex> lock(g_wmu);
	if (key == "on") g_on = value != "0";
	else if (key == "weather") g_weather = value != "0";
	else if (key == "temp") g_temp = value != "0";
	else if (key == "disk") g_disk = value != "0";
	else if (key == "pad") g_pad = value != "0";
	else if (key == "lang") g_lang = value.size() <= 5 ? value : g_lang;
	else if (key == "style") {
		static const char* const kStyles[] = {"wave", "neon", "rainbow", "follow", "fixed", "breathe", "off"};
		bool known = false;
		for (const char* k : kStyles) known = known || value == k;
		if (!known) return false;
		g_style = value;
	} else if (key == "color") {
		if (value.size() != 6 || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
		g_color = value;
	}
	else if (key == "city") {
		if (value.size() > 80) return false;
		g_city  = value;
		g_place.clear();
		g_geo   = false;
		g_auto_place = false;
		g_weather_text.clear();
		g_weather_err.clear();
	} else {
		return false;
	}
	save_locked();
	++g_kick;
	return true;
}

std::string widgets_json() {
	std::lock_guard<std::mutex> lock(g_wmu);
	auto js = [](const std::string& s) {
		std::string o = "\"";
		for (const char c : s) {
			if (c == '"' || c == '\\') o += '\\';
			if (static_cast<unsigned char>(c) >= 0x20) o += c;
		}
		return o + "\"";
	};
	return std::string("{\"on\":") + (g_on ? "true" : "false") + ",\"weather\":" + (g_weather ? "true" : "false") +
	       ",\"temp\":" + (g_temp ? "true" : "false") + ",\"disk\":" + (g_disk ? "true" : "false") + ",\"pad\":" + (g_pad ? "true" : "false") + ",\"city\":" + js(g_city) +
	       ",\"place\":" + js(g_place) + ",\"now\":" + js(g_weather_text) + ",\"err\":" + js(g_weather_err) +
	       ",\"battery\":" + js(g_battery) + ",\"auto\":" + (g_city.empty() ? "true" : "false") + ",\"style\":" + js(g_style) +
	       ",\"color\":" + js(g_color) + "}";
}

} // namespace bayosh

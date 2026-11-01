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
bool        g_clock   = true;  // v0.5 rows: time / date
bool        g_play    = true;  //           time played today / last game
int         g_utc_off = 0;     // seconds, from Open-Meteo (timezone=auto)
bool        g_utc_known = false;
int         g_play_day = 0;    // yyyymmdd that g_play_sec counts
long        g_play_sec = 0;    // finished sessions today
time_t      g_play_start = 0;  // a game is running since (0 = none)
std::string g_last_tid, g_last_name;
int         g_fx = 1;          // v0.5 weather effect over the wallpaper: 0 off, 1 soft, 2 strong
std::string g_fx_test;         // preview kind ("rain", "snow"...) until g_fx_test_until
time_t      g_fx_test_until = 0;
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

bool g_loaded = false;

void load() {
	g_loaded = true;
	const std::string t = read_file(kWidgetsFile);
	if (t.empty()) return;
	g_on      = value_of(t, "on") != "0";
	g_weather = value_of(t, "weather") != "0";
	g_temp    = value_of(t, "temp") != "0";
	g_disk    = value_of(t, "disk") != "0";
	g_pad     = value_of(t, "pad") != "0";
	g_city    = value_of(t, "city");
	g_clock   = value_of(t, "clock") != "0";
	g_play    = value_of(t, "play") != "0";
	g_play_day = atoi(value_of(t, "playday").c_str());
	g_play_sec = atol(value_of(t, "playsec").c_str());
	g_last_tid  = value_of(t, "lasttid");
	g_last_name = value_of(t, "lastname");
	const std::string fx = value_of(t, "fx");
	if (!fx.empty()) g_fx = atoi(fx.c_str()) < 0 ? 0 : atoi(fx.c_str()) > 2 ? 2 : atoi(fx.c_str());
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
	                      "\ntemp=" + (g_temp ? "1" : "0") + "\ndisk=" + (g_disk ? "1" : "0") + "\npad=" + (g_pad ? "1" : "0") + "\nfx=" + std::to_string(g_fx) + "\nclock=" + (g_clock ? "1" : "0") + "\nplay=" + (g_play ? "1" : "0") +
	                      "\nplayday=" + std::to_string(g_play_day) + "\nplaysec=" + std::to_string(g_play_sec) + "\nlasttid=" + g_last_tid +
	                      "\nlastname=" + g_last_name + "\ncity=" + g_city +
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
	static const Row rows[] = {{0, "Despejado", "Clear"}, {2, "Nubes", "Some clouds"}, {3, "Nublado", "Cloudy"},
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

// "Delegación Iztapalapa" -> "Iztapalapa": the card has room for the name, not the admin level.
std::string short_place(std::string p) {
	for (const char* pre : {"Delegación ", "Alcaldía ", "Municipio de ", "Municipio ", "Colonia ", "Borough of ", "City of "}) {
		const size_t n = strlen(pre);
		if (p.size() > n + 2 && p.compare(0, n, pre) == 0) return p.substr(n);
	}
	return p;
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
	if (http_get(url, 64 * 1024, body, error, "bayo.sh/0.5 (PS5 wallpaper app; github.com/bayo-labs/bayo.sh)")) {
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
	bool         oko;
	const double off = json_number(body, "utc_offset_seconds", 0, oko);
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
	if (oko) {
		g_utc_off   = static_cast<int>(off);
		g_utc_known = true;
	}
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

// Disk space as Settings > Storage shows it. statfs("/user") counts the whole user partition, but
// Sony keeps part of it for the system and takes it off both the size and the free space (test
// v0.4.0, PS5 Slim: statfs 273 GB free, Settings 187.2 GB free of 848.0 GB). The sizes Settings shows
// are fixed per model, so the reserve is the partition size minus the largest of them that fits.
struct Disk {
	double total = -1, free = -1; // GB (10^9), as Settings shows them
	double raw_total = -1, raw_free = -1;
};

Disk disk_space() {
	Disk          d;
	struct statfs sf {};
	if (statfs("/user", &sf) != 0) return d;
	const double gb = 1000.0 * 1000 * 1000;
	d.raw_total     = static_cast<double>(sf.f_blocks) * sf.f_bsize / gb;
	d.raw_free      = static_cast<double>(sf.f_bavail) * sf.f_bsize / gb;
	d.total         = d.raw_total;
	d.free          = d.raw_free;
	// PS5 (825 GB), PS5 Slim (1 TB), PS5 Pro (2 TB)
	static const double kShown[] = {1890.0, 848.0, 667.2};
	for (const double shown : kShown) {
		const double reserve = d.raw_total - shown;
		if (reserve >= 0 && reserve <= d.raw_total * 0.25) {
			d.total = shown;
			d.free  = d.raw_free - reserve > 0 ? d.raw_free - reserve : 0;
			break;
		}
	}
	static bool logged = false;
	if (!logged) {
		logged = true;
		log("widgets: disk statfs %.1f/%.1f GB (bsize %lu) -> shown %.1f/%.1f GB", d.raw_free, d.raw_total,
		    static_cast<unsigned long>(sf.f_bsize), d.free, d.total);
	}
	return d;
}

std::string gb_text(double v) {
	char b[32];
	if (v >= 1000) snprintf(b, sizeof(b), "%.2f TB", v / 1000);
	else if (v >= 100) snprintf(b, sizeof(b), "%.0f GB", v);
	else snprintf(b, sizeof(b), "%.1f GB", v);
	return b;
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

// v0.5: local time. The payload's own time zone may be UTC, so prefer the offset Open-Meteo gives
// for the place (timezone=auto); fall back to localtime. Caller holds g_wmu.
tm local_now(time_t t) {
	tm out {};
	if (g_utc_known) {
		const time_t shifted = t + g_utc_off;
		gmtime_r(&shifted, &out);
	} else {
		localtime_r(&t, &out);
	}
	return out;
}

int day_key(time_t t) {
	const tm l = local_now(t);
	return (l.tm_year + 1900) * 10000 + (l.tm_mon + 1) * 100 + l.tm_mday;
}

// Seconds played today, the running game included (from local midnight on). Caller holds g_wmu.
long played_today(time_t now) {
	const int today = day_key(now);
	long      sec   = g_play_day == today ? g_play_sec : 0;
	if (g_play_start != 0) {
		const tm l        = local_now(now);
		const time_t mid  = now - (l.tm_hour * 3600 + l.tm_min * 60 + l.tm_sec);
		sec += static_cast<long>(now - (g_play_start > mid ? g_play_start : mid));
	}
	return sec;
}

std::string clock_text(time_t now) {
	static const char* const kDayEs[] = {"dom", "lun", "mar", "mié", "jue", "vie", "sáb"};
	static const char* const kDayEn[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char* const kMonEs[] = {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};
	static const char* const kMonEn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	const tm l = local_now(now);
	char     b[96];
	if (en()) {
		// the board's 7-segment digits take "9:41"; "Sat 31" / "Oct · PM" go beside them (\x1e splits the lines)
		snprintf(b, sizeof(b), "%d:%02d\x1f%s %d\x1e%s %s", l.tm_hour % 12 == 0 ? 12 : l.tm_hour % 12, l.tm_min, kDayEn[l.tm_wday],
		         l.tm_mday, kMonEn[l.tm_mon], l.tm_hour < 12 ? "AM" : "PM");
	} else {
		snprintf(b, sizeof(b), "%02d:%02d\x1f%s %d %s", l.tm_hour, l.tm_min, kDayEs[l.tm_wday], l.tm_mday, kMonEs[l.tm_mon]);
	}
	return b;
}

std::string duration_text(long sec) {
	const long m = sec / 60;
	char       b[48];
	if (m < 60) snprintf(b, sizeof(b), "%ld min", m); // big text on the board: short
	else snprintf(b, sizeof(b), "%ldh %02ldm", m / 60, m % 60);
	return b;
}


// v0.5: the weather effect drawn over the user's own wallpaper (the wallpaper never changes).
// WMO codes: 45-48 fog, 51-57 drizzle, 61-67 / 80-82 rain, 71-77 / 85-86 snow, 95-99 storm.
const char* fx_kind(int code) {
	if (code >= 45 && code <= 48) return "fog";
	if (code >= 51 && code <= 57) return "drizzle";
	if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return "rain";
	if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "snow";
	if (code >= 95 && code <= 99) return "storm";
	return "none";
}

// "C <kind> <level>" -> the module's particle layer.
std::string fx_message() {
	std::lock_guard<std::mutex> lock(g_wmu);
	const bool  testing = !g_fx_test.empty() && time(nullptr) < g_fx_test_until;
	const char* kind    = testing ? g_fx_test.c_str() : g_fx > 0 && !g_weather_text.empty() ? fx_kind(g_wcode) : "none";
	const int   level   = testing && g_fx == 0 ? 1 : g_fx;
	return std::string("C ") + kind + " " + std::to_string(level);
}

// "W <on> <style> <c1> <c2>...\t<RRGGBB|title>\t<value>\t..." -> the module's card.
std::string message(const std::string& outline) {
	std::lock_guard<std::mutex> lock(g_wmu);
	std::string m = std::string("W ") + (g_on ? "1" : "0") + " " + g_style;
	if (!outline.empty()) m += " " + outline;
	else if (g_style == "fixed" || g_style == "breathe") m += " " + g_color;
	// v0.5 dashboard tile: "RRGGBB|title|pct|kind" then "main\x1fsmall" (pct empty = no gauge).
	// kind: t clock, w weather, j played, c console, d storage, p controller (the module places them).
	auto tile = [&](const char* color, const std::string& title, int pct, char kind, const std::string& main, const std::string& small) {
		m += std::string("\t") + color + "|" + title + "|" + (pct >= 0 ? std::to_string(pct > 100 ? 100 : pct) : std::string()) + "|" + kind +
		     "\t" + main + (small.empty() ? std::string() : "\x1f" + small);
	};
	const time_t now = time(nullptr);
	if (g_clock) {
		const std::string c = clock_text(now); // "21:31\x1fsáb 31 oct"
		const size_t      s = c.find('\x1f');
		tile("22b8ff", en() ? "Time" : "Hora", -1, 't', c.substr(0, s), s == std::string::npos ? std::string() : c.substr(s + 1));
	}
	if (g_weather) {
		const std::string place = short_place(g_place);
		const std::string title = place.empty() ? std::string(en() ? "Weather" : "Clima") : place;
		if (g_weather_text.empty()) {
			tile("a855f7", title, -1, 'w', "—",
			     g_weather_err == "notfound" ? (en() ? "Check your city" : "Revisa tu ciudad")
			     : g_weather_err.empty()     ? (en() ? "Looking…" : "Buscando…")
			                                 : (en() ? "No weather now" : "Sin clima ahora"));
		} else {
			// "21° · Algo nublado": shown whole when it fits the tile (~12 characters), else alternating
			const size_t      dot  = g_weather_text.find(" · ");
			const std::string temp = g_weather_text.substr(0, dot);
			const std::string sky  = dot == std::string::npos ? std::string() : g_weather_text.substr(dot + strlen(" · "));
			const std::string both = temp + " · " + sky;
			const bool        fits = both.size() <= 14; // bytes ("°" and "·" take two): ~11 characters fit the tile
			tile(weather_color(g_wcode, g_day), title, -1, 'w', fits ? both : temp, fits ? std::string() : sky);
		}
	}
	if (g_play) {
		const std::string last = g_last_name.empty() ? g_last_tid : g_last_name;
		tile(g_play_start != 0 ? "7cff00" : "ff9a3c", en() ? "Played today" : "Jugado hoy", -1, 'j', duration_text(played_today(now)),
		     last.empty() ? std::string() : (g_play_start != 0 ? std::string(en() ? "Playing: " : "Jugando: ") : std::string(en() ? "Last: " : "Último: ")) + last);
	}
	if (g_temp) {
		const int t = cpu_temperature();
		if (t >= 0) {
			tile(t >= 75 ? "ff4d4d" : t >= 62 ? "ff9a3c" : "2ee5b5", "PS5", t <= 35 ? 0 : (t - 35) * 100 / 50, 'c',
			     std::to_string(t) + "°", t >= 75 ? (en() ? "Hot!" : "¡Calor!") : t >= 62 ? (en() ? "Warm" : "Tibia") : (en() ? "Cool" : "Normal"));
		}
	}
	if (g_disk) {
		const Disk d = disk_space();
		if (d.free >= 0) {
			// v0.5.2: the ring shows how full the drive is (user: 184 GB left should look nearly full);
			// the small line alternates "184 GB" / "libres" (\x1e) so what is left is still there
			const int used = d.total > 0 ? static_cast<int>((d.total - d.free) * 100 / d.total + 0.5) : 0;
			tile(d.free < 50 || used >= 90 ? "ff4d6d" : used >= 85 ? "ff9a3c" : "a855f7", en() ? "Storage" : "Disco", used, 'd',
			     std::to_string(used) + "%", gb_text(d.free) + "\x1e" + (en() ? "free" : "libres"));
		}
	}
	if (g_pad && !g_battery.empty()) {
		const bool        charging = g_battery[0] == 'c';
		const std::string pct      = charging ? g_battery.substr(1) : g_battery;
		const int         n        = atoi(pct.c_str());
		tile(charging ? "7cff00" : n <= 20 ? "ff4d4d" : n <= 40 ? "ffb020" : "22b8ff", en() ? "Controller" : "Control", n, 'p', pct + "%",
		     charging ? (en() ? "Charging" : "Cargando") : n <= 20 ? (en() ? "Low" : "Baja") : (en() ? "Battery" : "Batería"));
	}
	return m;
}

void* widgets_thread(void*) {
	{
		std::lock_guard<std::mutex> lock(g_wmu);
		if (!g_loaded) load();
	}
	time_t last_weather = 0;
	int    seen_kick    = -1;
	for (int tick = 0;; ++tick) {
		const int kick = g_kick.load();
		bool      want_weather, missing;
		{
			std::lock_guard<std::mutex> lock(g_wmu);
			want_weather = (g_on && g_weather) || g_fx > 0;
			missing      = g_weather_text.empty();
		}
		// refetch every 20 min, or when it is missing (new city / turned on) - at most once a minute
		if (want_weather && ((missing && time(nullptr) - last_weather > 60) || time(nullptr) - last_weather > 20 * 60 ||
		                     (missing && kick != seen_kick))) {
			refresh_weather();
			last_weather = time(nullptr);
		}
		seen_kick = kick;
		std::string style, tid_to_name;
		{
			std::lock_guard<std::mutex> lock(g_wmu);
			style = g_style;
			if (g_play && g_last_name.empty() && !g_last_tid.empty()) tid_to_name = g_last_tid;
		}
		if (!tid_to_name.empty()) { // the game's title from its param (once per game)
			std::string name;
			for (const Game& gm : list_games()) {
				if (gm.tid == tid_to_name) name = gm.name;
			}
			std::lock_guard<std::mutex> lock(g_wmu);
			if (g_last_tid == tid_to_name) {
				g_last_name = name.empty() ? tid_to_name : name.substr(0, 40);
				save_locked();
			}
		}
		module_send(message(widget_colors(style)));
		module_send(fx_message());
		int wait = 30;
		{
			std::lock_guard<std::mutex> lock(g_wmu);
			if (g_clock && g_on) { // the clock turns over on the minute
				const int left = 60 - local_now(time(nullptr)).tm_sec;
				if (left < wait) wait = left;
			}
		}
		for (int i = 0; i < wait && g_kick.load() == seen_kick; ++i) {
			sleep(1); // every 30 s, at the minute for the clock, or right away after a change
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

// v0.5: the payload's game watcher: a game started (title id) or closed ("").
void widgets_game(const std::string& tid) {
	std::lock_guard<std::mutex> lock(g_wmu);
	if (!g_loaded) load(); // never save over widgets.txt before reading it
	const time_t now = time(nullptr);
	if (g_play_start != 0) { // close the running session into today's total
		const long today_sec = played_today(now);
		g_play_day           = day_key(now);
		g_play_sec           = today_sec;
		g_play_start         = 0;
	}
	if (!tid.empty()) {
		g_play_start = now;
		if (tid != g_last_tid) {
			g_last_tid = tid;
			g_last_name.clear(); // looked up by the widgets thread
		}
	}
	save_locked();
	++g_kick;
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
	if (!g_loaded) load();
	if (key == "on") g_on = value != "0";
	else if (key == "weather") g_weather = value != "0";
	else if (key == "temp") g_temp = value != "0";
	else if (key == "disk") g_disk = value != "0";
	else if (key == "pad") g_pad = value != "0";
	else if (key == "clock") g_clock = value != "0";
	else if (key == "play") g_play = value != "0";
	else if (key == "fx") {
		const int v = atoi(value.c_str());
		if (v < 0 || v > 2 || value.size() != 1) return false;
		g_fx = v;
	} else if (key == "fxtest") { // preview for 45 s, whatever the sky says
		static const char* const kKinds[] = {"rain", "drizzle", "storm", "snow", "fog", "none"};
		bool known = false;
		for (const char* k : kKinds) known = known || value == k;
		if (!known) return false;
		g_fx_test       = value;
		g_fx_test_until = value == "none" ? 0 : time(nullptr) + 45;
		++g_kick;
		return true; // not saved
	}
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
	       ",\"temp\":" + (g_temp ? "true" : "false") + ",\"disk\":" + (g_disk ? "true" : "false") + ",\"pad\":" + (g_pad ? "true" : "false") + ",\"clock\":" + (g_clock ? "true" : "false") +
	       ",\"play\":" + (g_play ? "true" : "false") + ",\"fx\":" + std::to_string(g_fx) +
	       ",\"fxnow\":" + js(g_fx > 0 && !g_weather_text.empty() ? fx_kind(g_wcode) : "none") + ",\"city\":" + js(g_city) +
	       ",\"place\":" + js(g_place) + ",\"now\":" + js(g_weather_text) + ",\"err\":" + js(g_weather_err) +
	       ",\"battery\":" + js(g_battery) + ",\"auto\":" + (g_city.empty() ? "true" : "false") + ",\"style\":" + js(g_style) +
	       ",\"color\":" + js(g_color) + "}";
}

} // namespace bayosh

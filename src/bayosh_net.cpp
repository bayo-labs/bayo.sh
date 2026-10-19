// Copyright (C) 2026 Josué Oswaldo Hernández López (bayo.sh). Licencia GPL-3.0, ver LICENSE.
// bayo.sh v0.3: downloads straight to the console (HTTPS through the system's libSceHttp2) and the
// PS5's own web browser, so wallpapers can be found and saved without a phone or PC.
#include "bayosh.hpp"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

extern "C" {
int sceUserServiceInitialize(void*);
int sceSystemServiceLaunchWebBrowser(const char* uri, void* param);
int sceSystemServiceNavigateToGoHome(void);
}

namespace bayosh {
namespace {

// libSceNet / libSceSsl / libSceHttp2 are loaded at run time: if a firmware lacks one, only the
// download button fails, not the whole payload.
std::mutex        g_dl_mu;
std::atomic<bool> g_dl_active {false};
std::atomic<bool> g_dl_cancel {false};
std::string       g_dl_name, g_dl_error, g_dl_url, g_dl_ref;
std::atomic<long long> g_dl_got {0}, g_dl_total {0};
bool              g_dl_done = false;

std::string url_file_name(const std::string& url) {
	std::string path = url;
	const size_t q   = path.find_first_of("?#");
	if (q != std::string::npos) {
		path.resize(q);
	}
	std::string last = path.substr(path.rfind('/') + 1);
	std::string name;
	for (const char c : last) {
		if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
			name += c;
		} else if (c == ' ' || c == '%') {
			name += '-';
		}
	}
	while (!name.empty() && name[0] == '.') {
		name.erase(0, 1);
	}
	if (name.size() > 100) {
		name = name.substr(name.size() - 100);
	}
	if (name.size() < 5 || strcasecmp(name.c_str() + name.size() - 4, ".mp4") != 0) {
		name = "descarga-" + std::to_string(static_cast<long long>(time(nullptr))) + ".mp4";
	}
	return name;
}

void finish(const std::string& error) {
	std::lock_guard<std::mutex> lock(g_dl_mu);
	g_dl_error = error;
	g_dl_done  = true;
	g_dl_active = false;
}

void* download_thread(void*) {
	std::string url, name, ref;
	{
		std::lock_guard<std::mutex> lock(g_dl_mu);
		url  = g_dl_url;
		name = g_dl_name;
		ref  = g_dl_ref;
	}
	std::string       error;
	const std::string path = "/data/bayosh/video/" + name;
	const std::string part = path + ".part";
	const int         f    = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (f < 0) {
		finish("No se puede escribir en /data/bayosh/video");
		return nullptr;
	}
	bool head_ok = false;
	int  step    = 0;
	WebGetOptions opt;
	opt.accept    = "video/mp4,video/*;q=0.9,*/*;q=0.5";
	opt.referer   = ref;
	opt.timeout_s = 30;
	int status = 0;
	constexpr size_t kWriteChunk = 1024 * 1024;
	std::string      wbuf;
	wbuf.reserve(kWriteChunk + 32 * 1024);
	auto flush = [&]() -> bool {
		size_t off = 0;
		while (off < wbuf.size()) {
			const ssize_t w = write(f, wbuf.data() + off, wbuf.size() - off);
			if (w <= 0) {
				error = "Sin espacio para guardar el video";
				return false;
			}
			off += static_cast<size_t>(w);
		}
		wbuf.clear();
		return true;
	};
	const bool ok = web_get(
	    url, opt,
	    [&](long long len, int st) {
		    if (st != 200) {
			    log("download: HTTP %d (%s)", st, url.c_str());
			    error = "El sitio respondió " + std::to_string(st) + ". Usa el enlace directo del .mp4";
			    return false;
		    }
		    g_dl_total = len > 0 ? len : 0;
		    log("download %s: %lld bytes from %s", name.c_str(), g_dl_total.load(), url.c_str());
		    return true;
	    },
	    [&](const char* p, size_t n) {
		    if (g_dl_cancel) {
			    error = "Descarga cancelada";
			    return false;
		    }
		    if (!head_ok && g_dl_got == 0) {
			    // An .mp4 starts with an "ftyp" box; a web page here means the link is not the video.
			    head_ok = n >= 8 && memcmp(p + 4, "ftyp", 4) == 0;
			    if (!head_ok) {
				    error = "Ese enlace no es un video .mp4 (abre el video y copia su enlace directo)";
				    return false;
			    }
		    }
		    // Batch the small TLS records into ~1 MB disk writes.
		    wbuf.append(p, n);
		    if (wbuf.size() >= kWriteChunk && !flush()) {
			    return false;
		    }
		    g_dl_got += static_cast<long long>(n);
		    if (g_dl_got > 8LL * 1024 * 1024 * 1024) {
			    error = "El video pesa demasiado";
			    return false;
		    }
		    const long long tot = g_dl_total;
		    if (tot > 0 && g_dl_got * 4 / tot > step) {
			    step = static_cast<int>(g_dl_got * 4 / tot);
			    log("download %s: %d%%", name.c_str(), step * 25);
		    }
		    return true;
	    },
	    error, status);
	if (!wbuf.empty() && error.empty()) {
		flush();
	}
	close(f);
	if (error.empty() && !ok) {
		error = "No se pudo conectar. Revisa el enlace y la red";
	}
	if (error.empty() && g_dl_got == 0) {
		error = "El sitio no envió nada";
	}
	if (error.empty() && g_dl_total > 0 && g_dl_got != g_dl_total) {
		error = "Se cortó la descarga";
	}
	if (error.empty() && rename(part.c_str(), path.c_str()) != 0) {
		error = "No se pudo guardar el video";
	}
	if (!error.empty()) {
		unlink(part.c_str());
	}
	log("download %s: %s", name.c_str(), error.empty() ? "done" : error.c_str());
	finish(error);
	return nullptr;
}

void user_service() {
	static bool once = false;
	if (!once) {
		once = true;
		sceUserServiceInitialize(nullptr); // already initialized is fine
	}
}

std::string jstr(const std::string& s) {
	std::string o = "\"";
	for (const char c : s) {
		if (c == '"' || c == '\\') {
			o += '\\';
			o += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			o += ' ';
		} else {
			o += c;
		}
	}
	return o + "\"";
}


// Turns a catalog title into a safe file name: "Noche Neón" -> "noche-neon.mp4".
std::string name_from_hint(const std::string& hint) {
	std::string n;
	for (const char c : hint) {
		const unsigned char u = static_cast<unsigned char>(c);
		if (isalnum(u)) {
			n += static_cast<char>(tolower(u));
		} else if ((c == ' ' || c == '-' || c == '_' || c == '.') && !n.empty() && n.back() != '-') {
			n += '-';
		}
		if (n.size() >= 80) {
			break;
		}
	}
	while (!n.empty() && n.back() == '-') {
		n.pop_back();
	}
	if (n.size() > 4 && n.compare(n.size() - 4, 4, "-mp4") == 0) {
		n.resize(n.size() - 4);
	}
	return n.empty() ? "" : n + ".mp4";
}

} // namespace

bool http_get(const std::string& url, size_t max_bytes, std::string& body, std::string& error, const std::string& user_agent) {
	body.clear();
	if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
		error = "El enlace debe empezar con https://";
		return false;
	}
	log("get %s", url.c_str());
	WebGetOptions opt;
	opt.user_agent = user_agent;
	int           status = 0;
	const bool    ok     = web_get(
        url, opt,
        [&](long long, int st) {
            if (st != 200) {
                error = "El sitio respondió " + std::to_string(st);
                return false;
            }
            return true;
        },
        [&](const char* p, size_t n) {
            body.append(p, n);
            if (body.size() > max_bytes) {
                error = "Respuesta demasiado grande";
                return false;
            }
            return true;
        },
        error, status);
	if (error.empty() && !ok) {
		error = "No se pudo conectar. Revisa la red";
	}
	log("get %s: HTTP %d, %zu bytes%s%s", url.c_str(), status, body.size(), error.empty() ? "" : ", ", error.c_str());
	if (!error.empty()) {
		body.clear();
		return false;
	}
	return true;
}

bool start_download(const std::string& url, std::string& error, const std::string& name_hint, const std::string& referer) {
	if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
		error = "El enlace debe empezar con https://";
		return false;
	}
	if (url.size() > 2000) {
		error = "Enlace demasiado largo";
		return false;
	}
	std::lock_guard<std::mutex> lock(g_dl_mu);
	if (g_dl_active) {
		error = "Ya hay una descarga en curso";
		return false;
	}
	g_dl_url    = url;
	g_dl_ref    = referer;
	g_dl_name   = name_hint.empty() ? url_file_name(url) : name_from_hint(name_hint);
	if (g_dl_name.empty()) {
		g_dl_name = url_file_name(url);
	}
	g_dl_error.clear();
	g_dl_done   = false;
	g_dl_got    = 0;
	g_dl_total  = 0;
	g_dl_cancel = false;
	g_dl_active = true;
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 512 * 1024);
	const int rc = pthread_create(&th, &attr, download_thread, nullptr);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		g_dl_active = false;
		error = "No se pudo iniciar la descarga";
		return false;
	}
	return true;
}

void cancel_download() {
	g_dl_cancel = true;
}

std::string download_json() {
	std::lock_guard<std::mutex> lock(g_dl_mu);
	if (g_dl_name.empty()) {
		return "null";
	}
	return std::string("{\"active\":") + (g_dl_active ? "true" : "false") + ",\"done\":" + (g_dl_done ? "true" : "false") +
	       ",\"name\":" + jstr(g_dl_name) + ",\"got\":" + std::to_string(g_dl_got.load()) +
	       ",\"total\":" + std::to_string(g_dl_total.load()) + ",\"error\":" + jstr(g_dl_error) + "}";
}

bool open_browser(const std::string& url) {
	if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
		return false;
	}
	user_service();
	const int rc = sceSystemServiceLaunchWebBrowser(url.c_str(), nullptr);
	log("browser: %s -> %#x", url.c_str(), static_cast<unsigned>(rc));
	return rc == 0;
}

void go_home() {
	user_service();
	const int rc = sceSystemServiceNavigateToGoHome();
	log("go home -> %#x", static_cast<unsigned>(rc));
}

} // namespace bayosh

// Copyright (C) 2026 Josué Oswaldo Hernández López (bayo.sh). Licencia GPL-3.0, ver LICENSE.
// bayo.sh v0.3: downloads straight to the console (HTTPS through the system's libSceHttp2) and the
// PS5's own web browser, so wallpapers can be found and saved without a phone or PC.
#include "bayosh.hpp"

#include <atomic>
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
using LoadStartFn  = int (*)(const char*, size_t, const void*, uint32_t, const void*, int*);
using NetInitFn    = int (*)();
using PoolFn       = int (*)(const char*, int, int);
using SslInitFn    = int (*)(size_t);
using HttpInitFn   = int (*)(int, int, size_t, int);
using TmplFn       = int (*)(int, const char*, int, int);
using ReqFn        = int (*)(int, const char*, const char*, uint64_t);
using IdFn         = int (*)(int);
using IdIntFn      = int (*)(int, int);
using SendFn       = int (*)(int, const void*, size_t);
using StatusFn     = int (*)(int, int*);
using ReadFn       = int (*)(int, void*, size_t);
using LenFn        = int (*)(int, int*, uint64_t*);
using HeaderFn     = int (*)(int, const char*, const char*, uint32_t);

struct Http {
	bool       ready = false;
	int        ctx   = -1;
	TmplFn     tmpl  = nullptr;
	IdFn       del_tmpl = nullptr;
	ReqFn      req   = nullptr;
	IdFn       del_req = nullptr;
	SendFn     send  = nullptr;
	StatusFn   status = nullptr;
	ReadFn     read  = nullptr;
	LenFn      length = nullptr;
	IdIntFn    redirect = nullptr;
	HeaderFn   header = nullptr;
	IdFn       abort = nullptr;
};

Http              g_http;
std::mutex        g_dl_mu;
std::atomic<bool> g_dl_active {false};
std::atomic<bool> g_dl_cancel {false};
std::string       g_dl_name, g_dl_error, g_dl_url;
std::atomic<long long> g_dl_got {0}, g_dl_total {0};
bool              g_dl_done = false;

uint32_t module_handle(const char* name) {
	uint32_t h = 0;
	if (kernel_dynlib_handle(-1, name, &h) == 0 && h != 0) {
		return h;
	}
	uint32_t kh = 0;
	for (const char* k : {"libkernel_web.sprx", "libkernel.sprx", "libkernel_sys.sprx"}) {
		if (kernel_dynlib_handle(-1, k, &kh) == 0 && kh != 0) {
			break;
		}
	}
	auto load = kh != 0 ? reinterpret_cast<LoadStartFn>(kernel_dynlib_resolve(-1, kh, "wzvqT4UqKX8")) : nullptr;
	if (load == nullptr) {
		return 0;
	}
	for (const char* root : {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/"}) {
		const std::string path = std::string(root) + name;
		int               res  = 0;
		const int         id   = load(path.c_str(), 0, nullptr, 0, nullptr, &res);
		if (id >= 0 && kernel_dynlib_handle(-1, name, &h) == 0 && h != 0) {
			log("net: loaded %s", path.c_str());
			return h;
		}
	}
	log("net: %s could not be loaded", name);
	return 0;
}

template <typename T>
T sym(uint32_t h, const char* name) {
	return h != 0 ? reinterpret_cast<T>(kernel_dynlib_dlsym(-1, h, name)) : nullptr;
}

bool http_ready() {
	if (g_http.ready) {
		return true;
	}
	const uint32_t net  = module_handle("libSceNet.sprx");
	const uint32_t ssl  = module_handle("libSceSsl.sprx");
	const uint32_t http = module_handle("libSceHttp2.sprx");
	auto net_init  = sym<NetInitFn>(net, "sceNetInit");
	auto pool      = sym<PoolFn>(net, "sceNetPoolCreate");
	auto ssl_init  = sym<SslInitFn>(ssl, "sceSslInit");
	auto http_init = sym<HttpInitFn>(http, "sceHttp2Init");
	g_http.tmpl     = sym<TmplFn>(http, "sceHttp2CreateTemplate");
	g_http.del_tmpl = sym<IdFn>(http, "sceHttp2DeleteTemplate");
	g_http.req      = sym<ReqFn>(http, "sceHttp2CreateRequestWithURL");
	g_http.del_req  = sym<IdFn>(http, "sceHttp2DeleteRequest");
	g_http.send     = sym<SendFn>(http, "sceHttp2SendRequest");
	g_http.status   = sym<StatusFn>(http, "sceHttp2GetStatusCode");
	g_http.read     = sym<ReadFn>(http, "sceHttp2ReadData");
	g_http.length   = sym<LenFn>(http, "sceHttp2GetResponseContentLength");
	g_http.redirect = sym<IdIntFn>(http, "sceHttp2SetAutoRedirect");
	g_http.header   = sym<HeaderFn>(http, "sceHttp2AddRequestHeader");
	g_http.abort    = sym<IdFn>(http, "sceHttp2AbortRequest");
	if (net_init == nullptr || pool == nullptr || ssl_init == nullptr || http_init == nullptr || g_http.tmpl == nullptr ||
	    g_http.req == nullptr || g_http.send == nullptr || g_http.status == nullptr || g_http.read == nullptr) {
		log("net: HTTP functions missing (net %#x ssl %#x http2 %#x)", net, ssl, http);
		return false;
	}
	const int n = net_init();
	const int mem = pool("bayosh_dl", 64 * 1024, 0);
	const int sslc = ssl_init(256 * 1024);
	if (mem < 0 || sslc < 0) {
		log("net: init failed (net %d pool %d ssl %d)", n, mem, sslc);
		return false;
	}
	g_http.ctx = http_init(mem, sslc, 256 * 1024, 2);
	if (g_http.ctx < 0) {
		log("net: sceHttp2Init failed (%#x)", static_cast<unsigned>(g_http.ctx));
		return false;
	}
	g_http.ready = true;
	log("net: HTTPS ready");
	return true;
}

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
	std::string url, name;
	{
		std::lock_guard<std::mutex> lock(g_dl_mu);
		url  = g_dl_url;
		name = g_dl_name;
	}
	if (!http_ready()) {
		finish("La consola no pudo iniciar HTTPS");
		return nullptr;
	}
	const int t = g_http.tmpl(g_http.ctx, "Mozilla/5.0 (PlayStation 5) bayo.sh", 3, 1);
	if (t < 0) {
		log("download: template failed %#x", static_cast<unsigned>(t));
		finish("No se pudo preparar la descarga");
		return nullptr;
	}
	if (g_http.redirect != nullptr) {
		g_http.redirect(t, 1);
	}
	const int r = g_http.req(t, "GET", url.c_str(), 0);
	if (r < 0) {
		log("download: bad request %#x for %s", static_cast<unsigned>(r), url.c_str());
		if (g_http.del_tmpl) g_http.del_tmpl(t);
		finish("Enlace no válido");
		return nullptr;
	}
	if (g_http.header != nullptr) {
		g_http.header(r, "Accept", "video/mp4,video/*;q=0.9,*/*;q=0.5", 0);
	}
	std::string error;
	const std::string path = "/data/bayosh/video/" + name;
	const std::string part = path + ".part";
	int status = 0;
	const int sr = g_http.send(r, nullptr, 0);
	if (sr != 0 || g_http.status(r, &status) != 0) {
		log("download: send failed %#x (%s)", static_cast<unsigned>(sr), url.c_str());
		error = "No se pudo conectar. Revisa el enlace y la red";
	} else if (status != 200) {
		log("download: HTTP %d (%s)", status, url.c_str());
		error = "El sitio respondió " + std::to_string(status) + ". Usa el enlace directo del .mp4";
	} else {
		int      res = -1;
		uint64_t len = 0;
		if (g_http.length != nullptr && g_http.length(r, &res, &len) == 0 && res == 0) {
			g_dl_total = static_cast<long long>(len);
		}
		log("download %s: %lld bytes from %s", name.c_str(), g_dl_total.load(), url.c_str());
		const int f = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (f < 0) {
			error = "No se puede escribir en /data/bayosh/video";
		} else {
			static char buf[256 * 1024];
			bool        head_ok = false;
			int         got     = 0;
			int         step    = 0;
			while (!g_dl_cancel && (got = g_http.read(r, buf, sizeof(buf))) > 0) {
				if (!head_ok && g_dl_got == 0) {
					// An .mp4 starts with an "ftyp" box; a web page here means the link is not the video.
					head_ok = got >= 8 && memcmp(buf + 4, "ftyp", 4) == 0;
					if (!head_ok) {
						error = "Ese enlace no es un video .mp4 (abre el video y copia su enlace directo)";
						break;
					}
				}
				if (write(f, buf, static_cast<size_t>(got)) != got) {
					error = "Sin espacio para guardar el video";
					break;
				}
				g_dl_got += got;
				if (g_dl_got > 8LL * 1024 * 1024 * 1024) {
					error = "El video pesa demasiado";
					break;
				}
				const long long tot = g_dl_total;
				if (tot > 0 && g_dl_got * 4 / tot > step) {
					step = static_cast<int>(g_dl_got * 4 / tot);
					log("download %s: %d%%", name.c_str(), step * 25);
				}
			}
			close(f);
			if (error.empty() && g_dl_cancel) {
				error = "Descarga cancelada";
			} else if (error.empty() && got < 0) {
				log("download: read error %#x", static_cast<unsigned>(got));
				error = "Se cortó la descarga";
			} else if (error.empty() && g_dl_total > 0 && g_dl_got != g_dl_total) {
				error = "Se cortó la descarga";
			} else if (error.empty() && g_dl_got == 0) {
				error = "El sitio no envió nada";
			}
			if (error.empty() && rename(part.c_str(), path.c_str()) != 0) {
				error = "No se pudo guardar el video";
			}
			if (!error.empty()) {
				unlink(part.c_str());
			}
		}
	}
	if (g_http.del_req) g_http.del_req(r);
	if (g_http.del_tmpl) g_http.del_tmpl(t);
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

} // namespace

bool start_download(const std::string& url, std::string& error) {
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
	g_dl_name   = url_file_name(url);
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

// Copyright (C) 2026 Josué Oswaldo Hernández López (bayo.sh). Licencia GPL-3.0, ver LICENSE.
// bayo.sh v0.4: HTTP/HTTPS client of its own. The system's libSceHttp2 answers every https:// request
// from a payload with 0x8095f00c (an SSL error) while plain HTTP works, so TLS is done here with
// mbedTLS (Apache-2.0) over ordinary sockets, checking certificates against Mozilla's CA list
// (certifi's cacert.pem, MPL-2.0) embedded in the payload.
#include "bayosh.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <immintrin.h>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

__asm__(".section .rodata\n"
        ".global bayosh_cacert_pem\n"
        ".global bayosh_cacert_pem_end\n"
        ".align  16\n"
        "bayosh_cacert_pem:\n"
        ".incbin \"" BAYOSH_CACERT_PEM "\"\n"
        "bayosh_cacert_pem_end:\n"
        ".byte 0\n"
        ".previous\n");
extern "C" const char bayosh_cacert_pem[];
extern "C" const char bayosh_cacert_pem_end[];

extern "C" {
// Hardware entropy for mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT): the PS5's Zen 2 has RDRAND.
__attribute__((target("rdrnd"))) int mbedtls_hardware_poll(void*, unsigned char* out, size_t len, size_t* olen) {
	size_t done = 0;
	while (done < len) {
		unsigned long long v = 0;
		int tries = 0;
		while (!_rdrand64_step(&v) && ++tries < 32) {
		}
		if (tries >= 32) {
			break;
		}
		const size_t n = len - done < sizeof(v) ? len - done : sizeof(v);
		memcpy(out + done, &v, n);
		done += n;
	}
	*olen = done;
	return 0;
}

int sceNetResolverCreate(const char*, int, int);
int sceNetResolverStartNtoa(int, const char*, in_addr*, int, int, int);
int sceNetResolverDestroy(int);
int sceNetPoolCreate(const char*, int, int);
int sceNetInit();
}

namespace bayosh {
namespace {

std::mutex         g_tls_mu;
bool               g_tls_ready = false;
mbedtls_x509_crt   g_ca;
int                g_net_mem = -1;

bool tls_ready() {
	std::lock_guard<std::mutex> lock(g_tls_mu);
	if (g_tls_ready) {
		return true;
	}
	const int n = sceNetInit();
	g_net_mem   = sceNetPoolCreate("bayosh_dns", 16 * 1024, 0);
	const psa_status_t ps = psa_crypto_init();
	mbedtls_x509_crt_init(&g_ca);
	const int rc = mbedtls_x509_crt_parse(&g_ca, reinterpret_cast<const unsigned char*>(bayosh_cacert_pem),
	                                      static_cast<size_t>(bayosh_cacert_pem_end - bayosh_cacert_pem) + 1);
	log("tls: net %d pool %d psa %d, CA list parsed (%d skipped)", n, g_net_mem, static_cast<int>(ps), rc);
	if (ps != PSA_SUCCESS || rc < 0) {
		return false;
	}
	g_tls_ready = true;
	return true;
}

bool resolve(const std::string& host, in_addr& addr, std::string& error) {
	if (inet_pton(AF_INET, host.c_str(), &addr) == 1) {
		return true;
	}
	const int rid = sceNetResolverCreate("bayosh", g_net_mem, 0);
	if (rid >= 0) {
		const int rc = sceNetResolverStartNtoa(rid, host.c_str(), &addr, 6 * 1000 * 1000, 2, 0);
		sceNetResolverDestroy(rid);
		if (rc >= 0) {
			return true;
		}
		log("tls: resolve %s failed %#x", host.c_str(), static_cast<unsigned>(rc));
	} else {
		log("tls: resolver create failed %#x", static_cast<unsigned>(rid));
	}
	error = "No se encontró el sitio (DNS)";
	return false;
}

int tcp_connect(const in_addr& addr, int port, int timeout_s) {
	const int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		return -1;
	}
	timeval tv {timeout_s, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	int one = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	const int rcv = 1024 * 1024; // big window: downloads were slow with the default
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
	sockaddr_in sa {};
	sa.sin_family = AF_INET;
	sa.sin_port   = htons(static_cast<uint16_t>(port));
	sa.sin_addr   = addr;
	if (connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
		close(s);
		return -1;
	}
	return s;
}

int bio_send(void* ctx, const unsigned char* buf, size_t len) {
	const int     s = *static_cast<int*>(ctx);
	const ssize_t n = send(s, buf, len, 0);
	if (n < 0) {
		return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
	return static_cast<int>(n);
}

int bio_recv(void* ctx, unsigned char* buf, size_t len) {
	const int     s = *static_cast<int*>(ctx);
	const ssize_t n = recv(s, buf, len, 0);
	if (n < 0) {
		return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
	return static_cast<int>(n); // 0 = peer closed
}

// One connection: plain socket or TLS on top of it.
struct Conn {
	int                      s   = -1;
	bool                     tls = false;
	mbedtls_ssl_context      ssl;
	mbedtls_ssl_config       conf;
	mbedtls_entropy_context  entropy;
	mbedtls_ctr_drbg_context drbg;
	bool                     inited = false;

	~Conn() {
		if (inited) {
			mbedtls_ssl_close_notify(&ssl);
			mbedtls_ssl_free(&ssl);
			mbedtls_ssl_config_free(&conf);
			mbedtls_ctr_drbg_free(&drbg);
			mbedtls_entropy_free(&entropy);
		}
		if (s >= 0) {
			close(s);
		}
	}
	int write(const char* p, size_t n) {
		size_t done = 0;
		while (done < n) {
			const int w = tls ? mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(p + done), n - done)
			                  : static_cast<int>(send(s, p + done, n - done, 0));
			if (w <= 0) {
				return -1;
			}
			done += static_cast<size_t>(w);
		}
		return static_cast<int>(done);
	}
	// >0 bytes, 0 end of stream, <0 error
	int read(char* p, size_t n) {
		if (!tls) {
			const ssize_t r = recv(s, p, n, 0);
			return r < 0 ? -1 : static_cast<int>(r);
		}
		for (;;) {
			const int r = mbedtls_ssl_read(&ssl, reinterpret_cast<unsigned char*>(p), n);
			if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE ||
			    r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
				continue;
			}
			if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
				return 0;
			}
			return r;
		}
	}
};

bool open_conn(Conn& c, const std::string& host, int port, bool tls, int timeout_s, std::string& error) {
	in_addr addr {};
	if (!resolve(host, addr, error)) {
		return false;
	}
	c.s = tcp_connect(addr, port, timeout_s);
	if (c.s < 0) {
		error = "No se pudo conectar. Revisa la red";
		log("tls: connect %s:%d failed (errno %d)", host.c_str(), port, errno);
		return false;
	}
	if (!tls) {
		return true;
	}
	c.tls = true;
	mbedtls_ssl_init(&c.ssl);
	mbedtls_ssl_config_init(&c.conf);
	mbedtls_entropy_init(&c.entropy);
	mbedtls_ctr_drbg_init(&c.drbg);
	c.inited = true;
	int rc = mbedtls_ctr_drbg_seed(&c.drbg, mbedtls_entropy_func, &c.entropy,
	                               reinterpret_cast<const unsigned char*>("bayo.sh"), 7);
	if (rc == 0) rc = mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
	                                             MBEDTLS_SSL_PRESET_DEFAULT);
	if (rc == 0) {
		mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
		mbedtls_ssl_conf_ca_chain(&c.conf, &g_ca, nullptr);
		mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, &c.drbg);
		rc = mbedtls_ssl_setup(&c.ssl, &c.conf);
	}
	if (rc == 0) rc = mbedtls_ssl_set_hostname(&c.ssl, host.c_str());
	if (rc == 0) {
		mbedtls_ssl_set_bio(&c.ssl, &c.s, bio_send, bio_recv, nullptr);
		while ((rc = mbedtls_ssl_handshake(&c.ssl)) == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
		}
	}
	if (rc != 0) {
		char msg[120];
		mbedtls_strerror(rc, msg, sizeof(msg));
		const uint32_t flags = mbedtls_ssl_get_verify_result(&c.ssl);
		log("tls: handshake with %s failed -0x%04x (%s), verify flags %#x", host.c_str(), static_cast<unsigned>(-rc), msg,
		    flags);
		error = flags != 0 && flags != 0xffffffffu ? "El certificado del sitio no es válido" : "No se pudo conectar de forma segura";
		return false;
	}
	return true;
}

std::string lower(std::string s) {
	for (char& ch : s) {
		if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
	}
	return s;
}

bool split_url(const std::string& url, bool& tls, std::string& host, int& port, std::string& path) {
	size_t p;
	if (url.rfind("https://", 0) == 0) {
		tls = true;
		p   = 8;
	} else if (url.rfind("http://", 0) == 0) {
		tls = false;
		p   = 7;
	} else {
		return false;
	}
	const size_t slash = url.find('/', p);
	std::string  hp    = url.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
	path               = slash == std::string::npos ? "/" : url.substr(slash);
	const size_t hash  = path.find('#');
	if (hash != std::string::npos) path.resize(hash);
	port              = tls ? 443 : 80;
	const size_t colon = hp.rfind(':');
	if (colon != std::string::npos && hp.find(']') == std::string::npos) {
		port = atoi(hp.c_str() + colon + 1);
		hp.resize(colon);
	}
	host = hp;
	return !host.empty() && port > 0 && port < 65536;
}

std::string resolve_location(const std::string& base, const std::string& loc) {
	if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) return loc;
	if (loc.rfind("//", 0) == 0) return base.substr(0, base.find(':') + 1) + loc;
	const size_t origin_end = base.find('/', base.find("//") + 2);
	const std::string origin = origin_end == std::string::npos ? base : base.substr(0, origin_end);
	if (!loc.empty() && loc[0] == '/') return origin + loc;
	const size_t q = base.find('?');
	const std::string b = base.substr(0, q);
	return b.substr(0, b.rfind('/') + 1) + loc;
}

} // namespace

bool web_get(const std::string& url0, const WebGetOptions& opt, const std::function<bool(long long, int)>& on_head,
             const std::function<bool(const char*, size_t)>& on_body, std::string& error, int& status) {
	status = 0;
	if (!tls_ready()) {
		error = "La consola no pudo iniciar HTTPS";
		return false;
	}
	std::string url = url0;
	for (int hop = 0; hop < 6; ++hop) {
		bool        tls;
		std::string host, path;
		int         port;
		if (!split_url(url, tls, host, port, path)) {
			error = "Enlace no válido";
			return false;
		}
		Conn c;
		if (!open_conn(c, host, port, tls, opt.timeout_s, error)) {
			return false;
		}
		std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ((tls && port != 443) || (!tls && port != 80) ? ":" + std::to_string(port) : "") +
		                  "\r\nUser-Agent: " + (opt.user_agent.empty() ? std::string("Mozilla/5.0 (PlayStation 5) bayo.sh") : opt.user_agent) + "\r\nAccept: " + (opt.accept.empty() ? "*/*" : opt.accept) +
		                  "\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
		if (!opt.referer.empty()) req += "Referer: " + opt.referer + "\r\n";
		req += "\r\n";
		if (c.write(req.data(), req.size()) < 0) {
			error = "No se pudo conectar. Revisa la red";
			return false;
		}
		// Headers
		std::string head;
		std::string extra;
		char        buf[16 * 1024];
		for (;;) {
			const int n = c.read(buf, sizeof(buf));
			if (n <= 0) {
				error = "El sitio cerró la conexión";
				log("tls: %s: no headers (%d)", url.c_str(), n);
				return false;
			}
			head.append(buf, static_cast<size_t>(n));
			const size_t e = head.find("\r\n\r\n");
			if (e != std::string::npos) {
				extra = head.substr(e + 4);
				head.resize(e + 2);
				break;
			}
			if (head.size() > 64 * 1024) {
				error = "Respuesta no válida";
				return false;
			}
		}
		status = atoi(head.c_str() + (head.find(' ') == std::string::npos ? 0 : head.find(' ') + 1));
		long long   length  = -1;
		bool        chunked = false;
		std::string location;
		size_t      ls      = head.find("\r\n");
		while (ls != std::string::npos && ls + 2 < head.size()) {
			const size_t le   = head.find("\r\n", ls + 2);
			std::string  line = head.substr(ls + 2, le - ls - 2);
			const size_t colon = line.find(':');
			if (colon != std::string::npos) {
				const std::string k = lower(line.substr(0, colon));
				std::string       v = line.substr(colon + 1);
				while (!v.empty() && v[0] == ' ') v.erase(0, 1);
				if (k == "content-length") length = atoll(v.c_str());
				else if (k == "transfer-encoding") chunked = lower(v).find("chunked") != std::string::npos;
				else if (k == "location") location = v;
			}
			ls = le;
		}
		if (status >= 300 && status < 400 && !location.empty()) {
			url = resolve_location(url, location);
			continue;
		}
		if (on_head && !on_head(length, status)) {
			return true; // caller does not want the body (e.g. HTTP error)
		}
		// Body: plain, Content-Length or chunked.
		long long got = 0;
		auto emit = [&](const char* p, size_t n) -> bool {
			got += static_cast<long long>(n);
			return on_body(p, n);
		};
		if (!chunked) {
			if (!extra.empty() && !emit(extra.data(), extra.size())) return true;
			while (length < 0 || got < length) {
				const int n = c.read(buf, sizeof(buf));
				if (n == 0) break;
				if (n < 0) {
					error = "Se cortó la conexión";
					return false;
				}
				if (!emit(buf, static_cast<size_t>(n))) return true;
			}
			if (length >= 0 && got < length) {
				error = "Se cortó la conexión";
				return false;
			}
			return true;
		}
		std::string pend = extra;
		auto fill = [&]() -> bool {
			const int n = c.read(buf, sizeof(buf));
			if (n <= 0) return false;
			pend.append(buf, static_cast<size_t>(n));
			return true;
		};
		for (;;) {
			size_t e;
			while ((e = pend.find("\r\n")) == std::string::npos) {
				if (!fill()) {
					error = "Se cortó la conexión";
					return false;
				}
			}
			const long long size = strtoll(pend.c_str(), nullptr, 16);
			pend.erase(0, e + 2);
			if (size <= 0) return true;
			long long left = size;
			while (left > 0) {
				if (pend.empty() && !fill()) {
					error = "Se cortó la conexión";
					return false;
				}
				const size_t take = static_cast<size_t>(std::min<long long>(left, static_cast<long long>(pend.size())));
				if (!emit(pend.data(), take)) return true;
				pend.erase(0, take);
				left -= static_cast<long long>(take);
			}
			while (pend.size() < 2) {
				if (!fill()) {
					error = "Se cortó la conexión";
					return false;
				}
			}
			pend.erase(0, 2);
		}
	}
	error = "Demasiadas redirecciones";
	return false;
}

} // namespace bayosh

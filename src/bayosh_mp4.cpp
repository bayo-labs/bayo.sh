// Copyright (C) 2026 Josué Oswaldo Hernández López (bayo.sh). Licencia GPL-3.0, ver LICENSE.
// bayo.sh v0.3.1: reads an .mp4's video track (codec, size, frame rate) so the window can tell
// which clips the PS5 wallpaper player cannot show (4K, HEVC, >60 fps) and offer to convert them.
#include "bayosh.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace bayosh {
namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

struct Track {
	bool        video = false;
	std::string codec;
	int         w = 0, h = 0;
	uint32_t    timescale = 0;
	uint64_t    duration  = 0;
	uint32_t    samples   = 0;
};

// Walks boxes in [p, end). Containers we need are entered; leaves fill the track.
void walk(const uint8_t* p, const uint8_t* end, Track& t, std::vector<Track>& out, int depth) {
	while (p + 8 <= end && depth < 12) {
		uint64_t     size = be32(p);
		const char*  type = reinterpret_cast<const char*>(p + 4);
		size_t       head = 8;
		if (size == 1 && p + 16 <= end) {
			size = be64(p + 8);
			head = 16;
		} else if (size == 0) {
			size = static_cast<uint64_t>(end - p);
		}
		if (size < head || size > static_cast<uint64_t>(end - p)) {
			return;
		}
		const uint8_t* body = p + head;
		const uint8_t* bend = p + size;
		if (memcmp(type, "trak", 4) == 0) {
			Track nt;
			walk(body, bend, nt, out, depth + 1);
			out.push_back(nt);
		} else if (memcmp(type, "mdia", 4) == 0 || memcmp(type, "minf", 4) == 0 || memcmp(type, "stbl", 4) == 0) {
			walk(body, bend, t, out, depth + 1);
		} else if (memcmp(type, "hdlr", 4) == 0 && bend - body >= 12) {
			t.video = memcmp(body + 8, "vide", 4) == 0;
		} else if (memcmp(type, "mdhd", 4) == 0 && bend - body >= 24) {
			if (body[0] == 1 && bend - body >= 32) {
				t.timescale = be32(body + 20);
				t.duration  = be64(body + 24);
			} else {
				t.timescale = be32(body + 12);
				t.duration  = be32(body + 16);
			}
		} else if (memcmp(type, "stsd", 4) == 0 && bend - body >= 8 + 8 + 36) {
			const uint8_t* e = body + 8; // fullbox + entry_count
			t.codec.assign(reinterpret_cast<const char*>(e + 4), 4);
			t.w = (e[32] << 8) | e[33];
			t.h = (e[34] << 8) | e[35];
		} else if (memcmp(type, "stsz", 4) == 0 && bend - body >= 12) {
			t.samples = be32(body + 8);
		}
		p = bend;
	}
}

Mp4Check check_file(const std::string& path) {
	Mp4Check r;
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		return r;
	}
	struct stat st {};
	fstat(fd, &st);
	const uint64_t fsize = static_cast<uint64_t>(st.st_size);
	uint64_t       off   = 0;
	std::vector<uint8_t> moov;
	uint8_t h[16];
	while (off + 8 <= fsize) {
		if (pread(fd, h, 16, static_cast<off_t>(off)) < 8) {
			break;
		}
		uint64_t size = be32(h);
		size_t   head = 8;
		if (size == 1) {
			size = be64(h + 8);
			head = 16;
		} else if (size == 0) {
			size = fsize - off;
		}
		if (size < head) {
			break;
		}
		if (memcmp(h + 4, "moov", 4) == 0) {
			if (size > 32u * 1024 * 1024) {
				break;
			}
			moov.resize(size - head);
			if (pread(fd, moov.data(), moov.size(), static_cast<off_t>(off + head)) != static_cast<ssize_t>(moov.size())) {
				moov.clear();
			}
			break;
		}
		off += size;
	}
	close(fd);
	if (moov.empty()) {
		return r;
	}
	std::vector<Track> tracks;
	Track               none;
	walk(moov.data(), moov.data() + moov.size(), none, tracks, 0);
	for (const Track& t : tracks) {
		if (!t.video) {
			continue;
		}
		r.parsed = true;
		r.codec  = t.codec;
		r.w      = t.w;
		r.h      = t.h;
		if (t.timescale != 0 && t.duration != 0) {
			r.fps = static_cast<double>(t.samples) * t.timescale / static_cast<double>(t.duration);
		}
		break;
	}
	if (!r.parsed) {
		return r;
	}
	// The PS5 home player: H.264 up to 1080p, up to 60 fps (test v0.3.0: 4K60 H.264 stays black).
	std::string why;
	const bool avc = r.codec == "avc1" || r.codec == "avc3";
	if (!avc) {
		why = r.codec == "hvc1" || r.codec == "hev1" ? "HEVC" : r.codec == "av01" ? "AV1" : r.codec == "vp09" ? "VP9" : r.codec;
	}
	if (r.w > 1920 || r.h > 1088) {
		why += (why.empty() ? "" : " · ") + std::string(r.w >= 3800 ? "4K" : r.w >= 2500 ? "2K" : "") +
		       (r.w >= 2500 ? " " : "") + std::to_string(r.w) + "×" + std::to_string(r.h);
	}
	if (r.fps > 61.0) {
		why += (why.empty() ? "" : " · ") + std::to_string(static_cast<int>(r.fps + 0.5)) + " fps";
	}
	r.ok  = why.empty();
	r.why = why;
	return r;
}

std::mutex                                        g_mu;
std::map<std::string, std::pair<long long, Mp4Check>> g_cache; // path -> (size ^ mtime, result)

} // namespace

Mp4Check mp4_check(const std::string& path) {
	struct stat st {};
	if (stat(path.c_str(), &st) != 0) {
		return {};
	}
	const long long key = static_cast<long long>(st.st_size) ^ (static_cast<long long>(st.st_mtime) << 20);
	{
		std::lock_guard<std::mutex> lock(g_mu);
		auto it = g_cache.find(path);
		if (it != g_cache.end() && it->second.first == key) {
			return it->second.second;
		}
	}
	const Mp4Check r = check_file(path);
	if (r.parsed) {
		log("format %s: %s %dx%d %.2f fps -> %s", path.c_str(), r.codec.c_str(), r.w, r.h, r.fps, r.ok ? "ok" : r.why.c_str());
	}
	std::lock_guard<std::mutex> lock(g_mu);
	g_cache[path] = {key, r};
	return r;
}

} // namespace bayosh

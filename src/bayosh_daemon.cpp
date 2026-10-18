/*
 * Copyright (C) 2026 Josué Oswaldo Hernández López (bayo.sh). Licencia GPL-3.0, ver LICENSE.
 *
 * bayosh_daemon.cpp - bayo.sh: the payload the user loads with their ELF launcher.
 * Lists the user's clips, mounts the chosen clip's folder into the system paths ShellUI's video
 * service can read, injects bayosh_shellui.elf into SceShellUI, adds the bayo.sh tile to the
 * home screen and serves its window (bayosh_web.cpp). Stays alive to re-inject after ShellUI
 * restarts. GPL-3.0; built on OnionHEN (aydencharles/onionHEN, v0.9.0-beta2).
 *
 * Files:
 *   /data/bayosh/video/          clips (also <USB>/bayosh/video)
 *   /data/bayosh/config.txt      enabled=1 / video=<path>   (written by the window)
 *   /data/bayosh/bayosh.log   this log + copies of the ShellUI log
 *   /system_tmp/bayosh/video.txt   what ShellUI should play: a path or "off"
 *   /system_tmp/bayosh/status.txt  what ShellUI is doing: playing <path> / stopped / failed <rc>
 */
#include "bayosh.hpp"
#include <sys/user.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <map>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <atomic>
#include <mutex>
#include <netinet/in.h>
#include <pthread.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <signal.h>

#include "onion/log.h"
#include "onion/proc_query.h"

extern "C" {
bool      Inject_Toolbox(int pid, uint8_t* elf);
int       pt_detach(pid_t pid, int sig);
uintptr_t set_ucred_to_ptrace(void);
uintptr_t set_proc_authid(pid_t pid, uintptr_t new_authid);
int       sceKernelTerminateProcess(int pid, int* ret);
int       sceSystemServiceGetAppIdOfRunningBigApp(void);
int       sceSystemServiceGetAppTitleId(int app_id, char* title_id);
int       kernel_dynlib_handle(pid_t pid, const char* basename, uint32_t* handle);
unsigned long kernel_dynlib_dlsym(int pid, unsigned int handle, const char* sym);
int       sceKernelOpenEventFlag(intptr_t* event_flag, const char* name);
int       sceKernelPollEventFlag(intptr_t event_flag, uint64_t bits, unsigned int wait_mode, uint64_t* result);
int       sceKernelCloseEventFlag(intptr_t event_flag);
int  sceKernelSendNotificationRequest(int, void*, size_t, int);
extern uint8_t bayosh_shellui_elf_start[];
}

__asm__(".section .rodata\n"
        ".global bayosh_shellui_elf_start\n"
        ".type   bayosh_shellui_elf_start, @object\n"
        ".align  16\n"
        "bayosh_shellui_elf_start:\n"
        ".incbin \"" BAYOSH_SHELLUI_ELF "\"\n"
        ".previous\n");

namespace bayosh {
namespace {

constexpr const char* kMountPoint     = "/system_ex/vsh_asset/wallpaper";
constexpr const char* kPathFile       = "/system_tmp/bayosh/video.txt";
constexpr const char* kStatusFile     = "/system_tmp/bayosh/status.txt";
constexpr const char* kShellUiLog     = "/system_tmp/bayosh/shellui.log";
constexpr const char* kDimFile        = "/system_tmp/bayosh/dim.txt";
constexpr const char* kVolumeFile     = "/system_tmp/bayosh/volume.txt";
constexpr const char* kBlurFile       = "/system_tmp/bayosh/blur.txt";
constexpr const char* kMenusFile      = "/system_tmp/bayosh/menus.txt";
constexpr const char* kStoreOnlyFile  = "/system_tmp/bayosh/storeonly.txt";
constexpr const char* kLooksFile      = "/system_tmp/bayosh/looks.txt";
constexpr const char* kLightFile      = "/system_tmp/bayosh/light.txt"; // RRGGBB or off (ShellUI module)
constexpr const char* kGuideFile      = "/data/bayosh/guide_done";
constexpr const char* kParallaxFile   = "/system_tmp/bayosh/parallax.txt";
constexpr const char* kLogFile        = "/data/bayosh/bayosh.log";
constexpr const char* kConfigFile     = "/data/bayosh/config.txt";
constexpr const char* kOurMarker      = "/system_tmp/bayosh/injected";
constexpr const char* kOnionToolbox   = "/system_tmp/onionhen/ready/toolbox";
constexpr int         kOnionCtrlPort  = 9048;
constexpr off_t       kLogMaxSize     = 512 * 1024;
constexpr long        kCrashWindowSec = 120;
// Runs next to OnionHEN (tested 2026-10-15): no "OnionHEN is running" refusal any more.
constexpr bool        kOnionGuard     = false;

volatile bool g_running        = true;
volatile bool g_stop_requested = false; // stop came from the user (port 9049), not from a crash
int           g_log     = -1;
std::mutex    g_log_lock;

// Control state; g_ctl guards everything below it.
std::mutex  g_ctl;
bool        g_enabled  = true;
int         g_dim      = 25;    // default on: clips are often bright
int         g_volume   = 100;   // sound of clips that have it, 0..100 %
int         g_blur     = 0;     // 0 off, 1 light, 2 strong
int         g_looks[4] = {0, 0, 0, 0}; // bottom fade, top fade, vignette, haze: 0 off, 1 light, 2 strong
bool        g_shuffle  = false; // a random clip each time the payload starts (off by default)
bool        g_main_video = true; // off = only tiles with their own video play one
bool        g_game_videos = true; // off = per-game videos ignored (games.txt kept)
bool        g_game_pause = true; // no video while a game runs
bool        g_menu_hide  = true;  // hide the video in Settings / system apps (default: safer)
bool        g_store_only = false; // video only while the PS Store tile is highlighted (off by default)
bool        g_parallax   = false; // nudge the video when moving between home tiles
bool        g_light      = false; // controller light bar follows the clip's colour (experimental, off by default)
std::string g_light_now  = "off"; // last value written to kLightFile
std::string g_light_mode = "follow"; // follow | fixed | rainbow | breathe | wave
std::string g_light_fix  = "22b8ff";
int         g_light_speed = 1;
bool        g_game_running = false;
std::string g_game_title;
std::string g_override;     // per-game clip playing instead of g_current ("" = main wallpaper)
volatile bool g_focus_recheck = false; // games.txt changed: re-evaluate the highlighted game now
std::string g_current;          // real path of the chosen clip
std::string g_mounted_folder;   // folder currently mounted on kMountPoint
bool        g_injected = false; // the ShellUI module is running (it reacts to the path file)

// ---- small file helpers ----------------------------------------------------------------------

bool write_text(const char* path, const std::string& text) {
	// beta10: /system_tmp is often full (errno 28). Truncating first frees the page and the write
	// then needs a new one; overwriting in place and shrinking afterwards needs no new space.
	const int fd = open(path, O_WRONLY | O_CREAT, 0666);
	if (fd < 0) {
		return false;
	}
	const bool ok = pwrite(fd, text.data(), text.size(), 0) == static_cast<ssize_t>(text.size()) &&
	                ftruncate(fd, static_cast<off_t>(text.size())) == 0;
	const int err = errno;
	close(fd);
	errno = err;
	return ok;
}

std::string read_text(const char* path, size_t max = 4096) {
	const int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return {};
	}
	std::string text(max, '\0');
	const ssize_t n = read(fd, &text[0], max);
	close(fd);
	text.resize(n > 0 ? static_cast<size_t>(n) : 0);
	return text;
}

std::string first_line(const std::string& text) {
	return text.substr(0, text.find_first_of("\r\n"));
}

bool exists(const char* path) {
	struct stat st {};
	return stat(path, &st) == 0;
}

void send_notification(const char* text, bool icon) {
	// Layout of the system's notification request (0xC30 bytes).
	struct Request {
		int32_t type, req_id, priority, msg_id, target_id, user_id, unk1, unk2, app_id, error_num, unk3;
		char    use_icon_image_uri;
		char    message[1024];
		char    uri[1024];
		char    unused[1027];
	};
	static_assert(sizeof(Request) == 0xC30, "notification request layout");
	Request req {};
	req.target_id = -1;
	snprintf(req.message, sizeof(req.message), "%s", text);
	if (icon) {
		req.use_icon_image_uri = 1;
		snprintf(req.uri, sizeof(req.uri), "%s", "cxml://psnotification/tex_icon_system");
	}
	sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
	log("notify: %s", text);
}

// ---- OnionHEN guard + markers ----------------------------------------------------------------

bool onion_daemon_answers() {
	const int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		return false;
	}
	sockaddr_in addr = {};
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons(kOnionCtrlPort);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const bool up = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
	close(s);
	return up;
}

long read_pid_file(const char* path) {
	const std::string text = read_text(path, 31);
	return text.empty() ? -1 : strtol(text.c_str(), nullptr, 10);
}

// ---- clips + mounting ------------------------------------------------------------------------

bool is_mp4(const char* name) {
	const size_t n = strlen(name);
	return n > 4 && name[0] != '.' && strcasecmp(name + n - 4, ".mp4") == 0;
}

std::string folder_of(const std::string& path) {
	return path.substr(0, path.rfind('/'));
}

std::string name_of(const std::string& path) {
	return path.substr(path.rfind('/') + 1);
}

bool mount_window_at(const char* mount_point, const char* source) {
	mkdir(mount_point, 0777);
	(void)unmount(mount_point, MNT_FORCE);
	char fstype[] = "nullfs", fspath_key[] = "fspath", target_key[] = "target", fstype_key[] = "fstype";
	char fspath[256], target[256];
	snprintf(fspath, sizeof(fspath), "%s", mount_point);
	snprintf(target, sizeof(target), "%s", source);
	iovec iov[] = {
	    {fstype_key, sizeof(fstype_key)}, {fstype, sizeof(fstype)},
	    {fspath_key, sizeof(fspath_key)}, {fspath, strlen(fspath) + 1},
	    {target_key, sizeof(target_key)}, {target, strlen(target) + 1},
	};
	if (nmount(iov, sizeof(iov) / sizeof(iov[0]), MNT_RDONLY) != 0) {
		log("nullfs mount %s -> %s failed (errno %d)", source, mount_point, errno);
		return false;
	}
	return true;
}

// /system_ex/vsh_asset/wallpaper is NOT Sony's: on the dev console it was made once while OnionHEN had
// /system_ex writable, so every test found it. A normal PS5 has no such folder and /system_ex is
// read-only -> "nullfs mount ... failed (errno 2)" and no video (GitHub issue #2). Make it once the
// way OnionHEN's bootstrapper does: remount /system_ex writable in place (it stays so, as with
// OnionHEN, which relies on that), add only this one empty folder, never touch Sony's files.
bool remount_system_ex_writable() {
	char k_fstype[] = "fstype", v_fstype[] = "exfatfs", k_fspath[] = "fspath", v_fspath[] = "/system_ex";
	char k_from[] = "from", v_from[] = "/dev/ssd0.system_ex", k_large[] = "large", v_yes[] = "yes";
	char k_tz[] = "timezone", v_tz[] = "static", k_async[] = "async", k_acl[] = "ignoreacl";
	iovec iov[] = {
	    {k_fstype, sizeof(k_fstype)}, {v_fstype, sizeof(v_fstype)}, {k_fspath, sizeof(k_fspath)},
	    {v_fspath, sizeof(v_fspath)}, {k_from, sizeof(k_from)},     {v_from, sizeof(v_from)},
	    {k_large, sizeof(k_large)},   {v_yes, sizeof(v_yes)},       {k_tz, sizeof(k_tz)},
	    {v_tz, sizeof(v_tz)},         {k_async, sizeof(k_async)},   {nullptr, 0},
	    {k_acl, sizeof(k_acl)},       {nullptr, 0},
	};
	return nmount(iov, sizeof(iov) / sizeof(iov[0]), MNT_UPDATE) == 0;
}

bool ensure_mount_point() {
	struct stat st {};
	if (stat(kMountPoint, &st) == 0 && S_ISDIR(st.st_mode)) {
		return true;
	}
	if (mkdir(kMountPoint, 0777) == 0) {
		log("made %s", kMountPoint);
		return true;
	}
	const int first = errno;
	if (!remount_system_ex_writable()) {
		log("cannot make %s (errno %d): /system_ex remount failed (errno %d)", kMountPoint, first, errno);
		return false;
	}
	if (mkdir(kMountPoint, 0777) != 0 && errno != EEXIST) {
		log("cannot make %s after remounting /system_ex (errno %d)", kMountPoint, errno);
		return false;
	}
	log("made %s (/system_ex remounted writable, as OnionHEN does)", kMountPoint);
	return true;
}

// System processes each see their own sandbox view (/mnt/sandbox/<id>/...): a mount on the real
// /system_ex does not show there, so the window is opened in every sandbox too.
void mount_window_everywhere(const std::string& folder) {
	if (!ensure_mount_point()) {
		notify("bayo.sh no pudo preparar su carpeta de video. Registro: /data/bayosh/bayosh.log");
		return;
	}
	if (!mount_window_at(kMountPoint, folder.c_str())) {
		return;
	}
	int  mounted = 0, tried = 0;
	DIR* dir     = opendir("/mnt/sandbox");
	if (dir != nullptr) {
		while (dirent* e = readdir(dir)) {
			if (e->d_name[0] == '.') {
				continue;
			}
			char point[512];
			snprintf(point, sizeof(point), "/mnt/sandbox/%s/system_ex/vsh_asset/wallpaper", e->d_name);
			struct stat st {};
			if (stat(point, &st) != 0 || !S_ISDIR(st.st_mode)) {
				continue;
			}
			++tried;
			mounted += mount_window_at(point, folder.c_str()) ? 1 : 0;
		}
		closedir(dir);
	}
	g_mounted_folder = folder;
	log("%s mounted on %s (+ %d of %d sandboxes)", folder.c_str(), kMountPoint, mounted, tried);
}

void unmount_window_everywhere() {
	int  done = 0;
	DIR* dir  = opendir("/mnt/sandbox");
	if (dir != nullptr) {
		while (dirent* e = readdir(dir)) {
			if (e->d_name[0] == '.') {
				continue;
			}
			char point[512];
			snprintf(point, sizeof(point), "/mnt/sandbox/%s/system_ex/vsh_asset/wallpaper", e->d_name);
			done += unmount(point, MNT_FORCE) == 0 ? 1 : 0;
		}
		closedir(dir);
	}
	const int main_rc = unmount(kMountPoint, MNT_FORCE);
	g_mounted_folder.clear();
	log("window closed (%s, %d sandboxes)", main_rc == 0 ? "main ok" : "main not mounted", done);
}

// Like OnionHEN's shutdown: end SceShellUI so the system starts a clean one (no module, no video).
bool restart_shellui() {
	const pid_t pid = onion_find_pid("SceShellUI");
	if (pid <= 1) {
		log("restart: SceShellUI not found");
		return false;
	}
	constexpr uintptr_t kDecidAuthId = 0x4800000000000022ULL; // needed for sceKernelTerminateProcess
	const uintptr_t     old          = set_proc_authid(getpid(), kDecidAuthId);
	int                 ret          = 0;
	const int           rc           = sceKernelTerminateProcess(static_cast<int>(pid), &ret);
	set_proc_authid(getpid(), old);
	if (rc != 0) {
		log("restart: TerminateProcess(%d) rc %d ret %d, SIGKILL instead", static_cast<int>(pid), rc, ret);
		kill(pid, SIGKILL);
	}
	// v0.3: make sure it really went (beta4: it did not, the old module stayed inside, and the next
	// start injected a second one on top: frozen menu).
	for (int i = 0; i < 60; ++i) {
		usleep(100 * 1000);
		const pid_t now = onion_find_pid("SceShellUI");
		if (now != pid) {
			log("restart: SceShellUI %d ended (now %d)", static_cast<int>(pid), static_cast<int>(now));
			return true;
		}
	}
	log("restart: SceShellUI %d is STILL running after 6 s", static_cast<int>(pid));
	return false;
}

// ---- config ----------------------------------------------------------------------------------

std::string looks_text() {
	return std::to_string(g_looks[0]) + " " + std::to_string(g_looks[1]) + " " + std::to_string(g_looks[2]) + " " + std::to_string(g_looks[3]);
}

void write_looks() {
	write_text(kLooksFile, looks_text() + "\n");
}

void load_config() {
	const std::string text = read_text(kConfigFile);
	size_t            pos  = 0;
	while (pos < text.size()) {
		const size_t      end  = text.find('\n', pos);
		const std::string line = first_line(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
		if (line.rfind("enabled=", 0) == 0) {
			g_enabled = line.substr(8) != "0";
		} else if (line.rfind("video=", 0) == 0) {
			g_current = line.substr(6);
		} else if (line.rfind("dim=", 0) == 0) {
			g_dim = std::max(0, std::min(80, atoi(line.c_str() + 4)));
		} else if (line.rfind("volume=", 0) == 0) {
			g_volume = std::max(0, std::min(100, atoi(line.c_str() + 7)));
		} else if (line.rfind("looks=", 0) == 0) {
			sscanf(line.c_str() + 6, "%d %d %d %d", &g_looks[0], &g_looks[1], &g_looks[2], &g_looks[3]);
			for (int& v : g_looks) {
				v = std::max(0, std::min(2, v));
			}
			g_looks[0] = g_looks[1] = 0; // bottom / top fade were removed (user)
		} else if (line.rfind("blur=", 0) == 0) {
			g_blur = std::max(0, std::min(2, atoi(line.c_str() + 5)));
		} else if (line.rfind("gamevideos=", 0) == 0) {
			g_game_videos = line.substr(11) != "0";
		} else if (line.rfind("mainvideo=", 0) == 0) {
			// ignored: the "Main video" switch was removed (it is always on)
		} else if (line.rfind("shuffle=", 0) == 0) {
			g_shuffle = line.substr(8) != "0";
		} else if (line.rfind("parallax=", 0) == 0) {
			g_parallax = line.substr(9) != "0";
		} else if (line.rfind("menuhide=", 0) == 0) {
			g_menu_hide = line.substr(9) != "0";
		} else if (line.rfind("storeonly=", 0) == 0) {
			g_store_only = line.substr(10) != "0";
		} else if (line.rfind("gamepause=", 0) == 0) {
			g_game_pause = line.substr(10) != "0";
		} else if (line.rfind("light=", 0) == 0) {
			g_light = line.substr(6) != "0";
		} else if (line.rfind("lightmode=", 0) == 0) {
			g_light_mode = line.substr(10);
		} else if (line.rfind("lightfix=", 0) == 0) {
			g_light_fix = line.substr(9);
		} else if (line.rfind("lightspeed=", 0) == 0) {
			g_light_speed = std::max(0, std::min(2, atoi(line.c_str() + 11)));
		}
		if (end == std::string::npos) {
			break;
		}
		pos = end + 1;
	}
	log("config: enabled=%d video=%s dim=%d shuffle=%d gamepause=%d", g_enabled ? 1 : 0, g_current.c_str(), g_dim,
	    g_shuffle ? 1 : 0, g_game_pause ? 1 : 0);
	write_text(kDimFile, std::to_string(g_dim) + "\n");
	write_text(kVolumeFile, std::to_string(g_volume) + "\n");
	write_text(kBlurFile, std::to_string(g_blur) + "\n");
	write_text(kMenusFile, g_menu_hide ? "1\n" : "0\n");
	write_text(kStoreOnlyFile, g_store_only ? "1\n" : "0\n");
	write_looks();
	write_text(kParallaxFile, g_parallax ? "1\n" : "0\n");
}

void save_config() {
	write_text(kConfigFile, std::string("enabled=") + (g_enabled ? "1" : "0") + "\nvideo=" + g_current + "\ndim=" + std::to_string(g_dim) + "\nvolume=" + std::to_string(g_volume) + "\nblur=" + std::to_string(g_blur) + "\nlooks=" + looks_text() +
	                            "\nshuffle=" + (g_shuffle ? "1" : "0") + "\nmainvideo=" + (g_main_video ? "1" : "0") + "\ngamevideos=" + (g_game_videos ? "1" : "0") + "\ngamepause=" + (g_game_pause ? "1" : "0") + "\nmenuhide=" + (g_menu_hide ? "1" : "0") + "\nstoreonly=" + (g_store_only ? "1" : "0") + "\nparallax=" + (g_parallax ? "1" : "0") + "\nlight=" + (g_light ? "1" : "0") + "\nlightmode=" + g_light_mode + "\nlightfix=" + g_light_fix + "\nlightspeed=" + std::to_string(g_light_speed) + "\n");
}

// ---- telling ShellUI what to play ------------------------------------------------------------

// ---- messages from our ShellUI module (UDP 127.0.0.1:9051) ------------------------------------
// ShellUI may only write ~16 KB of files per run (test v11-8: status.txt vanished, every switch took
// the slow path), so it sends "S<status>", "F<title id>", "L<log line>" here instead.
constexpr int         kUiPort    = 9051;
constexpr const char* kUiLogFile = "/data/bayosh/shellui.log";
std::mutex            g_ui_lock;
std::string           g_ui_status, g_ui_focus;
bool                  g_ui_status_set = false, g_ui_focus_set = false;
std::atomic<long>     g_ui_bytes{0}; // everything received: ShellUI is alive when this grows

void ui_forget_status() {
	std::lock_guard<std::mutex> lock(g_ui_lock);
	g_ui_status_set = false;
	g_ui_status.clear();
}

void ui_set_focus(const std::string& tid) {
	std::lock_guard<std::mutex> lock(g_ui_lock);
	g_ui_focus     = tid;
	g_ui_focus_set = true;
}

std::string shellui_status() {
	{
		std::lock_guard<std::mutex> lock(g_ui_lock);
		if (g_ui_status_set) {
			return g_ui_status;
		}
	}
	return first_line(read_text(kStatusFile, 700));
}

std::string shellui_focus() {
	{
		std::lock_guard<std::mutex> lock(g_ui_lock);
		if (g_ui_focus_set) {
			return g_ui_focus;
		}
	}
	return first_line(read_text("/system_tmp/bayosh/focus_game.txt", 64));
}

int open_ui_port() {
	const int   s = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in a {};
	a.sin_family      = AF_INET;
	a.sin_port        = htons(kUiPort);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (s < 0 || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
		log("ui channel: bind failed (errno %d), ShellUI falls back to files", errno);
		if (s >= 0) {
			close(s);
		}
		return -1;
	}
	timeval tv {1, 0}; // recv wakes up every second: rest mode / reopen checks
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return s;
}

void* ui_thread(void*) {
	int       s  = -1, fails = 0, epoch = net_epoch(); // reopened after rest mode (see power_thread)
	const int lf = open(kUiLogFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	char      buf[1600];
	while (g_running) {
		if (s >= 0 && epoch != net_epoch()) {
			log("ui channel: reopening after rest mode");
			close(s);
			s = -1;
		}
		if (s < 0) {
			epoch = net_epoch();
			s     = open_ui_port();
			if (s < 0) {
				sleep(2);
				continue;
			}
			fails = 0;
		}
		const ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue; // the 1 s timeout
			}
			if (++fails >= 40) {
				log("ui channel broken (errno %d), reopening", errno);
				close(s);
				s = -1;
			}
			usleep(50 * 1000);
			continue;
		}
		fails = 0;
		if (n == 0) {
			continue;
		}
		g_ui_bytes += n;
		if (buf[0] == 'S' || buf[0] == 'F') {
			std::lock_guard<std::mutex> lock(g_ui_lock);
			(buf[0] == 'S' ? g_ui_status : g_ui_focus).assign(buf + 1, static_cast<size_t>(n - 1));
			(buf[0] == 'S' ? g_ui_status_set : g_ui_focus_set) = true;
		} else if (buf[0] == 'L' && lf >= 0) {
			struct stat st {};
			if (fstat(lf, &st) == 0 && st.st_size > 2 * 1024 * 1024) {
				(void)ftruncate(lf, 0);
				(void)lseek(lf, 0, SEEK_SET);
			}
			(void)write(lf, buf + 1, static_cast<size_t>(n - 1));
		}
	}
	return nullptr;
}

// Wait until ShellUI no longer holds a clip (so its folder can be unmounted safely).
bool wait_shellui_stopped(int timeout_ms) {
	for (int waited = 0; waited < timeout_ms; waited += 100) {
		const std::string st = shellui_status();
		if (st.rfind("playing", 0) != 0 && st.rfind("starting", 0) != 0 && st.rfind("stopping", 0) != 0) {
			return true;
		}
		usleep(100 * 1000);
	}
	return false;
}

// The folder we mount for a clip: the whole video tree (internal video folder, or the USB bayosh
// folder), so clips in game subfolders (video/PPSA01234/) share one mount and can cross-fade.
std::string mount_base(const std::string& clip) {
	const std::string internal = "/data/bayosh/video";
	if (clip.rfind(internal + "/", 0) == 0) {
		return internal;
	}
	if (clip.rfind("/mnt/usb", 0) == 0) {
		const size_t a = clip.find('/', 5);               // after "/mnt/"
		const size_t b = a == std::string::npos ? a : clip.find('/', a + 1); // end of the bayosh folder
		if (b != std::string::npos) {
			return clip.substr(0, b);
		}
	}
	return folder_of(clip);
}

std::string shown_path(const std::string& clip) {
	return std::string(kMountPoint) + clip.substr(mount_base(clip).size());
}

// write_fast_map(): below, with games.txt (declared in bayosh.hpp)

bool apply_inner(std::string& error);

// PS Store only works on its own, like the main video: ONE clip plays (hidden on other tiles), so
// nothing ever swaps. It is the clip chosen for the Store (games.txt NPXS40047), else the main one.
// (The first try kept the main clip and swapped on the Store tile: its first frame showed, user.)
std::string store_clip() {
	const std::string c = saved_game_clip("NPXS40047");
	return !c.empty() && exists(c.c_str()) ? c : g_current;
}

// Caller holds g_ctl. Every change also refreshes ShellUI's fast-switch map.
bool apply_locked(std::string& error) {
	const bool ok = apply_inner(error);
	write_fast_map();
	return ok;
}

bool apply_inner(std::string& error) {
	// The tile's own clip wins; otherwise the main one, unless "Main video" is off (then nothing).
	const std::string clip = g_store_only ? store_clip()
	                         : !g_override.empty() ? g_override : (g_main_video ? g_current : std::string());
	// Same folder while a clip plays: quick swap in ShellUI (fade to black, swap, fade in), no
	// release wait: the folder stays mounted (the full stop took ~4 s per switch, test v11-4).
	if (g_injected && g_enabled && !(g_game_pause && g_game_running) && !clip.empty() && exists(clip.c_str()) &&
	    mount_base(clip) == g_mounted_folder &&
	    (shellui_status().rfind("playing", 0) == 0 || shellui_status().rfind("starting", 0) == 0)) {
		// "starting" too: ShellUI's own fast switch may be mid-swap (test v11-9: that moment sent us
		// down the slow path, ~2.5 s of black).
		const std::string shown = shown_path(clip);
		write_text(kPathFile, "switch:" + shown + "\n");
		log("apply: quick swap to %s", clip.c_str());
		return true;
	}
	const bool want_video = g_enabled && !(g_game_pause && g_game_running) && !clip.empty() && exists(clip.c_str());
	if (want_video && g_injected && mount_base(clip) == g_mounted_folder) {
		const std::string st = shellui_status();
		if (st.empty() || st.rfind("stopped", 0) == 0) {
			write_text(kPathFile, shown_path(clip) + "\n"); // nothing to release first
			log("apply: start %s (folder already mounted)", clip.c_str());
			return true;
		}
	}
	if (g_injected) {
		write_text(kPathFile, "off\n");
		if (!wait_shellui_stopped(8000)) {
			error = "The home screen did not release the old video";
			log("apply: ShellUI still busy (%s)", shellui_status().c_str());
			return false;
		}
	}
	if (!want_video) {
		write_text(kPathFile, "off\n");
		log("apply: video off");
		return true;
	}
	const std::string folder = mount_base(clip);
	if (folder != g_mounted_folder || !g_injected) {
		mount_window_everywhere(folder); // also refreshes the sandboxes before a (re)injection
	}
	const std::string shown = shown_path(clip);
	write_text(kPathFile, shown + "\n");
	log("apply: %s (shown as %s)", clip.c_str(), shown.c_str());
	return true;
}

// ---- injection -------------------------------------------------------------------------------

off_t file_size(const char* path) {
	struct stat st {};
	return stat(path, &st) == 0 ? st.st_size : -1;
}

// The injector stops ShellUI (ptrace), runs a stager up to a breakpoint and detaches. If that
// detach does not take, the whole ShellUI stays stopped (our module's heartbeat log freezes after
// ~2 ms) until the system kills it. Detect that and let it run again.
void unfreeze_if_stuck(pid_t pid) {
	usleep(400 * 1000);
	const off_t first = file_size(kShellUiLog) + g_ui_bytes;
	usleep(2600 * 1000); // the module waits 1.5 s, then logs ~4 lines/s
	const off_t second = file_size(kShellUiLog) + g_ui_bytes;
	if (second > first) {
		log("ShellUI is running (log %ld -> %ld bytes)", static_cast<long>(first), static_cast<long>(second));
		return;
	}
	log("ShellUI looks FROZEN after injection (log stuck at %ld bytes): detaching again + SIGCONT",
	    static_cast<long>(second));
	const uintptr_t old_authid = set_ucred_to_ptrace();
	const int       det        = pt_detach(pid, 0);
	const int       det_errno  = errno;
	const int       cont       = kill(pid, SIGCONT);
	if (old_authid != 0) {
		set_proc_authid(getpid(), old_authid);
	}
	usleep(1000 * 1000);
	const off_t third = file_size(kShellUiLog) + g_ui_bytes;
	log("unfreeze: detach %d (errno %d), SIGCONT %d, log now %ld bytes (%s)", det, det_errno, cont,
	    static_cast<long>(third), third > second ? "RUNNING again" : "still stuck");
}

// A just-started ShellUI is patched by kstuff once its trophy libraries are mapped; OnionHEN waits for
// the same libraries before injecting. We injected at once: right after a ShellUI restart that froze
// ShellUI every time (v1.1-rc3, "FROZEN after injection", then the crash guard). Wait the same way.
void wait_shellui_ready(pid_t pid) {
	bool waited = false;
	for (const char* name : {"libSceNpTrophy.sprx", "libSceNpTrophy2.sprx"}) {
		int i = 0;
		for (; i < 60; ++i) { // max 30 s each, like kstuff
			uint32_t handle = 0xffffffffu;
			if (kernel_dynlib_handle(pid, name, &handle) == 0 && handle != 0xffffffffu) {
				break;
			}
			if (kill(pid, 0) != 0) {
				return; // gone; the caller notices
			}
			waited = true;
			usleep(500 * 1000);
		}
		if (i == 60) {
			log("ShellUI %d: %s still not loaded after 30 s, injecting anyway", static_cast<int>(pid), name);
		} else if (i > 0) {
			log("ShellUI %d: %s loaded after %d ms", static_cast<int>(pid), name, i * 500);
		}
	}
	if (waited) {
		sleep(3); // a fresh ShellUI: let kstuff finish with it
	}
}

// bayo.sh v0.0.3: do ShellUI's kernel work from here, before injecting (see kSymsFile in the module).
constexpr const char* kSymsFile = "/system_tmp/bayosh/syms.txt";
void prepare_shellui(pid_t pid) {
	unlink(kSymsFile);
	uint32_t handle = 0xffffffffu;
	if (kernel_dynlib_handle(pid, "libkernel_sys.sprx", &handle) != 0 || handle == 0xffffffffu) {
		log("prep: libkernel_sys not found in ShellUI %d", static_cast<int>(pid));
		return;
	}
	const unsigned long mp = kernel_dynlib_dlsym(pid, handle, "sceKernelMprotect");
	const unsigned long dl = kernel_dynlib_dlsym(pid, handle, "sceKernelDlsym");
	if (mp == 0 || dl == 0) {
		log("prep: symbols missing (mprotect %#lx, dlsym %#lx)", mp, dl);
		return;
	}
	// v0.0.4: never change ShellUI's authid. Every freeze (v0.0.1-3) came right after it was raised
	// to PTRACE_AUTHID, whether from inside or from here. Lookups use sceKernelDlsym (no authid).
	const uintptr_t old = 0;
	char text[64];
	snprintf(text, sizeof(text), "%lx %lx\n", mp, dl);
	write_text(kSymsFile, text);
	log("prep: ShellUI %d authid %#lx untouched, mprotect %#lx, dlsym %#lx", static_cast<int>(pid),
	    static_cast<unsigned long>(old), mp, dl);
}

// v0.0.8: resolve ShellUI's symbols from here (kernel reads work in this process) and send them to
// the module over loopback UDP 9052; it has no way to find them itself without privileges.
const char* const kMonoSyms[] = {
	"mono_assembly_get_image",
	"mono_class_from_mono_type",
	"mono_class_from_name",
	"mono_class_get",
	"mono_class_get_field_from_name",
	"mono_class_get_fields",
	"mono_class_get_method_from_name",
	"mono_class_get_methods",
	"mono_class_get_name",
	"mono_class_get_parent",
	"mono_class_get_properties",
	"mono_class_get_property_from_name",
	"mono_class_get_type",
	"mono_class_is_enum",
	"mono_class_is_valuetype",
	"mono_class_vtable",
	"mono_compile_method",
	"mono_domain_assembly_open",
	"mono_field_get_name",
	"mono_field_get_value",
	"mono_field_set_value",
	"mono_field_static_get_value",
	"mono_gchandle_new",
	"mono_get_corlib",
	"mono_get_root_domain",
	"mono_image_get_table_rows",
	"mono_method_desc_free",
	"mono_method_desc_new",
	"mono_method_desc_search_in_class",
	"mono_method_get_name",
	"mono_method_get_object",
	"mono_method_get_param_names",
	"mono_method_signature",
	"mono_object_get_class",
	"mono_object_new",
	"mono_object_to_string",
	"mono_object_unbox",
	"mono_property_get_get_method",
	"mono_property_get_name",
	"mono_property_get_set_method",
	"mono_runtime_invoke",
	"mono_signature_get_params",
	"mono_signature_get_return_type",
	"mono_string_new",
	"mono_string_to_utf8",
	"mono_thread_attach",
	"mono_type_get_name",
	"mono_type_get_object"
};
void* send_symbols_thread(void* arg) {
	const pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(arg));
	std::string lines;
	int         count = 0;
	uint32_t    ks = 0xffffffffu, mono = 0xffffffffu;
	kernel_dynlib_handle(pid, "libkernel_sys.sprx", &ks);
	kernel_dynlib_handle(pid, "libmonosgen-2.0.sprx", &mono);
	char line[96];
	if (ks != 0xffffffffu) {
		const unsigned long a = kernel_dynlib_dlsym(pid, ks, "sceKernelMprotect");
		if (a != 0) {
			snprintf(line, sizeof(line), "sceKernelMprotect %lx\n", a);
			lines += line;
			++count;
		}
	}
	int missing = 0;
	for (const char* name : kMonoSyms) {
		const unsigned long a = mono != 0xffffffffu ? kernel_dynlib_dlsym(pid, mono, name) : 0;
		if (a == 0) {
			++missing;
			continue;
		}
		snprintf(line, sizeof(line), "%s %lx\n", name, a);
		lines += line;
		++count;
	}
	// Controller light bar (experimental): ShellUI owns the pad session, so the module calls these.
	uint32_t pad = 0xffffffffu, us = 0xffffffffu;
	kernel_dynlib_handle(pid, "libScePad.sprx", &pad);
	kernel_dynlib_handle(pid, "libSceUserService.sprx", &us);
	for (const char* name : {"scePadGetHandle", "scePadSetLightBar", "scePadResetLightBar", "scePadInit", "scePadOpen"}) {
		const unsigned long a = pad != 0xffffffffu ? kernel_dynlib_dlsym(pid, pad, name) : 0;
		if (a != 0) {
			snprintf(line, sizeof(line), "%s %lx\n", name, a);
			lines += line;
			++count;
		}
	}
	for (const char* name : {"sceUserServiceGetForegroundUser", "sceUserServiceGetLoginUserIdList"}) {
		const unsigned long a = us != 0xffffffffu ? kernel_dynlib_dlsym(pid, us, name) : 0;
		if (a != 0) {
			snprintf(line, sizeof(line), "%s %lx\n", name, a);
			lines += line;
			++count;
		}
	}
	log("light bar symbols: libScePad %#x, libSceUserService %#x", pad, us);
	log("symbols for ShellUI %d: libkernel_sys %#x, libmonosgen %#x, %d resolved, %d missing",
	    static_cast<int>(pid), ks, mono, count, missing);
	const int sock = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in to{};
	to.sin_family      = AF_INET;
	to.sin_port        = htons(9052);
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	snprintf(line, sizeof(line), "END %d\n", count);
	for (int round = 0; sock >= 0 && round < 40 && kill(pid, 0) == 0; ++round) { // ~20 s
		size_t pos = 0;
		while (pos < lines.size()) {
			size_t len = std::min<size_t>(1200, lines.size() - pos);
			const size_t cut = lines.rfind('\n', pos + len - 1);
			if (cut != std::string::npos && cut >= pos) {
				len = cut - pos + 1;
			}
			sendto(sock, lines.data() + pos, len, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
			pos += len;
		}
		sendto(sock, line, strlen(line), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
		usleep(500 * 1000);
	}
	if (sock >= 0) {
		close(sock);
	}
	return nullptr;
}

pid_t inject_shellui() {
	const pid_t pid = onion_find_pid("SceShellUI");
	if (pid <= 0) {
		log("SceShellUI not found");
		return 0;
	}
	if (kOnionGuard && (onion_daemon_answers() || read_pid_file(kOnionToolbox) == pid)) {
		notify("bayo.sh no puede correr mientras OnionHEN está activo.");
		return -1;
	}
	if (read_pid_file(kOurMarker) == pid) {
		log("already injected in ShellUI pid %d", static_cast<int>(pid));
		return pid;
	}
	wait_shellui_ready(pid);
	// v0.3: a bayo.sh module from an earlier start may still live in this ShellUI (marker lost). It
	// reports its status every ~2 s on our channel: if anything arrives, do not inject a second one.
	const long heard0 = g_ui_bytes; // only what arrives now counts (a module of an older, dead ShellUI is silent)
	for (int i = 0; i < 30 && g_ui_bytes == heard0; ++i) {
		usleep(100 * 1000);
	}
	if (g_ui_bytes != heard0) {
		log("a bayo.sh module is already inside ShellUI pid %d (it is talking on our channel): NOT injecting again",
		    static_cast<int>(pid));
		notify("Ya hay una bayo.sh dentro del menú. Reinicia la consola para cargar esta versión.");
		return -1;
	}
	unlink(kStatusFile);
	ui_forget_status();
	log("injecting into SceShellUI pid %d", static_cast<int>(pid));
	if (!Inject_Toolbox(static_cast<int>(pid), bayosh_shellui_elf_start)) {
		log("injection FAILED");
		notify("bayo.sh no pudo arrancar (falló la conexión con el menú).");
		return -1;
	}
	write_text(kOurMarker, std::to_string(pid));
	log("injected into ShellUI pid %d", static_cast<int>(pid));
	{
		pthread_t th;
		if (pthread_create(&th, nullptr, send_symbols_thread, reinterpret_cast<void*>(static_cast<intptr_t>(pid))) == 0) {
			pthread_detach(th);
		}
	}
	unfreeze_if_stuck(pid);
	return pid;
}

void copy_shellui_log() {
	std::string text = read_text(kUiLogFile, 2 * 1024 * 1024);
	if (text.size() > 64 * 1024) {
		text.erase(0, text.size() - 64 * 1024); // the end matters
	}
	if (text.empty()) {
		text = read_text(kShellUiLog, 64 * 1024);
	}
	if (text.empty()) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_log_lock);
	const char* head = "--- ShellUI log ---\n";
	(void)write(g_log, head, strlen(head));
	(void)write(g_log, text.data(), text.size());
}

// ---- stop port (tools/stop_bayosh.bat) ----------------------------------------------------

// ---- rest mode -------------------------------------------------------------------------------
// The system's power state (event flag "SceSystemStateMgrInfo", low 16 bits: 1000 working, 500 rest,
// 300 going to rest), read the way OnionHEN does. Waking up: wait 1 s for the network, then every
// listener reopens (net_epoch) - sockets from before rest are dead (user report: dead after waking).
std::atomic<int>    g_net_epoch{0};
std::atomic<bool>   g_sleeping{false}; // the PS5 is in (or going to) rest mode
std::atomic<time_t> g_rest_seen{0};    // last time rest mode started or ended

// Wake our own listeners: a blocked accept()/recv() returns and sees the new epoch (the window took
// ~2 min to reopen after waking: accept's timeout did not fire, test v1.1-rc).
void poke_ports() {
	for (const int port : {kWebPort, kStopPort}) {
		const int   s = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a {};
		a.sin_family      = AF_INET;
		a.sin_port        = htons(static_cast<uint16_t>(port));
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (s >= 0) {
			if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
				const char* poke = "poke\n";
				(void)write(s, poke, strlen(poke));
			}
			close(s);
		}
	}
	const int   u = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in a {};
	a.sin_family      = AF_INET;
	a.sin_port        = htons(kUiPort);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (u >= 0) {
		(void)sendto(u, "P", 1, 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
		close(u);
	}
}

void* power_thread(void*) {
	intptr_t flag     = -1;
	bool     sleeping = false;
	while (g_running) {
		usleep(200 * 1000);
		if (flag < 0 && sceKernelOpenEventFlag(&flag, "SceSystemStateMgrInfo") < 0) {
			flag = -1;
			sleep(5);
			continue;
		}
		uint64_t bits = 0;
		if (sceKernelPollEventFlag(flag, UINT64_MAX, 2 /* OR */, &bits) < 0) {
			sceKernelCloseEventFlag(flag);
			flag = -1;
			continue;
		}
		const unsigned state = static_cast<unsigned>(bits & 0xffff);
		if (state == 500 || state == 300) {
			if (!sleeping) {
				log("rest: going to rest mode (%u)", state);
				g_rest_seen = time(nullptr);
			}
			sleeping   = true;
			g_sleeping = true;
		} else if (state == 1000 && sleeping) {
			sleeping    = false;
			g_rest_seen = time(nullptr);
			log("rest: awake again, reopening ports in 1 s");
			sleep(1);
			++g_net_epoch;
			poke_ports();
			g_sleeping = false; // the re-inject loop may go on now
		}
	}
	return nullptr;
}

int open_stop_port() {
	const int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		return -1;
	}
	int on = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	sockaddr_in addr = {};
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons(kStopPort);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 1) != 0) {
		log("stop port %d unavailable (errno %d)", kStopPort, errno);
		close(s);
		return -1;
	}
	timeval tv {1, 0}; // accept wakes up every second: rest mode / reopen checks
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return s;
}

// Loading the ELF while bayo.sh already runs: ask the running one to leave (handover above), so
// this one gets the ports (they were "unavailable", and the status went to the old one, test v11-24).
// This build, so a handover can tell old from new ("handover <build>").
constexpr const char* kBuild = __DATE__ " " __TIME__;

bool take_over_running() {
	auto connect_stop = []() -> int {
		const int   s = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a {};
		a.sin_family      = AF_INET;
		a.sin_port        = htons(kStopPort);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (s >= 0 && connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
			return s;
		}
		if (s >= 0) {
			close(s);
		}
		return -1;
	};
	const int s = connect_stop();
	if (s < 0) {
		return false; // nothing running
	}
	const std::string hello = std::string("handover ") + kBuild + "\n";
	(void)write(s, hello.data(), hello.size());
	char          reply[16] = {};
	timeval       tv {2, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	const ssize_t got = recv(s, reply, sizeof(reply) - 1, 0);
	close(s);
	if (got < 8 || memcmp(reply, "handover", 8) != 0) {
		log("handover: the running bayo.sh is another build: it stops normally (ShellUI restarts)");
	}
	for (int i = 0; i < 150; ++i) { // until its ports are free (max 15 s: a normal stop restarts ShellUI)
		const int t = connect_stop();
		if (t < 0) {
			return true;
		}
		close(t);
		usleep(100 * 1000);
	}
	return true;
}

void* stop_thread(void*) {
	int s = -1, fails = 0, epoch = net_epoch(); // reopened after rest mode (see power_thread)
	while (g_running) {
		if (s >= 0 && epoch != net_epoch()) {
			log("stop port: reopening after rest mode");
			close(s);
			s = -1;
		}
		if (s < 0) {
			epoch = net_epoch();
			s     = open_stop_port();
			if (s < 0) {
				sleep(2);
				continue;
			}
			fails = 0;
		}
		const int c = accept(s, nullptr, nullptr);
		if (c < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue; // the 1 s timeout
			}
			if (++fails >= 30) {
				log("stop port broken (errno %d), reopening", errno);
				close(s);
				s = -1;
			}
			usleep(100 * 1000);
			continue;
		}
		fails = 0;
		// A new bayo.sh sends "handover": leave quietly (no ShellUI restart, mounts stay) so it can
		// take the ports. The stop tools send nothing -> after 0.3 s it is a normal stop.
		timeval rt {0, 300 * 1000};
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
		char          hello[48] = {};
		const ssize_t got       = recv(c, hello, sizeof(hello) - 1, 0);
		// Only the same build leaves quietly: ShellUI keeps the module the old one injected, so a
		// different build stops normally (ShellUI restarts, the new one injects its own module).
		if (got >= 4 && memcmp(hello, "poke", 4) == 0) {
			close(c); // our own wake-up nudge (poke_ports)
			continue;
		}
		if (got >= 8 && memcmp(hello, "handover", 8) == 0 && strstr(hello, kBuild) != nullptr) {
			const char* ok = "handover\n";
			(void)write(c, ok, strlen(ok));
			close(c);
			log("handover: a new bayo.sh takes over, leaving");
			_exit(0);
		}
		if (c >= 0) {
			const char* msg = "stopping\n";
			(void)write(c, msg, strlen(msg));
			close(c);
			g_stop_requested = true;
			g_running        = false;
			log("stop command received");
		}
	}
	close(s);
	return nullptr;
}

// After the first start: wait for the video to show, then add the tile (never in the video's way).
void* after_start_thread(void*) {
	bool want_video;
	{
		std::lock_guard<std::mutex> lock(g_ctl);
		want_video = g_enabled && !g_current.empty();
	}
	if (want_video) {
		for (int i = 0; i < 400 && shellui_status().rfind("playing", 0) != 0; ++i) {
			usleep(100 * 1000);
		}
		log("after start: ShellUI status '%s'", shellui_status().c_str());
		sleep(2);
	}
	const int rc = install_tile();
	if (rc == 1) {
		notify("bayo.sh se agregó a tu pantalla de inicio.");
	} else if (rc < 0) {
		notify_plain("bayo.sh: no se pudo agregar el ícono al inicio (ver bayosh.log).");
	}
	return nullptr;
}

void start_thread(void* (*fn)(void*)) {
	pthread_t t;
	if (pthread_create(&t, nullptr, fn, nullptr) == 0) {
		pthread_detach(t);
	}
}

} // namespace

// ---- shared API ------------------------------------------------------------------------------

void log(const char* fmt, ...) {
	if (g_log < 0) {
		return;
	}
	char line[600];
	const time_t now = time(nullptr);
	tm           t {};
	localtime_r(&now, &t);
	const int head = static_cast<int>(strftime(line, sizeof(line), "%H:%M:%S ", &t));
	va_list   args;
	va_start(args, fmt);
	int n = vsnprintf(line + head, sizeof(line) - 2 - head, fmt, args);
	va_end(args);
	if (n < 0) {
		return;
	}
	n = std::min(n + head, static_cast<int>(sizeof(line)) - 2);
	line[n++] = '\n';
	std::lock_guard<std::mutex> lock(g_log_lock);
	struct stat st {};
	if (fstat(g_log, &st) == 0 && st.st_size > kLogMaxSize) {
		(void)ftruncate(g_log, 0);
		(void)lseek(g_log, 0, SEEK_SET);
	}
	(void)write(g_log, line, static_cast<size_t>(n));
}

void notify(const char* text) {
	send_notification(text, true);
}

void notify_plain(const char* text) {
	send_notification(text, false);
}

std::vector<Video> list_videos() {
	std::vector<Video> all;
	auto scan = [&](const std::string& folder, const std::string& where) {
		DIR* dir = opendir(folder.c_str());
		if (dir == nullptr) {
			return;
		}
		std::vector<Video> found;
		while (dirent* e = readdir(dir)) {
			if (is_mp4(e->d_name)) {
				found.push_back({folder + "/" + e->d_name, e->d_name, where});
			}
		}
		closedir(dir);
		std::sort(found.begin(), found.end(), [](const Video& a, const Video& b) {
			return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
		});
		all.insert(all.end(), found.begin(), found.end());
	};
	// Game folders (named after a title id) inside a video folder: their clips are listed too.
	auto scan_games = [&](const std::string& folder, const std::string& where) {
		if (DIR* dir = opendir(folder.c_str())) {
			std::vector<std::string> subs;
			while (dirent* e = readdir(dir)) {
				if (is_title_id(e->d_name)) {
					subs.push_back(folder + "/" + e->d_name);
				}
			}
			closedir(dir);
			std::sort(subs.begin(), subs.end());
			for (const std::string& s : subs) {
				scan(s, where);
			}
		}
	};
	scan("/data/bayosh/video", "Internal");
	scan_games("/data/bayosh/video", "Internal");
	// USB: a folder called bayosh in any capitalisation (the drive's file system is case
	// sensitive here), clips directly inside it and/or in a "video" subfolder.
	auto subdir = [](const std::string& parent, const char* want) {
		std::string found;
		if (DIR* dir = opendir(parent.c_str())) {
			while (dirent* e = readdir(dir)) {
				if (strcasecmp(e->d_name, want) == 0) {
					found = parent + "/" + e->d_name;
					break;
				}
			}
			closedir(dir);
		}
		return found;
	};
	for (int i = 0; i < 8; ++i) {
		const std::string where = "USB " + std::to_string(i + 1);
		const std::string root  = subdir("/mnt/usb" + std::to_string(i), "bayosh");
		if (root.empty()) {
			continue;
		}
		scan(root, where);
		scan_games(root, where);
		const std::string video = subdir(root, "video");
		if (!video.empty()) {
			scan(video, where);
			scan_games(video, where);
		}
	}
	return all;
}

// The console's LAN address: a UDP "connect" picks the outgoing interface without sending anything.
std::string lan_ip() {
	const int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		return {};
	}
	sockaddr_in to {};
	to.sin_family      = AF_INET;
	to.sin_port        = htons(53);
	to.sin_addr.s_addr = htonl(0x08080808);
	sockaddr_in me {};
	socklen_t   len = sizeof(me);
	char        ip[INET_ADDRSTRLEN] = {};
	if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 &&
	    getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
		inet_ntop(AF_INET, &me.sin_addr, ip, sizeof(ip));
	}
	close(s);
	return ip;
}

State get_state() {
	State st;
	st.ip = lan_ip();
	{
		std::lock_guard<std::mutex> lock(g_ctl);
		st.enabled = g_enabled;
		st.current = g_current;
		st.dim     = g_dim;
		st.volume  = g_volume;
		st.blur    = g_blur;
		for (int l = 0; l < 4; ++l) {
			st.looks[l] = g_looks[l];
		}
		st.shuffle    = g_shuffle;
		st.main_video = g_main_video;
		st.game_videos = g_game_videos;
		st.game_pause = g_game_pause;
		st.menu_hide  = g_menu_hide;
		st.store_only = g_store_only;
		st.parallax   = g_parallax;
		st.light      = g_light;
		st.light_now  = g_light_now;
		st.light_mode = g_light_mode;
		st.light_fix  = g_light_fix;
		st.light_speed = g_light_speed;
		st.game       = g_game_running ? g_game_title : std::string();
	}
	st.status = shellui_status();
	st.videos = list_videos();
	st.guided = guide_done();
	return st;
}

bool select_video(const std::string& path, std::string& error) {
	if (!exists(path.c_str())) {
		error = "That video is gone";
		return false;
	}
	std::lock_guard<std::mutex> lock(g_ctl);
	g_current = path;
	g_enabled = true;
	save_config(); // g_override stays: a highlighted game's own clip wins over the main video
	return apply_locked(error);
}

int net_epoch() {
	return g_net_epoch.load();
}

void set_dim(int percent) {
	std::lock_guard<std::mutex> lock(g_ctl);
	g_dim = std::max(0, std::min(80, percent));
	save_config();
	write_text(kDimFile, std::to_string(g_dim) + "\n");
	log("dim: %d%%", g_dim);
}

void set_volume(int percent) {
	std::lock_guard<std::mutex> lock(g_ctl);
	g_volume = std::max(0, std::min(100, percent));
	save_config();
	write_text(kVolumeFile, std::to_string(g_volume) + "\n");
	log("volume: %d%%", g_volume);
}

void set_look(const std::string& key, int level) {
	static const char* const kKeys[4] = {"bottom", "top", "vignette", "haze"};
	std::lock_guard<std::mutex> lock(g_ctl);
	for (int l = 0; l < 4; ++l) {
		if (key == kKeys[l]) {
			g_looks[l] = std::max(0, std::min(2, level));
			save_config();
			write_looks();
			log("look %s: %d", kKeys[l], g_looks[l]);
		}
	}
}

void set_blur(int level) {
	std::lock_guard<std::mutex> lock(g_ctl);
	g_blur = std::max(0, std::min(2, level));
	save_config();
	write_text(kBlurFile, std::to_string(g_blur) + "\n");
	log("blur: %d", g_blur);
}

bool set_enabled(bool on, std::string& error) {
	std::lock_guard<std::mutex> lock(g_ctl);
	g_enabled = on;
	if (on && (g_current.empty() || !exists(g_current.c_str()))) {
		const std::vector<Video> videos = list_videos();
		g_current = videos.empty() ? std::string() : videos.front().path;
	}
	save_config();
	return apply_locked(error);
}

bool set_option(const std::string& key, bool on, std::string& error) {
	std::lock_guard<std::mutex> lock(g_ctl);
	if (key == "shuffle") {
		g_shuffle = on;
	} else if (key == "gamevideos") {
		g_game_videos = on;
		save_config();
		log("option gamevideos=%d", on ? 1 : 0);
		g_focus_recheck = true; // the highlighted tile decides again
		return !g_injected || apply_locked(error);
	} else if (key == "gamepause") {
		g_game_pause = on;
	} else if (key == "light") {
		g_light = on; // focus_thread (light_step) picks it up within 0.2 s
	} else if (key == "parallax") {
		g_parallax = on;
		write_text(kParallaxFile, on ? "1\n" : "0\n");
	} else if (key == "menuhide") {
		g_menu_hide = on;
		write_text(kMenusFile, on ? "1\n" : "0\n");
	} else if (key == "storeonly") {
		// PS Store only: ShellUI hides (fades + mutes) the video on every other tile; per-game videos
		// are not used. The player keeps running, so coming back to the Store is instant.
		g_store_only = on;
		write_text(kStoreOnlyFile, on ? "1\n" : "0\n");
		g_focus_recheck = true;
		save_config();
		log("option storeonly=%d", on ? 1 : 0);
		return !g_injected || apply_locked(error); // the Store's clip (or the normal one) now
	} else {
		error = "Unknown setting";
		return false;
	}
	save_config();
	log("option %s=%d", key.c_str(), on ? 1 : 0);
	// Pausing for a game that is running right now takes effect at once.
	return key != "gamepause" || !g_game_running || !g_injected || apply_locked(error);
}

bool delete_video(const std::string& path, std::string& error) {
	std::lock_guard<std::mutex> lock(g_ctl);
	if (path == g_current) {
		// Release it on the home screen first: switch to another clip (or off).
		std::string next;
		for (const Video& v : list_videos()) {
			if (v.path != path) {
				next = v.path;
				break;
			}
		}
		g_current = next;
		save_config();
		if (!apply_locked(error)) {
			return false;
		}
	}
	const std::string cover = path.substr(0, path.rfind('.')) + ".png";
	const bool        ok    = unlink(path.c_str()) == 0;
	unlink(cover.c_str());
	unlink((path.substr(0, path.rfind('.')) + ".color").c_str());
	log("delete %s: %s", path.c_str(), ok ? "done" : strerror(errno));
	if (ok) {
		// Games that used it go back to the main video (the Library showed the deleted name).
		const std::string text = read_text("/data/bayosh/games.txt", 64 * 1024);
		std::string       out;
		int               freed = 0;
		size_t            pos   = 0;
		while (pos < text.size()) {
			const size_t      end  = text.find('\n', pos);
			const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
			const size_t      eq   = line.find('=');
			if (eq != std::string::npos && line.substr(eq + 1) == path) {
				++freed;
			} else if (!line.empty()) {
				out += line + "\n";
			}
			if (end == std::string::npos) {
				break;
			}
			pos = end + 1;
		}
		if (freed > 0) {
			write_text("/data/bayosh/games.txt", out);
			write_text("/system_tmp/bayosh/games.txt", out);
			if (g_override == path) {
				g_override.clear();
			}
			g_focus_recheck = true;
			write_fast_map();
			log("delete: %d game(s) used it, back to the main video", freed);
		}
	}
	if (!ok) {
		error = "The file could not be deleted";
	}
	return ok;
}

bool reset_settings(std::string& error) {
	std::lock_guard<std::mutex> lock(g_ctl);
	g_enabled    = true;
	g_dim        = 25;
	g_volume     = 100;
	write_text(kVolumeFile, "100\n");
	g_blur       = 0;
	for (int& v : g_looks) {
		v = 0;
	}
	write_looks();
	g_shuffle    = false;
	g_main_video = true;
	g_game_videos = true;
	g_game_pause = true;
	g_menu_hide  = true;
	g_store_only = false;
	write_text(kStoreOnlyFile, "0\n");
	g_parallax   = false;
	write_text(kParallaxFile, "0\n");
	g_light      = false;
	g_light_mode = "follow";
	g_light_fix  = "22b8ff";
	g_light_speed = 1;
	write_text(kMenusFile, "1\n");
	if (g_current.empty() || !exists(g_current.c_str())) {
		const std::vector<Video> videos = list_videos();
		g_current = videos.empty() ? std::string() : videos.front().path;
	}
	save_config();
	write_text(kDimFile, std::to_string(g_dim) + "\n");
	write_text(kBlurFile, "0\n");
	log("settings reset");
	return apply_locked(error);
}

// v0.3: the app's own window (the home-screen tile BYSH00001) closes with the engine.
// sceLncUtilGetAppId("BYSH00001") does not find a web-app tile (test v0.3-beta4: window stayed), so
// look for the process that runs it: the one whose app info carries our title id.
extern "C" int sceLncUtilGetAppId(const char* title_id);
extern "C" int sceLncUtilKillApp(int app_id);
struct BayoAppInfo {
	uint32_t app_id;
	uint64_t unknown1;
	uint32_t app_type;
	char     title_id[10];
	char     unknown2[0x3c];
};
extern "C" int sceKernelGetAppInfo(pid_t pid, BayoAppInfo* info);

void close_app_window() {
	int closed = 0;
	const int byid = sceLncUtilGetAppId("BYSH00001");
	if (byid > 0) {
		log("app window: appId %#x by title, kill rc %#x", static_cast<unsigned>(byid),
		    static_cast<unsigned>(sceLncUtilKillApp(byid)));
		++closed;
	}
	int    mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
	size_t size   = 0;
	if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0 || size == 0) {
		log("app window: process list unavailable (errno %d)", errno);
		return;
	}
	std::string buf(size + 4096, '\0');
	size = buf.size();
	if (sysctl(mib, 4, &buf[0], &size, nullptr, 0) != 0) {
		log("app window: process list unavailable (errno %d)", errno);
		return;
	}
	std::string seen;
	for (size_t off = 0; off + sizeof(int) <= size;) {
		const auto* ki = reinterpret_cast<const kinfo_proc*>(buf.data() + off);
		if (ki->ki_structsize <= 0) {
			break;
		}
		off += static_cast<size_t>(ki->ki_structsize);
		BayoAppInfo info {};
		if (sceKernelGetAppInfo(ki->ki_pid, &info) != 0) {
			continue;
		}
		char tid[11] = {};
		memcpy(tid, info.title_id, 10);
		if (tid[0] != 0 && seen.size() < 900) {
			seen += std::string(ki->ki_comm) + "(" + tid + ") ";
		}
		if (strncmp(tid, "BYSH00001", 9) == 0 && ki->ki_pid != getpid()) {
			const int rc = info.app_id != 0 ? sceLncUtilKillApp(static_cast<int>(info.app_id)) : -1;
			log("app window: %s pid %d appId %#x -> kill rc %#x", ki->ki_comm, static_cast<int>(ki->ki_pid), info.app_id,
			    static_cast<unsigned>(rc));
			if (rc != 0) {
				kill(ki->ki_pid, SIGKILL); // our own window's process only
				log("app window: pid %d sent SIGKILL", static_cast<int>(ki->ki_pid));
			}
			++closed;
		}
	}
	if (closed == 0) {
		log("app window: not found. Apps running: %s", seen.c_str());
	}
}

bool menu_mods_present() {
	return onion_daemon_answers();
}

volatile bool g_full_close = false; // v0.3: "Cerrar y reiniciar el menú" — skip the soft stop

void request_shutdown(bool full) {
	log("shutdown requested from the window (%s)", full ? "full: restart the menu" : "quick");
	g_full_close     = full;
	g_stop_requested = true;
	if (!full) {
		go_home(); // leave the window right away; the engine winds down behind the home screen
	}
	g_running        = false;
}

// Per-game backgrounds: /data/bayosh/games.txt, one "TITLEID=/path/clip.mp4" per line.
constexpr const char* kGamesFile = "/data/bayosh/games.txt";

std::string games_text() {
	return read_text(kGamesFile, 64 * 1024);
}

void set_game_background(const std::string& tid, const std::string& path) {
	std::lock_guard<std::mutex> lock(g_ctl);
	std::string out;
	const std::string text = read_text(kGamesFile, 64 * 1024);
	size_t      pos  = 0;
	while (pos < text.size()) {
		const size_t      end  = text.find('\n', pos);
		const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		if (!line.empty() && line.rfind(tid + "=", 0) != 0) {
			out += line + "\n";
		}
		if (end == std::string::npos) {
			break;
		}
		pos = end + 1;
	}
	if (!path.empty()) {
		out += tid + "=" + path + "\n";
	}
	write_text(kGamesFile, out);
	write_text("/system_tmp/bayosh/games.txt", out);
	write_fast_map();
	g_focus_recheck = true;
	log("game background %s -> %s", tid.c_str(), path.empty() ? "main wallpaper" : path == "off" ? "its own background (no video)" : path.c_str());
	if (g_store_only && tid == "NPXS40047" && g_injected) {
		std::string error;
		apply_locked(error);
	}
}

// ShellUI's fast switch (test v11-8): it picks the highlighted game's clip itself from this map
// instead of waiting for our round trip. "*=<main clip>", then "TITLEID=<clip>", as ShellUI sees them;
// "-" = needs a remount first (we do that one). Caller holds g_ctl.
// Does the clip have a sound track? An mp4 'hdlr' box of type 'soun' inside 'moov'. Cached by
// size + date; unreadable = assume sound (the old behaviour). Caller holds g_ctl.
bool clip_has_audio(const std::string& path) {
	static std::map<std::string, std::pair<long long, bool>> cache;
	struct stat st {};
	if (stat(path.c_str(), &st) != 0) {
		return true;
	}
	const long long stamp = static_cast<long long>(st.st_size) * 31 + static_cast<long long>(st.st_mtime);
	auto            hit   = cache.find(path);
	if (hit != cache.end() && hit->second.first == stamp) {
		return hit->second.second;
	}
	bool      audio = true;
	const int fd    = open(path.c_str(), O_RDONLY);
	if (fd >= 0) {
		long long off = 0;
		unsigned char h[16];
		while (off + 8 <= static_cast<long long>(st.st_size) && pread(fd, h, 8, off) == 8) {
			long long size = (static_cast<long long>(h[0]) << 24) | (h[1] << 16) | (h[2] << 8) | h[3];
			long long head = 8;
			if (size == 1) {
				if (pread(fd, h + 8, 8, off + 8) != 8) {
					break;
				}
				size = 0;
				for (int i = 8; i < 16; ++i) {
					size = (size << 8) | h[i];
				}
				head = 16;
			} else if (size == 0) {
				size = static_cast<long long>(st.st_size) - off;
			}
			if (size < head) {
				break;
			}
			if (memcmp(h + 4, "moov", 4) == 0) {
				const size_t n = static_cast<size_t>(std::min<long long>(size, 8 * 1024 * 1024));
				std::string  moov(n, '\0');
				if (pread(fd, &moov[0], n, off) == static_cast<ssize_t>(n)) {
					audio = false;
					for (size_t at = moov.find("hdlr"); at != std::string::npos; at = moov.find("hdlr", at + 4)) {
						if (at + 16 <= n && moov.compare(at + 12, 4, "soun") == 0) {
							audio = true;
							break;
						}
					}
				}
				break;
			}
			off += size;
		}
		close(fd);
	}
	cache[path] = {stamp, audio};
	return audio;
}

void write_fast_map() {
	auto shown = [](const std::string& clip) -> std::string {
		if (clip == "off") {
			return "off"; // the game's own background and music
		}
		if (clip.empty() || g_mounted_folder.empty() || mount_base(clip) != g_mounted_folder || !exists(clip.c_str())) {
			return "-";
		}
		return shown_path(clip);
	};
	std::string       out  = "*=" + (g_store_only ? shown(store_clip()) : g_main_video ? shown(g_current) : std::string("off")) + "\n";
	const std::string text = g_game_videos && !g_store_only ? read_text(kGamesFile, 64 * 1024) : std::string();
	size_t            pos  = 0;
	while (pos < text.size()) {
		const size_t      end  = text.find('\n', pos);
		const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		const size_t      eq   = line.find('=');
		if (eq != std::string::npos && eq > 0) {
			out += line.substr(0, eq) + "=" + shown(line.substr(eq + 1)) + "\n";
		}
		if (end == std::string::npos) {
			break;
		}
		pos = end + 1;
	}
	// "!<clip>" = no sound track: ShellUI lets the PS5's own music (a game's theme) play with it.
	for (const Video& v : list_videos()) {
		if (!g_mounted_folder.empty() && mount_base(v.path) == g_mounted_folder && !clip_has_audio(v.path)) {
			out += "!" + shown_path(v.path) + "\n";
		}
	}
	write_text("/system_tmp/bayosh/fast.txt", out);
}

// ShellUI cannot read /data: it gets a copy of games.txt to label the menu item (Set / Remove).
void mirror_games() {
	write_text("/system_tmp/bayosh/games.txt", read_text(kGamesFile, 64 * 1024));
}


// ---- installed games (Games tab) ----------------------------------------------------------------

// PS4 titles keep their name in param.sfo (binary); PS5 titles in param.json.
std::string sfo_title(const std::string& path) {
	const std::string d = read_text(path.c_str(), 64 * 1024);
	if (d.size() < 20 || d.compare(0, 4, std::string("\0PSF", 4)) != 0) {
		return {};
	}
	auto u16 = [&](size_t o) { return static_cast<uint32_t>(static_cast<uint8_t>(d[o]) | static_cast<uint8_t>(d[o + 1]) << 8); };
	auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
	const uint32_t keys = u32(8), data = u32(12), count = u32(16);
	for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= d.size(); ++i) {
		const size_t   e   = 20 + i * 16;
		const uint32_t ko  = keys + u16(e), len = u32(e + 4), dof = data + u32(e + 12);
		if (ko < d.size() && strcmp(d.c_str() + ko, "TITLE") == 0 && dof + len <= d.size()) {
			return std::string(d.c_str() + dof);
		}
	}
	return {};
}

std::string json_title(const std::string& path) {
	const std::string d = read_text(path.c_str(), 256 * 1024);
	// Prefer en-US, else the first titleName.
	size_t at = d.find("\"en-US\"");
	at        = d.find("\"titleName\"", at == std::string::npos ? 0 : at);
	if (at == std::string::npos) {
		at = d.find("\"titleName\"");
	}
	if (at == std::string::npos) {
		return {};
	}
	const size_t q1 = d.find('"', d.find(':', at) + 1);
	const size_t q2 = q1 == std::string::npos ? q1 : d.find('"', q1 + 1);
	return q2 == std::string::npos ? std::string() : d.substr(q1 + 1, q2 - q1 - 1);
}

bool is_title_id(const char* n) {
	if (strlen(n) != 9) {
		return false;
	}
	for (int i = 0; i < 4; ++i) {
		if (!isupper(static_cast<unsigned char>(n[i]))) {
			return false;
		}
	}
	for (int i = 4; i < 9; ++i) {
		if (!isdigit(static_cast<unsigned char>(n[i]))) {
			return false;
		}
	}
	return strncmp(n, "NPXS", 4) != 0 && strcmp(n, "BYSH00001") != 0;
}

std::vector<Game> list_games() {
	std::vector<Game> out;
	DIR* dir = opendir("/user/appmeta");
	if (dir == nullptr) {
		return out;
	}
	while (dirent* e = readdir(dir)) {
		if (!is_title_id(e->d_name)) {
			continue;
		}
		const std::string base = std::string("/user/appmeta/") + e->d_name;
		std::string       name = json_title(base + "/param.json");
		if (name.empty()) {
			name = sfo_title(base + "/param.sfo");
		}
		if (name.empty()) {
			continue; // no title: not a game we can show (e.g. tools, leftovers)
		}
		out.push_back({e->d_name, name});
	}
	closedir(dir);
	std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
	return out;
}

// ---- per-game playback: the highlighted game's clip -----------------------------------------
// ShellUI writes the highlighted title id to focus_game.txt ("" for non-game tiles). After ~1 s on
// the same tile, play that game's clip (games.txt) instead of the main one; elsewhere, the main one.
// What PLAYS for a tile: its saved clip, unless "Videos per game" is off (then the main one).
std::string game_clip(const std::string& tid) {
	return g_game_videos && !g_store_only ? saved_game_clip(tid) : std::string();
}

// The clip saved for a tile in games.txt (the Library list shows it even when switched off).
std::string saved_game_clip(const std::string& tid) {
	if (tid.empty()) {
		return {};
	}
	const std::string text = read_text("/data/bayosh/games.txt", 64 * 1024);
	const size_t      at   = text.find(tid + "=");
	if (at == std::string::npos || (at > 0 && text[at - 1] != '\n')) {
		return {};
	}
	const size_t end = text.find('\n', at);
	return text.substr(at + tid.size() + 1, end == std::string::npos ? std::string::npos : end - at - tid.size() - 1);
}

void* focus_thread(void*) {
	std::string seen, pending;
	long        changed_ms = 0;
	auto        now_ms     = [] {
        timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
	};
	log("light bar: watching (option %s)", g_light ? "on" : "off");
	while (g_running) {
		usleep(200 * 1000);
		light_step();
		const std::string tid = shellui_focus();
		if (tid != seen) {
			seen       = tid;
			changed_ms = now_ms();
			continue;
		}
		if (g_focus_recheck) {
			g_focus_recheck = false;
			pending         = "?"; // games.txt changed: decide again for the same tile
		}
		if (now_ms() - changed_ms < 250 || tid == pending) {
			continue;
		}
		std::string clip = game_clip(tid);
		// "off" = this game keeps its own PS5 background and music (no video on its tile).
		if (!clip.empty() && clip != "off" && !exists(clip.c_str())) {
			clip.clear();
		}
		std::lock_guard<std::mutex> lock(g_ctl);
		if (!g_injected || !g_enabled || g_game_running) {
			continue; // not now: decide again once we can switch (pending unchanged)
		}
		if (tid != pending) {
			write_text("/data/bayosh/last_game.txt", tid + "\n"); // the PS5 comes back to this tile
		}
		pending = tid;
		if (clip == g_override) {
			continue;
		}
		g_override = clip;
		log("per-game: %s -> %s", tid.empty() ? "(no game)" : tid.c_str(), clip.empty() ? "main wallpaper" : clip.c_str());
		std::string error;
		if (!apply_locked(error)) {
			log("per-game: %s", error.c_str());
		}
	}
	return nullptr;
}

// A game (not a system app, not our own window) is running: no video, the game gets the decoder.
void* game_thread(void*) {
	bool last = false;
	while (g_running) {
		sleep(2);
		const int   app = sceSystemServiceGetAppIdOfRunningBigApp();
		char        id[32] {};
		const bool  now = app >= 0 && sceSystemServiceGetAppTitleId(app, id) == 0 && id[0] != 0 &&
		                 strncmp(id, "NPXS", 4) != 0 && strcmp(id, "BYSH00001") != 0;
		if (now == last) {
			continue;
		}
		last = now;
		std::lock_guard<std::mutex> lock(g_ctl);
		g_game_running = now;
		g_game_title   = now ? id : "";
		log("game %s (%s)", now ? "started" : "closed", now ? id : "-");
		if (g_game_pause && g_injected) {
			std::string error;
			if (!apply_locked(error)) {
				log("game pause: %s", error.c_str());
			}
		}
	}
	return nullptr;
}


// ---- light bar colour (experimental) ----------------------------------------------------------
// The page measures each clip's main colour and stores it as <clip>.color ("RRGGBB"). This thread
// tells the ShellUI module which colour the controller should show: the playing clip's (main
// wallpaper or the highlighted game's own), or "off" while a game runs, the wallpaper is off, or
// the option is off. The module does the pad call from inside ShellUI, which owns the controller.

bool is_rgb(const std::string& c) {
	if (c.size() != 6) {
		return false;
	}
	for (char ch : c) {
		if (!isxdigit(static_cast<unsigned char>(ch))) {
			return false;
		}
	}
	return true;
}

std::string clip_color(const std::string& path) {
	const size_t dot = path.rfind('.');
	if (dot == std::string::npos) {
		return {};
	}
	const std::string c = first_line(read_text((path.substr(0, dot) + ".color").c_str(), 32));
	return is_rgb(c) ? c : std::string();
}

bool set_clip_color(const std::string& path, const std::string& rgb) {
	const size_t dot = path.rfind('.');
	if (dot == std::string::npos || !is_rgb(rgb)) {
		return false;
	}
	const bool ok = write_text((path.substr(0, dot) + ".color").c_str(), rgb + "\n");
	log("colour %s: %s %s", path.c_str(), rgb.c_str(), ok ? "saved" : "NOT saved");
	return ok;
}

bool guide_done() {
	return exists(kGuideFile);
}

void set_guide_done(bool done) {
	if (done) {
		write_text(kGuideFile, "1\n");
	} else {
		unlink(kGuideFile);
	}
}

// Called every 0.2 s from focus_thread (a loop that is known to run): no thread of its own.
bool set_light_mode(const std::string& mode, const std::string& rgb, int speed) {
	static const char* const kModes[] = {"follow", "fixed", "rainbow", "breathe", "wave"};
	bool known = false;
	for (const char* m : kModes) {
		known = known || mode == m;
	}
	if (!known) {
		return false;
	}
	std::lock_guard<std::mutex> lock(g_ctl);
	g_light_mode  = mode;
	if (is_rgb(rgb)) {
		g_light_fix = rgb;
	}
	g_light_speed = std::max(0, std::min(2, speed));
	save_config();
	log("light bar: mode %s, colour %s, speed %d", g_light_mode.c_str(), g_light_fix.c_str(), g_light_speed);
	return true;
}

// Called every 0.2 s from focus_thread (a loop that is known to run): no thread of its own.
// light.txt for ShellUI: "off", or "<solid|breathe|rainbow|wave> <speed 0-2> [RRGGBB ...]".
// ShellUI animates it itself (bayosh_shellui.cpp, light_thread).
void light_step() {
	static std::string last;
	static int         fails = 0;
	static bool        said  = false;
	std::string want = "off";
	{
		std::lock_guard<std::mutex> lock(g_ctl);
		if (g_light && g_enabled && g_injected && !g_game_running) {
			const std::string clip = g_store_only ? store_clip() : (!g_override.empty() ? g_override : g_current);
			const std::string c    = clip.empty() || clip == "off" ? std::string() : clip_color(clip);
			const std::string sp   = std::to_string(g_light_speed);
			if (g_light_mode == "fixed") {
				want = "solid " + sp + " " + g_light_fix;
			} else if (g_light_mode == "rainbow") {
				want = "rainbow " + sp;
			} else if (g_light_mode == "breathe") {
				want = "breathe " + sp + " " + g_light_fix; // the colour picked in the app
			} else if (g_light_mode == "wave") {
				std::string list;
				int         n = 0;
				for (const Video& v : list_videos()) {
					const std::string vc = clip_color(v.path);
					if (!vc.empty() && list.find(vc) == std::string::npos && n < 6) {
						list += " " + vc;
						++n;
					}
				}
				for (const char* d : {"ff3d9a", "22b8ff", "7cff00"}) {
					if (n < 3 && list.find(d) == std::string::npos) {
						list += std::string(" ") + d;
						++n;
					}
				}
				want = "wave " + sp + list;
			} else if (!c.empty()) {
				want = "solid " + sp + " " + c;
			} else if (!said) {
				said = true;
				log("light bar: no colour for '%s' yet (open bayo.sh on a phone or pick one)", clip.c_str());
			}
		} else if (g_light && !said) {
			said = true;
			log("light bar: on but waiting (enabled %d, injected %d, game running %d)", g_enabled ? 1 : 0,
			    g_injected ? 1 : 0, g_game_running ? 1 : 0);
		}
		if (!g_light) {
			said = false;
		}
		g_light_now = want.rfind("solid ", 0) == 0 ? want.substr(want.size() - 6) : want == "off" ? "off" : g_light_mode;
	}
	if (want == last) {
		return;
	}
	if (write_text(kLightFile, want + "\n")) {
		log("light bar: %s", want.c_str());
		last  = want;
		fails = 0;
	} else if (fails++ < 3) {
		log("light bar: cannot write %s (errno %d)", kLightFile, errno);
	}
}

} // namespace bayosh

using namespace bayosh;

int main() {
	mkdir("/data/bayosh", 0777);
	mkdir("/data/bayosh/video", 0777);
	mkdir("/system_tmp/bayosh", 0777);
	// Loading the payload = try again: a crash marker from an earlier run must not keep the video
	// off until the next reboot.
	unlink("/system_tmp/bayosh/video_trying");
	g_log = open(kLogFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	log("bayo.sh v0.3.0 starting");
	if (take_over_running()) {
		log("handover: took over from the bayo.sh that was running");
	}
	// The injector's own steps (attach / load / stager / detach) go here.
	unlink("/data/bayosh/inject.log");
	onion_log_configure("bayosh", "/data/bayosh/inject.log");
	onion_log_set_level(ONION_LOG_DEBUG);

	if (kOnionGuard && onion_daemon_answers()) {
		notify("bayo.sh no puede correr mientras OnionHEN está activo.");
		return 1;
	}

	std::vector<Video> videos = list_videos();
	log("%zu video(s) found", videos.size());
	{
		std::lock_guard<std::mutex> lock(g_ctl);
		load_config();
		const bool chosen_ok = std::any_of(videos.begin(), videos.end(), [](const Video& v) { return v.path == g_current; });
		if (!chosen_ok) {
			g_current = videos.empty() ? std::string() : videos.front().path;
		}
		if (g_shuffle && videos.size() > 1) {
			// Random clip on every start, never the same one twice in a row.
			srand(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid()));
			std::vector<std::string> others;
			for (const Video& v : videos) {
				if (v.path != g_current) {
					others.push_back(v.path);
				}
			}
			g_current = others[static_cast<size_t>(rand()) % others.size()];
			save_config();
			log("shuffle: %s", g_current.c_str());
		}
		// Start with the clip of the game the home screen will highlight: the PS5 comes back to the
		// last focused tile, which we noted (focus info only arrives once the user moves).
		const std::string last = first_line(read_text("/data/bayosh/last_game.txt", 64));
		const std::string lclip = game_clip(last);
		if (!lclip.empty() && exists(lclip.c_str())) {
			g_override = lclip;
			write_text("/system_tmp/bayosh/focus_game.txt", last + "\n");
			ui_set_focus(last);
			log("start: last highlighted game %s -> %s", last.c_str(), lclip.c_str());
		}
		std::string error;
		apply_locked(error); // not injected yet: just mounts + writes the path file
	}
	if (videos.empty()) {
		notify_plain("No hay videos. Pon un .mp4 en /data/bayosh/video o en una carpeta bayosh de tu USB.");
	}

	start_thread(ui_thread); // before the injection: ShellUI talks to us from its first line
	start_thread(power_thread);
	start_thread(stop_thread);
	start_web_server();

	pid_t last_pid = inject_shellui();
	if (last_pid < 0) {
		return 3;
	}
	{
		std::lock_guard<std::mutex> lock(g_ctl);
		g_injected = last_pid > 0;
	}
	start_thread(after_start_thread);
	mirror_games();
	start_thread(game_thread);
	start_thread(focus_thread);

	// Re-inject after ShellUI restarts. A restart soon after an injection means our module crashed
	// it: stop for good instead of crash-looping the console.
	time_t injected_at = time(nullptr);
	while (g_running) {
		sleep(5);
		if (!g_running) {
			break;
		}
		pid_t current = onion_find_pid("SceShellUI");
		if (current <= 0 || current == last_pid) {
			continue;
		}
		const long alive = static_cast<long>(time(nullptr) - injected_at);
		log("ShellUI restarted: was %d, now %d (%ld s after injection)", static_cast<int>(last_pid),
		    static_cast<int>(current), alive);
		copy_shellui_log();
		// Rest mode restarts ShellUI itself (it went down 5 s after "going to rest", and our crash
		// guard stopped bayo.sh for good, test v1.1-rc2). Not a crash: wait until the PS5 is awake,
		// then inject into the new ShellUI (OnionHEN re-injects after resume the same way).
		const bool by_rest = g_sleeping || time(nullptr) - g_rest_seen.load() < 60;
		if (by_rest) {
			log("ShellUI restarted by rest mode: injecting again once the PS5 is awake");
			while (g_running && g_sleeping) {
				sleep(1);
			}
			if (!g_running) {
				break;
			}
			sleep(10); // as OnionHEN after a resume (resume_reinject_delay_seconds=10)
			const pid_t now = onion_find_pid("SceShellUI");
			if (now > 0) {
				current = now;
			}
		}
		if (!by_rest && alive < kCrashWindowSec) {
			log("ShellUI died %ld s after injection: treating it as a crash, not re-injecting", alive);
			notify("bayo.sh se detuvo: el menú se cerró. Registro: /data/bayosh/bayosh.log");
			break;
		}
		sleep(3); // let the new ShellUI settle
		{
			std::lock_guard<std::mutex> lock(g_ctl);
			g_injected = false; // the new ShellUI has no module yet: no waiting for it to let go
			std::string error;
			apply_locked(error); // fresh sandbox: mount the window again
		}
		last_pid    = inject_shellui();
		injected_at = time(nullptr);
		if (last_pid <= 0) {
			break;
		}
		std::lock_guard<std::mutex> lock(g_ctl);
		g_injected = true;
	}

	copy_shellui_log();
	if (g_stop_requested) {
		// v0.3: the controller light goes back to the system's colour before we leave (the module's
		// light thread reads light.txt every ~0.4 s).
		write_text(kLightFile, "off\n");
		usleep(1500 * 1000);
		close_app_window(); // "Cerrar": the window goes too when it is an app of its own
	}
	// With OnionHEN running, restarting ShellUI would take OnionHEN's toolbox down too. Soft stop
	// instead: video off, looks off, folder unmounted; the idle module stays parked in ShellUI and
	// the next start reuses it (the marker stays, so no second injection).
	const pid_t ui    = onion_find_pid("SceShellUI");
	const bool  onion_d = onion_daemon_answers();
	const bool  onion_t = ui > 0 && read_pid_file(kOnionToolbox) == ui;
	const bool  onion   = onion_d || onion_t;
	if (g_stop_requested) {
		log("stop: onion daemon=%d toolbox=%d full=%d", onion_d ? 1 : 0, onion_t ? 1 : 0, g_full_close ? 1 : 0);
	}
	if (g_stop_requested && onion && g_full_close) {
		// beta8 test: killing the menu with OnionHEN inside froze the console. Never again.
		log("stop: full close refused, another menu mod is running");
	}
	if (g_stop_requested && onion) {
		write_text(kDimFile, "0\n");
		write_text(kBlurFile, "0\n");
		write_text(kLooksFile, "0 0 0 0\n");
		write_text(kParallaxFile, "0\n");
		write_text(kPathFile, "off\n");
		if (!wait_shellui_stopped(8000)) {
			log("soft stop: ShellUI still holds the video (%s)", shellui_status().c_str());
		}
		unmount_window_everywhere();
		go_home(); // the window belongs to the menu: at least leave it and show the home screen
		log("bayo.sh stopped (soft: OnionHEN keeps running, the idle module stays in ShellUI)");
		notify("bayo.sh se cerró.");
		return 0;
	}
	write_text(kPathFile, "off\n");
	bool fresh_ui = true;
	if (g_stop_requested) {
		fresh_ui = restart_shellui();
		sleep(2);
	}
	unmount_window_everywhere();
	if (fresh_ui) {
		unlink(kOurMarker);
	} else {
		// The module is still parked in this ShellUI: keep the marker so a new start does not inject twice.
		log("bayo.sh stopped, but the menu did not restart: restart the console before starting it again");
		notify("bayo.sh se apagó, pero el menú no se reinició. Reinicia la consola antes de volver a lanzarlo.");
	}
	unlink(kStatusFile);
	ui_forget_status();
	log("bayo.sh stopped");
	notify("bayo.sh se cerró.");
	return 0;
}

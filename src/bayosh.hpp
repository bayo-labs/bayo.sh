// Copyright (C) 2026 Josué Oswaldo Hernández López (bayo.sh). Licencia GPL-3.0, ver LICENSE.
// bayo.sh daemon: shared declarations (daemon, web window, home-screen tile). GPL-3.0.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace bayosh {

constexpr int         kWebPort  = 9050;
constexpr int         kSitePort = 9051; // wallpaper site pages (another origin)
constexpr int         kStopPort = 9049;
constexpr const char* kWebUrl   = "http://127.0.0.1:9050/";

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void notify(const char* text);       // system icon
void notify_plain(const char* text); // text only (debug style)

struct Video {
	std::string path;   // real path, e.g. /data/bayosh/video/x.mp4
	std::string name;   // file name
	std::string where;  // "Internal" or "USB 1"..
};

struct Game {
	std::string tid;   // e.g. PPSA01234
	std::string name;  // title from param.json / param.sfo
};

struct State {
	bool               enabled = true;
	int                dim     = 25; // % black over the video (home screen readability)
	int                volume  = 100; // % sound of clips that have it
	int                blur    = 0;  // 0 off, 1 light, 2 strong
	int                looks[4] = {0, 0, 0, 0}; // bottom fade, top fade, vignette, haze
	std::string        ip;                      // console LAN address (remote page on phone / PC)
	bool               shuffle    = false; // random clip on every start
	int                rotate     = 0;     // change every N minutes (0 = never)
	bool               rot_order  = false; // rotation in list order instead of random
	bool               main_video = true;  // off = only tiles with their own video
	bool               game_videos = true; // off = per-game videos ignored
	bool               game_pause = true;  // no video while a game runs
	bool               menu_hide  = true;  // hide the video in Settings / system apps
	bool               store_only = false; // video only on the PS Store tile
	bool               parallax   = false; // nudge the video when moving between home tiles
	bool               light      = true;  // controller light bar follows the video's colour
	bool               guided     = false; // the first-run guide was shown
	std::string        light_now;          // colour sent to the light bar right now (RRGGBB or "off")
	std::string        light_mode = "follow"; // follow, fixed, rainbow, breathe, wave
	std::string        light_fix  = "22b8ff"; // colour for fixed / breathe
	int                light_speed = 1;        // 0 slow, 1 normal, 2 fast
	std::string        game;               // title id of the running game ("" = none)
	std::string        current;  // real path of the chosen clip ("" = none)
	std::string        status;   // ShellUI status line: playing / stopped / failed / starting
	std::vector<Video> videos;
};

std::vector<Video> list_videos();
State              get_state();
// Pick a clip / switch on or off. Safe to call from any thread; blocks until ShellUI let go of
// the old clip (a few seconds at most). Returns false with a reason on failure.
bool select_video(const std::string& path, std::string& error, bool save = true);
bool set_enabled(bool on, std::string& error);
void set_dim(int percent); // 0..80
void set_rotation(int minutes, int order); // minutes -1 / order -1 = unchanged
// Bumped after the PS5 wakes from rest mode: every listening socket closes and opens again.
int  net_epoch();
void set_volume(int percent); // 0..100
void set_blur(int level);  // 0 off, 1 light, 2 strong
void set_look(const std::string& key, int level); // "bottom", "top", "vignette", "haze"; 0..2
bool set_option(const std::string& key, bool on, std::string& error); // "shuffle", "gamepause", "menuhide", "parallax"
bool delete_video(const std::string& path, std::string& error);
// Light bar colour of a clip: <clip>.color next to it, "RRGGBB" (the page measures it).
std::string clip_color(const std::string& path);
bool        set_clip_color(const std::string& path, const std::string& rgb);
bool        guide_done();
void        set_guide_done(bool done);
bool        set_light_mode(const std::string& mode, const std::string& rgb, int speed);
void        light_step();
bool reset_settings(std::string& error);
void request_shutdown(bool full = false);
bool menu_mods_present(); // another ShellUI mod (OnionHEN-style) is running: restarting the menu is unsafe
// bayosh_mp4.cpp
struct Mp4Check {
	bool        parsed = false;
	bool        ok     = true; // unknown files count as playable
	std::string codec, why;    // why: "4K 3840×2160 · 120 fps" when not ok
	int         w = 0, h = 0;
	double      fps = 0;
};
Mp4Check mp4_check(const std::string& path);
// bayosh_net.cpp
bool        start_download(const std::string& url, std::string& error, const std::string& name_hint = "", const std::string& referer = "");
bool        http_get(const std::string& url, size_t max_bytes, std::string& body, std::string& error,
                     const std::string& user_agent = std::string()); // catalogs, thumbnails
// bayosh_tls.cpp: HTTP/HTTPS GET with redirects; on_head(length or -1, status) and on_body return false to stop
struct WebGetOptions {
	std::string accept, referer, user_agent;
	int         timeout_s = 12;
};
bool web_get(const std::string& url, const WebGetOptions& opt, const std::function<bool(long long, int)>& on_head,
             const std::function<bool(const char*, size_t)>& on_body, std::string& error, int& status);
void        cancel_download();
std::string download_json(); // "null" or {active,done,name,got,total,error}
bool        open_browser(const std::string& url);
void        go_home(); // same as the stop port: payload ends, ShellUI restarts clean
void* game_thread(void*);
// Per-game backgrounds (v1.1)
std::string games_text();                                          // games.txt contents
void        set_game_background(const std::string& tid, const std::string& path); // "" = main wallpaper
std::vector<Game> list_games();                   // installed games (/user/appmeta)
std::string       game_clip(const std::string& tid); // its clip from games.txt ("" = main wallpaper)
std::string       saved_game_clip(const std::string& tid); // saved choice, even when per-game videos are off
bool              is_title_id(const char* name);
void*             focus_thread(void*);
void              mirror_games();
void              write_fast_map(); // ShellUI's fast-switch map; caller holds g_ctl

void start_web_server();
// bayosh_widgets.cpp: home-screen widgets (weather, PS5, controller battery)
void        start_widgets();
bool        widgets_set(const std::string& key, const std::string& value);
std::string widgets_json();
void        widgets_battery(const std::string& value);
std::string widget_colors(const std::string& style); // daemon: outline colours (wave/follow)
void        module_send(const std::string& msg); // UDP to the ShellUI module (bayosh_daemon.cpp)

// Home-screen tile that opens kWebUrl. Returns 1 = newly added, 0 = was already there, -1 = failed.
int install_tile();

} // namespace bayosh

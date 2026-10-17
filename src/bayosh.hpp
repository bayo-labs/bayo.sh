// Copyright (C) 2026 Josué Oswaldo Hernández López (bayo.sh). Licencia GPL-3.0, ver LICENSE.
// bayo.sh daemon: shared declarations (daemon, web window, home-screen tile). GPL-3.0.
#pragma once

#include <string>
#include <vector>

namespace bayosh {

constexpr int         kWebPort  = 9050;
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
	bool               main_video = true;  // off = only tiles with their own video
	bool               game_videos = true; // off = per-game videos ignored
	bool               game_pause = true;  // no video while a game runs
	bool               menu_hide  = true;  // hide the video in Settings / system apps
	bool               store_only = false; // video only on the PS Store tile
	bool               parallax   = false; // nudge the video when moving between home tiles
	bool               light      = false; // controller light bar follows the video's colour (experimental)
	bool               guided     = false; // the first-run guide was shown
	std::string        light_now;          // colour sent to the light bar right now (RRGGBB or "off")
	std::string        game;               // title id of the running game ("" = none)
	std::string        current;  // real path of the chosen clip ("" = none)
	std::string        status;   // ShellUI status line: playing / stopped / failed / starting
	std::vector<Video> videos;
};

std::vector<Video> list_videos();
State              get_state();
// Pick a clip / switch on or off. Safe to call from any thread; blocks until ShellUI let go of
// the old clip (a few seconds at most). Returns false with a reason on failure.
bool select_video(const std::string& path, std::string& error);
bool set_enabled(bool on, std::string& error);
void set_dim(int percent); // 0..80
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
void        light_step();
bool reset_settings(std::string& error);
void request_shutdown(); // same as the stop port: payload ends, ShellUI restarts clean
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

// Home-screen tile that opens kWebUrl. Returns 1 = newly added, 0 = was already there, -1 = failed.
int install_tile();

} // namespace bayosh

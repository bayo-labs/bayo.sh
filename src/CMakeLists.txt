# Copyright (C) 2026 Josué Oswaldo Hernández López (bayo.sh). Licencia GPL-3.0, ver LICENSE.
# bayo.sh: PS5 animated wallpaper. Builds inside the OnionHEN beta2 tree as source/bayosh
# (add_subdirectory(bayosh)), using OnionHEN's libraries. GPL-3.0.
#   bayosh_shellui.elf - runs inside SceShellUI (embedded in bayosh.elf)
#   bayosh.elf         - the payload the user loads
cmake_minimum_required (VERSION 3.20)
project(bayosh C CXX ASM)

onion_module_prologue(C_STANDARD 11 CXX_STANDARD 20)
set(D_CWD "${CMAKE_CURRENT_SOURCE_DIR}")
onion_output_dirs()

set(CMAKE_C_FLAGS "${ONION_PS5_TARGET_FLAGS} -fPIC -fPIE -march=znver2 -Wall -fstack-protector-all -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS}")
set(CMAKE_C_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")

add_executable(bayosh_shellui ${D_CWD}/bayosh_shellui.cpp)
set_target_properties(bayosh_shellui PROPERTIES OUTPUT_NAME "bayosh_shellui.elf")
target_link_options(bayosh_shellui PRIVATE "-Wl,--gc-sections")
add_dependencies(bayosh_shellui onion_proc onion_platform onion_detour)
target_include_directories(bayosh_shellui PRIVATE
	"${PROJECT_ROOT}/libonion_proc/include"
	"${PROJECT_ROOT}/libonion_platform/include"
	"${PROJECT_ROOT}/libonion_detour/include"
	"${PROJECT_ROOT}/include")
target_link_directories(bayosh_shellui PUBLIC "${ONIONHEN_PS5_STUB_DIR}")
target_link_libraries(bayosh_shellui PUBLIC onion_payload onion_proc onion_platform onion_detour SceNotification kernel_sys)
onion_strip_debug(bayosh_shellui)

add_executable(bayosh ${D_CWD}/bayosh_daemon.cpp ${D_CWD}/bayosh_web.cpp ${D_CWD}/bayosh_tile.cpp ${D_CWD}/bayosh_net.cpp)
set_target_properties(bayosh PROPERTIES OUTPUT_NAME "bayosh.elf")
target_link_options(bayosh PRIVATE "-Wl,--gc-sections")
add_dependencies(bayosh bayosh_shellui NineS onion_proc onion_platform onion_elfldr)
set_source_files_properties(${D_CWD}/bayosh_daemon.cpp PROPERTIES OBJECT_DEPENDS "${ONIONHEN_OUT_BIN}/bayosh_shellui.elf")
set_source_files_properties(${D_CWD}/bayosh_web.cpp PROPERTIES OBJECT_DEPENDS "${D_CWD}/bayosh_page.html;${D_CWD}/bayosh_remote.html;${D_CWD}/bayosh_music.mp4")
set_source_files_properties(${D_CWD}/bayosh_tile.cpp PROPERTIES OBJECT_DEPENDS "${D_CWD}/icon0.png")
target_compile_definitions(bayosh PRIVATE
	BAYOSH_SHELLUI_ELF="${ONIONHEN_OUT_BIN}/bayosh_shellui.elf"
	BAYOSH_PAGE_HTML="${D_CWD}/bayosh_page.html"
	BAYOSH_MUSIC_MP4="${D_CWD}/bayosh_music.mp4"
	BAYOSH_REMOTE_HTML="${D_CWD}/bayosh_remote.html"
	BAYOSH_ICON_PNG="${D_CWD}/icon0.png")
target_include_directories(bayosh PRIVATE
	"${PROJECT_ROOT}/libonion_proc/include"
	"${PROJECT_ROOT}/libonion_platform/include"
	"${PROJECT_ROOT}/include")
target_link_directories(bayosh PUBLIC "${ONIONHEN_PS5_STUB_DIR}")
target_link_libraries(bayosh PUBLIC NineS onion_proc onion_platform onion_elfldr SceNotification SceSystemService SceUserService kernel_sys)
onion_strip_debug(bayosh)

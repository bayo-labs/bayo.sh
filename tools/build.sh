#!/usr/bin/env bash
# Copyright (C) 2026 Josué Oswaldo Hernández López (bayo.sh). Licencia GPL-3.0, ver LICENSE.
#
# Compila bayosh.elf (Linux / WSL).
#   ONIONHEN=~/onionHEN PS5_PAYLOAD_SDK=~/ps5-payload-sdk tools/build.sh
#
# ONIONHEN        árbol de OnionHEN en el tag v0.9.0-beta2 (git clone + git checkout v0.9.0-beta2)
# PS5_PAYLOAD_SDK ps5-payload-sdk (probado con v0.44)
# OUT             carpeta de salida (por defecto: dist/ dentro de este repo)
set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
TREE="${ONIONHEN:?Pon ONIONHEN=ruta del árbol de OnionHEN v0.9.0-beta2}"
SDK="${PS5_PAYLOAD_SDK:?Pon PS5_PAYLOAD_SDK=ruta del ps5-payload-sdk}"
OUT="${OUT:-$HERE/dist}"
BUILD="$TREE/build-bayosh"
DEST="$TREE/source/bayosh"

[ -d "$TREE/source" ] || { echo "No encuentro el árbol de OnionHEN en $TREE"; exit 1; }
[ -x "$SDK/bin/prospero-cmake" ] || { echo "No encuentro el SDK en $SDK"; exit 1; }

# Parche de bayo.sh para la librería de detours (compatibilidad con etaHEN 13.60).
if ! grep -q detour_no_kernel_fallback "$TREE/source/libonion_detour/source/detour.cpp"; then
	git -C "$TREE" apply "$HERE/patches/onionhen-detour.patch"
fi
# Solo hacen falta las librerías de OnionHEN, no sus payloads.
sed -i -E 's/^add_subdirectory\((util|daemon|bootstrapper|unpacker)\)/# &/' "$TREE/source/CMakeLists.txt"
grep -q '^add_subdirectory(bayosh)' "$TREE/source/CMakeLists.txt" || echo 'add_subdirectory(bayosh)' >> "$TREE/source/CMakeLists.txt"
mkdir -p "$TREE/.cache/dependencies" && touch "$TREE/.cache/dependencies/kstuff.elf"

rm -rf "$DEST" && mkdir -p "$DEST"
cp "$HERE"/src/* "$DEST/"
cp "$HERE/assets/icon0.png" "$DEST/icon0.png"

export PS5_PAYLOAD_SDK="$SDK"
[ -d /usr/lib/llvm-18/bin ] && export PATH="/usr/lib/llvm-18/bin:$PATH"

"$SDK/bin/prospero-cmake" -S "$TREE/source" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DONIONHEN_KSTUFF_ELF="$TREE/.cache/dependencies/kstuff.elf" -DPS5_PAYLOAD_SDK="$SDK" \
	> "$BUILD.configure.log" 2>&1 || { tail -30 "$BUILD.configure.log"; exit 1; }

if ! cmake --build "$BUILD" -j"$(nproc)" --target bayosh > "$BUILD.log" 2>&1; then
	grep -E "error:|FAILED|undefined symbol" "$BUILD.log" | sort -u | head -30
	exit 1
fi

mkdir -p "$OUT"
cp "$BUILD/bin/bayosh.elf" "$OUT/bayosh.elf"
sha256sum "$OUT/bayosh.elf"
echo "-> $OUT/bayosh.elf"

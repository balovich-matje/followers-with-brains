#!/bin/zsh
# Builds the SKSE plugin (clang-cl + xwin) and the Papyrus scripts (Caprica under Wine).
# Environment:
#   SDK              folder with clangcl-xwin.cmake, prefix/ (CommonLibSSE-NG, fmt, spdlog) and caprica/
#   PAPYRUS_IMPORTS  ';'-separated Windows paths with the base game, SKSE and SkyUI script sources
#   WINE             wine binary (WINEPREFIX etc. must already be set)
set -e
ROOT="${0:A:h:h}"
SDK="${SDK:-$HOME/dev/skyrim-sdk}"
WINE="${WINE:-wine}"

cmake -S "$ROOT/plugin" -B "$ROOT/build/plugin" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$SDK/clangcl-xwin.cmake" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_FIND_ROOT_PATH="$SDK/prefix" >/dev/null
cmake --build "$ROOT/build/plugin"

mkdir -p "$ROOT/build/pex"
WROOT="Z:${ROOT}"
( cd "$ROOT/papyrus/source" && "$WINE" "$SDK/caprica/Caprica.exe" --game skyrim \
	--flags "$WROOT/papyrus/flags/TESV_Papyrus_Flags.flg" \
	--import "${PAPYRUS_IMPORTS}" --output "$WROOT/build/pex" *.psc ) | grep -v -E '^(Adding|Import)'

# Remove the build machine's paths, user and computer name from the compiled scripts.
python3 "$ROOT/tools/pex_scrub.py" "$ROOT"/build/pex/*.pex

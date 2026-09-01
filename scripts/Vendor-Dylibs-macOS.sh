#!/usr/bin/env bash
# Make DKR-R.app self-contained.
#
# Build-macOS.sh produces a bundle that still links /opt/homebrew and carries
# /opt/homebrew/lib as its only LC_RPATH, so the app cannot run on a machine
# without Homebrew. Two things matter here:
#
#   1. dyld searches LC_RPATH entries IN ORDER. An @rpath load command is only
#      as good as that list, so `otool -L` looking correct proves nothing --
#      the Homebrew rpath must actually be removed.
#   2. sdl2-compat dlopen()s libSDL3.dylib by name, searching @loader_path
#      first. It is not a link-time dependency, so dependency walkers miss it
#      entirely and it must be vendored by hand.
set -euo pipefail

APP="${1:?usage: Vendor-Dylibs-macOS.sh /path/to/DKR-R.app}"
BIN="$APP/Contents/MacOS/DKR-R"
FW="$APP/Contents/Frameworks"
BREW="$(brew --prefix)"

[ -x "$BIN" ] || { echo "error: $BIN not found" >&2; exit 1; }
mkdir -p "$FW"

# Resolve the SDL2 dylib from the binary's own load command rather than from
# `brew --prefix sdl2`: that prefix reports the sdl2-compat keg while the
# recorded install name is /opt/homebrew/opt/sdl2/..., and -change only
# rewrites an exact match.
SDL2_REF="$(otool -L "$BIN" | awk '/libSDL2-2\.0\.0\.dylib/ {print $1; exit}')"
[ -n "$SDL2_REF" ] || { echo "error: no SDL2 load command in $BIN" >&2; exit 1; }

cp "$SDL2_REF" "$FW/libSDL2-2.0.0.dylib"
cp "$(brew --prefix sdl3)/lib/libSDL3.0.dylib" "$FW/libSDL3.dylib"
chmod u+w "$FW"/*.dylib

install_name_tool -id @rpath/libSDL2-2.0.0.dylib  "$FW/libSDL2-2.0.0.dylib"
install_name_tool -id @loader_path/libSDL3.dylib  "$FW/libSDL3.dylib"
install_name_tool -change "$SDL2_REF" @rpath/libSDL2-2.0.0.dylib "$BIN"
install_name_tool -add_rpath @executable_path/../Frameworks "$BIN"

# Drop every absolute Homebrew rpath so @rpath cannot resolve outside the app.
while otool -l "$BIN" | grep -A2 LC_RPATH | grep -q "path $BREW"; do
    RP="$(otool -l "$BIN" | grep -A2 LC_RPATH | awk -v p="$BREW" '$1=="path" && index($2,p)==1 {print $2; exit}')"
    [ -n "$RP" ] || break
    install_name_tool -delete_rpath "$RP" "$BIN"
done

# install_name_tool invalidates arm64 signatures; re-sign inside out.
codesign --force --sign - "$FW"/*.dylib
codesign --force --sign - "$BIN"
codesign --force --sign - "$APP"

if otool -L "$BIN" "$FW"/*.dylib | grep -q "$BREW"; then
    echo "error: bundle still references Homebrew paths" >&2; exit 1
fi
if otool -l "$BIN" | grep -A2 LC_RPATH | grep -q "path $BREW"; then
    echo "error: bundle still has a Homebrew LC_RPATH" >&2; exit 1
fi
echo "self-contained: $APP"

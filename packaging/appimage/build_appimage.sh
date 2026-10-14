#!/bin/sh
# Build JoanNative-<version>-x86_64.AppImage from a `cmake --install` prefix (bin/ lib/ share/).
#   build_appimage.sh PREFIX VERSION OUTDIR
# Needs linuxdeploy (+ linuxdeploy-plugin-qt when bin/joa-launcher exists) on PATH or via
# $LINUXDEPLOY; the Qt plugin finds Qt through $QMAKE. Contains only our binaries and their
# shared libraries; original game data is imported by the launcher, never bundled.
set -eu
[ $# -eq 3 ] || { echo "usage: $0 PREFIX VERSION OUTDIR" >&2; exit 2; }
prefix=$(cd "$1" && pwd) version=$2 out=$3
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
appdir=$work/AppDir
mkdir -p "$appdir/usr"
cp -a "$prefix"/. "$appdir/usr/"
set --
for exe in "$appdir"/usr/bin/*; do
  [ -f "$exe" ] && [ -x "$exe" ] && set -- "$@" --executable "$exe"
done
[ -x "$appdir/usr/bin/joa-launcher" ] && set -- "$@" --plugin qt
# Run tool AppImages without FUSE (CI containers); linuxdeploy's bundled strip cannot
# parse .relr.dyn sections of current distro libraries.
export APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1
export OUTPUT="$out/JoanNative-$version-x86_64.AppImage"
"${LINUXDEPLOY:-linuxdeploy}" --appdir "$appdir" "$@" \
  --desktop-file "$here/../desktop/joan-native.desktop" --icon-file "$here/../desktop/joan-native.svg" \
  --custom-apprun "$here/AppRun" --output appimage
echo "$OUTPUT"

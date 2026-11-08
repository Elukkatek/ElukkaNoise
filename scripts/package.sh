#!/usr/bin/env bash
# Packages a built ElukkaNoise VST3 bundle into a beta ZIP for testers.
#
#   scripts/package.sh <windows|linux|macos> <path/to/ElukkaNoise.vst3> <output-dir>
#
# Produces <output-dir>/ElukkaNoise-v<version>-<Windows-x64|Linux-x64|macOS-universal>.zip containing
#   ElukkaNoise.vst3/   (the bundle, unchanged)
#   README.txt          (install instructions from packaging/README-<platform>.txt)
# The version is read from project(... VERSION x.y.z) in CMakeLists.txt.
set -euo pipefail

platform="${1:-}"; bundle="${2:-}"; outdir="${3:-}"
if [[ -z "$platform" || -z "$bundle" || -z "$outdir" ]]; then
    echo "usage: $0 <windows|linux|macos> <path/to/ElukkaNoise.vst3> <output-dir>" >&2; exit 2
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
version="$(sed -n 's/^project(ElukkaNoise VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
[[ -n "$version" ]] || { echo "could not read version from CMakeLists.txt" >&2; exit 1; }

case "$platform" in
    windows) label="Windows-x64"; binary="Contents/x86_64-win/ElukkaNoise.vst3" ;;
    linux)   label="Linux-x64";   binary="Contents/x86_64-linux/ElukkaNoise.so" ;;
    macos)   label="macOS-universal"; binary="Contents/MacOS/ElukkaNoise" ;;
    *) echo "unknown platform '$platform' (expected windows, linux or macos)" >&2; exit 2 ;;
esac

# sanity-check the bundle layout before packaging it
[[ -d "$bundle" ]] || { echo "bundle not found: $bundle" >&2; exit 1; }
[[ -f "$bundle/$binary" ]] || { echo "missing $binary inside $bundle" >&2; exit 1; }
[[ -f "$bundle/Contents/Resources/moduleinfo.json" ]] || echo "warning: no Contents/Resources/moduleinfo.json in bundle" >&2
if [[ "$platform" == macos ]]; then
    archs="$(lipo -archs "$bundle/$binary")"
    [[ "$archs" == *x86_64* && "$archs" == *arm64* ]] || { echo "macOS binary is not universal (archs: $archs)" >&2; exit 1; }
fi

name="ElukkaNoise-v${version}-${label}"
stage="$outdir/$name"
rm -rf "$stage"; mkdir -p "$stage"
cp -R "$bundle" "$stage/ElukkaNoise.vst3"
sed "s/@VERSION@/$version/g" "$root/packaging/README-$platform.txt" > "$stage/README.txt"
# no build leftovers inside the zip
find "$stage" -name '.DS_Store' -delete

zipfile="$outdir/$name.zip"
rm -f "$zipfile"
if command -v zip >/dev/null 2>&1; then
    (cd "$outdir" && zip -r -q "$name.zip" "$name")
elif command -v 7z >/dev/null 2>&1; then
    (cd "$outdir" && 7z a -tzip -r "$name.zip" "$name" >/dev/null)
else
    python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', sys.argv[2], sys.argv[3])" "$outdir/$name" "$outdir" "$name"
fi
rm -rf "$stage"
echo "packaged $zipfile"

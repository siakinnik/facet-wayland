#!/usr/bin/env bash
# Puts the licenses of what a static executable has built in into a release
# directory: <dir>/licenses/STATIC lists the distribution packages the static
# libraries come from ("package version source-package source-version"), next
# to each package's copyright file, plus the full license texts those files
# refer to (common-licenses). scripts/ci/sources.sh packs their sources.
#   scripts/ci/licenses.sh <release-dir> <compiler> <static-lib>...
# e.g. scripts/ci/licenses.sh stage/plugin aarch64-linux-gnu-g++ libc.a libstdc++.a libgcc_eh.a
set -euo pipefail
dir="$1" cxx="$2"
shift 2
mkdir -p "$dir/licenses"
for lib in "$@"; do
    path="$("$cxx" -print-file-name="$lib")"
    if [[ "$path" != /* ]]; then  # not in the compiler's own directories: the target's library paths
        triplet="$("$cxx" -dumpmachine)"
        path="$(find "/usr/lib/$triplet" "/usr/$triplet/lib" -name "$lib" -print -quit 2>/dev/null || true)"
    fi
    [[ -n "$path" ]] || { echo "licenses.sh: $lib not found" >&2; exit 1; }
    real="$(readlink -f "$path")"
    pkg="$( (dpkg -S "$real" || dpkg -S "${real#/usr}") 2>/dev/null | head -n1 | cut -d: -f1)"
    [[ -n "$pkg" ]] || { echo "licenses.sh: no package owns $real" >&2; exit 1; }
    dpkg-query -W -f='${Package} ${Version} ${source:Package} ${source:Version}\n' "$pkg"
    cp -L "/usr/share/doc/$pkg/copyright" "$dir/licenses/$pkg.copyright"
done | sort -u >> "$dir/licenses/STATIC"
sort -u -o "$dir/licenses/STATIC" "$dir/licenses/STATIC"
rm -rf "$dir/licenses/common-licenses"
cp -rL /usr/share/common-licenses "$dir/licenses/common-licenses"

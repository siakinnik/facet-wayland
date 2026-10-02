#!/usr/bin/env bash
# Prints the plugin version from CMakeLists.txt ("0.0.1-alpha") and fails if
# manifest.json disagrees.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="$(sed -n 's/^project(facet-wayland VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
suffix="$(sed -n 's/^set(WAYLAND_VERSION_SUFFIX "\([^"]*\)").*/\1/p' "$root/CMakeLists.txt")"
[[ -n "$version" ]] || { echo "version not found in CMakeLists.txt" >&2; exit 1; }
version="$version${suffix:+-$suffix}"
manifest="$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$root/manifest.json" | head -n1)"
if [[ "$manifest" != "$version" ]]; then
    echo "manifest.json version '$manifest' != CMakeLists.txt version '$version'" >&2
    exit 1
fi
echo "$version"

#!/usr/bin/env bash
# Packs the sources of the third-party parts of a release (the project's own
# code is the repository at the release tag) into one tar, so that the LGPL
# and GPL parts are offered next to their binaries.
#   scripts/ci/sources.sh <out.tar> <entry>...
# Entries:
#   host:<list>           source packages from the build host's distribution
#   jammy:<list>          source packages from Ubuntu 22.04 (Docker unless on it)
#   git:<name>=<url>@<ref> a git checkout without .git
#   url:<url>             a downloaded file
#   file:<path>           a local file (patches)
# <list> files have lines "package version source-package source-version"
# (licenses/STATIC, runtime/licenses/SOURCES); duplicates are fetched once.
# GCC's runtime (libstdc++, libgcc) is under the GCC Runtime Library
# Exception, which asks for no sources: gcc-N and gcc-N-cross source packages
# are left out.
set -euo pipefail
out="$1"
shift
name="$(basename "$out" .tar)"
work="$(mktemp -d)"
trap 'rm -rf "$work" 2>/dev/null || sudo rm -rf "$work"' EXIT
mkdir -p "$work/$name"

# apt-get source of "<source-package>=<version>" lines, deb-src switched on.
fetch='set -e
for f in /etc/apt/sources.list.d/*.sources; do [ -f "$f" ] && sed -i "s/^Types: deb$/Types: deb deb-src/" "$f"; done
[ -f /etc/apt/sources.list ] && sed -i "s/^# *deb-src /deb-src /" /etc/apt/sources.list
apt-get update -qq
cd "$1" && for item in $(sort -u list); do
    apt-get -o APT::Sandbox::User=root source --download-only -qq "$item" >/dev/null 2>&1 || echo "$item" >> missing
done
rm -f list'

# Cross C libraries (cross-toolchain-base) are built from glibc's sources of
# the version in their own: "2.39-0ubuntu8cross1" -> glibc 2.39-0ubuntu8.
pkglist() {
    awk '$3 !~ /^gcc-[0-9]+(-cross)?$/ && NF >= 4 {print $3 "=" $4}
         $3 == "cross-toolchain-base" && $1 ~ /^libc6/ {v = $2; sub(/cross[0-9]+$/, "", v); print "glibc=" v}' "$@" | sort -u
}

# Versions no longer on the mirrors: from Launchpad, which keeps them all.
from_launchpad() {
    local dir="$1" item src ver base dsc
    [[ -f "$dir/missing" ]] || return 0
    while read -r item; do
        src="${item%%=*}" ver="${item#*=}"
        base="https://launchpad.net/ubuntu/+archive/primary/+sourcefiles/$src/$ver"
        dsc="${src}_${ver#*:}.dsc"
        curl -fsSL --retry 3 -o "$dir/$dsc" "$base/$dsc" || { echo "sources.sh: no source for $item" >&2; exit 1; }
        awk '/^Files:/ {f = 1; next} /^[^ ]/ {f = 0} f && NF == 3 {print $3}' "$dir/$dsc" | while read -r file; do
            curl -fsSL --retry 3 -o "$dir/$file" "$base/$file"
        done
    done < "$dir/missing"
    rm -f "$dir/missing"
}

for entry in "$@"; do
    kind="${entry%%:*}" arg="${entry#*:}"
    case "$kind" in
        host)
            mkdir -p "$work/$name/host"
            cat "$arg" >> "$work/$name/host/SOURCES"
            ;;
        jammy)
            mkdir -p "$work/$name/ubuntu-22.04"
            cat "$arg" >> "$work/$name/ubuntu-22.04/SOURCES"
            ;;
        git)
            gname="${arg%%=*}" rest="${arg#*=}"
            url="${rest%@*}" ref="${rest##*@}"
            # A tag or a commit: fetched alone.
            git init -q "$work/$gname"
            git -C "$work/$gname" fetch -q --depth 1 "$url" "$ref"
            git -C "$work/$gname" -c advice.detachedHead=false checkout -q FETCH_HEAD
            mkdir -p "$work/$name/git"
            git -C "$work/$gname" archive --format=tar.gz --prefix="$gname-$ref/" -o "$work/$name/git/$gname-$ref.tar.gz" HEAD
            rm -rf "$work/$gname"
            ;;
        url)
            mkdir -p "$work/$name/downloads"
            curl -fsSL --retry 3 -o "$work/$name/downloads/$(basename "$arg")" "$arg"
            ;;
        file)
            mkdir -p "$work/$name/patches"
            cp "$arg" "$work/$name/patches/"
            ;;
        *) echo "sources.sh: unknown entry $entry" >&2; exit 1 ;;
    esac
done

if [[ -f "$work/$name/host/SOURCES" ]]; then
    sort -u -o "$work/$name/host/SOURCES" "$work/$name/host/SOURCES"
    pkglist "$work/$name/host/SOURCES" > "$work/$name/host/list"
    if [[ $EUID == 0 ]]; then bash -c "$fetch" _ "$work/$name/host"; else sudo bash -c "$fetch" _ "$work/$name/host"; fi
    [[ $EUID == 0 ]] || sudo chown -R "$(id -u)" "$work/$name/host"
    from_launchpad "$work/$name/host"
fi
if [[ -f "$work/$name/ubuntu-22.04/SOURCES" ]]; then
    sort -u -o "$work/$name/ubuntu-22.04/SOURCES" "$work/$name/ubuntu-22.04/SOURCES"
    pkglist "$work/$name/ubuntu-22.04/SOURCES" > "$work/$name/ubuntu-22.04/list"
    if grep -q '^VERSION_CODENAME=jammy' /etc/os-release && [[ $EUID == 0 ]]; then
        bash -c "$fetch" _ "$work/$name/ubuntu-22.04"
    else
        docker run --rm -v "$work/$name/ubuntu-22.04:/w" ubuntu:22.04 bash -c "$fetch" _ /w
        [[ $EUID == 0 ]] || sudo chown -R "$(id -u)" "$work/$name/ubuntu-22.04"
    fi
    from_launchpad "$work/$name/ubuntu-22.04"
fi
mkdir -p "$(dirname "$out")"
tar -C "$work" -cf "$out" "$name"
echo "$out ($(du -h "$out" | cut -f1))"

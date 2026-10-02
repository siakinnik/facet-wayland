#!/usr/bin/env bash
# Builds the compositor's libraries (wlroots and what it needs) at the
# versions pinned in DEPS as static libraries into a prefix, so the plugin has
# no dependencies outside itself. Needs git, a C compiler, python3 (meson is
# installed into a private venv), ninja, pkg-config, bison and the libffi and
# expat development files. Takes a few minutes.
#   scripts/build-deps.sh <install-prefix> [jobs]   (CMake runs it when needed)
set -euo pipefail
prefix="$(mkdir -p "$1" && cd "$1" && pwd)"
jobs="${2:-$(nproc)}"
root="$(cd "$(dirname "$0")/.." && pwd)"
stamp="$(grep -v '^#' "$root/DEPS" | sha256sum | cut -c1-16)"

if [[ -f "$prefix/.deps" && "$(cat "$prefix/.deps")" == "$stamp" ]]; then
    echo "dependencies $stamp already installed in $prefix"
    exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 -m venv "$work/venv"
"$work/venv/bin/pip" install -q 'meson==1.5.2'
meson="$work/venv/bin/meson"

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/lib/x86_64-linux-gnu/pkgconfig:$prefix/share/pkgconfig"
export PATH="$prefix/bin:$PATH"

options() {
    case "$1" in
        wayland) echo -Ddocumentation=false -Dtests=false -Ddtd_validation=false ;;
        wayland-protocols) echo -Dtests=false ;;
        pixman) echo -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled ;;
        libdrm)
            echo -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled \
                -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled -Dvc4=disabled \
                -Detnaviv=disabled -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled \
                -Dtests=false -Dinstall-test-programs=false ;;
        libxkbcommon)
            echo -Denable-x11=false -Denable-docs=false -Denable-tools=false -Denable-wayland=false \
                -Denable-xkbregistry=false -Denable-bash-completion=false ;;
        # Headless backend and the pixman renderer only: no GPU, no input
        # devices, no seat/session; Facet owns the real screen.
        wlroots)
            echo -Dbackends= -Drenderers= -Dallocators= -Dsession=disabled -Dxwayland=disabled \
                -Dexamples=false -Dxcb-errors=disabled -Dlibliftoff=disabled ;;
    esac
}

grep -v '^#' "$root/DEPS" | while read -r name version url; do
    [[ -n "$name" ]] || continue
    echo "== $name $version"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$version" "$url" "$work/$name"
    # shellcheck disable=SC2046
    "$meson" setup "$work/$name/build" "$work/$name" --prefix "$prefix" --libdir lib --buildtype release \
        -Ddefault_library=static $(options "$name") >"$work/$name.log" 2>&1 ||
        { cat "$work/$name.log"; exit 1; }
    ninja -C "$work/$name/build" -j "$jobs" install >>"$work/$name.log" 2>&1 || { tail -50 "$work/$name.log"; exit 1; }
done
echo "$stamp" > "$prefix/.deps"
echo "dependencies installed in $prefix"

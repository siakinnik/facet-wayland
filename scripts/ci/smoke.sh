#!/usr/bin/env bash
# Protocol smoke test: plays the core's side of a short session (hello with
# the permissions, a client module starting, ping, shutdown) and checks that
# the compositor starts, lends that module a surface and opens its socket.
# FACET_RUNNER runs foreign-arch binaries (e.g. qemu-aarch64-static).
#   scripts/ci/smoke.sh build/wayland.plugin/wayland
set -euo pipefail
bin="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
runner=(${FACET_RUNNER:-})
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/data" "$work/surface" "$work/endpoints/display.wayland/some-app"

input="$(printf '%s\n' \
    "{\"t\":\"hello\",\"api\":3,\"data_dir\":\"$work/data\",\"locale\":\"en\",\"timezone\":\"UTC\",\"permissions\":[\"background\",\"display.surface\",\"wayland.compositor\"],\"surface_dir\":\"$work/surface\",\"screen\":{\"w\":640,\"h\":400},\"endpoints\":{\"provides\":{\"display.wayland\":\"$work/endpoints/display.wayland\"},\"requires\":{}}}" \
    '{"t":"wayland_client","module":"some-app","scopes":["wayland.window"],"running":true}' \
    "{\"t\":\"consumer\",\"capability\":\"display.wayland\",\"module\":\"some-app\",\"dir\":\"$work/endpoints/display.wayland/some-app\",\"running\":true,\"visible\":false}" \
    '{"t":"ping","seq":7}')"
out="$( (printf '%s\n' "$input"; sleep 1; [[ -S "$work/endpoints/display.wayland/some-app/wayland-0" ]] && echo '{"t":"ping","seq":8}'; echo '{"t":"shutdown"}') |
    FACET_PLUGIN_DATA="$work/data" timeout 30 "${runner[@]}" "$bin")"
echo "$out"

check() {
    if ! grep -q "$1" <<<"$out"; then
        echo "smoke test failed: missing $2" >&2
        exit 1
    fi
}
check '"t":"hello"' "hello reply"
check '"id":"wayland"' "plugin id"
check '"api":3' "API version"
check '"sdk":' "SDK version"
check '"t":"pong"' "pong"
check '"seq":7' "ping sequence"
check '"for":"some-app"' "surface lent to the client module"
check '"t":"background"' "background task"
check '"seq":8' "client socket (no pong after the socket check)"
echo "ok: protocol session passed"

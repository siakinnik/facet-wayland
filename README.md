# wayland — Facet plugin

Wayland display server for [facet-core](https://github.com/siakinnik/facet-core):
lets other modules run desktop apps (a browser, messengers) and show their
windows on their own screen. It is a dependency, not an app: it has no tile,
only a page under Settings > Modules.

Facet stays the owner of the screen and of input and never depends on this
module. The module plays along through the generic plugin SDK:

- It **provides** the capability `display.wayland@1`. App modules list it in
  their `requires`; without an installed provider they do not start and Facet
  tells the user to install `siakinnik/facet-wayland`.
- It runs its own compositor (wlroots, headless backend, pixman renderer: no
  GPU, no input devices, no seat). Each app module gets a display of its own,
  the size of the screen, whose picture is **lent** to that module as a Facet
  surface. Facet shows it full screen on the module's screen, a swipe down
  from the top edge goes back as everywhere.
- Touches come back from Facet (apps without touch support get a mouse).
  When a text field of the app is focused (text-input-v3: GTK, Qt, Firefox,
  Chromium, foot) Facet's keyboard opens, whole strings in any script are
  committed, and the window gets shorter so the field stays above the keyboard.
  Apps without text-input get key presses from a keymap that grows with what
  is typed (no XKB data files needed).
- Every app module connects through its own socket, in a directory the core
  shares only between this module and that app (a capability endpoint). The
  compositor therefore always knows which module a client belongs to, and
  each client sees only the protocols its module was granted:

  | Scope (permission of the app module) | Protocols |
  |---|---|
  | `wayland.window` | `xdg_wm_base`, decorations, activation, text input |
  | `wayland.clipboard` | `wl_data_device_manager`, primary selection |
  | `wayland.screencopy` | screen capture (not offered yet) |
  | never | layer shell, virtual keyboard/pointer, input method, data control, foreign toplevel, output management, gamma, session lock |

  Windows of different apps live on different displays: no app sees, covers
  or captures another app's windows.
- Displays render only while their app is on screen; hidden apps get no frame
  callbacks and stop drawing.

Needs Facet 0.6 (API 3) and the permissions `display.surface`,
`wayland.compositor` and `background`.

## Writing an app module

```json
{
  "id": "my-browser",
  "api": 3,
  "exec": "my-browser",
  "permissions": ["wayland.window", "network"],
  "requires": [{ "name": "display.wayland@1", "install": "siakinnik/facet-wayland" }],
  "tile": { "icon": "plugin" }
}
```

The module starts its app with `WAYLAND_DISPLAY=<plugin.endpoint("display.wayland")>/wayland-0`
and `XDG_RUNTIME_DIR=/tmp`, and shows the surface it is lent
(`on_surface_lent`) with `Screen::fullscreen(id)`. `examples/desktop-app` is
a complete template: it runs the command in its `command` file.

## Install

On a device that already runs [Facet](https://github.com/siakinnik/facet-core)
0.6 or newer (prebuilt static binary for x86_64):

```bash
curl -fsSL https://raw.githubusercontent.com/siakinnik/facet-core/main/scripts/get.sh | sudo bash -s -- --plugin siakinnik/facet-wayland
```

Run the same command to update. Remove with `… | sudo bash -s -- --remove-plugin wayland`.

## Build from source

Needs a C/C++ compiler, CMake, ninja, pkg-config, bison, python3 (venv) and
the libffi and expat development files. The first configure builds wlroots
and its libraries at the versions pinned in `DEPS` into `third_party/deps`
(a few minutes, once); they are linked statically.

```bash
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build
FACET_PLUGIN_PATH=$PWD/build/wayland.plugin:$PWD/build/examples ../facet-core/build/facet
```

The plugin SDK comes from facet-core: a checkout in `../facet-core` (or
`-DFACET_CORE_DIR=…`) is used when present, otherwise CMake downloads it at
`FACET_CORE_REF`.

## Releases

Same as the other Facet plugins: CI checks translations, versions, builds,
runs the unit tests and a protocol smoke test. To release, bump the version in
`CMakeLists.txt` (`project(VERSION)`, `WAYLAND_VERSION_SUFFIX`) and
`manifest.json`, push, then Actions → Release → *Run workflow*.

## Files

```
src/compositor.*   the wlroots compositor (C: wlroots headers are not C++)
src/scopes.*       which protocols each wayland.* scope allows (unit-tested)
src/keymap.*       the keymap that grows with typed text (unit-tested)
src/main.cpp       plugin glue: consumers, lent surfaces, touch, text, settings page
src/i18n/          translations
examples/          desktop-app: template of an app module
scripts/build-deps.sh   builds the pinned libraries (DEPS) statically
```

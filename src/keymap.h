// A keymap that grows with what is typed. Facet's keyboard sends text, not
// key positions, so every character gets a keycode of its own when first
// typed (like wtype). The keymap is self-contained: compiling it needs no XKB
// data files on the device.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { FW_KEYMAP_MIN = 9, FW_KEYMAP_MAX = 255 };

struct fw_keymap {
    uint32_t syms[FW_KEYMAP_MAX + 1];  // keysym per XKB keycode, 0 = free
    uint32_t used[FW_KEYMAP_MAX + 1];  // last use, for recycling
    uint32_t clock;
};

// Fixed keys (BackSpace, Return, Tab, Escape, arrows, ...) are mapped at once.
void fw_keymap_init(struct fw_keymap* km);
// XKB keycode for `keysym`; *changed is set when the keymap text changed
// (a new key, possibly replacing the least recently used one).
int fw_keymap_code(struct fw_keymap* km, uint32_t keysym, bool* changed);
// The keymap in XKB text format; free() it.
char* fw_keymap_text(const struct fw_keymap* km);

#ifdef __cplusplus
}
#endif

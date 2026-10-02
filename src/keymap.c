#include "keymap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// BackSpace, Return, Tab, Escape, Left, Up, Right, Down, Delete, Home, End.
static const uint32_t fixed[] = {0xff08, 0xff0d, 0xff09, 0xff1b, 0xff51, 0xff52, 0xff53, 0xff54, 0xffff, 0xff50, 0xff57};
enum { N_FIXED = sizeof fixed / sizeof fixed[0] };

void fw_keymap_init(struct fw_keymap* km) {
    memset(km, 0, sizeof *km);
    for (int i = 0; i < N_FIXED; ++i) km->syms[FW_KEYMAP_MIN + i] = fixed[i];
}

int fw_keymap_code(struct fw_keymap* km, uint32_t keysym, bool* changed) {
    *changed = false;
    ++km->clock;
    for (int c = FW_KEYMAP_MIN; c <= FW_KEYMAP_MAX; ++c) {
        if (km->syms[c] == keysym) {
            km->used[c] = km->clock;
            return c;
        }
    }
    // A free code, else the one unused for the longest time (never a fixed key).
    int best = -1;
    for (int c = FW_KEYMAP_MIN + N_FIXED; c <= FW_KEYMAP_MAX; ++c) {
        if (km->syms[c] == 0) {
            best = c;
            break;
        }
        if (best < 0 || km->used[c] < km->used[best]) best = c;
    }
    km->syms[best] = keysym;
    km->used[best] = km->clock;
    *changed = true;
    return best;
}

char* fw_keymap_text(const struct fw_keymap* km) {
    size_t cap = 4096 + (FW_KEYMAP_MAX + 1) * 64, len = 0;
    char* out = malloc(cap);
    if (!out) return NULL;
#define PUT(...) len += (size_t)snprintf(out + len, cap - len, __VA_ARGS__)
    PUT("xkb_keymap {\nxkb_keycodes \"facet\" {\n minimum = 8;\n maximum = %d;\n", FW_KEYMAP_MAX);
    for (int c = FW_KEYMAP_MIN; c <= FW_KEYMAP_MAX; ++c) PUT(" <K%d> = %d;\n", c, c);
    PUT("};\nxkb_types \"facet\" {\n type \"ONE_LEVEL\" {\n  modifiers = none;\n  level_name[Level1] = \"Any\";\n };\n};\n");
    PUT("xkb_compatibility \"facet\" {\n};\nxkb_symbols \"facet\" {\n");
    for (int c = FW_KEYMAP_MIN; c <= FW_KEYMAP_MAX; ++c)
        if (km->syms[c]) PUT(" key <K%d> { [ 0x%x ] };\n", c, km->syms[c]);
    PUT("};\n};\n");
#undef PUT
    return out;
}

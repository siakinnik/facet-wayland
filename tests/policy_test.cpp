// Scope rules and the generated keymap (it must compile with xkbcommon
// without any XKB data files, as on the device).
#include <xkbcommon/xkbcommon.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "keymap.h"
#include "scopes.h"

static int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

static void test_scopes() {
    const char* window[] = {"wayland.window"};
    const char* all[] = {"wayland.window", "wayland.clipboard", "wayland.screencopy"};
    CHECK(fw_scope_allows("wl_compositor", nullptr, 0));
    CHECK(fw_scope_allows("wl_seat", nullptr, 0));
    CHECK(!fw_scope_allows("xdg_wm_base", nullptr, 0));
    CHECK(fw_scope_allows("xdg_wm_base", window, 1));
    // GTK 3 needs the data device manager for its seat; the clipboard itself
    // is checked when used.
    CHECK(fw_scope_allows("wl_data_device_manager", nullptr, 0));
    CHECK(!fw_scope_allows("zwp_primary_selection_device_manager_v1", window, 1));
    CHECK(fw_scope_allows("zwp_primary_selection_device_manager_v1", all, 3));
    CHECK(!fw_scope_allows("zwlr_screencopy_manager_v1", window, 1));
    CHECK(fw_scope_allows("zwlr_screencopy_manager_v1", all, 3));
    // Never, whatever was granted.
    CHECK(!fw_scope_allows("zwlr_layer_shell_v1", all, 3));
    CHECK(!fw_scope_allows("zwp_virtual_keyboard_manager_v1", all, 3));
    CHECK(!fw_scope_allows("zwlr_data_control_manager_v1", all, 3));
}

static bool compiles(const fw_keymap& km, xkb_context* ctx, xkb_keycode_t code, xkb_keysym_t want) {
    char* text = fw_keymap_text(&km);
    xkb_keymap* keymap = xkb_keymap_new_from_string(ctx, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    std::free(text);
    if (!keymap) return false;
    xkb_state* state = xkb_state_new(keymap);
    bool ok = xkb_state_key_get_one_sym(state, code) == want;
    xkb_state_unref(state);
    xkb_keymap_unref(keymap);
    return ok;
}

static void test_keymap() {
    xkb_context* ctx = xkb_context_new(xkb_context_flags(XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES));
    CHECK(ctx);
    fw_keymap km;
    fw_keymap_init(&km);
    bool changed = true;
    int bs = fw_keymap_code(&km, XKB_KEY_BackSpace, &changed);
    CHECK(!changed);  // fixed key
    CHECK(compiles(km, ctx, xkb_keycode_t(bs), XKB_KEY_BackSpace));

    xkb_keysym_t cyr = xkb_utf32_to_keysym(0x0436);  // the Cyrillic letter zhe
    int c1 = fw_keymap_code(&km, cyr, &changed);
    CHECK(changed);
    CHECK(compiles(km, ctx, xkb_keycode_t(c1), cyr));
    int again = fw_keymap_code(&km, cyr, &changed);
    CHECK(!changed && again == c1);

    // Many distinct characters: codes are recycled, never beyond the range.
    for (uint32_t cp = 0x4e00; cp < 0x4e00 + 600; ++cp) {
        int c = fw_keymap_code(&km, xkb_utf32_to_keysym(cp), &changed);
        CHECK(c >= FW_KEYMAP_MIN && c <= FW_KEYMAP_MAX);
        if (c < FW_KEYMAP_MIN || c > FW_KEYMAP_MAX) break;
    }
    CHECK(fw_keymap_code(&km, XKB_KEY_Return, &changed) > 0 && !changed);  // fixed keys survive
    int a = fw_keymap_code(&km, 'a', &changed);
    CHECK(compiles(km, ctx, xkb_keycode_t(a), 'a'));
    xkb_context_unref(ctx);
}

int main() {
    test_scopes();
    test_keymap();
    if (failures) return 1;
    std::printf("ok: policy tests passed\n");
    return 0;
}

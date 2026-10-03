#include "scopes.h"

#include <stddef.h>
#include <string.h>

static const struct {
    const char* interface;
    const char* scope;
} rules[] = {
    // Windows.
    {"xdg_wm_base", "wayland.window"},
    {"zxdg_decoration_manager_v1", "wayland.window"},
    {"xdg_activation_v1", "wayland.window"},
    {"zwp_text_input_manager_v3", "wayland.window"},
    // Clipboard, shared between the modules that hold this scope.
    // wl_data_device_manager is for everyone: GTK 3 creates no input seat
    // without it. The clipboard itself is checked in the compositor.
    {"zwp_primary_selection_device_manager_v1", "wayland.clipboard"},
    // Reading other windows' pixels.
    {"zwlr_screencopy_manager_v1", "wayland.screencopy"},
    {"ext_image_copy_capture_manager_v1", "wayland.screencopy"},
    {"ext_output_image_capture_source_manager_v1", "wayland.screencopy"},
    // Never for clients: input injection, clipboard snooping, overlays
    // above Facet, control over outputs and other windows.
    {"zwlr_data_control_manager_v1", ""},
    {"ext_data_control_manager_v1", ""},
    {"zwp_virtual_keyboard_manager_v1", ""},
    {"zwlr_virtual_pointer_manager_v1", ""},
    {"zwp_input_method_manager_v2", ""},
    {"zwlr_layer_shell_v1", ""},
    {"zwlr_foreign_toplevel_manager_v1", ""},
    {"ext_foreign_toplevel_list_v1", ""},
    {"zwlr_output_manager_v1", ""},
    {"zwlr_output_power_manager_v1", ""},
    {"zwlr_gamma_control_manager_v1", ""},
    {"zwp_keyboard_shortcuts_inhibit_manager_v1", ""},
    {"ext_session_lock_manager_v1", ""},
};

const char* fw_scope_for_global(const char* interface_name) {
    for (size_t i = 0; i < sizeof rules / sizeof rules[0]; ++i)
        if (strcmp(rules[i].interface, interface_name) == 0) return rules[i].scope;
    return NULL;
}

bool fw_scope_allows(const char* interface_name, const char* const* scopes, int n_scopes) {
    const char* need = fw_scope_for_global(interface_name);
    if (!need) return true;
    if (!*need) return false;
    for (int i = 0; i < n_scopes; ++i)
        if (strcmp(scopes[i], need) == 0) return true;
    return false;
}

// The Wayland compositor: wlroots with the headless backend and the pixman
// renderer, so it needs no GPU, no input devices and no seat. Facet owns the
// real screen; every client module gets a display of its own (an output the
// size of the screen) whose picture goes to a Facet surface on that module's
// screen, and touches and typed text come back from Facet.
//
// Each client module connects through its own socket in its endpoint
// directory, so the compositor always knows which module a client belongs to
// and enforces that module's wayland.* scopes.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fw_server;

struct fw_callbacks {
    void* data;
    // Memory to copy a new frame of the module's display into (w x h XRGB8888,
    // stride w), or NULL if it cannot take one now (call fw_server_kick later).
    uint32_t* (*frame_target)(void* data, const char* module, int w, int h);
    void (*frame_done)(void* data, const char* module);
    // A text field of the module's focused window wants the keyboard (or not).
    void (*text_input)(void* data, const char* module, bool active);
    void (*windows_changed)(void* data);
};

struct fw_window {
    const char* module;
    const char* app_id;
    const char* title;
    bool focused;
};

// Returns NULL and an English message in `error` on failure.
struct fw_server* fw_server_create(const struct fw_callbacks* cb, char* error, int error_len);
void fw_server_destroy(struct fw_server* s);
// The event loop's fd: call fw_server_dispatch() when it is readable.
int fw_server_fd(struct fw_server* s);
void fw_server_dispatch(struct fw_server* s);
// Retries frames that could not be delivered, flushes clients.
void fw_server_kick(struct fw_server* s);

// A client module started: listens on <socket_dir>/wayland-0.
bool fw_display_add(struct fw_server* s, const char* module, const char* socket_dir, int w, int h, char* error,
                    int error_len);
// Closes its socket and disconnects its clients.
void fw_display_remove(struct fw_server* s, const char* module);
bool fw_display_exists(struct fw_server* s, const char* module);
void fw_display_resize(struct fw_server* s, const char* module, int w, int h);
// Its screen is open in Facet: render it and give it the keyboard focus.
void fw_display_set_visible(struct fw_server* s, const char* module, bool visible);
// Pixels at the bottom covered by Facet's keyboard: windows get shorter.
void fw_display_set_inset(struct fw_server* s, const char* module, int bottom);

// The wayland.* scopes the user granted the module (from Facet).
void fw_set_scopes(struct fw_server* s, const char* module, const char* const* scopes, int n);

enum fw_touch_kind { FW_TOUCH_DOWN, FW_TOUCH_MOVE, FW_TOUCH_UP };
// A finger on the module's display, in its pixels.
void fw_touch(struct fw_server* s, const char* module, enum fw_touch_kind kind, double x, double y);
// Typed on Facet's keyboard: action "insert" (text), "backspace", "enter".
void fw_text(struct fw_server* s, const char* module, const char* action, const char* text);

// Mapped windows, at most `max`; strings stay valid until the next dispatch.
int fw_windows(struct fw_server* s, struct fw_window* out, int max);

#ifdef __cplusplus
}
#endif

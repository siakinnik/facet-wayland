#define _GNU_SOURCE  // accept4
#include "compositor.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "keymap.h"
#include "scopes.h"

// Displays sit side by side in the layout, far enough apart never to touch.
enum { DISPLAY_SPACING = 10000, MAX_SCOPES = 16, GPU_SLOTS = 8 };

struct fw_display;

// A client's GPU buffer attached to the display's Facet surface.
struct fw_slot {
    struct fw_display* display;
    struct wlr_buffer* buffer;  // NULL = free
    bool locked;                // held for Facet (the client gets it back when unlocked)
    struct wl_listener destroy;
};

struct fw_display {
    struct wl_list link;
    struct fw_server* server;
    char* module;
    char socket_path[108];
    int listen_fd;
    struct wl_event_source* listen_source;
    int x, w, h, inset;  // w, h, inset in pixels
    int scale;            // output scale; windows, layout and input use w / scale
    bool visible, pending, force;
    // Frame statistics, logged every 30 s while on screen.
    struct {
        int frames, idle, waits, commits, gpu_frames;
        char why[96];  // why the last frame did not go to Facet as it is
        double render_ms, copy_ms, wait_ms, wait_since, since;
    } stats;
    struct wlr_output* output;
    struct wlr_scene_output* scene_output;
    struct wlr_scene_tree* root;
    struct wlr_scene_rect* background;
    struct wl_list views;  // fw_view.link, topmost first
    struct wl_listener frame, output_destroy;
    struct fw_slot slots[GPU_SLOTS];
    int presented;               // slot Facet has not acknowledged yet, -1 if none
    struct wlr_buffer* last_gpu; // the buffer presented last, NULL after a software frame
};

struct fw_client {
    struct wl_list link;
    struct wl_client* client;
    struct fw_display* display;
    struct wl_listener destroy;
};

struct fw_view {
    struct wl_list link;  // in display->views while mapped
    struct fw_display* display;
    struct wlr_xdg_toplevel* toplevel;
    struct wlr_scene_tree* tree;
    struct wlr_xdg_toplevel_decoration_v1* decoration;
    bool mapped;
    struct wl_listener map, unmap, commit, destroy, set_title, set_app_id, request_maximize, request_fullscreen;
};

struct fw_popup {
    struct wlr_xdg_popup* popup;
    struct fw_display* display;
    struct wl_listener commit, destroy;
};

struct fw_decoration {
    struct wlr_xdg_toplevel_decoration_v1* decoration;
    struct wl_listener request_mode, destroy;
};

struct fw_text_input {
    struct wl_list link;
    struct fw_server* server;
    struct wlr_text_input_v3* input;
    struct wl_listener enable, commit, disable, destroy;
};

struct fw_scopes {
    struct wl_list link;
    char* module;
    char* scopes[MAX_SCOPES];
    int n;
};

struct fw_server {
    struct fw_callbacks cb;
    struct wl_display* display;
    struct wl_event_loop* loop;
    struct wlr_backend* backend;
    struct wlr_renderer* renderer;
    struct wlr_allocator* allocator;
    struct wlr_output_layout* layout;
    struct wlr_scene* scene;
    struct wlr_scene_output_layout* scene_layout;
    struct wlr_xdg_shell* xdg_shell;
    struct wlr_xdg_decoration_manager_v1* decorations;
    struct wlr_text_input_manager_v3* text_inputs;
    struct wlr_seat* seat;
    struct wlr_keyboard keyboard;
    struct xkb_context* xkb;
    struct fw_keymap keymap;

    struct wl_list displays, clients, inputs, scopes;
    struct fw_display* adding;  // display whose output is being created
    int next_slot;
    bool gpu;  // clients may use GPU buffers
    struct wlr_linux_dmabuf_v1* dmabuf;
    bool touching, touch_pointer;
    double touch_ox, touch_oy;
    char* text_module;  // module whose text field has the keyboard, NULL if none
    bool text_numeric;

    struct wl_listener new_output, new_toplevel, new_popup, new_decoration, new_text_input;
    struct wl_listener request_selection, request_primary_selection;
};

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Runs what wlroots deferred to "idle" (frame scheduling above all) and sends
// pending events. The event loop is driven from the plugin's own loop, which
// only dispatches when the loop's fd is readable, and idle work does not make
// it readable: without this, frames would wait for the next unrelated event.
static void flush(struct fw_server* s) {
    wl_event_loop_dispatch_idle(s->loop);
    wl_display_flush_clients(s->display);
}

static void set_error(char* error, int len, const char* fmt, ...) {
    if (!error || len <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error, (size_t)len, fmt, ap);
    va_end(ap);
}

// High-density screens get scale 2, so apps are as large as Facet's own UI.
static int scale_for(int h) { return h >= 1800 ? 2 : 1; }

// Logical (surface-local) size of a display, as windows see it.
static int lw(const struct fw_display* d) { return d->w / d->scale; }
static int lh(const struct fw_display* d) { return (d->h - d->inset) / d->scale; }

static struct fw_display* find_display(struct fw_server* s, const char* module) {
    struct fw_display* d;
    wl_list_for_each(d, &s->displays, link) if (strcmp(d->module, module) == 0) return d;
    return NULL;
}

static struct fw_display* display_of_client(struct fw_server* s, const struct wl_client* client) {
    struct fw_client* c;
    wl_list_for_each(c, &s->clients, link) if (c->client == client) return c->display;
    return NULL;
}

static struct fw_scopes* find_scopes(struct fw_server* s, const char* module) {
    struct fw_scopes* sc;
    wl_list_for_each(sc, &s->scopes, link) if (strcmp(sc->module, module) == 0) return sc;
    return NULL;
}

// ------------------------------------------------------------------ focus and text input

static struct fw_display* visible_display(struct fw_server* s) {
    struct fw_display* d;
    wl_list_for_each(d, &s->displays, link) if (d->visible) return d;
    return NULL;
}

static struct fw_text_input* focused_input(struct fw_server* s) {
    struct wlr_surface* focus = s->seat->keyboard_state.focused_surface;
    struct fw_text_input* ti;
    if (!focus) return NULL;
    wl_list_for_each(ti, &s->inputs, link) if (ti->input->focused_surface == focus) return ti;
    return NULL;
}

// text-input-v3 content purposes that want a numeric keyboard: digits,
// number, phone, PIN.
static bool numeric_purpose(uint32_t purpose) { return purpose == 2 || purpose == 4 || purpose == 5 || purpose == 10; }

static void update_text_wanted(struct fw_server* s) {
    struct fw_text_input* ti = focused_input(s);
    struct fw_display* d = visible_display(s);
    const char* module = ti && ti->input->current_enabled && d ? d->module : NULL;
    bool numeric = module && numeric_purpose(ti->input->current.content_type.purpose);
    if ((!module && !s->text_module) ||
        (module && s->text_module && strcmp(module, s->text_module) == 0 && numeric == s->text_numeric))
        return;
    if (s->text_module && (!module || strcmp(module, s->text_module) != 0))
        s->cb.text_input(s->cb.data, s->text_module, false, false);
    free(s->text_module);
    s->text_module = NULL;
    if (module) {
        s->text_module = strdup(module);
        s->text_numeric = numeric;
        s->cb.text_input(s->cb.data, module, true, numeric);
    }
}

// Text inputs of the focused surface's client enter it, all others leave.
static void update_text_focus(struct fw_server* s) {
    struct wlr_surface* focus = s->seat->keyboard_state.focused_surface;
    struct fw_text_input* ti;
    wl_list_for_each(ti, &s->inputs, link) {
        bool same_client = focus && wl_resource_get_client(ti->input->resource) == wl_resource_get_client(focus->resource);
        if (ti->input->focused_surface && ti->input->focused_surface != focus) wlr_text_input_v3_send_leave(ti->input);
        if (same_client && ti->input->focused_surface != focus) wlr_text_input_v3_send_enter(ti->input, focus);
    }
    update_text_wanted(s);
}

// The keyboard goes to the topmost window of the display on screen.
static void update_focus(struct fw_server* s) {
    struct fw_display* d = visible_display(s);
    struct fw_view* top = NULL;
    if (d && !wl_list_empty(&d->views)) top = wl_container_of(d->views.next, top, link);
    struct wlr_surface* want = top ? top->toplevel->base->surface : NULL;
    struct wlr_surface* prev = s->seat->keyboard_state.focused_surface;
    if (want != prev) {
        struct wlr_xdg_toplevel* old = prev ? wlr_xdg_toplevel_try_from_wlr_surface(prev) : NULL;
        if (old) wlr_xdg_toplevel_set_activated(old, false);
        if (want) {
            wlr_xdg_toplevel_set_activated(top->toplevel, true);
            wlr_seat_keyboard_notify_enter(s->seat, want, NULL, 0, &s->keyboard.modifiers);
        } else {
            wlr_seat_keyboard_notify_clear_focus(s->seat);
        }
    }
    update_text_focus(s);
}

static void text_input_changed(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_text_input* ti = wl_container_of(l, ti, commit);
    update_text_wanted(ti->server);
}

static void text_input_enable(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_text_input* ti = wl_container_of(l, ti, enable);
    update_text_wanted(ti->server);
}

static void text_input_disable(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_text_input* ti = wl_container_of(l, ti, disable);
    update_text_wanted(ti->server);
}

static void text_input_destroy(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_text_input* ti = wl_container_of(l, ti, destroy);
    struct fw_server* s = ti->server;
    wl_list_remove(&ti->link);
    wl_list_remove(&ti->enable.link);
    wl_list_remove(&ti->commit.link);
    wl_list_remove(&ti->disable.link);
    wl_list_remove(&ti->destroy.link);
    free(ti);
    update_text_wanted(s);
}

static void new_text_input(struct wl_listener* l, void* data) {
    struct fw_server* s = wl_container_of(l, s, new_text_input);
    struct fw_text_input* ti = calloc(1, sizeof *ti);
    if (!ti) return;
    ti->server = s;
    ti->input = data;
    ti->enable.notify = text_input_enable;
    ti->commit.notify = text_input_changed;
    ti->disable.notify = text_input_disable;
    ti->destroy.notify = text_input_destroy;
    wl_signal_add(&ti->input->events.enable, &ti->enable);
    wl_signal_add(&ti->input->events.commit, &ti->commit);
    wl_signal_add(&ti->input->events.disable, &ti->disable);
    wl_signal_add(&ti->input->events.destroy, &ti->destroy);
    wl_list_insert(&s->inputs, &ti->link);
    update_text_focus(s);
}

// ------------------------------------------------------------------ windows

static void configure_view(struct fw_view* v) {
    struct fw_display* d = v->display;
    if (!v->toplevel->base->initialized) return;
    if (v->decoration)
        wlr_xdg_toplevel_decoration_v1_set_mode(v->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    if (v->toplevel->parent) {  // dialogs keep their own size
        wlr_xdg_surface_schedule_configure(v->toplevel->base);
        return;
    }
    // Apps fill their display, like on a phone; no window frames to drag.
    // Full screen (kiosk browsers, video) when they ask: the same size, without
    // their own bars.
    wlr_xdg_toplevel_set_size(v->toplevel, lw(d), lh(d) > 1 ? lh(d) : 1);
    wlr_xdg_toplevel_set_fullscreen(v->toplevel, v->toplevel->requested.fullscreen);
    wlr_xdg_toplevel_set_maximized(v->toplevel, !v->toplevel->requested.fullscreen);
    wlr_xdg_toplevel_set_tiled(v->toplevel, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
}

static void center_dialog(struct fw_view* v) {
    if (!v->toplevel->parent) return;
    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(v->toplevel->base, &geo);
    int x = (lw(v->display) - geo.width) / 2, y = (lh(v->display) - geo.height) / 2;
    wlr_scene_node_set_position(&v->tree->node, x > 0 ? x : 0, y > 0 ? y : 0);
}

static void view_commit(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_view* v = wl_container_of(l, v, commit);
    v->display->stats.commits++;
    if (v->toplevel->base->initial_commit) configure_view(v);
    else if (v->mapped) center_dialog(v);
}

static void view_map(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_view* v = wl_container_of(l, v, map);
    v->mapped = true;
    wl_list_insert(&v->display->views, &v->link);
    wlr_scene_node_raise_to_top(&v->tree->node);
    center_dialog(v);
    update_focus(v->display->server);
    v->display->server->cb.windows_changed(v->display->server->cb.data);
}

static void view_unmap(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_view* v = wl_container_of(l, v, unmap);
    if (!v->mapped) return;
    v->mapped = false;
    wl_list_remove(&v->link);
    update_focus(v->display->server);
    v->display->server->cb.windows_changed(v->display->server->cb.data);
}

static void view_set_title(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_view* v = wl_container_of(l, v, set_title);
    if (v->mapped) v->display->server->cb.windows_changed(v->display->server->cb.data);
}

static void view_set_app_id(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_view* v = wl_container_of(l, v, set_app_id);
    if (v->mapped) v->display->server->cb.windows_changed(v->display->server->cb.data);
}

// The protocol wants a configure in reply; the answer is always "fill the display".
static void view_request_maximize(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_view* v = wl_container_of(l, v, request_maximize);
    if (v->toplevel->base->initialized) wlr_xdg_surface_schedule_configure(v->toplevel->base);
}

static void view_request_fullscreen(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_view* v = wl_container_of(l, v, request_fullscreen);
    if (v->toplevel->base->initialized) configure_view(v);
}

static void view_destroy(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_view* v = wl_container_of(l, v, destroy);
    if (v->mapped) wl_list_remove(&v->link);
    wl_list_remove(&v->map.link);
    wl_list_remove(&v->unmap.link);
    wl_list_remove(&v->commit.link);
    wl_list_remove(&v->destroy.link);
    wl_list_remove(&v->set_title.link);
    wl_list_remove(&v->set_app_id.link);
    wl_list_remove(&v->request_maximize.link);
    wl_list_remove(&v->request_fullscreen.link);
    free(v);
}

static void new_toplevel(struct wl_listener* l, void* data) {
    struct fw_server* s = wl_container_of(l, s, new_toplevel);
    struct wlr_xdg_toplevel* toplevel = data;
    struct fw_display* d = display_of_client(s, wl_resource_get_client(toplevel->resource));
    if (!d) return;  // cannot happen: every client comes through a display's socket
    struct fw_view* v = calloc(1, sizeof *v);
    if (!v) return;
    v->display = d;
    v->toplevel = toplevel;
    v->tree = wlr_scene_xdg_surface_create(d->root, toplevel->base);
    v->tree->node.data = v;
    toplevel->base->data = v->tree;
    v->map.notify = view_map;
    v->unmap.notify = view_unmap;
    v->commit.notify = view_commit;
    v->destroy.notify = view_destroy;
    v->set_title.notify = view_set_title;
    v->set_app_id.notify = view_set_app_id;
    v->request_maximize.notify = view_request_maximize;
    v->request_fullscreen.notify = view_request_fullscreen;
    wl_signal_add(&toplevel->base->surface->events.map, &v->map);
    wl_signal_add(&toplevel->base->surface->events.unmap, &v->unmap);
    wl_signal_add(&toplevel->base->surface->events.commit, &v->commit);
    wl_signal_add(&toplevel->events.destroy, &v->destroy);
    wl_signal_add(&toplevel->events.set_title, &v->set_title);
    wl_signal_add(&toplevel->events.set_app_id, &v->set_app_id);
    wl_signal_add(&toplevel->events.request_maximize, &v->request_maximize);
    wl_signal_add(&toplevel->events.request_fullscreen, &v->request_fullscreen);
}

static struct fw_view* view_of_toplevel(struct wlr_xdg_toplevel* toplevel) {
    struct wlr_scene_tree* tree = toplevel->base->data;
    return tree ? tree->node.data : NULL;
}

// ------------------------------------------------------------------ popups

static void popup_commit(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_popup* p = wl_container_of(l, p, commit);
    if (!p->popup->base->initial_commit) return;
    // Keep menus inside the display: the box is in the root toplevel's coordinates.
    struct wlr_xdg_surface* root = p->popup->base;
    while (root->role == WLR_XDG_SURFACE_ROLE_POPUP && root->popup->parent) {
        struct wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(root->popup->parent);
        if (!parent) break;
        root = parent;
    }
    int tx = 0, ty = 0;
    struct wlr_scene_tree* tree = root->data;
    if (tree) wlr_scene_node_coords(&tree->node, &tx, &ty);
    struct wlr_box box = {p->display->x - tx, -ty, lw(p->display), lh(p->display)};
    wlr_xdg_popup_unconstrain_from_box(p->popup, &box);
    wlr_xdg_surface_schedule_configure(p->popup->base);
}

static void popup_destroy(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_popup* p = wl_container_of(l, p, destroy);
    wl_list_remove(&p->commit.link);
    wl_list_remove(&p->destroy.link);
    free(p);
}

static void new_popup(struct wl_listener* l, void* data) {
    struct fw_server* s = wl_container_of(l, s, new_popup);
    struct wlr_xdg_popup* popup = data;
    struct wlr_xdg_surface* parent = popup->parent ? wlr_xdg_surface_try_from_wlr_surface(popup->parent) : NULL;
    struct fw_display* d = display_of_client(s, wl_resource_get_client(popup->resource));
    if (!parent || !parent->data || !d) return;
    struct fw_popup* p = calloc(1, sizeof *p);
    if (!p) return;
    p->popup = popup;
    p->display = d;
    popup->base->data = wlr_scene_xdg_surface_create(parent->data, popup->base);
    p->commit.notify = popup_commit;
    p->destroy.notify = popup_destroy;
    wl_signal_add(&popup->base->surface->events.commit, &p->commit);
    wl_signal_add(&popup->events.destroy, &p->destroy);
}

// ------------------------------------------------------------------ decorations

static void decoration_request_mode(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_decoration* fd = wl_container_of(l, fd, request_mode);
    if (fd->decoration->toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(fd->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void decoration_destroy(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_decoration* fd = wl_container_of(l, fd, destroy);
    struct fw_view* v = view_of_toplevel(fd->decoration->toplevel);
    if (v && v->decoration == fd->decoration) v->decoration = NULL;
    wl_list_remove(&fd->request_mode.link);
    wl_list_remove(&fd->destroy.link);
    free(fd);
}

// Server-side "decorations" are none at all: apps fill the display without
// title bars, Facet's own navigation leaves them.
static void new_decoration(struct wl_listener* l, void* data) {
    (void)l;
    struct wlr_xdg_toplevel_decoration_v1* deco = data;
    struct fw_decoration* fd = calloc(1, sizeof *fd);
    if (!fd) return;
    fd->decoration = deco;
    struct fw_view* v = view_of_toplevel(deco->toplevel);
    if (v) v->decoration = deco;
    fd->request_mode.notify = decoration_request_mode;
    fd->destroy.notify = decoration_destroy;
    wl_signal_add(&deco->events.request_mode, &fd->request_mode);
    wl_signal_add(&deco->events.destroy, &fd->destroy);
    decoration_request_mode(&fd->request_mode, NULL);
}

// ------------------------------------------------------------------ clipboard

// The clipboard is shared between apps whose modules hold wayland.clipboard;
// the others neither put anything into it nor see what is there.
static bool client_has_clipboard(struct fw_server* s, const struct wl_client* client) {
    struct fw_display* d = client ? display_of_client(s, client) : NULL;
    if (!d) return false;
    struct fw_scopes* sc = find_scopes(s, d->module);
    if (!sc) return false;
    for (int i = 0; i < sc->n; ++i)
        if (strcmp(sc->scopes[i], "wayland.clipboard") == 0) return true;
    return false;
}

static bool selection_filter(struct wlr_seat_client* client, void* data) {
    return client_has_clipboard(data, client->client);
}

static void request_selection(struct wl_listener* l, void* data) {
    struct fw_server* s = wl_container_of(l, s, request_selection);
    struct wlr_seat_request_set_selection_event* e = data;
    // Only the focused client may set the selection (wlroots checks the serial).
    struct wlr_seat_client* from = s->seat->keyboard_state.focused_client;
    if (e->source && !client_has_clipboard(s, from ? from->client : NULL)) {
        wlr_data_source_destroy(e->source);
        return;
    }
    wlr_seat_set_selection(s->seat, e->source, e->serial);
}

static void request_primary_selection(struct wl_listener* l, void* data) {
    struct fw_server* s = wl_container_of(l, s, request_primary_selection);
    struct wlr_seat_request_set_primary_selection_event* e = data;
    wlr_seat_set_primary_selection(s->seat, e->source, e->serial);
}

// ------------------------------------------------------------------ outputs and frames

static double now_ms_f(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void log_stats(struct fw_display* d) {
    double t = now_ms_f();
    if (d->stats.since == 0) d->stats.since = t;
    double span = t - d->stats.since;
    if (span < 30000) return;
    int f = d->stats.frames;
    if (f || d->stats.waits || d->stats.commits)
        fprintf(stderr,
                "wayland: %s: %.1f fps, %d app commits, %d idle frames, %d waits for Facet (%.1f ms total), "
                "render %.1f ms, copy %.1f ms per frame, %d frames straight from the GPU%s%s\n",
                d->module, f * 1000.0 / span, d->stats.commits, d->stats.idle, d->stats.waits, d->stats.wait_ms,
                f > d->stats.gpu_frames ? d->stats.render_ms / (f - d->stats.gpu_frames) : 0.0,
                f > d->stats.gpu_frames ? d->stats.copy_ms / (f - d->stats.gpu_frames) : 0.0, d->stats.gpu_frames,
                d->stats.why[0] ? "; otherwise: " : "", d->stats.why);
    memset(&d->stats, 0, sizeof d->stats);
    d->stats.since = t;
}

static void send_frame_done(struct fw_display* d) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(d->scene_output, &now);
}

// ------------------------------------------------------------------ GPU buffers

static void slot_clear(struct fw_slot* sl) {
    if (!sl->buffer) return;
    wl_list_remove(&sl->destroy.link);
    if (sl->locked) wlr_buffer_unlock(sl->buffer);
    sl->buffer = NULL;
    sl->locked = false;
}

static void slot_destroy(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_slot* sl = wl_container_of(l, sl, destroy);
    struct fw_display* d = sl->display;
    struct fw_server* s = d->server;
    int i = (int)(sl - d->slots);
    if (d->last_gpu == sl->buffer) d->last_gpu = NULL;
    wl_list_remove(&sl->destroy.link);
    sl->buffer = NULL;
    sl->locked = false;
    s->cb.gpu_detach(s->cb.data, d->module, i);
}

static void slots_reset(struct fw_display* d) {
    for (int i = 0; i < GPU_SLOTS; ++i) slot_clear(&d->slots[i]);
    d->presented = -1;
    d->last_gpu = NULL;
}

// Hands back to clients the buffers Facet no longer shows.
static void slots_release(struct fw_display* d) {
    struct fw_server* s = d->server;
    if (!s->gpu) return;
    if (d->presented >= 0 && s->cb.gpu_ready(s->cb.data, d->module)) d->presented = -1;
    int shown = s->cb.gpu_shown(s->cb.data, d->module);
    for (int i = 0; i < GPU_SLOTS; ++i) {
        struct fw_slot* sl = &d->slots[i];
        if (sl->locked && i != shown && i != d->presented) {
            wlr_buffer_unlock(sl->buffer);
            sl->locked = false;
        }
    }
}

struct fw_cover {
    int count;
    struct wlr_scene_buffer* buffer;
    int sx, sy;
};

static void count_buffer(struct wlr_scene_buffer* b, int sx, int sy, void* data) {
    struct fw_cover* c = data;
    if (++c->count == 1) {
        c->buffer = b;
        c->sx = sx;
        c->sy = sy;
    }
}

static bool gpu_format(uint32_t format) { return format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888; }

// The client's GPU buffer if it alone is on the display, pixel for pixel,
// from the top left corner over the whole width (the keyboard may make the
// window shorter), else NULL (with the reason in the statistics).
static struct wlr_buffer* covering_buffer(struct fw_display* d) {
    char* why = d->stats.why;
    size_t n = sizeof d->stats.why;
    struct fw_cover c = {0};
    wlr_scene_output_for_each_buffer(d->scene_output, count_buffer, &c);
    if (c.count != 1 || !c.buffer->buffer) {
        snprintf(why, n, "%d buffers on screen", c.count);
        return NULL;
    }
    struct wlr_scene_buffer* sb = c.buffer;
    struct wlr_client_buffer* cb = wlr_client_buffer_get(sb->buffer);
    struct wlr_buffer* src = cb ? cb->source : sb->buffer;
    struct wlr_dmabuf_attributes a;
    if (!src || !wlr_buffer_get_dmabuf(src, &a)) {
        snprintf(why, n, "%s", src ? "shared memory (the app draws in software)" : "buffer released");
        return NULL;
    }
    if (a.n_planes != 1 || a.modifier != DRM_FORMAT_MOD_LINEAR || !gpu_format(a.format)) {
        snprintf(why, n, "GPU buffer: %d planes, format 0x%x, modifier 0x%llx", a.n_planes, a.format,
                 (unsigned long long)a.modifier);
        return NULL;
    }
    int bw = sb->buffer->width, bh = sb->buffer->height;
    // A source box of the whole buffer (viewporter) is the same as none.
    const struct wlr_fbox* sbox = &sb->src_box;
    bool whole = wlr_fbox_empty(sbox) || (sbox->x == 0 && sbox->y == 0 && (int)sbox->width == bw &&
                                          (int)sbox->height == bh);
    if (sb->transform != WL_OUTPUT_TRANSFORM_NORMAL || sb->opacity < 1.f || !whole) {
        snprintf(why, n, "transform %d, opacity %.2f, source %.0f,%.0f %.0fx%.0f of %dx%d", (int)sb->transform,
                 sb->opacity, sbox->x, sbox->y, sbox->width, sbox->height, bw, bh);
        return NULL;
    }
    // On screen it must take exactly its pixels (logical size x scale).
    int dw = sb->dst_width ? sb->dst_width * d->scale : bw, dh = sb->dst_height ? sb->dst_height * d->scale : bh;
    if (c.sx != 0 || c.sy != 0 || bw != d->w || bh > d->h || dw != bw || dh != bh) {
        snprintf(why, n, "window %dx%d (shown %dx%d) at %d,%d", bw, bh, dw, dh, c.sx, c.sy);
        return NULL;
    }
    return src;
}

// The slot holding `b`, attaching it to a free one first; -1 if impossible.
static int slot_for(struct fw_display* d, struct wlr_buffer* b) {
    struct fw_server* s = d->server;
    int free_slot = -1;
    for (int i = 0; i < GPU_SLOTS; ++i) {
        if (d->slots[i].buffer == b) return i;
        if (!d->slots[i].buffer && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {  // all taken: give up one Facet does not need
        int shown = s->cb.gpu_shown(s->cb.data, d->module);
        for (int i = 0; i < GPU_SLOTS && free_slot < 0; ++i)
            if (!d->slots[i].locked && i != shown && i != d->presented) free_slot = i;
        if (free_slot < 0) return -1;
        slot_clear(&d->slots[free_slot]);
        s->cb.gpu_detach(s->cb.data, d->module, free_slot);
    }
    struct wlr_dmabuf_attributes a;
    if (!wlr_buffer_get_dmabuf(b, &a)) return -1;
    if (!s->cb.gpu_attach(s->cb.data, d->module, free_slot, a.fd[0], a.format, a.modifier, a.offset[0], a.stride[0],
                          a.width, a.height))
        return -1;
    struct fw_slot* sl = &d->slots[free_slot];
    sl->display = d;
    sl->buffer = b;
    sl->locked = false;
    sl->destroy.notify = slot_destroy;
    wl_signal_add(&b->events.destroy, &sl->destroy);
    return free_slot;
}

static bool check_dmabuf(struct wlr_dmabuf_attributes* a, void* data) {
    (void)data;
    return a->n_planes == 1 && a->modifier == DRM_FORMAT_MOD_LINEAR && gpu_format(a->format);
}

bool fw_server_enable_gpu(struct fw_server* s, uint64_t device) {
    if (s->dmabuf || !s->cb.gpu_attach) return s->dmabuf != NULL;
    struct wlr_linux_dmabuf_feedback_v1 fb = {.main_device = (dev_t)device};
    wl_array_init(&fb.tranches);
    struct wlr_linux_dmabuf_feedback_v1_tranche* t = wl_array_add(&fb.tranches, sizeof *t);
    if (!t) return false;
    memset(t, 0, sizeof *t);
    t->target_device = (dev_t)device;
    wlr_drm_format_set_add(&t->formats, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR);
    wlr_drm_format_set_add(&t->formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);
    s->dmabuf = wlr_linux_dmabuf_v1_create(s->display, 4, &fb);
    wlr_drm_format_set_finish(&t->formats);
    wl_array_release(&fb.tranches);
    if (!s->dmabuf) return false;
    wlr_linux_dmabuf_v1_set_check_dmabuf_callback(s->dmabuf, check_dmabuf, s);
    s->gpu = true;
    return true;
}

// A window drawn by the GPU that fills the display goes to Facet as it is:
// true if this frame was handled so (or has to wait for Facet).
static bool present_gpu(struct fw_display* d, bool damaged) {
    struct fw_server* s = d->server;
    if (!s->gpu) return false;
    slots_release(d);
    struct wlr_buffer* b = covering_buffer(d);
    if (!b) {
        if (d->last_gpu) {  // back to software: everything is drawn again
            d->last_gpu = NULL;
            wlr_damage_ring_add_whole(&d->scene_output->damage_ring);
        }
        return false;
    }
    if (b == d->last_gpu && !d->force) {  // nothing new
        pixman_region32_clear(&d->scene_output->damage_ring.current);
        if (!damaged) d->stats.idle++;
        send_frame_done(d);
        return true;
    }
    if (!s->cb.gpu_ready(s->cb.data, d->module)) {
        if (!d->pending) {
            d->stats.waits++;
            d->stats.wait_since = now_ms_f();
        }
        d->pending = true;
        return true;
    }
    int slot = slot_for(d, b);
    if (slot < 0) {
        snprintf(d->stats.why, sizeof d->stats.why, "no free slot, or Facet refused the buffer");
        return false;
    }
    if (d->stats.wait_since > 0) {
        d->stats.wait_ms += now_ms_f() - d->stats.wait_since;
        d->stats.wait_since = 0;
    }
    struct fw_slot* sl = &d->slots[slot];
    if (!sl->locked) {
        wlr_buffer_lock(b);
        sl->locked = true;
    }
    s->cb.gpu_present(s->cb.data, d->module, slot);
    d->presented = slot;
    d->last_gpu = b;
    d->force = false;
    d->stats.frames++;
    d->stats.gpu_frames++;
    pixman_region32_clear(&d->scene_output->damage_ring.current);
    send_frame_done(d);
    return true;
}

static void display_frame(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_display* d = wl_container_of(l, d, frame);
    struct fw_server* s = d->server;
    // Hidden: no frames, so clients stop drawing too.
    if (!d->visible || !d->scene_output) return;
    bool damaged = d->output->needs_frame || pixman_region32_not_empty(&d->scene_output->damage_ring.current);
    log_stats(d);
    if (present_gpu(d, damaged)) return;
    if (!d->force && !damaged) {
        d->stats.idle++;
        send_frame_done(d);
        return;
    }
    uint32_t* dst = s->cb.frame_target(s->cb.data, d->module, d->w, d->h);
    if (!dst) {
        if (!d->pending) {
            d->stats.waits++;
            d->stats.wait_since = now_ms_f();
        }
        d->pending = true;  // Facet has not taken the previous frame yet
        return;
    }
    if (d->stats.wait_since > 0) {
        d->stats.wait_ms += now_ms_f() - d->stats.wait_since;
        d->stats.wait_since = 0;
    }
    double t0 = now_ms_f();
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (!wlr_scene_output_build_state(d->scene_output, &state, NULL)) {
        wlr_output_state_finish(&state);
        return;
    }
    if ((state.committed & WLR_OUTPUT_STATE_BUFFER) && state.buffer) {
        void* px;
        uint32_t format;
        size_t stride;
        if (wlr_buffer_begin_data_ptr_access(state.buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &px, &format, &stride)) {
            int w = state.buffer->width < d->w ? state.buffer->width : d->w;
            int h = state.buffer->height < d->h ? state.buffer->height : d->h;
            double t1 = now_ms_f();
            d->stats.render_ms += t1 - t0;
            d->stats.frames++;
            // XRGB8888 and ARGB8888 share the layout Facet expects.
            for (int y = 0; y < h; ++y) memcpy(dst + (size_t)y * (size_t)d->w, (char*)px + (size_t)y * stride, (size_t)w * 4);
            wlr_buffer_end_data_ptr_access(state.buffer);
            d->stats.copy_ms += now_ms_f() - t1;
            s->cb.frame_done(s->cb.data, d->module);
            d->force = false;
        }
    }
    wlr_output_commit_state(d->output, &state);
    wlr_output_state_finish(&state);
    send_frame_done(d);
}

static void display_output_destroy(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_display* d = wl_container_of(l, d, output_destroy);
    wl_list_remove(&d->frame.link);
    wl_list_remove(&d->output_destroy.link);
    d->output = NULL;
    d->scene_output = NULL;
}

static void new_output(struct wl_listener* l, void* data) {
    struct fw_server* s = wl_container_of(l, s, new_output);
    struct wlr_output* output = data;
    struct fw_display* d = s->adding;
    if (!d) return;
    wlr_output_init_render(output, s->allocator, s->renderer);
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, d->w, d->h, 60000);
    wlr_output_state_set_scale(&state, (float)d->scale);
    wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);
    d->output = output;
    d->frame.notify = display_frame;
    d->output_destroy.notify = display_output_destroy;
    wl_signal_add(&output->events.frame, &d->frame);
    wl_signal_add(&output->events.destroy, &d->output_destroy);
    struct wlr_output_layout_output* lo = wlr_output_layout_add(s->layout, output, d->x, 0);
    d->scene_output = wlr_scene_output_create(s->scene, output);
    if (lo && d->scene_output) wlr_scene_output_layout_add_output(s->scene_layout, lo, d->scene_output);
}

static void repaint(struct fw_display* d) {
    if (!d->output || !d->scene_output) return;
    d->force = true;
    wlr_damage_ring_add_whole(&d->scene_output->damage_ring);
    wlr_output_schedule_frame(d->output);
}

// ------------------------------------------------------------------ clients

static void client_destroy(struct wl_listener* l, void* data) {
    (void)data;
    struct fw_client* c = wl_container_of(l, c, destroy);
    wl_list_remove(&c->link);
    wl_list_remove(&c->destroy.link);
    free(c);
}

static int accept_client(int fd, uint32_t mask, void* data) {
    struct fw_display* d = data;
    (void)mask;
    int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
    if (cfd < 0) return 0;
    struct fw_client* c = calloc(1, sizeof *c);
    struct wl_client* client = c ? wl_client_create(d->server->display, cfd) : NULL;
    if (!client) {
        free(c);
        close(cfd);
        return 0;
    }
    c->client = client;
    c->display = d;
    c->destroy.notify = client_destroy;
    wl_client_add_destroy_listener(client, &c->destroy);
    wl_list_insert(&d->server->clients, &c->link);
    return 0;
}

// Only what the client's module was granted is visible to it at all.
static bool global_filter(const struct wl_client* client, const struct wl_global* global, void* data) {
    struct fw_server* s = data;
    const char* name = wl_global_get_interface(global)->name;
    struct fw_display* d = display_of_client(s, client);
    if (!d) return fw_scope_for_global(name) == NULL;
    struct fw_scopes* sc = find_scopes(s, d->module);
    return fw_scope_allows(name, sc ? (const char* const*)sc->scopes : NULL, sc ? sc->n : 0);
}

// ------------------------------------------------------------------ keyboard

static void keyboard_led_update(struct wlr_keyboard* kb, uint32_t leds) {
    (void)kb;
    (void)leds;
}

static const struct wlr_keyboard_impl keyboard_impl = {.name = "facet-keyboard", .led_update = keyboard_led_update};

static bool apply_keymap(struct fw_server* s) {
    char* text = fw_keymap_text(&s->keymap);
    if (!text) return false;
    struct xkb_keymap* km = xkb_keymap_new_from_string(s->xkb, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    free(text);
    if (!km) return false;
    bool ok = wlr_keyboard_set_keymap(&s->keyboard, km);
    xkb_keymap_unref(km);
    return ok;
}

static void tap(struct fw_server* s, uint32_t keysym) {
    bool changed = false;
    int code = fw_keymap_code(&s->keymap, keysym, &changed);
    if (changed && !apply_keymap(s)) return;
    uint32_t t = now_ms();
    wlr_seat_keyboard_notify_key(s->seat, t, (uint32_t)(code - 8), WL_KEYBOARD_KEY_STATE_PRESSED);
    wlr_seat_keyboard_notify_key(s->seat, t, (uint32_t)(code - 8), WL_KEYBOARD_KEY_STATE_RELEASED);
}

// Next code point of UTF-8 text, 0 at the end.
static uint32_t next_utf8(const char** p) {
    const unsigned char* s = (const unsigned char*)*p;
    if (!*s) return 0;
    uint32_t c = *s++;
    int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (extra) c &= 0x3f >> extra;
    while (extra-- > 0 && (*s & 0xc0) == 0x80) c = (c << 6) | (*s++ & 0x3f);
    *p = (const char*)s;
    return c;
}

// ------------------------------------------------------------------ API

struct fw_server* fw_server_create(const struct fw_callbacks* cb, char* error, int error_len) {
    wlr_log_init(WLR_ERROR, NULL);
    struct fw_server* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cb = *cb;
    wl_list_init(&s->displays);
    wl_list_init(&s->clients);
    wl_list_init(&s->inputs);
    wl_list_init(&s->scopes);
    s->display = wl_display_create();
    if (!s->display) goto fail;
    s->loop = wl_display_get_event_loop(s->display);
    s->backend = wlr_headless_backend_create(s->loop);
    if (!s->backend) {
        set_error(error, error_len, "cannot create the headless backend");
        goto fail;
    }
    s->renderer = wlr_pixman_renderer_create();
    if (!s->renderer || !wlr_renderer_init_wl_shm(s->renderer, s->display)) {
        set_error(error, error_len, "cannot create the pixman renderer");
        goto fail;
    }
    s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
    if (!s->allocator) {
        set_error(error, error_len, "cannot create a buffer allocator");
        goto fail;
    }
    wlr_compositor_create(s->display, 6, s->renderer);
    wlr_subcompositor_create(s->display);
    wlr_data_device_manager_create(s->display);
    wlr_primary_selection_v1_device_manager_create(s->display);
    wlr_viewporter_create(s->display);
    wlr_single_pixel_buffer_manager_v1_create(s->display);
    s->layout = wlr_output_layout_create(s->display);
    wlr_xdg_output_manager_v1_create(s->display, s->layout);
    // Every frame is copied into Facet's surface, so the scene must always
    // render into our own buffers: a client buffer scanned out directly
    // cannot be read back.
    setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);
    s->scene = wlr_scene_create();
    s->scene_layout = wlr_scene_attach_output_layout(s->scene, s->layout);

    s->xdg_shell = wlr_xdg_shell_create(s->display, 6);
    s->new_toplevel.notify = new_toplevel;
    wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_toplevel);
    s->new_popup.notify = new_popup;
    wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_popup);
    s->decorations = wlr_xdg_decoration_manager_v1_create(s->display);
    s->new_decoration.notify = new_decoration;
    wl_signal_add(&s->decorations->events.new_toplevel_decoration, &s->new_decoration);
    s->text_inputs = wlr_text_input_manager_v3_create(s->display);
    s->new_text_input.notify = new_text_input;
    wl_signal_add(&s->text_inputs->events.text_input, &s->new_text_input);

    s->seat = wlr_seat_create(s->display, "seat0");
    wlr_seat_set_capabilities(s->seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH);
    s->seat->selection_filter = selection_filter;
    s->seat->selection_filter_data = s;
    s->request_selection.notify = request_selection;
    wl_signal_add(&s->seat->events.request_set_selection, &s->request_selection);
    s->request_primary_selection.notify = request_primary_selection;
    wl_signal_add(&s->seat->events.request_set_primary_selection, &s->request_primary_selection);

    // No XKB data files needed: the keymap is generated (keymap.c).
    s->xkb = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
    wlr_keyboard_init(&s->keyboard, &keyboard_impl, "facet-keyboard");
    fw_keymap_init(&s->keymap);
    if (!s->xkb || !apply_keymap(s)) {
        set_error(error, error_len, "cannot compile the keymap");
        goto fail;
    }
    wlr_seat_set_keyboard(s->seat, &s->keyboard);

    s->new_output.notify = new_output;
    wl_signal_add(&s->backend->events.new_output, &s->new_output);
    wl_display_set_global_filter(s->display, global_filter, s);
    if (!wlr_backend_start(s->backend)) {
        set_error(error, error_len, "cannot start the backend");
        goto fail;
    }
    return s;
fail:
    if (error && !*error) set_error(error, error_len, "cannot create the Wayland display");
    if (s->display) wl_display_destroy(s->display);
    free(s);
    return NULL;
}

void fw_server_destroy(struct fw_server* s) {
    if (!s) return;
    while (!wl_list_empty(&s->displays)) {
        struct fw_display* d = wl_container_of(s->displays.next, d, link);
        fw_display_remove(s, d->module);
    }
    wl_display_destroy_clients(s->display);
    wlr_backend_destroy(s->backend);
    wl_display_destroy(s->display);
    free(s->text_module);
    free(s);
}

int fw_server_fd(struct fw_server* s) { return wl_event_loop_get_fd(s->loop); }

void fw_server_dispatch(struct fw_server* s) {
    wl_event_loop_dispatch(s->loop, 0);
    flush(s);
}

void fw_server_kick(struct fw_server* s) {
    struct fw_display* d;
    wl_list_for_each(d, &s->displays, link) {
        slots_release(d);  // buffers Facet showed and replaced go back to their clients
        if (d->pending && d->output) {
            d->pending = false;
            wlr_output_schedule_frame(d->output);
        }
    }
    flush(s);
}

bool fw_display_exists(struct fw_server* s, const char* module) { return find_display(s, module) != NULL; }

bool fw_display_add(struct fw_server* s, const char* module, const char* socket_dir, int w, int h, char* error,
                    int error_len) {
    if (find_display(s, module)) return true;
    struct fw_display* d = calloc(1, sizeof *d);
    if (d) d->presented = -1;
    if (!d) return false;
    d->server = s;
    d->module = strdup(module);
    d->w = w > 0 ? w : 1280;
    d->h = h > 0 ? h : 800;
    d->scale = scale_for(d->h);
    d->x = s->next_slot++ * DISPLAY_SPACING;
    d->listen_fd = -1;
    wl_list_init(&d->views);
    if (snprintf(d->socket_path, sizeof d->socket_path, "%s/wayland-0", socket_dir) >= (int)sizeof d->socket_path) {
        set_error(error, error_len, "socket path too long");
        goto fail;
    }
    // The socket is the module's door: clients that use it belong to it.
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    memcpy(addr.sun_path, d->socket_path, sizeof d->socket_path);
    unlink(d->socket_path);
    d->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (d->listen_fd < 0 || bind(d->listen_fd, (struct sockaddr*)&addr, sizeof addr) != 0 ||
        listen(d->listen_fd, 16) != 0) {
        set_error(error, error_len, "cannot listen on %s: %s", d->socket_path, strerror(errno));
        goto fail;
    }
    chmod(d->socket_path, 0666);  // the module runs as its own user; only it sees this directory
    d->listen_source = wl_event_loop_add_fd(s->loop, d->listen_fd, WL_EVENT_READABLE, accept_client, d);

    d->root = wlr_scene_tree_create(&s->scene->tree);
    wlr_scene_node_set_position(&d->root->node, d->x, 0);
    const float black[4] = {0, 0, 0, 1};
    d->background = wlr_scene_rect_create(d->root, lw(d), d->h / d->scale, black);
    wl_list_insert(s->displays.prev, &d->link);
    s->adding = d;
    wlr_headless_add_output(s->backend, (unsigned)d->w, (unsigned)d->h);
    s->adding = NULL;
    if (!d->output) {
        set_error(error, error_len, "cannot create an output");
        fw_display_remove(s, module);
        return false;
    }
    return true;
fail:
    if (d->listen_fd >= 0) close(d->listen_fd);
    free(d->module);
    free(d);
    return false;
}

void fw_display_remove(struct fw_server* s, const char* module) {
    struct fw_display* d = find_display(s, module);
    if (!d) return;
    slots_reset(d);
    struct fw_client *c, *tmp;
    wl_list_for_each_safe(c, tmp, &s->clients, link) if (c->display == d) wl_client_destroy(c->client);
    if (d->listen_source) wl_event_source_remove(d->listen_source);
    if (d->listen_fd >= 0) close(d->listen_fd);
    unlink(d->socket_path);
    if (d->output) wlr_output_destroy(d->output);
    wlr_scene_node_destroy(&d->root->node);
    wl_list_remove(&d->link);
    if (s->text_module && strcmp(s->text_module, d->module) == 0) {
        free(s->text_module);
        s->text_module = NULL;
    }
    free(d->module);
    free(d);
    update_focus(s);
    s->cb.windows_changed(s->cb.data);
}

static void reconfigure(struct fw_display* d) {
    struct fw_view* v;
    wl_list_for_each(v, &d->views, link) configure_view(v);
}

void fw_display_resize(struct fw_server* s, const char* module, int w, int h) {
    struct fw_display* d = find_display(s, module);
    if (!d || w <= 0 || h <= 0 || (w == d->w && h == d->h)) return;
    d->w = w;
    d->h = h;
    d->scale = scale_for(h);
    slots_reset(d);  // Facet's surface is made anew: its GPU buffers too
    wlr_scene_rect_set_size(d->background, lw(d), h / d->scale);
    if (d->output) {
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_custom_mode(&state, w, h, 60000);
        wlr_output_state_set_scale(&state, (float)d->scale);
        wlr_output_commit_state(d->output, &state);
        wlr_output_state_finish(&state);
    }
    reconfigure(d);
    repaint(d);
}

void fw_display_set_visible(struct fw_server* s, const char* module, bool visible) {
    struct fw_display* d = find_display(s, module);
    if (!d || d->visible == visible) return;
    d->visible = visible;
    if (visible) repaint(d);
    update_focus(s);
}

void fw_display_set_inset(struct fw_server* s, const char* module, int bottom) {
    struct fw_display* d = find_display(s, module);
    if (!d || bottom < 0 || bottom == d->inset || bottom >= d->h) return;
    d->inset = bottom;
    reconfigure(d);
}

void fw_set_scopes(struct fw_server* s, const char* module, const char* const* scopes, int n) {
    struct fw_scopes* sc = find_scopes(s, module);
    if (!sc) {
        sc = calloc(1, sizeof *sc);
        if (!sc) return;
        sc->module = strdup(module);
        wl_list_insert(&s->scopes, &sc->link);
    }
    for (int i = 0; i < sc->n; ++i) free(sc->scopes[i]);
    sc->n = 0;
    for (int i = 0; i < n && i < MAX_SCOPES; ++i) sc->scopes[sc->n++] = strdup(scopes[i]);
}

static struct wlr_surface* surface_at(struct fw_server* s, double lx, double ly, double* sx, double* sy) {
    struct wlr_scene_node* node = wlr_scene_node_at(&s->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
    struct wlr_scene_surface* ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    return ss ? ss->surface : NULL;
}

// A tap on a window brings it to the front and gives it the keyboard.
static void raise_window_of(struct fw_display* d, struct wlr_surface* surface) {
    struct wlr_surface* root = wlr_surface_get_root_surface(surface);
    struct wlr_xdg_surface* xs = wlr_xdg_surface_try_from_wlr_surface(root);
    while (xs && xs->role == WLR_XDG_SURFACE_ROLE_POPUP && xs->popup->parent)
        xs = wlr_xdg_surface_try_from_wlr_surface(xs->popup->parent);
    if (!xs || xs->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;
    struct fw_view* v = view_of_toplevel(xs->toplevel);
    if (!v || !v->mapped || v->display != d) return;
    if (d->views.next == &v->link) return;
    wl_list_remove(&v->link);
    wl_list_insert(&d->views, &v->link);
    wlr_scene_node_raise_to_top(&v->tree->node);
    update_focus(d->server);
}

void fw_touch(struct fw_server* s, const char* module, enum fw_touch_kind kind, double x, double y) {
    struct fw_display* d = find_display(s, module);
    if (!d) return;
    double lx = d->x + x / d->scale, ly = y / d->scale;
    uint32_t t = now_ms();
    if (kind == FW_TOUCH_DOWN) {
        double sx = 0, sy = 0;
        struct wlr_surface* surface = surface_at(s, lx, ly, &sx, &sy);
        s->touching = surface != NULL;
        if (!surface) return;
        raise_window_of(d, surface);
        s->touch_ox = lx - sx;
        s->touch_oy = ly - sy;
        // Apps without touch support get a mouse instead.
        s->touch_pointer = !wlr_surface_accepts_touch(s->seat, surface);
        if (s->touch_pointer) {
            wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(s->seat, t, sx, sy);
            wlr_seat_pointer_notify_button(s->seat, t, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
            wlr_seat_pointer_notify_frame(s->seat);
        } else {
            wlr_seat_touch_notify_down(s->seat, surface, t, 0, sx, sy);
            wlr_seat_touch_notify_frame(s->seat);
        }
    } else if (s->touching) {
        double sx = lx - s->touch_ox, sy = ly - s->touch_oy;
        if (s->touch_pointer) {
            wlr_seat_pointer_notify_motion(s->seat, t, sx, sy);
            if (kind == FW_TOUCH_UP)
                wlr_seat_pointer_notify_button(s->seat, t, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
            wlr_seat_pointer_notify_frame(s->seat);
        } else {
            if (kind == FW_TOUCH_UP) wlr_seat_touch_notify_up(s->seat, t, 0);
            else wlr_seat_touch_notify_motion(s->seat, t, 0, sx, sy);
            wlr_seat_touch_notify_frame(s->seat);
        }
        if (kind == FW_TOUCH_UP) s->touching = false;
    }
    flush(s);
}

// Characters typed as key presses: printable ASCII, newline and tab.
static bool is_key_char(uint32_t cp) { return cp == '\n' || cp == '\t' || (cp >= 0x20 && cp < 0x7f); }

void fw_text(struct fw_server* s, const char* module, const char* action, const char* text) {
    struct fw_display* d = find_display(s, module);
    if (!d || !d->visible || !s->seat->keyboard_state.focused_surface) return;
    if (strcmp(action, "insert") == 0 && text) {
        // ASCII goes as key presses: every field understands them, including
        // ones that only listen to keys (e.g. one box per digit of a code).
        // Other scripts go as whole strings through text-input when the
        // field speaks it, else as keys from the growing keymap.
        struct fw_text_input* ti = focused_input(s);
        bool commit = ti && ti->input->current_enabled;
        const char* p = text;
        while (*p) {
            const char* run = p;
            uint32_t cp = next_utf8(&p);
            if (!is_key_char(cp) && commit) {
                const char* end = p;
                while (*end) {  // the whole run of other characters in one string
                    const char* q = end;
                    if (is_key_char(next_utf8(&q))) break;
                    end = q;
                }
                char* chunk = strndup(run, (size_t)(end - run));
                if (chunk) {
                    wlr_text_input_v3_send_commit_string(ti->input, chunk);
                    wlr_text_input_v3_send_done(ti->input);
                    free(chunk);
                }
                p = end;
                continue;
            }
            uint32_t sym = cp == '\n' ? XKB_KEY_Return : cp == '\t' ? XKB_KEY_Tab : xkb_utf32_to_keysym(cp);
            if (sym) tap(s, sym);
        }
    } else if (strcmp(action, "backspace") == 0) {
        tap(s, XKB_KEY_BackSpace);
    } else if (strcmp(action, "enter") == 0) {
        tap(s, XKB_KEY_Return);
    }
    flush(s);
}

int fw_windows(struct fw_server* s, struct fw_window* out, int max) {
    int n = 0;
    struct wlr_surface* focus = s->seat->keyboard_state.focused_surface;
    struct fw_display* d;
    wl_list_for_each(d, &s->displays, link) {
        struct fw_view* v;
        wl_list_for_each(v, &d->views, link) {
            if (n >= max) return n;
            out[n].module = d->module;
            out[n].app_id = v->toplevel->app_id ? v->toplevel->app_id : "";
            out[n].title = v->toplevel->title ? v->toplevel->title : "";
            out[n].focused = v->toplevel->base->surface == focus;
            ++n;
        }
    }
    return n;
}

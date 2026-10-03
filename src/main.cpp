// wayland: provides display.wayland for Facet modules that run desktop apps
// (a browser, messengers). It runs its own Wayland compositor without a
// screen of its own: each client module gets a display whose picture Facet
// shows on that module's screen, through a surface lent to it. Facet stays the
// owner of the real screen and of input; this module only plays along through
// the generic plugin SDK.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "compositor.h"
#include "facet/plugin.h"
#include "i18n/i18n.h"

using facet::Json;
using facet::sdk::Consumer;
using facet::sdk::Plugin;
using facet::sdk::Screen;
using facet::sdk::Surface;

namespace {

#ifndef WAYLAND_VERSION
#define WAYLAND_VERSION "dev"  // set by CMake
#endif

constexpr const char* kCapability = "display.wayland";

class Server {
public:
    explicit Server(Plugin& plugin) : plugin_(plugin) {}
    ~Server() { stop(); }

    void start() {
        if (srv_) return;
        if (!plugin_.has_permission("display.surface") || !plugin_.has_permission("wayland.compositor")) {
            error_ = tr("Missing permissions.");
            return;
        }
        fw_callbacks cb{};
        cb.data = this;
        cb.frame_target = [](void* self, const char* module, int w, int h) {
            return static_cast<Server*>(self)->frame_target(module, w, h);
        };
        cb.frame_done = [](void* self, const char* module) { static_cast<Server*>(self)->frame_done(module); };
        cb.text_input = [](void* self, const char* module, bool active, bool numeric) {
            static_cast<Server*>(self)->text_input(module, active, numeric);
        };
        cb.windows_changed = [](void* self) { static_cast<Server*>(self)->windows_dirty_ = true; };
        cb.gpu_attach = [](void* self, const char* module, int slot, int fd, uint32_t format, uint64_t modifier,
                           uint32_t offset, uint32_t stride, int width, int height) {
            Surface* s = static_cast<Server*>(self)->surface_of(module);
            return s && s->attach_buffer(slot, fd, format, modifier, offset, stride, width, height);
        };
        cb.gpu_detach = [](void* self, const char* module, int slot) {
            if (Surface* s = static_cast<Server*>(self)->surface_of(module)) s->detach_buffer(slot);
        };
        cb.gpu_ready = [](void* self, const char* module) {
            Surface* s = static_cast<Server*>(self)->surface_of(module);
            return s && s->ready();
        };
        cb.gpu_present = [](void* self, const char* module, int slot) {
            if (Surface* s = static_cast<Server*>(self)->surface_of(module)) s->present_buffer(slot);
        };
        cb.gpu_shown = [](void* self, const char* module) {
            Surface* s = static_cast<Server*>(self)->surface_of(module);
            return s ? s->shown_buffer() : -1;
        };
        char err[256] = {};
        srv_ = fw_server_create(&cb, err, sizeof err);
        if (!srv_) {
            error_ = err;
            Plugin::log("wayland: %s", err);
            return;
        }
        plugin_.watch_fd(fw_server_fd(srv_), [this] { fw_server_dispatch(srv_); });
        // Facet draws with the GPU: apps may too, and their windows reach it without copies.
        if (plugin_.gpu_buffers() && plugin_.gpu_device_id()) {
            if (fw_server_enable_gpu(srv_, plugin_.gpu_device_id())) Plugin::log("wayland: apps may draw with the GPU");
            else Plugin::log("wayland: GPU buffers are unavailable; apps draw in software");
        }
        Plugin::log("wayland: compositor ready");
    }

    void stop() {
        if (!srv_) return;
        plugin_.unwatch_fd(fw_server_fd(srv_));
        fw_server_destroy(srv_);
        srv_ = nullptr;
        displays_.clear();
    }

    // A module that requires display.wayland started, stopped, or opened or closed its screen.
    void consumer(const Consumer& c) {
        if (c.capability != kCapability || !srv_) return;
        if (!c.running) {
            remove(c.module);
        } else {
            if (!displays_.count(c.module)) add(c.module, c.dir);
            if (displays_.count(c.module)) fw_display_set_visible(srv_, c.module.c_str(), c.visible);
        }
        refresh();
    }

    void scopes(const std::string& module, const std::vector<std::string>& scopes) {
        if (!srv_) return;
        std::vector<const char*> v;
        for (const auto& s : scopes) v.push_back(s.c_str());
        fw_set_scopes(srv_, module.c_str(), v.data(), int(v.size()));
    }

    void touch(const std::string& surface, const std::string& kind, float x, float y) {
        const std::string* module = module_of_surface(surface);
        if (!module || !srv_) return;
        fw_touch_kind k = kind == "down" ? FW_TOUCH_DOWN : kind == "up" ? FW_TOUCH_UP : FW_TOUCH_MOVE;
        fw_touch(srv_, module->c_str(), k, x, y);
    }

    // Facet's keyboard types into the window on screen.
    void text(const std::string& action, const std::string& text) {
        if (!srv_ || text_module_.empty()) return;
        if (action == "hide") {
            plugin_.text_input(false);
            return;
        }
        fw_text(srv_, text_module_.c_str(), action.c_str(), text.c_str());
    }

    void insets(int bottom) {
        if (srv_ && !text_module_.empty()) fw_display_set_inset(srv_, text_module_.c_str(), bottom);
    }

    void tick() {
        if (!srv_) return;
        // Displays follow the screen size (rotation, another panel).
        int w = screen_w(), h = screen_h();
        for (auto& [module, d] : displays_) {
            if (d.surface->width() == w && d.surface->height() == h) continue;
            d.surface->create(plugin_, d.surface_id, w, h, module);
            fw_display_resize(srv_, module.c_str(), w, h);
        }
        fw_server_kick(srv_);
        if (windows_dirty_) {
            windows_dirty_ = false;
            report_windows();
            refresh();
        }
    }

    void refresh() {
        if (displays_.empty()) plugin_.end_background();
        else plugin_.begin_background(tr("Showing the windows of {} apps", {std::to_string(displays_.size())}));
        if (plugin_.visible()) plugin_.set_ui(build());
    }

private:
    struct Display {
        std::unique_ptr<Surface> surface;
        std::string surface_id;
    };

    std::string tr(std::string_view k) const { return plugin_.tr(k); }
    std::string tr(std::string_view k, const std::vector<std::string>& a) const { return plugin_.tr(k, a); }
    int screen_w() const { return plugin_.screen_width() > 0 ? plugin_.screen_width() : 1280; }
    int screen_h() const { return plugin_.screen_height() > 0 ? plugin_.screen_height() : 800; }

    void add(const std::string& module, const std::string& dir) {
        if (dir.empty()) {
            Plugin::log("wayland: %s has no endpoint directory (Facet older than 0.6?)", module.c_str());
            return;
        }
        Display d;
        d.surface = std::make_unique<Surface>();
        d.surface_id = "wl" + std::to_string(++next_id_);
        if (!d.surface->create(plugin_, d.surface_id, screen_w(), screen_h(), module)) {
            Plugin::log("wayland: no surface for %s", module.c_str());
            return;
        }
        char err[256] = {};
        if (!fw_display_add(srv_, module.c_str(), dir.c_str(), screen_w(), screen_h(), err, sizeof err)) {
            Plugin::log("wayland: %s: %s", module.c_str(), err);
            return;
        }
        Plugin::log("wayland: display for %s (%dx%d)", module.c_str(), screen_w(), screen_h());
        displays_[module] = std::move(d);
    }

    void remove(const std::string& module) {
        fw_display_remove(srv_, module.c_str());
        displays_.erase(module);
        if (text_module_ == module) {
            text_module_.clear();
            plugin_.text_input(false);
        }
    }

    const std::string* module_of_surface(const std::string& surface) const {
        for (const auto& [module, d] : displays_)
            if (d.surface_id == surface) return &module;
        return nullptr;
    }

    Surface* surface_of(const char* module) {
        auto it = displays_.find(module);
        return it == displays_.end() ? nullptr : it->second.surface.get();
    }

    uint32_t* frame_target(const char* module, int w, int h) {
        auto it = displays_.find(module);
        if (it == displays_.end()) return nullptr;
        Surface& s = *it->second.surface;
        if (!s.ready() || s.width() != w || s.height() != h) return nullptr;
        return s.pixels();
    }

    void frame_done(const char* module) {
        auto it = displays_.find(module);
        if (it != displays_.end()) it->second.surface->present();
    }

    void text_input(const char* module, bool active, bool numeric) {
        text_module_ = active ? module : "";
        plugin_.text_input(active, numeric ? "number" : "text");
    }

    void report_windows() {
        fw_window list[64];
        int n = fw_windows(srv_, list, 64);
        windows_.clear();
        for (int i = 0; i < n; ++i)
            windows_.push_back({list[i].module, list[i].app_id, list[i].title, 0, list[i].focused});
        plugin_.report_wayland_clients(windows_);
    }

    Screen build() const {
        Screen ui(tr("Wayland"));
        ui.section(tr("Display server"));
        if (!srv_) {
            ui.info(tr("State"), tr("not running"), "bad");
            if (!error_.empty()) ui.note(error_);
        } else {
            ui.info(tr("State"), tr("running"), "good");
            ui.info(tr("Screen size"), std::to_string(screen_w()) + " × " + std::to_string(screen_h()));
        }
        ui.section(tr("Apps"));
        if (displays_.empty())
            ui.note(tr("No app uses Wayland yet. Modules that need it (a browser, messengers) show their "
                       "windows on their own screen."));
        for (const auto& [module, d] : displays_) {
            int n = 0;
            for (const auto& w : windows_)
                if (w.module == module) ++n;
            ui.info(module, tr("{} windows", {std::to_string(n)}));
            for (const auto& w : windows_)
                if (w.module == module)
                    ui.info("  " + (w.title.empty() ? w.app_id : w.title), w.title.empty() || w.title == w.app_id ? "" : w.app_id);
        }
        ui.section(tr("Privacy"));
        ui.note(tr("Each app sees only its own windows. The clipboard and reading the screen need their own "
                   "permissions in Settings > Apps."));
        return ui;
    }

    Plugin& plugin_;
    fw_server* srv_ = nullptr;
    std::string error_;
    std::map<std::string, Display> displays_;
    std::vector<facet::sdk::WaylandClient> windows_;
    std::string text_module_;
    bool windows_dirty_ = false;
    int next_id_ = 0;
};

}  // namespace

int main() {
    Plugin plugin("wayland", WAYLAND_VERSION);  // keep in sync with manifest.json
    wayland::register_translations(plugin.catalog());
    Server server(plugin);
    plugin.on_hello = [&](const Json&) {
        server.start();
        server.refresh();
    };
    plugin.on_consumer = [&](const Consumer& c) { server.consumer(c); };
    plugin.on_wayland_client = [&](const std::string& module, const std::vector<std::string>& scopes, bool running) {
        if (running) server.scopes(module, scopes);
    };
    plugin.on_touch = [&](const std::string& s, const std::string& k, float x, float y) { server.touch(s, k, x, y); };
    plugin.on_text = [&](const std::string& a, const std::string& t) { server.text(a, t); };
    plugin.on_insets = [&](int bottom) { server.insets(bottom); };
    plugin.on_visible = [&](bool) { server.refresh(); };
    plugin.on_locale = [&](const std::string&) { server.refresh(); };
    plugin.on_tick = [&] { server.tick(); };
    plugin.on_shutdown = [&] { server.stop(); };
    return plugin.run(16);
}

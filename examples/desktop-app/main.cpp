// Example client module: runs one desktop (Wayland) app in its own container
// and shows its windows on its own screen. A template for app modules such as
// a browser or a messenger: require display.wayland in the manifest, start the
// app with WAYLAND_DISPLAY pointing at the endpoint the core shares with the
// compositor module, and show the surface the compositor lends.
//
// The command comes from the file "command" next to the executable.
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

#include "facet/plugin.h"
#include "i18n/i18n.h"

using facet::Json;
using facet::sdk::LentSurface;
using facet::sdk::Plugin;
using facet::sdk::Screen;

namespace {

std::string read_command() {
    char exe[4096] = {};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string dir = n > 0 ? std::string(exe, size_t(n)) : std::string();
    dir = dir.substr(0, dir.rfind('/'));
    std::ifstream f(dir + "/command");
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

class App {
public:
    explicit App(Plugin& plugin) : plugin_(plugin), command_(read_command()) {}

    void tick() {
        if (pid_ > 0) {
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                state_ = tr("The app has closed.");
                refresh();
            }
        } else if (want_start_) {
            start();
        }
    }

    void start() {
        std::string dir = plugin_.endpoint("display.wayland");
        struct stat st;
        if (dir.empty() || stat((dir + "/wayland-0").c_str(), &st) != 0) {
            state_ = tr("Waiting for the Wayland module…");
            want_start_ = true;  // the compositor creates the socket shortly
            refresh();
            return;
        }
        want_start_ = false;
        if (command_.empty()) {
            state_ = tr("No command configured.");
            refresh();
            return;
        }
        pid_ = fork();
        if (pid_ == 0) {
            setenv("WAYLAND_DISPLAY", (dir + "/wayland-0").c_str(), 1);
            setenv("XDG_RUNTIME_DIR", "/tmp", 1);
            setenv("GDK_BACKEND", "wayland", 1);
            setenv("QT_QPA_PLATFORM", "wayland", 1);
            setenv("MOZ_ENABLE_WAYLAND", "1", 1);
            int null = open("/dev/null", 0);
            if (null >= 0) dup2(null, 0);  // stdin/stdout are the protocol pipes
            dup2(2, 1);
            execl("/bin/sh", "sh", "-c", command_.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        state_ = pid_ > 0 ? tr("Starting…") : tr("Could not start the app.");
        refresh();
    }

    void on_event(const std::string& id) {
        if (id == "start" && pid_ <= 0) start();
    }

    void on_lent(const std::string& id, bool available) {
        if (available) surface_ = id;
        else if (surface_ == id) surface_.clear();
        refresh();
    }

    void refresh() {
        plugin_.set_tile(pid_ > 0 ? tr("running") : tr("stopped"));
        if (!plugin_.visible()) return;
        Screen ui(tr("Desktop app"));
        if (!surface_.empty()) ui.fullscreen(surface_);  // shown once the first frame is there
        ui.info(tr("Command"), command_.empty() ? "—" : command_);
        ui.info(tr("State"), state_);
        if (pid_ <= 0) ui.button("start", tr("Start"), "primary");
        plugin_.set_ui(ui);
    }

private:
    std::string tr(std::string_view k) const { return plugin_.tr(k); }

    Plugin& plugin_;
    std::string command_;
    std::string surface_;
    std::string state_;
    pid_t pid_ = -1;
    bool want_start_ = false;
};

}  // namespace

int main() {
    Plugin plugin("desktop-app", "0.1.0");  // keep in sync with manifest.json
    desktop_app::register_translations(plugin.catalog());
    App app(plugin);
    plugin.on_hello = [&](const Json&) { app.start(); };
    plugin.on_event = [&](const std::string& id, const Json&) { app.on_event(id); };
    plugin.on_surface_lent = [&](const std::string& id, const LentSurface&, bool available) {
        app.on_lent(id, available);
    };
    plugin.on_visible = [&](bool) { app.refresh(); };
    plugin.on_locale = [&](const std::string&) { app.refresh(); };
    plugin.on_tick = [&] { app.tick(); };
    return plugin.run(200);
}

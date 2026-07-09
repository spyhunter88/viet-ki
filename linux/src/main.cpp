// VietKi Linux shell entry point (Phase 4.1) — an IBus engine process.
//
// This replaces the earlier X11 hook+inject prototype, which cannot work under
// Wayland (a process may not intercept or synthesize another process's keys).
// The IBus composition model works on both X11 and Wayland (PHASE4.1 §1, §6).
//
// Startup (PHASE4.1 §3): connect to the IBus daemon, publish an IBusFactory
// that builds our engine on demand, claim the well-known bus name, and run the
// IBus main loop. Invoked by the daemon as `ibus-engine-vietki --ibus`.
#include <ibus.h>

#include <cstring>

#include "engine_ibus.h"

namespace {

IBusBus* g_bus = nullptr;

const char* kBusName    = "org.freedesktop.IBus.VietKi";
const char* kEngineName = "vietki";

void onDisconnected(IBusBus*, gpointer) { ibus_quit(); }

// Describe the engine to IBus when we run standalone (not launched by the
// daemon via the installed component XML). Lets `ibus-engine-vietki` be started
// by hand for testing against a running ibus-daemon.
IBusComponent* buildComponent() {
    IBusComponent* component = ibus_component_new(
        kBusName,
        "VietKi",
        "0.6",
        "GPL",
        "VietKi",
        "https://github.com/",
        "",
        "ibus-vietki");
    ibus_component_add_engine(
        component,
        ibus_engine_desc_new(kEngineName,
                             "VietKi (Telex/VNI)",
                             "Bộ gõ tiếng Việt",
                             "vi",
                             "GPL",
                             "VietKi",
                             "/usr/share/ibus-vietki/icon.png",
                             "us"));
    return component;
}

} // namespace

int main(int argc, char** argv) {
    bool byIBus = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--ibus") == 0 || std::strcmp(argv[i], "-i") == 0)
            byIBus = true;

    ibus_init();

    g_bus = ibus_bus_new();
    if (!ibus_bus_is_connected(g_bus)) {
        g_printerr("VietKi: cannot connect to the IBus daemon. Is ibus running?\n");
        return 1;
    }
    g_signal_connect(g_bus, "disconnected", G_CALLBACK(onDisconnected), nullptr);

    IBusFactory* factory = ibus_factory_new(ibus_bus_get_connection(g_bus));
    ibus_factory_add_engine(factory, kEngineName, IBUS_TYPE_VIETKI_ENGINE);

    if (byIBus) {
        // Launched by the daemon: just own the name declared in the component XML.
        ibus_bus_request_name(g_bus, kBusName, 0);
    } else {
        // Standalone/dev: register the component so the daemon learns about us.
        ibus_bus_register_component(g_bus, buildComponent());
        ibus_bus_request_name(g_bus, kBusName, 0);
        g_print("VietKi IBus engine registered. Select \"VietKi\" in your input "
                "sources (or run: ibus engine vietki).\n");
    }

    ibus_main();
    return 0;
}

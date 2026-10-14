// VSTGUI 4.15 can leave cached Cairo references alive after its last X11
// editor closes. Finish that device BEFORE disconnecting XCB, otherwise a
// reused connection address can retrieve Cairo's stale screen and abort.
// Linker wrapping affects this plugin's static VSTGUI calls, not the host.
// Keep the device for the connection lifetime: finishing on each editor's
// destruction would invalidate other editors sharing the same connection.
#include <cairo/cairo-xcb.h>
#include <mutex>
#include <unordered_map>

namespace {
struct Devices {
    std::mutex mutex;
    std::unordered_map<xcb_connection_t*, cairo_device_t*> connections;
};

Devices& devices() {
    static Devices registry;
    return registry;
}
} // namespace

extern "C" {
cairo_surface_t* __real_cairo_xcb_surface_create(xcb_connection_t*, xcb_drawable_t,
                                               xcb_visualtype_t*, int, int);
void __real_xcb_disconnect(xcb_connection_t*);

__attribute__((visibility("hidden")))
cairo_surface_t* __wrap_cairo_xcb_surface_create(xcb_connection_t* connection,
        xcb_drawable_t drawable, xcb_visualtype_t* visual, int width, int height) {
    auto* surface = __real_cairo_xcb_surface_create(connection, drawable, visual, width, height);
    if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
        if (auto* device = cairo_surface_get_device(surface)) {
            auto& registry = devices();
            std::lock_guard<std::mutex> lock(registry.mutex);
            if (!registry.connections.contains(connection))
                registry.connections.emplace(connection, cairo_device_reference(device));
        }
    }
    return surface;
}

__attribute__((visibility("hidden")))
void __wrap_xcb_disconnect(xcb_connection_t* connection) {
    cairo_device_t* device = nullptr;
    auto& registry = devices();
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        auto entry = registry.connections.find(connection);
        if (entry != registry.connections.end()) {
            device = entry->second;
            registry.connections.erase(entry);
        }
    }
    if (device) {
        cairo_device_finish(device);
        cairo_device_destroy(device);
    }
    __real_xcb_disconnect(connection);
}

// VSTGUI calls this after dispatching X11 events and after cursor changes.
// A synchronous reply can consume newly arrived expose events into XCB's
// internal queue, leaving the host's polled descriptor unreadable. Flush
// ordered requests without reading replies so the host receives that wakeup.
__attribute__((visibility("hidden")))
void __wrap_xcb_aux_sync(xcb_connection_t* connection) {
    xcb_flush(connection);
}
}

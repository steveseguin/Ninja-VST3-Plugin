#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <pluginterfaces/gui/iplugview.h>
#include <xcb/xcb.h>
#include <poll.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Steinberg;
using Clock = std::chrono::steady_clock;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// Supply the real VST3 Linux host interfaces and dispatch both timers and XCB
// descriptors. Merely attaching a view without pumping does not test drawing.
class Host final : public Vst::HostApplication, public IPlugFrame, public Linux::IRunLoop {
    struct Event { IPtr<Linux::IEventHandler> handler; int fd; };
    struct Timer { IPtr<Linux::ITimerHandler> handler; Clock::time_point next; uint64 interval; };
    std::map<Linux::IEventHandler*, Event> events;
    std::map<Linux::ITimerHandler*, Timer> timers;
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** result) override {
        if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid)) *result = static_cast<IPlugFrame*>(this);
        else if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid)) *result = static_cast<Linux::IRunLoop*>(this);
        else return HostApplication::queryInterface(iid, result);
        addRef(); return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return HostApplication::addRef(); }
    uint32 PLUGIN_API release() override { return HostApplication::release(); }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* rect) override { return view->onSize(rect); }
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* h, int fd) override {
        events.insert_or_assign(h, Event{IPtr<Linux::IEventHandler>(h), fd}); return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* h) override {
        events.erase(h); return kResultOk;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* h, uint64 interval) override {
        timers.insert_or_assign(h, Timer{IPtr<Linux::ITimerHandler>(h), Clock::now(), std::max<uint64>(1, interval)});
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* h) override {
        timers.erase(h); return kResultOk;
    }
    void describe() { std::cerr << "Host handlers=" << events.size() << " timers=" << timers.size() << '\n'; }
    void pump(int milliseconds) {
        const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
        do {
            const auto eventSnapshot = events;
            for (const auto& [key, event] : eventSnapshot) {
                pollfd fd{event.fd, POLLIN, 0};
                if (events.contains(key) && poll(&fd, 1, 0) > 0 && (fd.revents & POLLIN))
                    event.handler->onFDIsSet(event.fd);
            }
            const auto timerSnapshot = timers;
            for (const auto& [key, timer] : timerSnapshot) {
                auto it = timers.find(key);
                if (it != timers.end() && Clock::now() >= it->second.next) {
                    it->second.next = Clock::now() + std::chrono::milliseconds(timer.interval);
                    timer.handler->onTimer();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (Clock::now() < end);
    }
};

class Editor {
    Host& host;
    xcb_connection_t* xcb;
    xcb_window_t window;
    IPtr<IPlugView> view;
public:
    Editor(Host& host, xcb_connection_t* xcb, xcb_screen_t* screen, Vst::IEditController* controller)
        : host(host), xcb(xcb), window(xcb_generate_id(xcb)),
          view(controller->createView(Vst::ViewType::kEditor), false) {
        require(view && view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultTrue,
                "X11 editor unavailable");
        ViewRect rect;
        require(view->getSize(&rect) == kResultOk && rect.getWidth() >= 700 && rect.getHeight() >= 300,
                "Invalid editor dimensions");
        const uint32_t values[] = {screen->black_pixel, 1, XCB_EVENT_MASK_EXPOSURE};
        xcb_create_window(xcb, XCB_COPY_FROM_PARENT, window, screen->root, 10, 10,
            rect.getWidth(), rect.getHeight(), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
        xcb_map_window(xcb, window); xcb_flush(xcb);
        view->setFrame(&host);
        require(view->attached(reinterpret_cast<void*>(static_cast<uintptr_t>(window)),
                kPlatformTypeX11EmbedWindowID) == kResultOk, "Cannot attach X11 editor");
        view->onSize(&rect);
        host.pump(80);
    }
    ~Editor() {
        view->removed();
        view->setFrame(nullptr);
        view = nullptr;
        xcb_destroy_window(xcb, window); xcb_flush(xcb);
    }
    void checkPaint() {
        const uint32_t above = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(xcb, window, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        auto* tree = xcb_query_tree_reply(xcb, xcb_query_tree(xcb, window), nullptr);
        require(tree && xcb_query_tree_children_length(tree) > 0, "No native editor child");
        xcb_window_t child = 0;
        int width = 0, height = 0;
        for (int i = 0; i < xcb_query_tree_children_length(tree); ++i) {
            const auto id = xcb_query_tree_children(tree)[i];
            auto* geometry = xcb_get_geometry_reply(xcb, xcb_get_geometry(xcb, id), nullptr);
            if (geometry && geometry->width * geometry->height > width * height) {
                child = id; width = geometry->width; height = geometry->height;
            }
            free(geometry);
        }
        free(tree);
        require(child && width >= 600 && height >= 200, "Invalid native editor child size");
        // The embedding host maps the requested child and announces itself.
        auto* atom = xcb_intern_atom_reply(xcb, xcb_intern_atom(xcb, 0, 7, "_XEMBED"), nullptr);
        require(atom != nullptr, "Cannot obtain XEMBED atom");
        xcb_client_message_event_t embedded{};
        embedded.response_type = XCB_CLIENT_MESSAGE; embedded.format = 32;
        embedded.window = child; embedded.type = atom->atom;
        embedded.data.data32[0] = XCB_CURRENT_TIME;
        embedded.data.data32[1] = 0; // XEMBED_EMBEDDED_NOTIFY
        embedded.data.data32[3] = window;
        free(atom);
        xcb_send_event(xcb, 0, child, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&embedded));
        xcb_map_window(xcb, child);
        // Erase the child and force a repaint, catching a device accidentally
        // finished while a different editor still needs it.
        const uint32_t black = 0;
        xcb_change_window_attributes(xcb, child, XCB_CW_BACK_PIXEL, &black);
        xcb_clear_area(xcb, 1, child, 0, 0, 0, 0); xcb_flush(xcb);
        host.pump(80);
        xcb_generic_error_t* imageError = nullptr;
        auto* image = xcb_get_image_reply(xcb, xcb_get_image(xcb, XCB_IMAGE_FORMAT_Z_PIXMAP,
            child, 0, 0, std::min(width, 700), std::min(height, 300), ~0U), &imageError);
        if (imageError) {
            auto* attrs = xcb_get_window_attributes_reply(xcb, xcb_get_window_attributes(xcb, child), nullptr);
            std::cerr << "Image error " << int(imageError->error_code) << " child=" << child
                      << " size=" << width << 'x' << height << " map=" << (attrs ? int(attrs->map_state) : -1) << '\n';
            free(attrs); free(imageError);
        }
        require(image != nullptr, "Cannot read editor pixels");
        const auto* pixels = reinterpret_cast<const uint32_t*>(xcb_get_image_data(image));
        std::set<uint32_t> colors;
        for (int i = 0; i < xcb_get_image_data_length(image) / 4; ++i) colors.insert(pixels[i]);
        free(image);
        if (colors.size() <= 8) { std::cerr << "Redraw color count=" << colors.size() << " child=" << child << '\n'; host.describe(); }
        require(colors.size() > 8, "Editor failed to redraw its controls");
    }
};

int main(int argc, char** argv) {
    setenv("WEBRTC_VST_HANDSHAKE_URL", "ws://127.0.0.1:1", 1);
    auto host = owned(new Host);
    // Audacity exposes IRunLoop on IPlugFrame, not the factory/host context.
    // The first editor must initialize correctly in that valid arrangement.
    auto context = owned(new Vst::HostApplication);
    Vst::HostApplication* hostContext = argc > 2 ? host.get() : context.get();
    Vst::PluginContextFactory::instance().setPluginContext(hostContext);
    int screenIndex = 0;
    auto* xcb = xcb_connect(nullptr, &screenIndex);
    require(!xcb_connection_has_error(xcb), "An X11 display is required (use xvfb-run)");
    auto screens = xcb_setup_roots_iterator(xcb_get_setup(xcb));
    for (int i = 0; i < screenIndex; ++i) xcb_screen_next(&screens);
    try {
        std::string error;
        auto module = VST3::Hosting::Module::create(argc > 1 ? argv[1] : WEBRTC_VST_DEFAULT_PLUGIN_PATH, error);
        require(module != nullptr, error.c_str());
        const auto& factory = module->getFactory();
        factory.setHostContext(hostContext);
        std::vector<std::unique_ptr<Vst::PlugProvider>> providers;
        for (int i = 0; i < 4; ++i) {
            auto provider = std::make_unique<Vst::PlugProvider>(factory, factory.classInfos().front(), true);
            require(provider->initialize() && provider->getControllerPtr(), "Cannot initialize plugin");
            providers.push_back(std::move(provider));
        }
        auto open = [&](int index = 0) {
            auto editor = std::make_unique<Editor>(*host, xcb, screens.data, providers[index]->getControllerPtr());
            editor->checkPaint(); return editor;
        };
        for (int cycle = 0; cycle < 32; ++cycle) {
            auto editor = open();
            std::cout << "Reopen " << cycle + 1 << " painted\n" << std::flush;
        }
        for (int cycle = 0; cycle < 8; ++cycle) {
            std::cout << "Overlap " << cycle + 1 << " open A/B/C\n" << std::flush;
            auto a = open(0); auto b = open(1); auto c = open(2);
            std::cout << "Close A, open D\n" << std::flush;
            a.reset(); std::cout << "Redraw C after A\n" << std::flush; c->checkPaint();
            auto d = open(3);
            std::cout << "Redraw C after D\n" << std::flush; c->checkPaint();
            std::cout << "Close B, redraw C/D\n" << std::flush;
            b.reset(); c->checkPaint(); d->checkPaint();
            c.reset(); d->checkPaint(); d.reset();
        }
        std::cout << "PASS: 64 editor opens and overlapping-editor redraws\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
    Vst::PluginContextFactory::instance().setPluginContext(nullptr);
    xcb_disconnect(xcb);
    return 0;
}

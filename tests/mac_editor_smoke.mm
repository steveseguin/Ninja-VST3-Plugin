#import <Cocoa/Cocoa.h>

#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <iostream>

// Exercise the actual Cocoa editor and bundled uidesc; headless audio tests cannot
// detect missing resources or an unsupported native parent view.
int main(int argc, char** argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
        static Steinberg::Vst::HostApplication host;
        Steinberg::Vst::PluginContextFactory::instance().setPluginContext(&host);
        std::string error;
        auto module = VST3::Hosting::Module::create(
            argc > 1 ? argv[1] : WEBRTC_VST_DEFAULT_PLUGIN_PATH, error);
        if (!module) {
            std::cerr << error << '\n';
            return 1;
        }
        const auto& factory = module->getFactory();
        auto classes = factory.classInfos();
        if (classes.empty()) return 1;
        Steinberg::Vst::PlugProvider provider(factory, classes.front(), true);
        if (!provider.initialize()) return 1;
        auto controller = provider.getControllerPtr();
        if (!controller) return 1;

        for (int cycle = 0; cycle < 5; ++cycle) {
            Steinberg::IPtr<Steinberg::IPlugView> view(
                controller->createView(Steinberg::Vst::ViewType::kEditor), false);
            if (!view || view->isPlatformTypeSupported(Steinberg::kPlatformTypeNSView) != Steinberg::kResultTrue) {
                std::cerr << "Cocoa editor is unavailable\n";
                return 1;
            }
            Steinberg::ViewRect rect;
            if (view->getSize(&rect) != Steinberg::kResultOk || rect.getWidth() <= 0 || rect.getHeight() <= 0) return 1;
            NSWindow* window = [[NSWindow alloc]
                initWithContentRect:NSMakeRect(0, 0, rect.getWidth(), rect.getHeight())
                          styleMask:NSWindowStyleMaskTitled
                            backing:NSBackingStoreBuffered defer:NO];
            [window setReleasedWhenClosed:NO];
            if (view->attached((__bridge void*)[window contentView], Steinberg::kPlatformTypeNSView) != Steinberg::kResultOk) {
                std::cerr << "Failed to attach editor (check bundled resources)\n";
                return 1;
            }
            [window orderBack:nil];
            for (int tick = 0; tick < 10; ++tick) {
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.03]];
            }
            if (view->removed() != Steinberg::kResultOk) return 1;
            [window close];
        }
        std::cout << "PASS: Cocoa editor attached, rendered and closed five times\n";
    }
    return 0;
}

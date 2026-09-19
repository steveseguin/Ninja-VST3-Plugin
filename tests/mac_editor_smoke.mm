#import <Cocoa/Cocoa.h>
#import <Vision/Vision.h>
#import <objc/runtime.h>

#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include "../webrtc_vst/src/ParameterIDs.h"

#include <iostream>
#include <regex>
#include <vector>

static NSURL* requestedQrUrl = nil;
static unsigned qrOpenRequests = 0;
static BOOL captureQrOpen(id, SEL, NSURL* url) { requestedQrUrl = url; ++qrOpenRequests; return YES; }
static NSString* decodeQrPage(NSURL* url) {
    NSString* html = [NSString stringWithContentsOfURL:url encoding:NSUTF8StringEncoding error:nil];
    if (!html) return nil;
    std::string text = [html UTF8String];
    std::smatch dimensions;
    if (!std::regex_search(text, dimensions, std::regex("viewBox='0 0 ([0-9]+) ([0-9]+)'"))) return nil;
    const int side = std::stoi(dimensions[1]), scale = 6, pixels = side * scale;
    std::vector<uint8_t> raster(static_cast<size_t>(pixels) * pixels, 255);
    const std::regex module("M([0-9]+),([0-9]+)h1v1h-1z");
    for (auto match = std::sregex_iterator(text.begin(), text.end(), module); match != std::sregex_iterator(); ++match) {
        const int x = std::stoi((*match)[1]), y = std::stoi((*match)[2]);
        if (x >= side || y >= side) return nil;
        for (int row = 0; row < scale; ++row) for (int col = 0; col < scale; ++col)
            raster[(y * scale + row) * pixels + x * scale + col] = 0;
    }
    CGColorSpaceRef color = CGColorSpaceCreateDeviceGray();
    CGContextRef context = CGBitmapContextCreate(raster.data(), pixels, pixels, 8, pixels, color, kCGImageAlphaNone);
    CGImageRef image = context ? CGBitmapContextCreateImage(context) : nullptr;
    VNDetectBarcodesRequest* request = [[VNDetectBarcodesRequest alloc] init];
    request.symbologies = @[VNBarcodeSymbologyQR];
    VNImageRequestHandler* handler = image ? [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}] : nil;
    NSString* decoded = [handler performRequests:@[request] error:nil] ? request.results.firstObject.payloadStringValue : nil;
    if (image) CGImageRelease(image);
    if (context) CGContextRelease(context);
    CGColorSpaceRelease(color);
    return decoded;
}

static NSView* nativeEditor(NSView* view) {
    if ([view respondsToSelector:NSSelectorFromString(@"onMouseDown:")]) return view;
    for (NSView* child in [view subviews]) {
        if (NSView* found = nativeEditor(child)) return found;
    }
    return nil;
}

static void clickEditor(NSWindow* window, NSPoint point) {
    NSView* target = nativeEditor([window contentView]);
    for (const auto type : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp}) {
        NSEvent* event = [NSEvent mouseEventWithType:type location:point modifierFlags:0
            timestamp:0 windowNumber:[window windowNumber] context:nil eventNumber:1 clickCount:1 pressure:1];
        [window sendEvent:event];
    }
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
}

static bool typeField(NSWindow* window, NSPoint point, NSString* value) {
    clickEditor(window, point);
    id responder = [window firstResponder];
    if (![responder respondsToSelector:@selector(insertText:replacementRange:)]) return false;
    NSString* old = [[[responder attributedSubstringForProposedRange:NSMakeRange(0, 127) actualRange:nil] string] copy];
    [responder insertText:value replacementRange:NSMakeRange(0, [old length])];
    [responder didChangeText];
    // Deliver Return through Cocoa so the field editor synchronizes its owning
    // NSTextField before VSTGUI reads it (direct mouse dispatch skips that step).
    NSEvent* enter = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
        timestamp:0 windowNumber:[window windowNumber] context:nil characters:@"\r"
        charactersIgnoringModifiers:@"\r" isARepeat:NO keyCode:36];
    [window sendEvent:enter];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    return true;
}

// Exercise the actual Cocoa editor and bundled uidesc; headless audio tests cannot
// detect missing resources or an unsupported native parent view.
int main(int argc, char** argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
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
            view->onSize(&rect);
            [window makeKeyAndOrderFront:nil];
            [window makeFirstResponder:nativeEditor([window contentView])];
            view->onFocus(true); // Hosts must activate the VSTGUI frame for text commits.
            [[NSNotificationCenter defaultCenter] postNotificationName:NSWindowDidBecomeKeyNotification object:window];
            for (int tick = 0; tick < 10; ++tick) {
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.03]];
            }
            for (int toggle = 0; toggle < 4; ++toggle) {
                clickEditor(window, NSMakePoint(650, 310));
                clickEditor(window, NSMakePoint(450, 222)); // web domain field
                NSResponder* responder = [window firstResponder];
                NSString* fieldText = nil;
                if ([responder respondsToSelector:@selector(attributedSubstringForProposedRange:actualRange:)]) {
                    fieldText = [[(id<NSTextInputClient>)responder
                        attributedSubstringForProposedRange:NSMakeRange(0, 127) actualRange:nil] string];
                }
                if (![fieldText isEqualToString:@"https://vdo.ninja/"]) {
                    NSLog(@"Advanced field text: %@", fieldText);
                    std::cerr << "Advanced domain field did not become editable\n";
                    return 1;
                }
                clickEditor(window, NSMakePoint(650, 310));
            }
            if (cycle == 4) {
                if (!typeField(window, NSMakePoint(350, 230), @"uiowned") ||
                    !typeField(window, NSMakePoint(350, 150), @"test &é+pass")) return 1;
                clickEditor(window, NSMakePoint(650, 310));
                if (!typeField(window, NSMakePoint(450, 222), @"https://backup.vdo.ninja/") ||
                    !typeField(window, NSMakePoint(450, 161), @"mix &+/=?#-é🔊") ||
                    !typeField(window, NSMakePoint(450, 100), @"wss://apibackup.vdo.ninja/")) return 1;
                clickEditor(window, NSMakePoint(600, 31)); // Apply atomically via actual native button.
                clickEditor(window, NSMakePoint(650, 310));
                NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
                NSMutableArray* previous = [NSMutableArray array];
                for (NSPasteboardItem* item in [pasteboard pasteboardItems]) {
                    NSPasteboardItem* copy = [[NSPasteboardItem alloc] init];
                    for (NSString* type in [item types]) if (NSData* data = [item dataForType:type]) [copy setData:data forType:type];
                    [previous addObject:copy];
                }
                // Hosts/validators can write even non-automatable parameters.
                // Intercept the opener before this regression so a failure can
                // never launch hundreds of tabs in the user's default browser.
                Method opener = class_getInstanceMethod([NSWorkspace class], @selector(openURL:));
                IMP original = method_setImplementation(opener, reinterpret_cast<IMP>(captureQrOpen));
                const auto pasteboardVersion = [pasteboard changeCount];
                for (int edit = 0; edit < 200; ++edit) {
                    for (auto tag : {webrtc_vst::kParamCopyPushLink, webrtc_vst::kParamShowPushQr}) {
                        controller->setParamNormalized(tag, 1.0);
                        controller->setParamNormalized(tag, 0.0);
                    }
                }
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
                method_setImplementation(opener, original);
                const bool hostActionsInert = qrOpenRequests == 0 && [pasteboard changeCount] == pasteboardVersion;
                if (!hostActionsInert) {
                    [pasteboard clearContents];
                    if ([previous count]) [pasteboard writeObjects:previous];
                    std::cerr << "Host parameter writes triggered external actions: QR requests=" << qrOpenRequests << '\n';
                    return 1;
                }
                std::cout << "PASS: 200 host Copy/QR parameter cycles caused no browser or clipboard side effects\n";
                clickEditor(window, NSMakePoint(575, 70));
                NSString* link = [pasteboard stringForType:NSPasteboardTypeString];
                NSURLComponents* parsed = [NSURLComponents componentsWithString:link];
                NSMutableDictionary* fields = [NSMutableDictionary dictionary];
                for (NSURLQueryItem* item in parsed.queryItems) fields[item.name] = item.value ?: @"";
                const bool correct = [parsed.host isEqualToString:@"backup.vdo.ninja"] &&
                    [fields[@"push"] isEqualToString:@"uiowned"] && [fields[@"salt"] isEqualToString:@"mix &+/=?#-é🔊"] &&
                    [fields[@"wss2"] isEqualToString:@"wss://apibackup.vdo.ninja/"] && fields[@"hash"] && !fields[@"password"];
                [pasteboard clearContents];
                if ([previous count]) [pasteboard writeObjects:previous];
                if (!correct) { NSLog(@"Actual copied test link: %@", link); std::cerr << "UI edit/Apply/Copy regression\n"; return 1; }
                // Capture only this test process's browser-open request; do not
                // leave an unsolicited QR tab in the user's ordinary browser.
                original = method_setImplementation(opener, reinterpret_cast<IMP>(captureQrOpen));
                clickEditor(window, NSMakePoint(660, 70));
                method_setImplementation(opener, original);
                if (qrOpenRequests != 1 || !requestedQrUrl || ![decodeQrPage(requestedQrUrl) isEqualToString:link]) {
                    std::cerr << "Actual QR failed independent Apple Vision decode / copied-link comparison\n"; return 1;
                }
                std::cout << "PASS: QR button wrote private local page; independent Apple Vision decoder recovered exact copied link\n";
                if (argc > 2) {
                    NSData* json = [NSJSONSerialization dataWithJSONObject:@{@"link":link} options:NSJSONWritingPrettyPrinted error:nil];
                    if (![json writeToFile:[NSString stringWithUTF8String:argv[2]] options:NSDataWritingAtomic error:nil]) return 1;
                }
                std::cout << "PASS: actual Cocoa text edits, Unicode salt/password, Apply and Copy; clipboard restored\n";
            }
            view->onFocus(false);
            if (view->removed() != Steinberg::kResultOk) return 1;
            [window close];
        }
        std::cout << "PASS: Cocoa editor opened/closed five times; Advanced opened/closed twenty times\n";
    }
    return 0;
}

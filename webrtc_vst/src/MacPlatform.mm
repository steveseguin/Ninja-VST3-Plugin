#include "MacPlatform.h"

#import <AppKit/AppKit.h>

namespace webrtc_vst {
std::string normalizeMacEditedText(const std::string& input) {
    @autoreleasepool {
        NSString* text = [[NSString alloc] initWithBytes:input.data() length:input.size() encoding:NSUTF8StringEncoding];
        const char* utf8 = [[text precomposedStringWithCanonicalMapping] UTF8String];
        std::string result = utf8 ? utf8 : "";
#if !__has_feature(objc_arc)
        [text release];
#endif
        return result;
    }
}

bool openMacExternalUrl(const std::string& url) {
    @autoreleasepool {
        NSString* text = [[NSString alloc] initWithBytes:url.data()
                                               length:url.size()
                                             encoding:NSUTF8StringEncoding];
        // QR viewers are filesystem paths, not URL strings. fileURLWithPath
        // also correctly escapes spaces, percent signs and Unicode filenames.
        NSURL* target = text ? ([text isAbsolutePath] ? [NSURL fileURLWithPath:text] : [NSURL URLWithString:text]) : nil;
        const bool opened = target && [[NSWorkspace sharedWorkspace] openURL:target];
#if !__has_feature(objc_arc)
        [text release];
#endif
        return opened;
    }
}
}

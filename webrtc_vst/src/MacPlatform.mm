#include "MacPlatform.h"

#import <AppKit/AppKit.h>

namespace webrtc_vst {
bool openMacExternalUrl(const std::string& url) {
    @autoreleasepool {
        NSString* text = [[NSString alloc] initWithBytes:url.data()
                                               length:url.size()
                                             encoding:NSUTF8StringEncoding];
        NSURL* target = text ? [NSURL URLWithString:text] : nil;
        const bool opened = target && [[NSWorkspace sharedWorkspace] openURL:target];
#if !__has_feature(objc_arc)
        [text release];
#endif
        return opened;
    }
}
}

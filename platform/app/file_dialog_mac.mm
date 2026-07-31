// file_dialog_mac.mm — NSOpenPanel. See file_dialog.h.
//
// A path returned from NSOpenPanel is TCC-exempt because the user selected it
// personally, so this reaches Documents/Downloads/an external drive with no
// permission prompt — unlike the directory scan it replaces.
#include "file_dialog.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace filedialog {

bool isAvailable() { return true; }

bool openRom(std::string &out) {
    @autoreleasepool {
        // The panel must be driven on the main thread; the launcher's UI loop is
        // the main thread, so this is a straight call rather than a dispatch.
        if (![NSThread isMainThread]) {
            return false;
        }

        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Choose a Nintendo 64 ROM";
        panel.message = @"Select your own copy of the original game (.z64, .n64 or .v64).";
        panel.prompt = @"Choose";
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        panel.resolvesAliases = YES;
        // Let the user reach anywhere they can normally reach, including
        // external volumes where dumps commonly live.
        panel.treatsFilePackagesAsDirectories = NO;
        panel.showsHiddenFiles = NO;

        // Filter to the three N64 image extensions. UTType is 11.0+; the
        // deprecated string API is the fallback for older deployment targets.
        // Extensions are not registered system types, so build them explicitly.
        if (@available(macOS 11.0, *)) {
            NSMutableArray<UTType *> *types = [NSMutableArray array];
            for (NSString *ext in @[ @"z64", @"n64", @"v64" ]) {
                UTType *t = [UTType typeWithFilenameExtension:ext];
                if (t != nil) [types addObject:t];
            }
            if (types.count > 0) {
                panel.allowedContentTypes = types;
            }
        } else {
#if defined(MAC_OS_X_VERSION_10_0)
            // Deprecated on 11+, still correct below it.
            panel.allowedFileTypes = @[ @"z64", @"n64", @"v64" ];
#endif
        }
        // Never trap the user: if their dump has an unusual extension they can
        // still select it.
        panel.allowsOtherFileTypes = YES;

        // The launcher runs as a plain SDL app that may not be the active
        // application; without this the panel can open behind the game window.
        [NSApp activateIgnoringOtherApps:YES];

        NSModalResponse response = [panel runModal];
        if (response != NSModalResponseOK) {
            return false;   // user cancelled — leave `out` untouched
        }

        NSURL *url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) {
            return false;
        }
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') {
            return false;
        }
        out = path;
        return true;
    }
}

}  // namespace filedialog

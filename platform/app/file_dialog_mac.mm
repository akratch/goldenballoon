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

        // Filter to the three N64 image extensions. Golden Balloon targets
        // macOS 13+, so use the current Uniform Type Identifiers API directly
        // and keep deprecated allowedFileTypes out of warning-clean builds.
        NSMutableArray<UTType *> *types = [NSMutableArray array];
        for (NSString *ext in @[ @"z64", @"n64", @"v64" ]) {
            UTType *type = [UTType typeWithFilenameExtension:ext];
            if (type != nil) [types addObject:type];
        }
        if (types.count > 0) {
            panel.allowedContentTypes = types;
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

bool openContentPack(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Install a content pack";
        // Not "a folder containing pack.ini": the commonest pack by far is a
        // high-resolution texture pack, which has no pack.ini at all. Naming
        // the file a player does not have tells them their pack is wrong.
        panel.message = @"Choose a pack — a .zip or a folder, just as it downloaded.";
        panel.prompt = @"Install";
        panel.allowsMultipleSelection = NO;
        // Both shapes are legal packs, so both are selectable.
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = YES;
        panel.resolvesAliases = YES;
        // A pack folder is an ordinary directory; treating file packages as
        // directories would let the player descend into a .app by mistake.
        panel.treatsFilePackagesAsDirectories = NO;
        panel.showsHiddenFiles = NO;
        UTType *zip = [UTType typeWithFilenameExtension:@"zip"];
        if (zip != nil) {
            panel.allowedContentTypes = @[ zip, UTTypeFolder ];
        }
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool openCharacterSource(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Import a custom character";
        panel.message = @"Choose a package, model, data-only adapter result, archive, or DCC source. External adapter code is never executed.";
        panel.prompt = @"Choose Source";
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        panel.resolvesAliases = YES;
        panel.treatsFilePackagesAsDirectories = NO;
        panel.showsHiddenFiles = NO;
        NSMutableArray<UTType *> *types = [NSMutableArray array];
        for (NSString *ext in @[
                 @"mdkrchar", @"mdkrsource", @"glb", @"dae", @"gltf", @"fbx", @"obj",
                 @"blend", @"usd", @"usda", @"usdc", @"usdz", @"ma",
                 @"mb", @"max", @"c4d", @"3ds"
             ]) {
            UTType *type = [UTType typeWithFilenameExtension:ext];
            if (type != nil) [types addObject:type];
        }
        [types addObject:UTTypeZIP];
        panel.allowedContentTypes = types;
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool openCharacterLicense(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Choose the character license or notice file";
        panel.message = @"Choose the exact LICENSE, COPYING, or notice text to embed in the source package.";
        panel.prompt = @"Choose License";
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        panel.resolvesAliases = YES;
        panel.treatsFilePackagesAsDirectories = NO;
        panel.showsHiddenFiles = NO;
        panel.allowsOtherFileTypes = YES;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool openPortraitImage(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Choose character portrait artwork";
        panel.message = @"Choose a square PNG (16–1024 pixels). The Workshop previews the exact 40×40 in-game result.";
        panel.prompt = @"Choose Portrait";
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        panel.resolvesAliases = YES;
        panel.treatsFilePackagesAsDirectories = NO;
        panel.showsHiddenFiles = NO;
        panel.allowedContentTypes = @[ UTTypePNG ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool openCharacterDraftBundle(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Review a character draft bundle";
        panel.message = @"Choose a Golden Balloon named-draft bundle. Nothing is imported until compatibility, contents, and local-use rights are reviewed.";
        panel.prompt = @"Review Bundle";
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        panel.resolvesAliases = YES;
        panel.treatsFilePackagesAsDirectories = NO;
        panel.showsHiddenFiles = NO;
        UTType *type = [UTType typeWithFilenameExtension:@"mdkrdrafts"];
        if (type != nil) panel.allowedContentTypes = @[ type ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveCharacterConvertedGlb(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Save converted character model";
        panel.message = @"Choose a new GLB filename. Golden Balloon never overwrites an existing model.";
        panel.prompt = @"Choose Filename";
        panel.nameFieldStringValue = @"converted-character.glb";
        panel.canCreateDirectories = YES;
        UTType *glbType = [UTType typeWithFilenameExtension:@"glb"];
        if (glbType != nil) panel.allowedContentTypes = @[ glbType ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveCharacterCapture(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Save a custom character inspection";
        panel.message = @"Choose a new PNG filename. Golden Balloon never overwrites an existing capture.";
        panel.prompt = @"Choose Filename";
        panel.nameFieldStringValue = @"character-inspection.png";
        panel.canCreateDirectories = YES;
        panel.allowedContentTypes = @[ UTTypePNG ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveGameProgress(std::string &out, const std::string &suggestedName) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Export game save";
        panel.message = @"Save your adventure progress, unlocks and time records as a portable file.";
        panel.prompt = @"Choose Filename";
        panel.canCreateDirectories = YES;
        UTType *type = [UTType typeWithFilenameExtension:@"mdkr-save"];
        if (type != nil) panel.allowedContentTypes = @[ type ];
        panel.allowsOtherFileTypes = NO;
        // Configure the type and extension display before assigning the name.
        // Reused panel state must not append another suffix on the next export.
        panel.extensionHidden = NO;
        panel.nameFieldStringValue = [NSString stringWithUTF8String:suggestedName.c_str()];
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveCharacterReport(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Export custom character visual report";
        panel.message = @"Choose a new HTML filename. The report embeds its PNGs and never overwrites an existing file.";
        panel.prompt = @"Choose Filename";
        panel.nameFieldStringValue = @"character-visual-report.html";
        panel.canCreateDirectories = YES;
        panel.allowedContentTypes = @[ UTTypeHTML ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveCharacterDeviceProfile(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Export custom character device profile";
        panel.message = @"Choose a new JSON filename. The profile identifies the GPU and driver, omits character identity and content, and Golden Balloon never overwrites an existing file.";
        panel.prompt = @"Choose Filename";
        panel.nameFieldStringValue = @"character-device-profile.json";
        panel.canCreateDirectories = YES;
        panel.allowedContentTypes = @[ UTTypeJSON ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveCharacterPackage(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Export custom character package";
        panel.message = @"Choose a new mdkrchar filename. Golden Balloon never overwrites an existing package.";
        panel.prompt = @"Choose Filename";
        panel.nameFieldStringValue = @"custom-character.mdkrchar";
        panel.canCreateDirectories = YES;
        UTType *type = [UTType typeWithFilenameExtension:@"mdkrchar"];
        if (type != nil) panel.allowedContentTypes = @[ type ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveCharacterDraftBundle(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Export character named drafts";
        panel.message = @"Choose a new mdkrdrafts filename. The bundle omits models, ROM data, and local paths, and Golden Balloon never overwrites an existing file.";
        panel.prompt = @"Choose Filename";
        panel.nameFieldStringValue = @"character-drafts.mdkrdrafts";
        panel.canCreateDirectories = YES;
        UTType *type = [UTType typeWithFilenameExtension:@"mdkrdrafts"];
        if (type != nil) panel.allowedContentTypes = @[ type ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveCharacterDiagnostic(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Export failed-import diagnostic";
        panel.message = @"Choose a new JSON filename. The diagnostic contains no model bytes and never overwrites an existing file.";
        panel.prompt = @"Choose Filename";
        panel.nameFieldStringValue = @"character-import-diagnostic.json";
        panel.canCreateDirectories = YES;
        panel.allowedContentTypes = @[ UTTypeJSON ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool revealInFileManager(const std::string &path) {
    @autoreleasepool {
        if (![NSThread isMainThread] || path.empty()) return false;
        NSURL *url = [NSURL fileURLWithFileSystemRepresentation:path.c_str()
                                                     isDirectory:NO
                                                   relativeToURL:nil];
        if (url == nil) return false;
        [[NSWorkspace sharedWorkspace]
            activateFileViewerSelectingURLs:@[ url ]];
        return true;
    }
}

}  // namespace filedialog

// app_activation_mac.mm — LaunchServices-equivalent foreground activation.
#include "app_activation.h"

#import <AppKit/AppKit.h>
#import <SDL_syswm.h>

void AppActivation_prepareProcess() {
    if (!AppActivation_backgroundAutomation()) return;

    /* Set SDL's own Cocoa policy before SDL_Init constructs NSApplication.
     * Demoting only after SDL_CreateWindow left a small but observable window
     * in which a test process could become the foreground application. */
    SDL_SetHintWithPriority(SDL_HINT_MAC_BACKGROUND_APP,
                            "1", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN,
                            "1", SDL_HINT_OVERRIDE);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

void AppActivation_requestForeground(SDL_Window *window, void *nativeView) {
    /* Finder performs this activation when it opens a bundle. Direct binary
     * launches used by release tests do not, and a background CAMetalLayer can
     * report every drawable as unavailable. Exercise the same foreground state
     * for both paths before qualifying or adopting the surface. */
    NSWindow *nativeWindow = nil;
    if (nativeView != nullptr) {
        nativeWindow = [(NSView *)nativeView window];
    }
    SDL_SysWMinfo info = {};
    SDL_VERSION(&info.version);
    if (nativeWindow == nil && SDL_GetWindowWMInfo(window, &info) == SDL_TRUE &&
        info.subsystem == SDL_SYSWM_COCOA) {
        nativeWindow = info.info.cocoa.window;
    }
    if (AppActivation_backgroundAutomation()) {
        /* A hidden CAMetalLayer may be permanently occluded and never vend a
         * drawable. Order the automation surface behind existing windows so
         * it remains renderable, but deliberately never activate NSApp, make
         * the NSWindow key, or raise it. Accessory policy also keeps dozens of
         * isolated screenshot processes out of the Dock/app switcher. */
        AppActivation_prepareProcess();
        if (nativeWindow != nil) {
            [nativeWindow setIsVisible:YES];
            [nativeWindow orderBack:nil];
        }
        SDL_PumpEvents();
        return;
    }

    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    SDL_ShowWindow(window);
    if (nativeWindow != nil) {
        [nativeWindow deminiaturize:nil];
        [nativeWindow setIsVisible:YES];
        [nativeWindow makeKeyAndOrderFront:nil];
        [nativeWindow orderFrontRegardless];
    }
    SDL_RaiseWindow(window);
    /* Apply the ordering synchronously before WebGPU configures/acquires the
     * CAMetalLayer surface. This is a single pump during host initialization,
     * not an event loop or gameplay timing input. */
    SDL_PumpEvents();
}

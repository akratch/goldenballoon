// app_activation_mac.mm — LaunchServices-equivalent foreground activation.
#include "app_activation.h"

#import <AppKit/AppKit.h>
#import <SDL_syswm.h>

void AppActivation_requestForeground(SDL_Window *window, void *nativeView) {
    /* Finder performs this activation when it opens a bundle. Direct binary
     * launches used by release tests do not, and a background CAMetalLayer can
     * report every drawable as unavailable. Exercise the same foreground state
     * for both paths before qualifying or adopting the surface. */
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    SDL_ShowWindow(window);

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

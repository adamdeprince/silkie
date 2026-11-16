// License: GPLv3 Copyright: 2026, Silkie contributors
// Exercise AppKit's real native tab layout with the GLFW content view. No OpenGL
// context or physical display is needed to verify terminal resize notifications.
#define _GLFW_COCOA
#include "../glfw/internal.h"
#import <objc/message.h>
#include <dlfcn.h>

@interface NSView (GLFWTestCleanup)
- (void)removeGLFWWindow;
@end

@interface TabTestWindow : NSWindow
@end
@implementation TabTestWindow
- (NSRect)constrainFrameRect:(NSRect)rect toScreen:(NSScreen *)screen {
    (void)screen;
    // A headless test session may otherwise constrain every window to 1x0.
    return rect;
}
@end

static void
require_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

typedef struct {
    _GLFWwindow window;
    NSSize notified_size, notified_framebuffer;
} TestTab;

static void
on_size(GLFWwindow *window, int width, int height) {
    TestTab *tab = ((_GLFWwindow *)window)->userPointer;
    tab->notified_size = NSMakeSize(width, height);
}

static void
on_framebuffer(GLFWwindow *window, int width, int height) {
    TestTab *tab = ((_GLFWwindow *)window)->userPointer;
    tab->notified_framebuffer = NSMakeSize(width, height);
}

static void
init_tab(TestTab *tab) {
    _GLFWwindow *w = &tab->window;
    w->userPointer = tab;
    w->context.client = GLFW_NO_API;
    w->callbacks.size = on_size;
    w->callbacks.fbsize = on_framebuffer;
    NSWindow *nw = [[TabTestWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable | NSWindowStyleMaskClosable
        backing:NSBackingStoreBuffered defer:NO];
    nw.releasedWhenClosed = NO;
    w->ns.object = nw;
    Class cls = NSClassFromString(@"GLFWContentView");
    require_true(cls != Nil, "GLFW content view missing");
    w->ns.view = ((id (*)(id, SEL, void *))objc_msgSend)([cls alloc], NSSelectorFromString(@"initWithGlfwWindow:"), w);
    [nw setContentView:w->ns.view];
    [nw setFrame:NSMakeRect(0, 0, 800, 632) display:NO];
    // GLFW initializes these sizes after installing the content view. Seed the
    // simulated terminal with the same initial dimensions before tabbing it.
    NSRect view = [w->ns.view bounds];
    NSRect fb = [w->ns.view convertRectToBacking:view];
    w->ns.width = view.size.width;
    w->ns.height = view.size.height;
    w->ns.fbWidth = fb.size.width;
    w->ns.fbHeight = fb.size.height;
    tab->notified_size = view.size;
    tab->notified_framebuffer = fb.size;
}

static void
check_tab(TestTab *tab) {
    _GLFWwindow *w = &tab->window;
    NSRect view = [w->ns.view bounds];
    NSRect framebuffer = [w->ns.view convertRectToBacking:view];
    require_true(view.size.height > 0, "empty terminal view");
    require_true(w->ns.width == view.size.width && w->ns.height == view.size.height, "cached size overlaps native tab bar");
    require_true(NSEqualSizes(tab->notified_size, view.size), "terminal did not receive content resize");
    require_true(NSEqualSizes(tab->notified_framebuffer, framebuffer.size), "terminal did not receive framebuffer resize");
}

static void
close_tab(TestTab *tab) {
    [tab->window.ns.object close];
    [tab->window.ns.view removeGLFWWindow];
    [tab->window.ns.view release];
    [tab->window.ns.object release];
}

int
main(int argc, char **argv) {
    require_true(argc == 2, "expected Cocoa module path");
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSWindow setAllowsAutomaticWindowTabbing:NO];
        require_true(dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL) != NULL, "cannot load Cocoa module");
        TestTab first = {0};
        init_tab(&first);
        [first.window.ns.object orderFront:nil];
        check_tab(&first);
        const CGFloat full_height = [first.window.ns.view bounds].size.height;
        // Repeatedly cross the one-tab/two-tab boundary and switch between tabs.
        for (int cycle = 0; cycle < 100; cycle++) {
            @autoreleasepool {
                TestTab second = {0}, third = {0};
                init_tab(&second);
                [first.window.ns.object addTabbedWindow:second.window.ns.object ordered:NSWindowAbove];
                [second.window.ns.object orderFront:nil];
                check_tab(&first);
                check_tab(&second);
                require_true([first.window.ns.view bounds].size.height < full_height, "tab bar did not reduce terminal height");
                init_tab(&third);
                [second.window.ns.object addTabbedWindow:third.window.ns.object ordered:NSWindowAbove];
                [third.window.ns.object orderFront:nil];
                [first.window.ns.object tabGroup].selectedWindow = first.window.ns.object;
                check_tab(&first);
                check_tab(&second);
                check_tab(&third);
                [first.window.ns.object setFrame:NSMakeRect(0, 0, 900, 700) display:NO];
                check_tab(&first);
                [first.window.ns.object setFrame:NSMakeRect(0, 0, 800, 632) display:NO];
                close_tab(&third);
                close_tab(&second);
                check_tab(&first);
                require_true([first.window.ns.view bounds].size.height == full_height, "closing tabs did not restore terminal height");
            }
        }
        close_tab(&first);
        puts("native tab resizing passed");
    }
    return 0;
}

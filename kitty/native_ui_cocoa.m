/*
 * native_ui_cocoa.m
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

// The macOS native user interface: tabs that are NSWindow tab groups, menus built from the Python
// menu model and the Settings window. The Settings window renders the data from
// kitty/settings_model.py and kitty/profiles.py, it knows nothing about individual options.

#include "native_ui.h"
#include "cocoa_window.h"
#include "glfw-wrapper.h"
#include <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#include <objc/runtime.h>
#include <dlfcn.h>

// Python to Foundation {{{
static id
ns_from_python(PyObject *o) {
    if (o == Py_None) return [NSNull null];
    if (PyBool_Check(o)) return @(o == Py_True);
    if (PyLong_Check(o)) return @(PyLong_AsLongLong(o));
    if (PyFloat_Check(o)) return @(PyFloat_AsDouble(o));
    if (PyUnicode_Check(o)) {
        const char *s = PyUnicode_AsUTF8(o);
        return s ? @(s) : @"";
    }
    if (PyList_Check(o) || PyTuple_Check(o)) {
        RAII_PyObject(seq, PySequence_Fast(o, ""));
        const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        NSMutableArray *ans = [NSMutableArray arrayWithCapacity:n];
        for (Py_ssize_t i = 0; i < n; i++) [ans addObject:ns_from_python(PySequence_Fast_GET_ITEM(seq, i))];
        return ans;
    }
    if (PyDict_Check(o)) {
        NSMutableDictionary *ans = [NSMutableDictionary dictionaryWithCapacity:PyDict_Size(o)];
        PyObject *k, *v;
        Py_ssize_t pos = 0;
        while (PyDict_Next(o, &pos, &k, &v)) {
            if (PyUnicode_Check(k)) ans[ns_from_python(k)] = ns_from_python(v);
        }
        return ans;
    }
    return [NSNull null];
}

static NSString *
str_of(NSDictionary *d, NSString *key) {
    id v = d[key];
    return [v isKindOfClass:[NSString class]] ? v : @"";
}

static BOOL
bool_of(NSDictionary *d, NSString *key) {
    id v = d[key];
    return [v isKindOfClass:[NSNumber class]] && [v boolValue];
}

static NSArray *
array_of(NSDictionary *d, NSString *key) {
    id v = d[key];
    return [v isKindOfClass:[NSArray class]] ? v : @[];
}
// }}}

// Native tabs {{{
static NSWindow *
ns_window(OSWindow *w) {
    return w && w->handle ? (NSWindow *)glfwGetCocoaWindow(w->handle) : nil;
}

static NSMutableDictionary<NSNumber *, AVSpeechSynthesizer *> *speakers;
static NSMutableDictionary<NSNumber *, NSSound *> *speech_sounds;

bool
native_ui_cocoa_speech_control(id_type pane, const char *op) {
    @autoreleasepool {
        AVSpeechSynthesizer *speaker = speakers[@(pane)];
        NSSound *sound = speech_sounds[@(pane)];
        if (!speaker && !sound) return false;
        if (strcmp(op, "stop") == 0) {
            [speaker stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
            [speakers removeObjectForKey:@(pane)];
            [sound stop];
            [speech_sounds removeObjectForKey:@(pane)];
            return true;
        }
        if (strcmp(op, "pause") == 0) {
            [sound pause];
            return [speaker pauseSpeakingAtBoundary:AVSpeechBoundaryImmediate];
        }
        if (strcmp(op, "resume") == 0) {
            [sound resume];
            return [speaker continueSpeaking];
        }
        return speaker.speaking || speaker.paused;
    }
}

bool
native_ui_cocoa_speak(id_type pane, const char *text, const char *voice, const char *language, int rate, int pitch, int volume) {
    @autoreleasepool {
        AVSpeechUtterance *utterance = [AVSpeechUtterance speechUtteranceWithString:@(text)];
        if (!utterance) return false;
        if (*voice) {
            utterance.voice = [AVSpeechSynthesisVoice voiceWithIdentifier:@(voice)];
            if (!utterance.voice) {
                for (AVSpeechSynthesisVoice *candidate in AVSpeechSynthesisVoice.speechVoices) {
                    if ([candidate.name isEqualToString:@(voice)]) { utterance.voice = candidate; break; }
                }
            }
        }
        if (!utterance.voice && *language) utterance.voice = [AVSpeechSynthesisVoice voiceWithLanguage:@(language)];
        if (rate) {
            const float limit = rate > 0 ? AVSpeechUtteranceMaximumSpeechRate : AVSpeechUtteranceMinimumSpeechRate;
            utterance.rate = AVSpeechUtteranceDefaultSpeechRate +
                (limit - AVSpeechUtteranceDefaultSpeechRate) * ((float)abs(rate) / 100.0f);
        }
        if (pitch) utterance.pitchMultiplier = pitch > 0 ? 1.0f + (float)pitch / 100.0f : 1.0f + (float)pitch / 200.0f;
        if (volume >= 0) utterance.volume = (float)volume / 100.0f;
        if (!speakers) speakers = [[NSMutableDictionary alloc] init];
        AVSpeechSynthesizer *speaker = speakers[@(pane)];
        if (!speaker) {
            speaker = [[[AVSpeechSynthesizer alloc] init] autorelease];
            speakers[@(pane)] = speaker;
        }
        [speaker speakUtterance:utterance];
        return true;
    }
}

double
native_ui_cocoa_speech_sound(id_type pane, const char *name) {
    @autoreleasepool {
        NSSound *sound = [[[NSSound soundNamed:@(name)] copy] autorelease];
        if (sound && [sound play]) {
            if (!speech_sounds) speech_sounds = [[NSMutableDictionary alloc] init];
            speech_sounds[@(pane)] = sound;
            return sound.duration;
        }
        if (strcmp(name, "bell") == 0) NSBeep();
        return 0;
    }
}

@interface SilkieAccessiblePane : NSAccessibilityElement <NSAccessibilityNavigableStaticText> {
    NSDictionary *pane;
    NSString *text;
    NSArray<NSNumber *> *utf16Offsets;
    NSPointerArray *parent;
}
- (instancetype)initWithView:(NSView *)parent;
- (void)update:(NSDictionary *)data;
- (NSNumber *)paneID;
@end

@implementation SilkieAccessiblePane
- (instancetype)initWithView:(NSView *)container {
    if ((self = [super init])) {
        parent = [[NSPointerArray weakObjectsPointerArray] retain];
        [parent addPointer:container];
    }
    return self;
}

- (void)dealloc {
    [pane release];
    [text release];
    [utf16Offsets release];
    [parent release];
    [super dealloc];
}

- (NSNumber *)paneID { return pane[@"id"]; }
- (NSString *)accessibilityIdentifier { return [NSString stringWithFormat:@"silkie-pane-%@", self.paneID]; }
- (id)accessibilityParent { return [parent pointerAtIndex:0]; }
- (NSString *)accessibilityRole { return NSAccessibilityTextAreaRole; }
- (NSString *)accessibilityLabel { return str_of(pane, @"title"); }
- (BOOL)isAccessibilityFocused { return bool_of(pane, @"focused"); }
- (NSString *)accessibilityValue { return text; }
- (void)setAccessibilityValue:(id)value {
    NSView *view = self.accessibilityParent;
    if (self.isAccessibilityFocused && [value isKindOfClass:[NSString class]]) [view setAccessibilityValue:value];
}
- (NSInteger)accessibilityNumberOfCharacters { return (NSInteger)text.length; }
- (NSRange)accessibilityVisibleCharacterRange { return NSMakeRange(0, text.length); }

- (NSUInteger)indexForOffset:(NSNumber *)offset {
    const NSUInteger n = offset.unsignedIntegerValue;
    return n < utf16Offsets.count ? utf16Offsets[n].unsignedIntegerValue : text.length;
}

- (NSRange)rangeFrom:(NSNumber *)start to:(NSNumber *)end {
    const NSUInteger first = [self indexForOffset:start], last = [self indexForOffset:end];
    return NSMakeRange(first, last >= first ? last - first : 0);
}

- (NSArray *)accessibilitySelectedTextRanges {
    NSMutableArray *ranges = [NSMutableArray array];
    for (NSArray *pair in array_of(pane, @"selections")) {
        if (pair.count == 2) [ranges addObject:[NSValue valueWithRange:[self rangeFrom:pair[0] to:pair[1]]]];
    }
    return ranges;
}

- (NSRange)accessibilitySelectedTextRange {
    NSValue *selection = self.accessibilitySelectedTextRanges.firstObject;
    if (selection) return selection.rangeValue;
    NSNumber *caret = pane[@"caret"];
    return caret.integerValue < 0 ? NSMakeRange(NSNotFound, 0) : [self rangeFrom:caret to:caret];
}

- (NSString *)accessibilitySelectedText {
    return [self accessibilityStringForRange:self.accessibilitySelectedTextRange];
}

- (NSString *)accessibilityStringForRange:(NSRange)range {
    if (range.location > text.length) return nil;
    range.length = MIN(range.length, text.length - range.location);
    return [text substringWithRange:range];
}

- (NSInteger)accessibilityLineForIndex:(NSInteger)index {
    if (index < 0 || (NSUInteger)index > text.length) return NSNotFound;
    NSInteger line = -1;
    for (NSArray *row in array_of(pane, @"rows")) {
        if ([self indexForOffset:row.firstObject] > (NSUInteger)index) break;
        line++;
    }
    return line < 0 ? NSNotFound : line;
}

- (NSInteger)accessibilityInsertionPointLineNumber {
    NSNumber *caret = pane[@"caret"];
    return caret.integerValue < 0 ? NSNotFound : [self accessibilityLineForIndex:(NSInteger)[self indexForOffset:caret]];
}

- (NSRange)accessibilityRangeForLine:(NSInteger)line {
    NSArray *rows = array_of(pane, @"rows");
    if (line < 0 || (NSUInteger)line >= rows.count) return NSMakeRange(NSNotFound, 0);
    NSArray *row = rows[(NSUInteger)line];
    return [self rangeFrom:row.firstObject to:row.lastObject];
}

- (NSRect)screenRect:(NSRect)pixels {
    NSView *view = self.accessibilityParent;
    if (!view.window) return NSZeroRect;
    NSRect rect = [view convertRectFromBacking:pixels];
    if (!view.isFlipped) rect.origin.y = NSHeight(view.bounds) - NSMaxY(rect);
    return NSAccessibilityFrameInView(view, rect);
}

- (NSRect)accessibilityFrame {
    NSArray *r = array_of(pane, @"frame");
    return r.count == 4 ? [self screenRect:NSMakeRect([r[0] doubleValue], [r[1] doubleValue], [r[2] doubleValue], [r[3] doubleValue])] : NSZeroRect;
}

- (NSRect)accessibilityFrameForRange:(NSRange)range {
    if (range.location > text.length) return NSZeroRect;
    NSArray *r = array_of(pane, @"frame"), *rows = array_of(pane, @"rows");
    if (r.count != 4 || !rows.count) return NSZeroRect;
    const CGFloat x = [r[0] doubleValue], y = [r[1] doubleValue], h = [r[3] doubleValue] / rows.count;
    NSUInteger first = (NSUInteger)[self accessibilityLineForIndex:(NSInteger)range.location];
    NSUInteger last = (NSUInteger)[self accessibilityLineForIndex:(NSInteger)(range.location + MIN(range.length ? range.length - 1 : 0, text.length - range.location))];
    if (first >= rows.count || last >= rows.count || first > last) return NSZeroRect;
    NSRect rect = NSZeroRect;
    for (NSUInteger row = first; row <= last; row++) {
        NSArray *columns = rows[row];
        if (columns.count < 2) continue;
        const CGFloat w = [r[2] doubleValue] / (columns.count - 1);
        NSUInteger start = 0, end = columns.count - 1;
        if (row == first) {
            while (start + 1 < end && [self indexForOffset:columns[start + 1]] <= range.location) start++;
        }
        if (row == last) {
            NSUInteger limit = range.location + MIN(range.length, text.length - range.location);
            while (end > start + 1 && [self indexForOffset:columns[end - 1]] >= limit) end--;
        }
        NSRect part = NSMakeRect(x + w * start, y + h * row, w * (end - start), h);
        rect = row == first ? part : NSUnionRect(rect, part);
    }
    return [self screenRect:rect];
}

- (void)update:(NSDictionary *)data {
    NSString *newText = str_of(data, @"text");
    const BOOL textChanged = ![text isEqualToString:newText];
    const BOOL selectionChanged = ![pane[@"selections"] isEqual:data[@"selections"]] || ![pane[@"caret"] isEqual:data[@"caret"]];
    const BOOL focusGained = bool_of(data, @"focused") && !bool_of(pane, @"focused");
    const BOOL newPane = !pane;
    [pane release];
    pane = [data retain];
    if (textChanged) {
        [text release];
        text = [newText copy];
        NSMutableArray *offsets = [NSMutableArray arrayWithCapacity:text.length];
        for (NSUInteger i = 0; i < text.length; i++) {
            [offsets addObject:@(i)];
            if (CFStringIsSurrogateHighCharacter([text characterAtIndex:i]) && i + 1 < text.length &&
                    CFStringIsSurrogateLowCharacter([text characterAtIndex:i + 1])) i++;
        }
        [offsets addObject:@(text.length)];
        [utf16Offsets release];
        utf16Offsets = [offsets retain];
    }
    if (!newPane && textChanged) NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
    if (!newPane && selectionChanged) NSAccessibilityPostNotification(self, NSAccessibilitySelectedTextChangedNotification);
    if (focusGained) NSAccessibilityPostNotification(self, NSAccessibilityFocusedUIElementChangedNotification);
}
@end

void
native_ui_cocoa_accessibility_update(OSWindow *w, PyObject *panes) {
    @autoreleasepool {
        NSView *view = ns_window(w).contentView;
        if (!view) return;
        SEL key = @selector(accessibilityChildren);
        NSArray<SilkieAccessiblePane *> *old = objc_getAssociatedObject(view, key);
        NSMutableDictionary *byID = [NSMutableDictionary dictionary];
        for (SilkieAccessiblePane *item in old) byID[item.paneID] = item;
        NSMutableArray *children = [NSMutableArray array];
        for (NSDictionary *data in ns_from_python(panes)) {
            SilkieAccessiblePane *item = byID[data[@"id"]];
            if (!item) item = [[[SilkieAccessiblePane alloc] initWithView:view] autorelease];
            [item update:data];
            [children addObject:item];
        }
        if (![children isEqualToArray:old]) {
            objc_setAssociatedObject(view, key, children, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            NSAccessibilityPostNotification(view, NSAccessibilityLayoutChangedNotification);
        }
    }
}

void
native_ui_cocoa_accessibility_announce(OSWindow *w, const char *message, bool assertive) {
    @autoreleasepool {
        NSWindow *window = ns_window(w);
        if (window) NSAccessibilityPostNotificationWithUserInfo(window, NSAccessibilityAnnouncementRequestedNotification,
            @{NSAccessibilityAnnouncementKey: @(message),
              NSAccessibilityPriorityKey: @(assertive ? NSAccessibilityPriorityHigh : NSAccessibilityPriorityMedium)});
    }
}

bool
native_ui_cocoa_accessibility_active(void) {
    if (NSWorkspace.sharedWorkspace.voiceOverEnabled) return true;
    for (size_t i = 0; i < global_state.num_os_windows; i++) {
        NSView *view = ns_window(global_state.os_windows + i).contentView;
        if (view && objc_getAssociatedObject(view, @selector(accessibilityFocusedUIElement))) return true;
    }
    return false;
}

PyObject *
native_ui_cocoa_preferred_languages(void) {
    @autoreleasepool {
        NSArray<NSString *> *languages = NSLocale.preferredLanguages;
        RAII_PyObject(result, PyList_New(0));
        if (!result) return NULL;
        for (NSString *language in languages) {
            const char *value = language.UTF8String;
            if (value) {
                RAII_PyObject(tag, PyUnicode_FromString(value));
                if (!tag || PyList_Append(result, tag) < 0) return NULL;
            }
        }
        return Py_NewRef(result);
    }
}

static OSWindow *
os_window_of(NSWindow *nw) {
    for (size_t i = 0; i < global_state.num_os_windows; i++) {
        OSWindow *w = global_state.os_windows + i;
        if (ns_window(w) == nw) return w;
    }
    return NULL;
}

static void
insert_in_group(NSWindow *child, NSWindow *anchor, const char *location) {
    NSArray<NSWindow *> *members = anchor.tabGroup.windows;
    if (strcmp(location, "first") == 0 && members.count) [members.firstObject addTabbedWindow:child ordered:NSWindowBelow];
    else if (strcmp(location, "before") == 0) [anchor addTabbedWindow:child ordered:NSWindowBelow];
    else if (strcmp(location, "last") == 0 && members.count) [members.lastObject addTabbedWindow:child ordered:NSWindowAbove];
    else [anchor addTabbedWindow:child ordered:NSWindowAbove];
}

static void new_window_for_tab(id self UNUSED, SEL _cmd UNUSED, id sender UNUSED);

static bool
adopt_os_window(OSWindow *w, OSWindow *parent, const char *location) {
    NSWindow *nw = ns_window(w);
    if (!nw) return false;
    static bool plus_button_enabled = false;
    if (!plus_button_enabled && [NSApp delegate]) {
        // The responder chain answering this is what makes the tab bar show a + button
        plus_button_enabled = true;
        class_addMethod(object_getClass([NSApp delegate]), @selector(newWindowForTab:), (IMP)new_window_for_tab, "v@:@");
    }
    // Tab groups form only when asked for below, never because of the system-wide "prefer tabs" setting. The
    // setting would otherwise put New Window, which the user asked to be separate, in the tab group.
    [NSWindow setAllowsAutomaticWindowTabbing:NO];
    nw.tabbingMode = NSWindowTabbingModeAutomatic;
    nw.tabbingIdentifier = @"silkie";
    NSWindow *np = ns_window(parent);
    if (np) insert_in_group(nw, np, location);
    return false;  // the OS window is the native window
}

static void
attach_tab(OSWindow *w, OSWindow *target, const char *location) {
    NSWindow *nw = ns_window(w), *nt = ns_window(target);
    if (nw && nt && nw != nt) insert_in_group(nw, nt, location);
}

static void
select_tab(OSWindow *w) {
    NSWindow *nw = ns_window(w);
    if (!nw) return;
    if (nw.tabGroup) nw.tabGroup.selectedWindow = nw;
    [nw makeKeyAndOrderFront:nil];
}

static void
move_tab(OSWindow *w, int delta) {
    NSWindow *nw = ns_window(w);
    NSWindowTabGroup *group = nw.tabGroup;
    if (!group) return;
    NSArray<NSWindow *> *members = group.windows;
    const NSInteger n = members.count, idx = [members indexOfObject:nw];
    if (n < 2 || idx == NSNotFound) return;
    const NSInteger nidx = ((idx + delta) % n + n) % n;
    [group removeWindow:nw];
    [group insertWindow:nw atIndex:nidx];
    group.selectedWindow = nw;
    [nw makeKeyAndOrderFront:nil];
}

static void
detach_tab(OSWindow *w) {
    NSWindow *nw = ns_window(w);
    if (nw && nw.tabGroup.windows.count > 1) [nw moveTabToNewWindow:nil];
}

static size_t
tab_members(OSWindow *w, id_type *ids, size_t cap) {
    NSWindow *nw = ns_window(w);
    if (!nw) return 0;
    NSArray<NSWindow *> *members = nw.tabGroup.windows;
    if (!members.count) {
        if (cap) ids[0] = w->id;
        return 1;
    }
    size_t n = 0;
    for (NSWindow *m in members) {
        OSWindow *o = os_window_of(m);
        if (o) {
            if (n < cap) ids[n] = o->id;
            n++;
        }
    }
    return n;
}

// The + button of the native tab bar asks the responder chain for this
static void
new_window_for_tab(id self UNUSED, SEL _cmd UNUSED, id sender UNUSED) {
    set_cocoa_pending_action(NEW_TAB, NULL);
}
// }}}

// Program status badge {{{
// The accessory view of the NSWindowTab of a native tab: an indicator for the most urgent program status of
// the windows of the tab, see update_native_tab_status() in kitty/native_ui.py, with the text that goes with it.
static const CGFloat BADGE_ICON_SIZE = 14;

@interface TabStatusView : NSStackView
@property(nonatomic, copy) NSString *state;
@property(nonatomic, copy) NSString *kind;
@property(nonatomic) BOOL determinate;
@property(nonatomic, retain) NSProgressIndicator *spinner;
@property(nonatomic, retain) NSTextField *label;
@end

@implementation TabStatusView
- (void)dealloc {
    [_spinner stopAnimation:nil];
    [_state release];
    [_kind release];
    [_spinner release];
    [_label release];
    [super dealloc];
}
@end

static NSInteger
int_of(NSDictionary *d, NSString *key, NSInteger defval) {
    id v = d[key];
    return [v isKindOfClass:[NSNumber class]] ? [v integerValue] : defval;
}

static NSView *
badge_indicator(TabStatusView *v, NSString *state, NSString *kind, NSInteger progress) {
    if ([state isEqualToString:@"working"]) {
        NSProgressIndicator *p = [[[NSProgressIndicator alloc] init] autorelease];
        p.style = NSProgressIndicatorStyleSpinning;
        p.controlSize = NSControlSizeSmall;
        p.displayedWhenStopped = YES;
        p.indeterminate = progress < 0;
        p.minValue = 0; p.maxValue = 100;
        if (progress >= 0) p.doubleValue = progress;
        else [p startAnimation:nil];
        v.spinner = p;
        v.determinate = progress >= 0;
        return p;
    }
    NSString *name = @"exclamationmark.triangle.fill";
    NSColor *color = [NSColor systemOrangeColor];
    if ([state isEqualToString:@"done"]) { name = @"checkmark.circle.fill"; color = [NSColor systemGreenColor]; }
    else if ([state isEqualToString:@"error"]) { name = @"xmark.octagon.fill"; color = [NSColor systemRedColor]; }
    else if ([kind isEqualToString:@"permission"]) name = @"hand.raised.fill";
    else if ([kind isEqualToString:@"question"]) name = @"questionmark.circle.fill";
    else if ([kind isEqualToString:@"auth"]) name = @"key.fill";
    NSImage *img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    NSImageView *iv = [NSImageView imageViewWithImage:img ? img : [NSImage imageNamed:NSImageNameCaution]];
    iv.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:BADGE_ICON_SIZE - 2 weight:NSFontWeightSemibold];
    iv.contentTintColor = color;
    iv.imageScaling = NSImageScaleProportionallyDown;
    return iv;
}

static TabStatusView *
new_badge(NSString *state, NSString *kind, NSInteger progress) {
    TabStatusView *v = [[[TabStatusView alloc] init] autorelease];
    v.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    v.spacing = 3;
    v.alignment = NSLayoutAttributeCenterY;
    v.state = state;
    v.kind = kind;
    NSView *indicator = badge_indicator(v, state, kind, progress);
    indicator.translatesAutoresizingMaskIntoConstraints = NO;
    [indicator.widthAnchor constraintEqualToConstant:BADGE_ICON_SIZE].active = YES;
    [indicator.heightAnchor constraintEqualToConstant:BADGE_ICON_SIZE].active = YES;
    [indicator setAccessibilityElement:NO];
    [v addArrangedSubview:indicator];
    NSTextField *label = [NSTextField labelWithString:@""];
    label.font = [NSFont monospacedDigitSystemFontOfSize:[NSFont smallSystemFontSize] weight:NSFontWeightRegular];
    label.textColor = [NSColor secondaryLabelColor];
    [label setAccessibilityElement:NO];
    [v addArrangedSubview:label];
    v.label = label;
    // A narrow tab shortens its title rather than the badge
    [label setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [v setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    // The badge is one element for assistive technology, labelled with the status in words
    [v setAccessibilityElement:YES];
    [v setAccessibilityRole:NSAccessibilityImageRole];
    return v;
}

// The accessory view of a native tab holds its program status badge and the gear that opens its settings. It lives
// in the tab bar, which is in whichever window of the group is selected, so it knows the OS window of its tab by id.
@interface TabAccessoryView : NSStackView
@property(nonatomic) id_type osWindowId;
@property(nonatomic, retain) TabStatusView *badge;
@property(nonatomic, retain) NSButton *gear;
@end

@implementation TabAccessoryView
- (void)dealloc {
    [_badge release];
    [_gear release];
    [super dealloc];
}
@end

static TabAccessoryView *
tab_accessory(OSWindow *w, bool create) {
    NSWindow *nw = ns_window(w);
    if (!nw) return nil;
    NSView *v = nw.tab.accessoryView;
    if ([v isKindOfClass:[TabAccessoryView class]]) return (TabAccessoryView *)v;
    if (!create) return nil;
    TabAccessoryView *a = [[[TabAccessoryView alloc] init] autorelease];
    a.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    a.spacing = 4;
    a.alignment = NSLayoutAttributeCenterY;
    a.osWindowId = w->id;
    [a setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    nw.tab.accessoryView = a;
    return a;
}

static void
drop_tab_accessory_if_empty(OSWindow *w) {
    TabAccessoryView *a = tab_accessory(w, false);
    if (a && !a.badge && !a.gear) ns_window(w).tab.accessoryView = nil;
}

static void
tab_status(OSWindow *w, PyObject *status) {
    @autoreleasepool {
        if (status == Py_None) {
            TabAccessoryView *a = tab_accessory(w, false);
            if (a.badge) {
                [a removeArrangedSubview:a.badge];
                [a.badge removeFromSuperview];
                a.badge = nil;
            }
            drop_tab_accessory_if_empty(w);
            return;
        }
        TabAccessoryView *a = tab_accessory(w, true);
        if (!a) return;
        NSDictionary *d = ns_from_python(status);
        NSString *state = str_of(d, @"state"), *kind = str_of(d, @"kind"), *text = str_of(d, @"text");
        const NSInteger progress = int_of(d, @"progress", -1);
        TabStatusView *v = a.badge;
        // Progress reports come often, they update the badge in place to keep the spinner turning smoothly
        if (!v || ![v.state isEqualToString:state] || ![v.kind isEqualToString:kind] || (v.spinner && v.determinate != (progress >= 0))) {
            if (v) {
                [a removeArrangedSubview:v];
                [v removeFromSuperview];
            }
            v = new_badge(state, kind, progress);
            a.badge = v;
            [a insertArrangedSubview:v atIndex:0];  // the gear, if any, stays last
        } else if (v.spinner && progress >= 0) v.spinner.doubleValue = progress;
        v.label.stringValue = text;
        v.label.hidden = text.length == 0;
        v.toolTip = str_of(d, @"tooltip");
        [v setAccessibilityLabel:str_of(d, @"accessibility")];
    }
}
// }}}

// Tab settings popover {{{
@interface TabGearTarget : NSObject
+ (TabGearTarget *)shared;
@end

@implementation TabGearTarget
+ (TabGearTarget *)shared {
    static TabGearTarget *ans = nil;
    if (!ans) ans = [[TabGearTarget alloc] init];
    return ans;
}

- (void)openTabSettings:(NSButton *)sender {
    for (NSView *v = sender.superview; v; v = v.superview) {
        if ([v isKindOfClass:[TabAccessoryView class]]) {
            char action[64];  // the name of the action and a 64 bit number fit in 64 bytes
            snprintf(action, sizeof(action), "open_tab_settings %llu", ((TabAccessoryView *)v).osWindowId);
            set_cocoa_pending_action(USER_MENU_ACTION, action);
            return;
        }
    }
}
@end

static void
tab_settings_button(OSWindow *w, bool show) {
    @autoreleasepool {
        TabAccessoryView *a = tab_accessory(w, show);
        if (!a) return;
        if (show && !a.gear) {
            NSImage *img = [NSImage imageWithSystemSymbolName:@"gearshape" accessibilityDescription:@"Tab Settings"];
            NSButton *b = [NSButton buttonWithImage:img target:[TabGearTarget shared] action:@selector(openTabSettings:)];
            b.bordered = NO;
            b.imagePosition = NSImageOnly;
            b.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:BADGE_ICON_SIZE - 3 weight:NSFontWeightRegular];
            b.contentTintColor = [NSColor secondaryLabelColor];
            b.toolTip = @"Tab Settings";
            [b setAccessibilityLabel:@"Tab Settings"];
            b.translatesAutoresizingMaskIntoConstraints = NO;
            [b.widthAnchor constraintEqualToConstant:BADGE_ICON_SIZE].active = YES;
            [b.heightAnchor constraintEqualToConstant:BADGE_ICON_SIZE].active = YES;
            a.gear = b;
            [a addArrangedSubview:b];
        } else if (!show && a.gear) {
            [a removeArrangedSubview:a.gear];
            [a.gear removeFromSuperview];
            a.gear = nil;
            drop_tab_accessory_if_empty(w);
        }
    }
}

@interface TabSettingsController : NSViewController <NSPopoverDelegate, NSTextFieldDelegate, NSComboBoxDelegate>
@property(nonatomic) PyObject *callback;
@property(nonatomic, retain) NSPopover *popover;
@property(nonatomic, retain) NSTextField *errorLabel;
@property(nonatomic, retain) NSMutableArray<NSControl *> *textControls;
@property(nonatomic, retain) NSView *firstControl;
@end

static TabSettingsController *tab_settings_controller = nil;
static const void *FIELD_NAME_KEY = &FIELD_NAME_KEY;

@implementation TabSettingsController

- (void)dealloc {
    Py_CLEAR(_callback);
    [_popover release];
    [_errorLabel release];
    [_textControls release];
    [_firstControl release];
    [super dealloc];
}

- (void)emit:(NSString *)verb name:(NSString *)name value:(NSString *)value {
    if (!_callback) return;
    PyObject *ret = PyObject_CallFunction(_callback, "sss", verb.UTF8String, name.UTF8String, value.UTF8String);
    NSString *err = @"";
    if (!ret) PyErr_Print();
    else {
        if (PyUnicode_Check(ret)) {
            const char *e = PyUnicode_AsUTF8(ret);
            if (e) err = @(e);
            else PyErr_Clear();
        }
        Py_DECREF(ret);
    }
    _errorLabel.stringValue = err;
    _errorLabel.hidden = err.length == 0;
}

static NSString *
field_name(id control) {
    NSString *n = objc_getAssociatedObject(control, FIELD_NAME_KEY);
    return n ? n : @"";
}

- (void)choiceChanged:(NSPopUpButton *)sender {
    NSString *value = sender.selectedItem.representedObject;
    [self emit:@"set" name:field_name(sender) value:value ? value : @""];
}

- (void)buttonPressed:(NSButton *)sender { [self emit:@"activate" name:field_name(sender) value:@""]; }

- (void)textCommitted:(NSTextField *)sender { [self emit:@"set" name:field_name(sender) value:sender.stringValue]; }

- (void)controlTextDidEndEditing:(NSNotification *)n {
    NSTextField *f = n.object;
    [self emit:@"set" name:field_name(f) value:f.stringValue];
}

- (void)comboBoxSelectionDidChange:(NSNotification *)n {
    NSComboBox *c = n.object;
    const NSInteger i = c.indexOfSelectedItem;
    if (i >= 0) [self emit:@"set" name:field_name(c) value:[c itemObjectValueAtIndex:i]];
}

- (void)popoverDidShow:(NSNotification *)n {
    NSWindow *pw = _popover.contentViewController.view.window;
    pw.autorecalculatesKeyViewLoop = YES;
    // The keyboard goes to the popover, not to the terminal under it, while it is open
    [pw makeKeyWindow];
    if (_firstControl) [pw makeFirstResponder:_firstControl];
}

- (void)popoverWillClose:(NSNotification *)n {
    // Commit what is being typed, a popover closes without the field it is in ending its editing
    [_popover.contentViewController.view.window makeFirstResponder:nil];
}

- (void)popoverDidClose:(NSNotification *)n {
    Py_CLEAR(_callback);
    if (tab_settings_controller == self) {
        tab_settings_controller = nil;
        [self autorelease];
    }
}

static NSTextField *
label_for(NSString *title, BOOL heading) {
    NSTextField *l = [NSTextField labelWithString:title];
    if (heading) l.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    else l.alignment = NSTextAlignmentRight;
    return l;
}

- (NSView *)controlFor:(NSDictionary *)f {
    NSString *kind = str_of(f, @"kind"), *title = str_of(f, @"title"), *value = str_of(f, @"value");
    NSControl *c = nil;
    if ([kind isEqualToString:@"choice"]) {
        NSPopUpButton *p = [[[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO] autorelease];
        for (NSArray *choice in array_of(f, @"choices")) {
            if (![choice isKindOfClass:[NSArray class]] || choice.count < 2) continue;
            // Not addItemWithTitle:, which replaces an item of the same title
            NSMenuItem *mi = [[[NSMenuItem alloc] initWithTitle:[choice[1] description] action:NULL keyEquivalent:@""] autorelease];
            [p.menu addItem:mi];
            mi.representedObject = [choice[0] description];
            if ([mi.representedObject isEqualToString:value]) [p selectItem:mi];
        }
        p.target = self; p.action = @selector(choiceChanged:);
        c = p;
    } else if ([kind isEqualToString:@"combo"]) {
        NSComboBox *cb = [[[NSComboBox alloc] init] autorelease];
        for (NSArray *choice in array_of(f, @"choices")) {
            if ([choice isKindOfClass:[NSArray class]] && choice.count) [cb addItemWithObjectValue:[choice[0] description]];
        }
        cb.stringValue = value;
        cb.completes = YES;
        cb.delegate = self;
        cb.target = self; cb.action = @selector(textCommitted:);
        [_textControls addObject:cb];
        c = cb;
    } else if ([kind isEqualToString:@"text"]) {
        NSTextField *t = [NSTextField textFieldWithString:value];
        t.placeholderString = str_of(f, @"placeholder");
        t.delegate = self;
        t.target = self; t.action = @selector(textCommitted:);
        [_textControls addObject:t];
        c = t;
    } else if ([kind isEqualToString:@"button"]) {
        c = [NSButton buttonWithTitle:title target:self action:@selector(buttonPressed:)];
    } else return nil;
    objc_setAssociatedObject(c, FIELD_NAME_KEY, str_of(f, @"name"), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    c.toolTip = str_of(f, @"help");
    [c setAccessibilityLabel:title];
    if (f[@"enabled"] && !bool_of(f, @"enabled")) c.enabled = NO;
    else if (!_firstControl) self.firstControl = c;
    if (![kind isEqualToString:@"button"]) [c.widthAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
    return c;
}

- (void)buildFrom:(NSDictionary *)model {
    self.textControls = [NSMutableArray array];
    NSGridView *grid = [NSGridView gridViewWithNumberOfColumns:2 rows:0];
    grid.rowSpacing = 6;
    grid.columnSpacing = 8;
    for (NSDictionary *f in array_of(model, @"fields")) {
        if (![f isKindOfClass:[NSDictionary class]]) continue;
        NSString *kind = str_of(f, @"kind");
        if ([kind isEqualToString:@"section"]) {
            NSGridRow *row = [grid addRowWithViews:@[label_for(str_of(f, @"title"), YES)]];
            if (grid.numberOfRows > 1) row.topPadding = 8;
            [row mergeCellsInRange:NSMakeRange(0, 2)];
            continue;
        }
        NSView *c = [self controlFor:f];
        if (!c) continue;
        NSView *label = [kind isEqualToString:@"button"] ? [NSGridCell emptyContentView] : label_for(str_of(f, @"title"), NO);
        [grid addRowWithViews:@[label, c]];
    }
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementFill;
    self.errorLabel = [NSTextField wrappingLabelWithString:@""];
    _errorLabel.textColor = [NSColor systemRedColor];
    _errorLabel.hidden = YES;
    _errorLabel.preferredMaxLayoutWidth = 300;
    NSStackView *stack = [NSStackView stackViewWithViews:@[grid, _errorLabel]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.edgeInsets = NSEdgeInsetsMake(14, 14, 14, 14);
    [stack setAccessibilityLabel:str_of(model, @"title")];
    self.view = stack;
    self.preferredContentSize = stack.fittingSize;
}
@end

static void
show_tab_settings(OSWindow *w, PyObject *model, PyObject *callback) {
    @autoreleasepool {
        NSWindow *nw = ns_window(w);
        if (!nw) return;
        if (tab_settings_controller) [tab_settings_controller.popover close];
        TabSettingsController *c = [[TabSettingsController alloc] initWithNibName:nil bundle:nil];  // released when its popover closes
        c.callback = Py_NewRef(callback);
        [c buildFrom:ns_from_python(model)];
        NSPopover *p = [[[NSPopover alloc] init] autorelease];
        p.contentViewController = c;
        p.behavior = NSPopoverBehaviorTransient;
        p.delegate = c;
        c.popover = p;
        tab_settings_controller = c;
        // At the gear of the tab when it is visible, otherwise at the top of the terminal, as in a window whose tab bar is hidden
        NSButton *gear = tab_accessory(w, false).gear;
        if (gear && gear.window && !gear.hiddenOrHasHiddenAncestor && nw.tabGroup.isTabBarVisible) {
            [p showRelativeToRect:gear.bounds ofView:gear preferredEdge:gear.isFlipped ? NSRectEdgeMaxY : NSRectEdgeMinY];
        } else {
            NSWindow *shown = nw.tabGroup.selectedWindow ? nw.tabGroup.selectedWindow : nw;
            NSView *v = shown.contentView;
            const NSRect b = v.bounds;
            const NSRect top = NSMakeRect(NSMidX(b) - 1, v.isFlipped ? NSMinY(b) : NSMaxY(b) - 1, 2, 1);
            [p showRelativeToRect:top ofView:v preferredEdge:v.isFlipped ? NSRectEdgeMaxY : NSRectEdgeMinY];
        }
    }
}

static bool
snapshot_tab(OSWindow *w, const char *path) {
    @autoreleasepool {
        NSWindow *nw = ns_window(w);
        if (!nw) return false;
        // Only the selected window of a tab group is on screen, the others have no image
        if (nw.tabGroup.selectedWindow) nw = nw.tabGroup.selectedWindow;
        // Drawing the views of the window does not draw its title bar, where the tab bar is: that is composited
        // by the window server. The window server gives a process images of its own windows without the Screen
        // Recording permission. The function is deprecated and absent from recent SDKs, hence the lookup.
        typedef CGImageRef (*CreateImage)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);
        CreateImage create = (CreateImage)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
        if (!create) return false;
        CGImageRef img = create(CGRectNull, kCGWindowListOptionIncludingWindow, (CGWindowID)nw.windowNumber, kCGWindowImageBoundsIgnoreFraming);
        if (!img) return false;
        NSBitmapImageRep *rep = [[[NSBitmapImageRep alloc] initWithCGImage:img] autorelease];
        CGImageRelease(img);
        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return [png writeToFile:@(path) atomically:YES];
    }
}
// }}}

// Find bar {{{
// The find bar of kitty/find.py: a panel at the top right of the terminal, over it as in iTerm2 and Safari, so
// that opening it does not resize the terminal. It reports what the user does to the callback.
@interface FindSearchField : NSSearchField
@property(nonatomic, assign) id findBar;
@end

@interface FindBarView : NSVisualEffectView <NSSearchFieldDelegate>
@property(nonatomic) PyObject *callback;
@property(nonatomic, retain) FindSearchField *field;
@property(nonatomic, retain) NSTextField *status;
@property(nonatomic, retain) NSSegmentedControl *arrows;
@property(nonatomic, retain) NSButton *caseButton, *regexButton, *closeButton;
@property(nonatomic) BOOL updating;
- (void)emit:(const char *)verb value:(PyObject *)value;
@end

@implementation FindSearchField
- (BOOL)becomeFirstResponder {
    const BOOL ans = [super becomeFirstResponder];
    if (ans) [(FindBarView *)_findBar emit:"focus" value:Py_True];
    return ans;
}
@end

static const CGFloat FIND_BAR_MARGIN = 8;
static const void *FIND_BAR_KEY = &FIND_BAR_KEY;

static NSButton *
find_bar_toggle(NSString *title, NSString *tip, id target, SEL action) {
    NSButton *b = [NSButton buttonWithTitle:title target:target action:action];
    b.buttonType = NSButtonTypePushOnPushOff;
    b.bezelStyle = NSBezelStyleTexturedRounded;
    b.font = [NSFont monospacedSystemFontOfSize:[NSFont smallSystemFontSize] weight:NSFontWeightSemibold];
    b.toolTip = tip;
    [b setAccessibilityLabel:tip];
    return b;
}

@implementation FindBarView

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.material = NSVisualEffectMaterialPopover;
    self.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    self.state = NSVisualEffectStateActive;
    self.wantsLayer = YES;
    self.layer.cornerRadius = 8;
    self.layer.masksToBounds = YES;
    self.layer.borderWidth = 0.5;
    self.layer.borderColor = [NSColor separatorColor].CGColor;
    [self setAccessibilityRole:NSAccessibilityGroupRole];
    [self setAccessibilityLabel:@"Find"];

    _field = [[FindSearchField alloc] init];
    _field.findBar = self;
    _field.placeholderString = @"Find";
    _field.delegate = self;
    _field.sendsSearchStringImmediately = YES;
    _field.sendsWholeSearchString = NO;
    [_field.widthAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
    [_field setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    _status = [NSTextField labelWithString:@""];
    _status.font = [NSFont monospacedDigitSystemFontOfSize:[NSFont smallSystemFontSize] weight:NSFontWeightRegular];
    _status.textColor = [NSColor secondaryLabelColor];
    _status.alignment = NSTextAlignmentRight;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    [_status.widthAnchor constraintGreaterThanOrEqualToConstant:70].active = YES;
    [_status retain];

    NSImage *up = [NSImage imageWithSystemSymbolName:@"chevron.up" accessibilityDescription:@"Find Next, up"];
    NSImage *down = [NSImage imageWithSystemSymbolName:@"chevron.down" accessibilityDescription:@"Find Previous, down"];
    _arrows = [[NSSegmentedControl segmentedControlWithImages:@[up, down] trackingMode:NSSegmentSwitchTrackingMomentary target:self action:@selector(arrowClicked:)] retain];
    _arrows.segmentStyle = NSSegmentStyleTexturedRounded;
    [_arrows setToolTip:@"Find Next: the match above, older output" forSegment:0];
    [_arrows setToolTip:@"Find Previous: the match below, newer output" forSegment:1];

    _caseButton = [find_bar_toggle(@"Aa", @"Match case", self, @selector(caseClicked:)) retain];
    _regexButton = [find_bar_toggle(@".*", @"Regular expression", self, @selector(regexClicked:)) retain];

    _closeButton = [[NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"xmark.circle.fill" accessibilityDescription:@"Done"] target:self action:@selector(closeClicked:)] retain];
    _closeButton.bordered = NO;
    _closeButton.toolTip = @"Done (Escape)";
    _closeButton.contentTintColor = [NSColor secondaryLabelColor];

    NSStackView *stack = [NSStackView stackViewWithViews:@[_field, _status, _arrows, _caseButton, _regexButton, _closeButton]];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.spacing = 6;
    stack.edgeInsets = NSEdgeInsetsMake(6, 8, 6, 8);
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor], [stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:self.topAnchor], [stack.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    return self;
}

- (void)dealloc {
    Py_CLEAR(_callback);
    [_field release];
    [_status release];
    [_arrows release];
    [_caseButton release];
    [_regexButton release];
    [_closeButton release];
    [super dealloc];
}

- (void)emit:(const char *)verb value:(PyObject *)value {
    if (!_callback || _updating) return;
    PyObject *cb = Py_NewRef(_callback);  // the callback can hide the bar
    PyObject *ret = PyObject_CallFunction(cb, "sO", verb, value ? value : Py_None);
    if (!ret) PyErr_Print();
    Py_XDECREF(ret);
    Py_DECREF(cb);
}

- (void)controlTextDidChange:(NSNotification *)n {
    (void)n;
    RAII_PyObject(q, PyUnicode_FromString(_field.stringValue.UTF8String));
    if (q) [self emit:"query" value:q];
    else PyErr_Print();
}

- (void)controlTextDidEndEditing:(NSNotification *)n {
    (void)n;
    [self emit:"focus" value:Py_False];
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)control; (void)tv;
    const BOOL shift = ([NSApp currentEvent].modifierFlags & NSEventModifierFlagShift) != 0;
    if (sel == @selector(insertNewline:) || sel == @selector(insertLineBreak:)) {
        [self emit:shift ? "previous" : "next" value:NULL];
        return YES;
    }
    if (sel == @selector(moveUp:)) { [self emit:"next" value:NULL]; return YES; }
    if (sel == @selector(moveDown:)) { [self emit:"previous" value:NULL]; return YES; }
    if (sel == @selector(cancelOperation:)) {
        [self emit:"close" value:NULL];
        return YES;
    }
    return NO;
}

- (void)arrowClicked:(NSSegmentedControl *)c { [self emit:c.selectedSegment == 0 ? "next" : "previous" value:NULL]; }
- (void)caseClicked:(NSButton *)b { [self emit:"case_sensitive" value:b.state == NSControlStateValueOn ? Py_True : Py_False]; }
- (void)regexClicked:(NSButton *)b { [self emit:"regex" value:b.state == NSControlStateValueOn ? Py_True : Py_False]; }
- (void)closeClicked:(NSButton *)b { (void)b; [self emit:"close" value:NULL]; }

- (void)applyModel:(NSDictionary *)m {
    _updating = YES;
    NSString *q = str_of(m, @"query");
    // replacing the text while the user types moves the insertion point
    if (![_field.stringValue isEqualToString:q]) _field.stringValue = q;
    _caseButton.state = bool_of(m, @"case_sensitive") ? NSControlStateValueOn : NSControlStateValueOff;
    _regexButton.state = bool_of(m, @"regex") ? NSControlStateValueOn : NSControlStateValueOff;
    _status.stringValue = q.length ? str_of(m, @"status") : @"";
    _status.textColor = bool_of(m, @"error") || ([str_of(m, @"status") isEqualToString:@"Not found"] && q.length) ? [NSColor systemRedColor] :
                        bool_of(m, @"wrapped") ? [NSColor systemOrangeColor] : [NSColor secondaryLabelColor];
    [_status setAccessibilityLabel:_status.stringValue];
    _arrows.enabled = [m[@"total"] integerValue] > 0;
    _updating = NO;
}

- (void)placeIn:(NSView *)v {
    const NSSize sz = self.fittingSize;
    const CGFloat width = MIN(MAX(sz.width, 380), MAX(v.bounds.size.width - 2 * FIND_BAR_MARGIN, 120));
    const CGFloat y = v.isFlipped ? FIND_BAR_MARGIN : NSMaxY(v.bounds) - sz.height - FIND_BAR_MARGIN;
    self.frame = NSMakeRect(NSMaxX(v.bounds) - width - FIND_BAR_MARGIN, y, width, sz.height);
    self.autoresizingMask = NSViewMinXMargin | (v.isFlipped ? NSViewMaxYMargin : NSViewMinYMargin);
}
@end

static FindBarView *
find_bar_of(NSWindow *nw) {
    return nw ? objc_getAssociatedObject(nw, FIND_BAR_KEY) : nil;
}

static bool
find_bar(OSWindow *w, PyObject *model, PyObject *callback) {
    @autoreleasepool {
        NSWindow *nw = ns_window(w);
        NSView *content = nw.contentView;
        if (!nw || !content) return false;
        FindBarView *bar = find_bar_of(nw);
        if (model == Py_None) {
            if (bar) {
                const BOOL had_keyboard = [nw.firstResponder isKindOfClass:[NSText class]] && [(NSView *)nw.firstResponder isDescendantOf:bar];
                PyObject *cb = bar.callback;
                bar.callback = NULL;
                Py_XDECREF(cb);
                [bar removeFromSuperview];
                objc_setAssociatedObject(nw, FIND_BAR_KEY, nil, OBJC_ASSOCIATION_RETAIN);
                // the keyboard goes back to the terminal
                if (had_keyboard || !nw.firstResponder || nw.firstResponder == nw) [nw makeFirstResponder:content];
            }
            return true;
        }
        NSDictionary *m = ns_from_python(model);
        if (!bar) {
            bar = [[[FindBarView alloc] initWithFrame:NSZeroRect] autorelease];
            objc_setAssociatedObject(nw, FIND_BAR_KEY, bar, OBJC_ASSOCIATION_RETAIN);
        }
        if (bar.callback != callback) {
            Py_XDECREF(bar.callback);
            bar.callback = Py_NewRef(callback);
        }
        [bar applyModel:m];
        if (bar.superview != content) {
            [content addSubview:bar positioned:NSWindowAbove relativeTo:nil];
            [bar placeIn:content];
        }
        if (bool_of(m, @"focus")) {
            bar.updating = YES;
            [nw makeFirstResponder:bar.field];
            if (bool_of(m, @"select_all")) [bar.field selectText:nil];
            bar.updating = NO;
            RAII_PyObject(r, PyObject_CallFunction(callback, "sO", "focus", Py_True));
            if (!r) PyErr_Print();
        }
        return true;
    }
}

// For tests: draw the find bar of w to a PNG file, as the screen capture of a hidden window has no image. The
// blur of the visual effect is done by the window server, it is drawn here as the window background.
static bool
snapshot_find_bar(OSWindow *w, const char *path) {
    @autoreleasepool {
        FindBarView *bar = find_bar_of(ns_window(w));
        if (!bar || !bar.layer) return false;
        [bar layoutSubtreeIfNeeded];
        [bar displayIfNeeded];
        const CGFloat scale = bar.window.backingScaleFactor > 0 ? bar.window.backingScaleFactor : 2;
        const NSSize sz = bar.bounds.size;
        NSBitmapImageRep *rep = [[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)(sz.width * scale)
            pixelsHigh:(NSInteger)(sz.height * scale) bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
            colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0] autorelease];
        if (!rep) return false;
        rep.size = sz;
        NSGraphicsContext *gc = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:gc];
        CGContextRef ctx = gc.CGContext;
        [bar.effectiveAppearance performAsCurrentDrawingAppearance:^{
            [[NSColor windowBackgroundColor] setFill];
            [[NSBezierPath bezierPathWithRoundedRect:bar.bounds xRadius:8 yRadius:8] fill];
        }];
        if (bar.isFlipped) {
            CGContextTranslateCTM(ctx, 0, sz.height);
            CGContextScaleCTM(ctx, 1, -1);
        }
        [bar.layer renderInContext:ctx];
        [NSGraphicsContext restoreGraphicsState];
        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return [png writeToFile:@(path) atomically:YES];
    }
}
// }}}

// Menus {{{
@interface NativeMenuTarget : NSObject
+ (NativeMenuTarget *)shared;
@end

@implementation NativeMenuTarget
+ (NativeMenuTarget *)shared {
    static NativeMenuTarget *ans = nil;
    if (!ans) ans = [[NativeMenuTarget alloc] init];
    return ans;
}

- (void)kitty_action:(NSMenuItem *)sender {
    NSDictionary *d = sender.representedObject;
    set_cocoa_pending_action(USER_MENU_ACTION, [str_of(d, @"action") UTF8String]);
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    NSDictionary *d = item.representedObject;
    if (d[@"enabled"] && !bool_of(d, @"enabled")) return NO;
    NSString *when = str_of(d, @"when");
    if ([when isEqualToString:@"window"] || [when isEqualToString:@"selection"]) return [NSApp keyWindow] != nil;
    return YES;
}
@end

static NSMutableArray *menu_model = nil;
static NSMutableArray<NSMenuItem *> *menu_model_items = nil;

static void
fill_menu(NSMenu *menu, NSArray *items) {
    for (NSDictionary *d in items) {
        if (bool_of(d, @"separator")) {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSMenuItem *mi = [[[NSMenuItem alloc] initWithTitle:str_of(d, @"title") action:NULL keyEquivalent:@""] autorelease];
        id sub = d[@"items"];
        if ([sub isKindOfClass:[NSArray class]]) {
            NSMenu *m = [[[NSMenu alloc] initWithTitle:mi.title] autorelease];
            fill_menu(m, sub);
            mi.submenu = m;
        } else {
            mi.action = @selector(kitty_action:);
            mi.target = [NativeMenuTarget shared];
            mi.representedObject = d;
            if (bool_of(d, @"checked")) mi.state = NSControlStateValueOn;
            NSNumber *key = d[@"key"], *mods = d[@"mods"];
            if (key && mods) {
                char kbuf[32] = {0};  // holds one UTF-8 encoded code point, at most 4 bytes, and its NUL
                int cocoa_mods = 0;
                get_cocoa_key_equivalent((uint32_t)key.unsignedIntValue, mods.intValue, kbuf, sizeof(kbuf), &cocoa_mods);
                if (kbuf[0]) {
                    mi.keyEquivalent = @(kbuf);
                    mi.keyEquivalentModifierMask = cocoa_mods;
                }
            }
        }
        [menu addItem:mi];
    }
}

void
native_ui_cocoa_add_menus(void *menu_bar) {
    NSMenu *bar = menu_bar;
    if (!menu_model_items) menu_model_items = [[NSMutableArray alloc] init];
    [menu_model_items removeAllObjects];
    if (!menu_model) return;
    NSInteger idx = bar.numberOfItems;
    // Just before Window, if it has been added
    NSMenu *wm = [NSApp windowsMenu];
    if (wm) {
        for (NSInteger i = 0; i < bar.numberOfItems; i++) {
            if ([bar itemAtIndex:i].submenu == wm) { idx = i; break; }
        }
    }
    for (NSDictionary *top in menu_model) {
        NSMenuItem *mi = [[[NSMenuItem alloc] initWithTitle:str_of(top, @"title") action:NULL keyEquivalent:@""] autorelease];
        NSMenu *m = [[[NSMenu alloc] initWithTitle:mi.title] autorelease];
        fill_menu(m, array_of(top, @"items"));
        mi.submenu = m;
        [bar insertItem:mi atIndex:idx++];
        [menu_model_items addObject:mi];
    }
}

static void
update_menu(PyObject *model) {
    @autoreleasepool {
        id m = ns_from_python(model);
        if (![m isKindOfClass:[NSArray class]]) return;
        [menu_model release];
        menu_model = [m retain];
        NSMenu *bar = [NSApp mainMenu];
        if (!bar) return;
        for (NSMenuItem *mi in menu_model_items) {
            if ([bar indexOfItem:mi] != -1) [bar removeItem:mi];
        }
        native_ui_cocoa_add_menus(bar);
    }
}
// }}}

// Settings window {{{
static const CGFloat LABEL_WIDTH = 210, ROW_GAP = 10, RIGHT_WIDTH = 620, SIDEBAR_WIDTH = 200;
static const void *FIELD_KEY = &FIELD_KEY;

@interface FlippedView : NSView
@end
@implementation FlippedView
- (BOOL)isFlipped { return YES; }
@end

@interface SettingsController : NSObject <NSWindowDelegate, NSToolbarDelegate, NSTableViewDataSource, NSTableViewDelegate, NSTextViewDelegate, NSTextFieldDelegate, NSSearchFieldDelegate>
@property(nonatomic, retain) NSWindow *window;
@property(nonatomic, retain) NSDictionary *model;
@property(nonatomic, retain) NSTableView *sidebar;
@property(nonatomic, retain) NSScrollView *sidebarScroll;
@property(nonatomic, retain) NSScrollView *rightScroll;
@property(nonatomic, retain) NSStackView *rows;
@property(nonatomic, retain) NSView *profileBar;
@property(nonatomic, retain) NSSegmentedControl *profileTabs;
@property(nonatomic, retain) NSSearchField *searchField;
@property(nonatomic, copy) NSString *pageId;
@property(nonatomic) PyObject *callback;
@property(nonatomic, retain) id keyMonitor;
@end

static SettingsController *settings_controller = nil;

@implementation SettingsController {
    NSInteger sidebarIndex;
    NSInteger profileTabIndex;
    NSString *searchText;
    NSButton *recording;
}

- (void)dealloc {
    Py_CLEAR(_callback);
    [searchText release];
    [super dealloc];
}

// Talking to Python {{{
- (NSString *)emit:(NSString *)verb args:(NSArray<NSString *> *)args {
    if (!_callback) return @"";
    RAII_PyObject(pyargs, PyTuple_New(args.count + 1));
    if (!pyargs) { PyErr_Print(); return @""; }
    PyTuple_SET_ITEM(pyargs, 0, PyUnicode_FromString(verb.UTF8String));
    for (NSUInteger i = 0; i < args.count; i++) PyTuple_SET_ITEM(pyargs, i + 1, PyUnicode_FromString(args[i].UTF8String));
    PyObject *ret = PyObject_Call(_callback, pyargs, NULL);
    if (!ret) {
        PyErr_Print();
        return @"";
    }
    NSString *ans = PyUnicode_Check(ret) ? @(PyUnicode_AsUTF8(ret)) : @"";
    Py_DECREF(ret);
    return ans;
}

- (void)showError:(NSString *)message {
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    a.messageText = @"That value cannot be used";
    a.informativeText = message;
    a.alertStyle = NSAlertStyleWarning;
    [a beginSheetModalForWindow:self.window completionHandler:nil];
}

- (BOOL)commitField:(NSMutableDictionary *)field value:(NSString *)value {
    NSString *err = [self emit:@"set" args:@[ str_of(field, @"scope"), str_of(field, @"name"), value ]];
    if (err.length) {
        [self showError:err];
        return NO;
    }
    field[@"value"] = value;
    field[@"is_set"] = @YES;
    return YES;
}
// }}}

// Building the window {{{
- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    self.window = [[[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, SIDEBAR_WIDTH + RIGHT_WIDTH + 40, 640)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO] autorelease];
    _window.title = @"Settings";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;
    _window.tabbingMode = NSWindowTabbingModeDisallowed;
    _window.minSize = NSMakeSize(SIDEBAR_WIDTH + 480, 400);
    NSToolbar *tb = [[[NSToolbar alloc] initWithIdentifier:@"silkie.settings"] autorelease];
    tb.delegate = self;
    tb.displayMode = NSToolbarDisplayModeIconAndLabel;
    tb.allowsUserCustomization = NO;
    _window.toolbar = tb;
    if (@available(macOS 11.0, *)) _window.toolbarStyle = NSWindowToolbarStylePreference;

    NSView *content = _window.contentView;
    self.sidebar = [[[NSTableView alloc] init] autorelease];
    NSTableColumn *col = [[[NSTableColumn alloc] initWithIdentifier:@"title"] autorelease];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_sidebar addTableColumn:col];
    _sidebar.headerView = nil;
    _sidebar.dataSource = self;
    _sidebar.delegate = self;
    _sidebar.rowSizeStyle = NSTableViewRowSizeStyleMedium;
    self.sidebarScroll = [[[NSScrollView alloc] init] autorelease];
    _sidebarScroll.documentView = _sidebar;
    _sidebarScroll.hasVerticalScroller = YES;
    _sidebarScroll.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_sidebarScroll];

    self.profileBar = [[[NSStackView alloc] init] autorelease];
    NSStackView *pb = (NSStackView *)_profileBar;
    pb.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    pb.spacing = 4;
    pb.translatesAutoresizingMaskIntoConstraints = NO;
    [pb addArrangedSubview:[self barButton:NSImageNameAddTemplate tip:@"New profile" action:@selector(profileNew:)]];
    [pb addArrangedSubview:[self barButton:NSImageNameRemoveTemplate tip:@"Delete profile" action:@selector(profileDelete:)]];
    NSButton *dup = [NSButton buttonWithTitle:@"Duplicate" target:self action:@selector(profileDuplicate:)];
    dup.controlSize = NSControlSizeSmall;
    [pb addArrangedSubview:dup];
    NSButton *def = [NSButton buttonWithTitle:@"Set as Default" target:self action:@selector(profileSetDefault:)];
    def.controlSize = NSControlSizeSmall;
    [pb addArrangedSubview:def];
    [content addSubview:pb];

    self.profileTabs = [[[NSSegmentedControl alloc] init] autorelease];
    _profileTabs.target = self;
    _profileTabs.action = @selector(profileTabChanged:);
    _profileTabs.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_profileTabs];

    self.rows = [[[NSStackView alloc] init] autorelease];
    _rows.orientation = NSUserInterfaceLayoutOrientationVertical;
    _rows.alignment = NSLayoutAttributeLeading;
    _rows.spacing = ROW_GAP;
    _rows.edgeInsets = NSEdgeInsetsMake(14, 14, 20, 14);
    _rows.translatesAutoresizingMaskIntoConstraints = NO;
    FlippedView *doc = [[[FlippedView alloc] init] autorelease];
    doc.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:_rows];
    self.rightScroll = [[[NSScrollView alloc] init] autorelease];
    _rightScroll.documentView = doc;
    _rightScroll.hasVerticalScroller = YES;
    _rightScroll.drawsBackground = NO;
    _rightScroll.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_rightScroll];

    [NSLayoutConstraint activateConstraints:@[
        [_sidebarScroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_sidebarScroll.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_sidebarScroll.widthAnchor constraintEqualToConstant:SIDEBAR_WIDTH],
        [_sidebarScroll.bottomAnchor constraintEqualToAnchor:pb.topAnchor constant:-4],
        [pb.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:4],
        [pb.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-6],
        [_profileTabs.topAnchor constraintEqualToAnchor:content.topAnchor constant:8],
        [_profileTabs.centerXAnchor constraintEqualToAnchor:_rightScroll.centerXAnchor],
        [_rightScroll.leadingAnchor constraintEqualToAnchor:_sidebarScroll.trailingAnchor],
        [_rightScroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_rightScroll.topAnchor constraintEqualToAnchor:_profileTabs.bottomAnchor constant:8],
        [_rightScroll.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [doc.topAnchor constraintEqualToAnchor:_rightScroll.contentView.topAnchor],
        [doc.leadingAnchor constraintEqualToAnchor:_rightScroll.contentView.leadingAnchor],
        [doc.trailingAnchor constraintEqualToAnchor:_rightScroll.contentView.trailingAnchor],
        [_rows.topAnchor constraintEqualToAnchor:doc.topAnchor],
        [_rows.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor],
        [_rows.trailingAnchor constraintEqualToAnchor:doc.trailingAnchor],
        [_rows.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor],
    ]];
    [_window center];
    return self;
}

- (NSButton *)barButton:(NSImageName)image tip:(NSString *)tip action:(SEL)action {
    NSButton *b = [NSButton buttonWithImage:[NSImage imageNamed:image] target:self action:action];
    b.bezelStyle = NSBezelStyleSmallSquare;
    b.toolTip = tip;
    return b;
}

- (NSArray *)pages { return array_of(_model, @"pages"); }

- (NSDictionary *)currentPage {
    for (NSDictionary *p in self.pages) {
        if ([str_of(p, @"id") isEqualToString:_pageId]) return p;
    }
    return self.pages.firstObject;
}

- (BOOL)isProfilesPage { return [str_of(self.currentPage, @"id") isEqualToString:@"profiles"]; }

- (NSArray *)sidebarItems {
    NSDictionary *p = self.currentPage;
    return self.isProfilesPage ? array_of(p, @"profiles") : array_of(p, @"sections");
}

- (NSDictionary *)currentProfile {
    if (!self.isProfilesPage) return nil;
    NSArray *items = self.sidebarItems;
    return sidebarIndex >= 0 && sidebarIndex < (NSInteger)items.count ? items[sidebarIndex] : nil;
}

- (void)setModelFrom:(NSDictionary *)model {
    NSString *selectedProfile = str_of(self.currentProfile, @"id");
    self.model = model;
    NSString *page = str_of(model, @"select_page");
    if (page.length) {
        self.pageId = page;
        sidebarIndex = 0;
    }
    if (!_pageId.length) self.pageId = str_of(self.pages.firstObject, @"id");
    // A profile asked for, such as the profile of a tab that its settings popover edits, wins over the one shown
    NSString *wanted = str_of(model, @"select_profile");
    if (page.length && wanted.length) selectedProfile = wanted;
    if (self.isProfilesPage && selectedProfile.length) {
        NSArray *items = self.sidebarItems;
        for (NSUInteger i = 0; i < items.count; i++) {
            if ([str_of(items[i], @"id") isEqualToString:selectedProfile]) sidebarIndex = i;
        }
        if (sidebarIndex >= (NSInteger)items.count) sidebarIndex = MAX(0, (NSInteger)items.count - 1);
    }
    NSToolbar *tb = _window.toolbar;
    if (tb.items.count == 0) {
        for (NSDictionary *p in self.pages) [tb insertItemWithItemIdentifier:str_of(p, @"id") atIndex:tb.items.count];
        [tb insertItemWithItemIdentifier:NSToolbarFlexibleSpaceItemIdentifier atIndex:tb.items.count];
        [tb insertItemWithItemIdentifier:@"search" atIndex:tb.items.count];
    }
    tb.selectedItemIdentifier = _pageId;
    [self reloadAll];
}

- (void)reloadAll {
    const BOOL searching = searchText.length > 0;
    BOOL profiles = self.isProfilesPage && !searching;
    _profileBar.hidden = !profiles;
    NSDictionary *prof = self.currentProfile;
    if (profiles && prof) {
        NSArray *tabs = array_of(prof, @"pages");
        _profileTabs.segmentCount = tabs.count;
        for (NSUInteger i = 0; i < tabs.count; i++) {
            [_profileTabs setLabel:str_of(tabs[i], @"title") forSegment:i];
            [_profileTabs setWidth:0 forSegment:i];
        }
        if (profileTabIndex >= (NSInteger)tabs.count) profileTabIndex = 0;
        _profileTabs.selectedSegment = profileTabIndex;
    }
    _profileTabs.hidden = !(profiles && prof);
    [_sidebar reloadData];
    if (sidebarIndex < _sidebar.numberOfRows) [_sidebar selectRowIndexes:[NSIndexSet indexSetWithIndex:sidebarIndex] byExtendingSelection:NO];
    [self reloadRows];
}
// }}}

// Toolbar {{{
- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar itemForItemIdentifier:(NSToolbarItemIdentifier)ident willBeInsertedIntoToolbar:(BOOL)flag {
    (void)toolbar; (void)flag;
    if ([ident isEqualToString:@"search"]) {
        self.searchField = [[[NSSearchField alloc] init] autorelease];
        _searchField.placeholderString = @"Search all settings";
        _searchField.delegate = self;
        [_searchField.widthAnchor constraintEqualToConstant:180].active = YES;
        NSToolbarItem *it = [[[NSToolbarItem alloc] initWithItemIdentifier:ident] autorelease];
        it.view = _searchField;
        it.label = @"Search";
        return it;
    }
    NSToolbarItem *it = [[[NSToolbarItem alloc] initWithItemIdentifier:ident] autorelease];
    NSString *symbol = @"gearshape";
    if ([ident isEqualToString:@"appearance"]) symbol = @"paintbrush";
    else if ([ident isEqualToString:@"profiles"]) symbol = @"person.crop.rectangle.stack";
    else if ([ident isEqualToString:@"keys"]) symbol = @"keyboard";
    else if ([ident isEqualToString:@"pointer"]) symbol = @"cursorarrow";
    else if ([ident isEqualToString:@"advanced"]) symbol = @"gearshape.2";
    if (@available(macOS 11.0, *)) it.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil];
    for (NSDictionary *p in self.pages) {
        if ([str_of(p, @"id") isEqualToString:ident]) it.label = str_of(p, @"title");
    }
    it.target = self;
    it.action = @selector(toolbarClicked:);
    return it;
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar {
    (void)toolbar;
    NSMutableArray *ans = [NSMutableArray array];
    for (NSDictionary *p in self.pages) [ans addObject:str_of(p, @"id")];
    [ans addObject:NSToolbarFlexibleSpaceItemIdentifier];
    [ans addObject:@"search"];
    return ans;
}
- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar { (void)toolbar; return @[]; }
- (NSArray<NSToolbarItemIdentifier> *)toolbarSelectableItemIdentifiers:(NSToolbar *)toolbar {
    (void)toolbar;
    NSMutableArray *ans = [NSMutableArray array];
    for (NSDictionary *p in self.pages) [ans addObject:str_of(p, @"id")];
    return ans;
}

- (void)toolbarClicked:(NSToolbarItem *)item {
    self.pageId = item.itemIdentifier;
    sidebarIndex = 0;
    profileTabIndex = 0;
    if (_searchField.stringValue.length) {
        _searchField.stringValue = @"";
        [searchText release];
        searchText = nil;
    }
    [self reloadAll];
}

- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object != _searchField) return;
    [searchText release];
    searchText = [[_searchField.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] copy];
    [self reloadAll];
}
// }}}

// Sidebar {{{
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return searchText.length ? 0 : (NSInteger)self.sidebarItems.count;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    (void)tv; (void)col;
    NSDictionary *d = self.sidebarItems[row];
    NSString *title = self.isProfilesPage ? str_of(d, @"name") : str_of(d, @"title");
    if (bool_of(d, @"is_default")) title = [title stringByAppendingString:@" ★"];
    NSTextField *t = [NSTextField labelWithString:title];
    t.lineBreakMode = NSLineBreakByTruncatingTail;
    return t;
}

- (void)tableViewSelectionDidChange:(NSNotification *)note {
    (void)note;
    const NSInteger r = _sidebar.selectedRow;
    if (r < 0 || r == sidebarIndex) return;
    sidebarIndex = r;
    [self reloadAll];
}
// }}}

// Rows {{{
static BOOL
matches(NSDictionary *field, NSString *query) {
    return [str_of(field, @"title") localizedCaseInsensitiveContainsString:query] ||
           [str_of(field, @"name") localizedCaseInsensitiveContainsString:query] ||
           [str_of(field, @"help") localizedCaseInsensitiveContainsString:query];
}

- (void)reloadRows {
    for (NSView *v in [[_rows.arrangedSubviews copy] autorelease]) {
        [_rows removeArrangedSubview:v];
        [v removeFromSuperview];
    }
    NSString *notice = str_of(_model, @"notice");
    if (notice.length) [self addCaption:notice color:NSColor.systemOrangeColor];
    if (searchText.length) {
        NSUInteger shown = 0;
        for (NSDictionary *p in self.pages) {
            for (NSDictionary *s in array_of(p, @"sections")) {
                for (NSMutableDictionary *f in array_of(s, @"fields")) {
                    if (matches(f, searchText)) {
                        [self addRow:f prefix:[NSString stringWithFormat:@"%@ › %@", str_of(p, @"title"), str_of(s, @"title")]];
                        shown++;
                    }
                }
            }
        }
        if (!shown) [self addCaption:@"Nothing matches" color:NSColor.secondaryLabelColor];
    } else if (self.isProfilesPage) {
        NSArray *tabs = array_of(self.currentProfile, @"pages");
        if (profileTabIndex < (NSInteger)tabs.count) {
            for (NSMutableDictionary *f in array_of(tabs[profileTabIndex], @"fields")) [self addRow:f prefix:nil];
        }
    } else {
        NSArray *secs = self.sidebarItems;
        if (sidebarIndex < (NSInteger)secs.count) {
            NSDictionary *s = secs[sidebarIndex];
            [self addHeading:str_of(s, @"title")];
            for (NSMutableDictionary *f in array_of(s, @"fields")) [self addRow:f prefix:nil];
        }
    }
    [_rightScroll.documentView scrollPoint:NSZeroPoint];
}

- (void)addHeading:(NSString *)title {
    NSTextField *t = [NSTextField labelWithString:title];
    t.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize] + 2];
    [_rows addArrangedSubview:t];
}

- (void)addCaption:(NSString *)text color:(NSColor *)color {
    NSTextField *t = [NSTextField wrappingLabelWithString:text];
    t.textColor = color;
    t.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    [t.widthAnchor constraintLessThanOrEqualToConstant:RIGHT_WIDTH - 20].active = YES;
    [_rows addArrangedSubview:t];
}

- (void)addRow:(NSMutableDictionary *)field prefix:(NSString *)prefix {
    NSTextField *label = [NSTextField labelWithString:str_of(field, @"title")];
    label.alignment = NSTextAlignmentRight;
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    NSString *help = str_of(field, @"help");
    label.toolTip = help.length ? help : nil;
    [label.widthAnchor constraintEqualToConstant:LABEL_WIDTH].active = YES;
    NSView *control = [self controlFor:field];
    control.toolTip = label.toolTip;

    NSButton *reset = [NSButton buttonWithTitle:@"↺" target:self action:@selector(resetClicked:)];
    reset.bezelStyle = NSBezelStyleInline;
    reset.toolTip = @"Back to the default";
    reset.hidden = !bool_of(field, @"is_set");
    objc_setAssociatedObject(reset, FIELD_KEY, field, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(control, "reset", reset, OBJC_ASSOCIATION_ASSIGN);

    NSStackView *row = [NSStackView stackViewWithViews:@[ label, control, reset ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeFirstBaseline;
    row.spacing = 8;
    NSString *caption = [self captionOf:field prefix:prefix];
    if (!caption.length) {
        [_rows addArrangedSubview:row];
        return;
    }
    NSTextField *cap = [NSTextField wrappingLabelWithString:caption];
    cap.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    cap.textColor = NSColor.secondaryLabelColor;
    [cap.widthAnchor constraintLessThanOrEqualToConstant:RIGHT_WIDTH - LABEL_WIDTH - 20].active = YES;
    NSView *spacer = [[[NSView alloc] init] autorelease];
    [spacer.widthAnchor constraintEqualToConstant:LABEL_WIDTH].active = YES;
    NSStackView *capRow = [NSStackView stackViewWithViews:@[ spacer, cap ]];
    capRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    capRow.alignment = NSLayoutAttributeTop;
    capRow.spacing = 8;
    NSStackView *both = [NSStackView stackViewWithViews:@[ row, capRow ]];
    both.orientation = NSUserInterfaceLayoutOrientationVertical;
    both.alignment = NSLayoutAttributeLeading;
    both.spacing = 2;
    [_rows addArrangedSubview:both];
}

- (NSString *)captionOf:(NSDictionary *)field prefix:(NSString *)prefix {
    NSMutableArray *parts = [NSMutableArray array];
    if (prefix.length) [parts addObject:prefix];
    NSString *help = str_of(field, @"help");
    NSRange end = [help rangeOfString:@". "];
    NSRange para = [help rangeOfString:@"\n"];
    NSUInteger cut = help.length;
    if (end.location != NSNotFound) cut = MIN(cut, end.location + 1);
    if (para.location != NSNotFound) cut = MIN(cut, para.location);
    if (cut) [parts addObject:[help substringToIndex:cut]];
    if (bool_of(field, @"restart")) [parts addObject:@"Applies after a restart."];
    return [parts componentsJoinedByString:@" "];
}

- (NSView *)controlFor:(NSMutableDictionary *)field {
    NSString *kind = str_of(field, @"kind"), *value = str_of(field, @"value");
    NSControl *c = nil;
    if ([kind isEqualToString:@"bool"]) {
        NSSwitch *s = [[[NSSwitch alloc] init] autorelease];
        NSString *lv = value.lowercaseString;
        s.state = ([lv isEqualToString:@"yes"] || [lv isEqualToString:@"y"] || [lv isEqualToString:@"true"]) ? NSControlStateValueOn : NSControlStateValueOff;
        s.action = @selector(switchChanged:);
        c = s;
    } else if ([kind isEqualToString:@"choice"]) {
        NSPopUpButton *p = [[[NSPopUpButton alloc] init] autorelease];
        [p addItemsWithTitles:array_of(field, @"choices")];
        [p selectItemWithTitle:value];
        p.action = @selector(popupChanged:);
        c = p;
    } else if ([kind isEqualToString:@"color"]) {
        return [self colorControlFor:field];
    } else if ([kind isEqualToString:@"font"]) {
        NSComboBox *b = [[[NSComboBox alloc] init] autorelease];
        [b addItemsWithObjectValues:array_of(_model, @"families")];
        b.numberOfVisibleItems = 16;
        b.completes = YES;
        b.stringValue = value;
        b.action = @selector(textChanged:);
        [b.widthAnchor constraintEqualToConstant:280].active = YES;
        c = b;
    } else if ([kind isEqualToString:@"lines"]) {
        return [self linesControlFor:field];
    } else if ([kind isEqualToString:@"shortcut"]) {
        return [self shortcutControlFor:field];
    } else if ([kind isEqualToString:@"int"] || [kind isEqualToString:@"float"]) {
        NSTextField *t = [NSTextField textFieldWithString:value];
        t.action = @selector(textChanged:);
        t.target = self;
        [t.widthAnchor constraintEqualToConstant:80].active = YES;
        objc_setAssociatedObject(t, FIELD_KEY, field, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        NSStepper *st = [[[NSStepper alloc] init] autorelease];
        // A relative direction control, not a bounded copy of the setting.
        // Python performs exact decimal stepping and domain validation.
        st.minValue = -1;
        st.maxValue = 1;
        st.increment = 1;
        st.doubleValue = 0;
        st.target = self;
        st.action = @selector(stepperChanged:);
        objc_setAssociatedObject(st, "text", t, OBJC_ASSOCIATION_ASSIGN);
        objc_setAssociatedObject(st, FIELD_KEY, field, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        NSStackView *row = [NSStackView stackViewWithViews:@[ t, st ]];
        row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        row.spacing = 2;
        return row;
    } else {
        NSTextField *t = [NSTextField textFieldWithString:value];
        t.action = @selector(textChanged:);
        [t.widthAnchor constraintEqualToConstant:280].active = YES;
        c = t;
    }
    c.target = self;
    objc_setAssociatedObject(c, FIELD_KEY, field, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return c;
}

static NSColor *
color_from_hex(NSString *s) {
    if (![s hasPrefix:@"#"] || s.length != 7) return nil;
    unsigned v = 0;
    if (![[NSScanner scannerWithString:[s substringFromIndex:1]] scanHexInt:&v]) return nil;
    return [NSColor colorWithSRGBRed:((v >> 16) & 0xff) / 255.0 green:((v >> 8) & 0xff) / 255.0 blue:(v & 0xff) / 255.0 alpha:1];
}

static NSString *
hex_from_color(NSColor *c) {
    NSColor *q = [c colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    return [NSString stringWithFormat:@"#%02x%02x%02x", (int)lround(q.redComponent * 255), (int)lround(q.greenComponent * 255), (int)lround(q.blueComponent * 255)];
}

- (NSView *)colorControlFor:(NSMutableDictionary *)field {
    NSString *value = str_of(field, @"value");
    NSColor *color = color_from_hex(value);
    NSMutableArray *views = [NSMutableArray array];
    NSColorWell *well = [[[NSColorWell alloc] init] autorelease];
    [well.widthAnchor constraintEqualToConstant:44].active = YES;
    [well.heightAnchor constraintEqualToConstant:22].active = YES;
    if (color) well.color = color;
    well.target = self;
    well.action = @selector(colorChanged:);
    objc_setAssociatedObject(well, FIELD_KEY, field, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [views addObject:well];
    if (bool_of(field, @"allow_none")) {
        NSButton *none = [NSButton checkboxWithTitle:@"None" target:self action:@selector(noneToggled:)];
        none.state = [value isEqualToString:@"none"] ? NSControlStateValueOn : NSControlStateValueOff;
        well.enabled = none.state != NSControlStateValueOn;
        objc_setAssociatedObject(none, FIELD_KEY, field, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(none, "well", well, OBJC_ASSOCIATION_ASSIGN);
        [views addObject:none];
    }
    NSStackView *row = [NSStackView stackViewWithViews:views];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 8;
    return row;
}

- (NSView *)linesControlFor:(NSMutableDictionary *)field {
    NSScrollView *sv = [[[NSScrollView alloc] init] autorelease];
    NSTextView *tv = [[[NSTextView alloc] init] autorelease];
    tv.font = [NSFont userFixedPitchFontOfSize:[NSFont smallSystemFontSize]];
    tv.string = str_of(field, @"value");
    tv.delegate = self;
    tv.richText = NO;
    tv.automaticQuoteSubstitutionEnabled = NO;
    tv.automaticDashSubstitutionEnabled = NO;
    tv.verticallyResizable = YES;
    tv.autoresizingMask = NSViewWidthSizable;
    objc_setAssociatedObject(tv, FIELD_KEY, field, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    sv.documentView = tv;
    sv.hasVerticalScroller = YES;
    sv.borderType = NSBezelBorder;
    [sv.widthAnchor constraintEqualToConstant:360].active = YES;
    [sv.heightAnchor constraintEqualToConstant:88].active = YES;
    tv.frame = NSMakeRect(0, 0, 360, 88);
    return sv;
}

- (NSView *)shortcutControlFor:(NSMutableDictionary *)field {
    NSTextField *t = [NSTextField textFieldWithString:str_of(field, @"value")];
    t.placeholderString = @"No shortcut";
    t.target = self;
    t.action = @selector(textChanged:);
    objc_setAssociatedObject(t, FIELD_KEY, field, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [t.widthAnchor constraintEqualToConstant:220].active = YES;
    NSButton *rec = [NSButton buttonWithTitle:@"Record" target:self action:@selector(recordClicked:)];
    rec.controlSize = NSControlSizeSmall;
    objc_setAssociatedObject(rec, "text", t, OBJC_ASSOCIATION_ASSIGN);
    objc_setAssociatedObject(rec, FIELD_KEY, field, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSStackView *row = [NSStackView stackViewWithViews:@[ t, rec ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 6;
    return row;
}
// }}}

// Control actions {{{
- (void)markSet:(NSMutableDictionary *)field control:(NSView *)control {
    NSButton *reset = objc_getAssociatedObject(control, "reset");
    if (reset) reset.hidden = !bool_of(field, @"is_set");
}

- (void)switchChanged:(NSSwitch *)s {
    NSMutableDictionary *f = objc_getAssociatedObject(s, FIELD_KEY);
    if (![self commitField:f value:s.state == NSControlStateValueOn ? @"yes" : @"no"]) return;
    [self markSet:f control:s];
}

- (void)popupChanged:(NSPopUpButton *)p {
    NSMutableDictionary *f = objc_getAssociatedObject(p, FIELD_KEY);
    if ([self commitField:f value:p.titleOfSelectedItem]) [self markSet:f control:p];
    else [p selectItemWithTitle:str_of(f, @"value")];
}

- (void)textChanged:(NSControl *)t {
    NSMutableDictionary *f = objc_getAssociatedObject(t, FIELD_KEY);
    NSString *val = [t.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if ([val isEqualToString:str_of(f, @"value")]) return;
    if ([self commitField:f value:val]) [self markSet:f control:t];
    else t.stringValue = str_of(f, @"value");
}

- (void)stepperChanged:(NSStepper *)st {
    NSMutableDictionary *f = objc_getAssociatedObject(st, FIELD_KEY);
    NSTextField *t = objc_getAssociatedObject(st, "text");
    NSString *direction = st.doubleValue < 0 ? @"-1" : @"1";
    st.doubleValue = 0;
    NSString *val = [self emit:@"adjust" args:@[str_of(f, @"name"), t.stringValue, direction]];
    if (!val.length || [val isEqualToString:str_of(f, @"value")]) return;
    if ([self commitField:f value:val]) {
        t.stringValue = val;
        [self markSet:f control:t];
    } else t.stringValue = str_of(f, @"value");
}

- (void)colorChanged:(NSColorWell *)well {
    NSMutableDictionary *f = objc_getAssociatedObject(well, FIELD_KEY);
    [self commitField:f value:hex_from_color(well.color)];
}

- (void)noneToggled:(NSButton *)none {
    NSMutableDictionary *f = objc_getAssociatedObject(none, FIELD_KEY);
    NSColorWell *well = objc_getAssociatedObject(none, "well");
    const BOOL on = none.state == NSControlStateValueOn;
    well.enabled = !on;
    [self commitField:f value:on ? @"none" : hex_from_color(well.color)];
}

- (void)textDidEndEditing:(NSNotification *)note {
    NSTextView *tv = note.object;
    NSMutableDictionary *f = objc_getAssociatedObject(tv, FIELD_KEY);
    if (!f || [tv.string isEqualToString:str_of(f, @"value")]) return;
    if (![self commitField:f value:tv.string]) tv.string = str_of(f, @"value");
}

- (void)resetClicked:(NSButton *)b {
    NSDictionary *f = objc_getAssociatedObject(b, FIELD_KEY);
    [self emit:@"reset" args:@[ str_of(f, @"scope"), str_of(f, @"name") ]];
}

static NSString *
key_name_for_event(NSEvent *ev) {
    NSMutableArray *parts = [NSMutableArray array];
    const NSEventModifierFlags m = ev.modifierFlags;
    if (m & NSEventModifierFlagControl) [parts addObject:@"ctrl"];
    if (m & NSEventModifierFlagOption) [parts addObject:@"alt"];
    if (m & NSEventModifierFlagShift) [parts addObject:@"shift"];
    if (m & NSEventModifierFlagCommand) [parts addObject:@"cmd"];
    NSString *key = ev.charactersIgnoringModifiers.lowercaseString;
    switch (ev.keyCode) {
        case 36: key = @"enter"; break;
        case 48: key = @"tab"; break;
        case 49: key = @"space"; break;
        case 51: key = @"backspace"; break;
        case 53: key = @"escape"; break;
        case 117: key = @"delete"; break;
        case 115: key = @"home"; break;
        case 119: key = @"end"; break;
        case 116: key = @"page_up"; break;
        case 121: key = @"page_down"; break;
        case 123: key = @"left"; break;
        case 124: key = @"right"; break;
        case 125: key = @"down"; break;
        case 126: key = @"up"; break;
        case 122: key = @"f1"; break;
        case 120: key = @"f2"; break;
        case 99: key = @"f3"; break;
        case 118: key = @"f4"; break;
        case 96: key = @"f5"; break;
        case 97: key = @"f6"; break;
        case 98: key = @"f7"; break;
        case 100: key = @"f8"; break;
        case 101: key = @"f9"; break;
        case 109: key = @"f10"; break;
        case 103: key = @"f11"; break;
        case 111: key = @"f12"; break;
    }
    if (!key.length) return nil;
    [parts addObject:key];
    return [parts componentsJoinedByString:@"+"];
}

- (void)recordClicked:(NSButton *)rec {
    [self stopRecording];
    recording = rec;
    rec.title = @"Press keys…";
    NSMutableDictionary *f = objc_getAssociatedObject(rec, FIELD_KEY);
    NSTextField *t = objc_getAssociatedObject(rec, "text");
    self.keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *ev) {
      NSString *name = key_name_for_event(ev);
      [self stopRecording];
      if (name && ev.keyCode != 53 /* escape cancels */) {
          if ([self commitField:f value:name]) t.stringValue = name;
      }
      return nil;
    }];
}

- (void)stopRecording {
    if (_keyMonitor) {
        [NSEvent removeMonitor:_keyMonitor];
        self.keyMonitor = nil;
    }
    if (recording) recording.title = @"Record";
    recording = nil;
}

- (void)profileNew:(id)sender {
    (void)sender;
    [self emit:@"profile_new" args:@[ @"New Profile" ]];
}
- (void)profileDuplicate:(id)sender {
    (void)sender;
    [self emit:@"profile_duplicate" args:@[ str_of(self.currentProfile, @"id") ]];
}
- (void)profileDelete:(id)sender {
    (void)sender;
    NSDictionary *p = self.currentProfile;
    if (!bool_of(p, @"can_delete")) {
        [self showError:@"The default profile cannot be deleted"];
        return;
    }
    NSString *pid = [[str_of(p, @"id") copy] autorelease];
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    a.messageText = [NSString stringWithFormat:@"Delete the profile “%@”?", str_of(p, @"name")];
    a.informativeText = @"Windows using it keep running with the default profile.";
    [a addButtonWithTitle:@"Delete"];
    [a addButtonWithTitle:@"Cancel"];
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
      if (r == NSAlertFirstButtonReturn) {
          NSString *err = [self emit:@"profile_delete" args:@[ pid ]];
          if (err.length) [self showError:err];
      }
    }];
}
- (void)profileSetDefault:(id)sender {
    (void)sender;
    [self emit:@"profile_set_default" args:@[ str_of(self.currentProfile, @"id") ]];
}
- (void)profileTabChanged:(NSSegmentedControl *)c {
    profileTabIndex = c.selectedSegment;
    [self reloadRows];
}
// }}}

- (void)windowWillClose:(NSNotification *)note {
    (void)note;
    [self stopRecording];
    [_window makeFirstResponder:nil]; // commits an edit in progress
    [[NSColorPanel sharedColorPanel] orderOut:nil];
}
@end

static void
show_settings(PyObject *model, PyObject *callback) {
    @autoreleasepool {
        if (!settings_controller) settings_controller = [[SettingsController alloc] init];
        Py_XDECREF(settings_controller.callback);
        settings_controller.callback = Py_NewRef(callback);
        [settings_controller setModelFrom:ns_from_python(model)];
        [settings_controller.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

static void
update_settings(PyObject *model) {
    @autoreleasepool {
        if (settings_controller && settings_controller.window.visible) [settings_controller setModelFrom:ns_from_python(model)];
    }
}

static bool
snapshot_settings(const char *path) {
    @autoreleasepool {
        if (!settings_controller) return false;
        // The frame view, so that the toolbar is included
        NSView *v = settings_controller.window.contentView.superview;
        [v layoutSubtreeIfNeeded];
        NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
        if (!rep) return false;
        [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return [png writeToFile:@(path) atomically:YES];
    }
}
// }}}

static bool
show_text_review(OSWindow *w, const char *title, const char *text) {
    NSWindow *parent = ns_window(w);
    if (!parent) return false;
    NSAlert *alert = [[[NSAlert alloc] init] autorelease];
    alert.messageText = @(title);
    [alert addButtonWithTitle:@"Close"].keyEquivalent = @"\033";
    NSScrollView *scroll = [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 480, 280)] autorelease];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    NSTextView *view = [[[NSTextView alloc] initWithFrame:(NSRect){NSZeroPoint, scroll.contentSize}] autorelease];
    view.string = @(text);
    view.font = [NSFont systemFontOfSize:[NSFont systemFontSize]];
    view.editable = NO;
    view.selectable = YES;
    view.richText = NO;
    view.verticallyResizable = YES;
    view.autoresizingMask = NSViewWidthSizable;
    view.textContainer.widthTracksTextView = YES;
    view.accessibilityLabel = @(title);
    scroll.documentView = view;
    alert.accessoryView = scroll;
    alert.window.initialFirstResponder = view;
    [alert beginSheetModalForWindow:parent completionHandler:nil];
    return true;
}

const NativeUIBackend *
native_ui_cocoa_backend(void) {
    static const NativeUIBackend b = {
        .adopt_os_window = adopt_os_window,
        .attach_tab = attach_tab,
        .select_tab = select_tab,
        .move_tab = move_tab,
        .detach_tab = detach_tab,
        .tab_members = tab_members,
        .tab_status = tab_status,
        .tab_settings_button = tab_settings_button,
        .show_tab_settings = show_tab_settings,
        .snapshot_tab = snapshot_tab,
        .show_settings = show_settings,
        .update_settings = update_settings,
        .update_menu = update_menu,
        .snapshot_settings = snapshot_settings,
        .find_bar = find_bar,
        .snapshot_find_bar = snapshot_find_bar,
        .show_text_review = show_text_review,
    };
    return &b;
}

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
#include <objc/runtime.h>

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

static bool
adopt_os_window(OSWindow *w, OSWindow *parent, const char *location) {
    NSWindow *nw = ns_window(w);
    if (!nw) return false;
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
            NSNumber *key = d[@"key"], *mods = d[@"mods"];
            if (key && mods) {
                char kbuf[32] = {0};
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
        st.minValue = -1e9;
        st.maxValue = 1e9;
        st.increment = [value doubleValue] > 0 && [value doubleValue] < 1 ? 0.05 : 1;
        st.doubleValue = value.doubleValue;
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
    NSString *val = [str_of(f, @"kind") isEqualToString:@"int"] ? [NSString stringWithFormat:@"%ld", (long)lround(st.doubleValue)] : [NSString stringWithFormat:@"%g", st.doubleValue];
    if ([self commitField:f value:val]) {
        t.stringValue = val;
        [self markSet:f control:t];
    } else st.doubleValue = str_of(f, @"value").doubleValue;
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

const NativeUIBackend *
native_ui_cocoa_backend(void) {
    static const NativeUIBackend b = {
        .adopt_os_window = adopt_os_window,
        .attach_tab = attach_tab,
        .select_tab = select_tab,
        .move_tab = move_tab,
        .detach_tab = detach_tab,
        .tab_members = tab_members,
        .show_settings = show_settings,
        .update_settings = update_settings,
        .update_menu = update_menu,
        .snapshot_settings = snapshot_settings,
    };
    static bool installed = false;
    if (!installed && [NSApp delegate]) {
        installed = true;
        class_addMethod(object_getClass([NSApp delegate]), @selector(newWindowForTab:), (IMP)new_window_for_tab, "v@:@");
    }
    return &b;
}

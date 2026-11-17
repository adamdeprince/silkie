/*
 * gtk_ui.c
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

// The native user interface on Linux, the counterpart of native_ui_cocoa.m. GTK 3 is available and themed
// to match on both GNOME and UKUI (Kylin), which is why it is the toolkit here. This is a module that is loaded
// at runtime, so that the terminal itself does not depend on GTK. Windows hold a menu bar and a GtkNotebook
// whose pages hold the windows of the terminal, which keep doing all their own drawing and input.

#include "native_ui_api.h"
#include <gtk/gtk.h>
#include <poll.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

// A HarfBuzz built without GLib, as the terminal bundles where the system one is too old (Kylin), lacks these two
// and the Pango of the system needs them to load. hb_script_t values are the ISO 15924 tags, so they are easy to provide.
__attribute__((visibility("default"))) uint32_t hb_glib_script_to_script(GUnicodeScript script);
__attribute__((visibility("default"))) GUnicodeScript hb_glib_script_from_script(uint32_t script);
uint32_t hb_glib_script_to_script(GUnicodeScript script) { return g_unicode_script_to_iso15924(script); }
GUnicodeScript hb_glib_script_from_script(uint32_t script) { return g_unicode_script_from_iso15924(script); }

static const NativeUIHost *host = NULL;
static bool shares_wayland_display = false;

// The event loop {{{
// GLib sources are serviced by the event loop of the terminal, once per iteration, before it prepares to read the
// display. Nothing runs a GLib main loop, so opening a menu or a dialog does not stop the terminal from running.
typedef struct {
    int fd;
    int events;
    unsigned long long watch_id;
} ExternalFD;

static ExternalFD *external_fds = NULL;
static size_t num_external_fds = 0, external_fds_capacity = 0;
static GPollFD *poll_fds = NULL;
static gint poll_fds_capacity = 0;

static void
on_fd_ready(int fd, int events, void *data) {
    // Only here to wake up the event loop, the sources are dispatched by the hook on the next iteration
    (void)fd; (void)events; (void)data;
}

static void
sync_fd_watches(const GPollFD *fds, gint n) {
    for (size_t i = 0; i < num_external_fds;) {
        bool found = false;
        for (gint j = 0; j < n && !found; j++) found = fds[j].fd == external_fds[i].fd && fds[j].events == external_fds[i].events;
        if (found) { i++; continue; }
        host->remove_fd_watch(external_fds[i].watch_id);
        external_fds[i] = external_fds[--num_external_fds];
    }
    for (gint j = 0; j < n; j++) {
        bool found = false;
        for (size_t i = 0; i < num_external_fds && !found; i++) found = fds[j].fd == external_fds[i].fd && fds[j].events == external_fds[i].events;
        if (found) continue;
        unsigned long long id = host->add_fd_watch(fds[j].fd, fds[j].events, on_fd_ready, NULL);
        if (!id) {
            // The event loop has a fixed number of slots for the poll() set. The source still runs on the next wakeup.
            g_warning("No room in the event loop to watch fd: %d", fds[j].fd);
            continue;
        }
        if (num_external_fds >= external_fds_capacity) {
            size_t cap = external_fds_capacity ? external_fds_capacity * 2 : 8;
            ExternalFD *q = g_try_renew(ExternalFD, external_fds, cap);
            if (!q) { host->remove_fd_watch(id); return; }
            external_fds = q; external_fds_capacity = cap;
        }
        external_fds[num_external_fds++] = (ExternalFD){.fd = fds[j].fd, .events = fds[j].events, .watch_id = id};
    }
}

static int64_t
event_loop_hook(int64_t timeout) {
    GMainContext *ctx = g_main_context_default();
    // Sources can make other sources ready. A source that is always ready must not stop the terminal from
    // running, and does not need to: the loop then does not wait, and comes back here at once.
    bool more = true;
    for (int i = 0; i < 64 && (more = g_main_context_iteration(ctx, FALSE)); i++);
    if (more) return 0;
    gint priority = 0, source_timeout = -1;
    g_main_context_prepare(ctx, &priority);
    gint n;
    while ((n = g_main_context_query(ctx, priority, &source_timeout, poll_fds, poll_fds_capacity)) > poll_fds_capacity) {
        poll_fds_capacity = n * 2;
        poll_fds = g_renew(GPollFD, poll_fds, poll_fds_capacity);
    }
    for (gint i = 0; i < n; i++) poll_fds[i].revents = 0;
    bool ready = g_main_context_check(ctx, priority, poll_fds, n);
    sync_fd_watches(poll_fds, n);
    if (ready) { g_main_context_dispatch(ctx); return 0; }
    if (source_timeout >= 0) {
        const int64_t ns = (int64_t)source_timeout * 1000000;
        if (timeout < 0 || ns < timeout) return ns;
    }
    return timeout;
}
// }}}

// Python helpers {{{
static const char *
str_item(PyObject *d, const char *key) {
    PyObject *v = d ? PyDict_GetItemString(d, key) : NULL;
    if (!v || !PyUnicode_Check(v)) return "";
    const char *s = PyUnicode_AsUTF8(v);
    if (!s) { PyErr_Clear(); return ""; }
    return s;
}

static bool
bool_item(PyObject *d, const char *key) {
    PyObject *v = d ? PyDict_GetItemString(d, key) : NULL;
    return v && PyObject_IsTrue(v) == 1;
}

static PyObject *
list_item(PyObject *d, const char *key) {
    PyObject *v = d ? PyDict_GetItemString(d, key) : NULL;
    return v && PyList_Check(v) ? v : NULL;
}

static void
drop_ref(gpointer p) {
    PyObject *o = p;
    Py_XDECREF(o);
}

#define for_each_in(list, var)                                                                    \
    for (Py_ssize_t var##_i = 0, var##_n = (list) ? PyList_GET_SIZE(list) : 0; var##_i < var##_n; var##_i++) \
        for (PyObject *var = PyList_GET_ITEM(list, var##_i); var; var = NULL)
// }}}

static void
free_closure_data(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

// Menus {{{
static PyObject *menu_model = NULL;

static void
on_menu_activate(GtkMenuItem *item, gpointer data) {
    (void)item;
    host->run_action((const char *)data);
}

static GtkWidget *menu_from_items(PyObject *items);

static GtkWidget *
menu_item_from(PyObject *d) {
    if (bool_item(d, "separator")) return gtk_separator_menu_item_new();
    PyObject *sub = list_item(d, "items");
    const char *shortcut = str_item(d, "shortcut");
    GtkWidget *item = gtk_menu_item_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    GtkWidget *label = gtk_label_new(str_item(d, "title"));
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    if (shortcut[0]) {
        GtkWidget *keys = gtk_label_new(shortcut);
        gtk_style_context_add_class(gtk_widget_get_style_context(keys), "dim-label");
        gtk_box_pack_end(GTK_BOX(box), keys, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(item), box);
    if (sub) gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu_from_items(sub));
    else g_signal_connect_data(item, "activate", G_CALLBACK(on_menu_activate), g_strdup(str_item(d, "action")), free_closure_data, 0);
    return item;
}

static GtkWidget *
menu_from_items(PyObject *items) {
    GtkWidget *menu = gtk_menu_new();
    for_each_in(items, d) gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item_from(d));
    return menu;
}

static void
fill_menubar(GtkWidget *bar) {
    GList *old = gtk_container_get_children(GTK_CONTAINER(bar));
    for (GList *x = old; x; x = x->next) gtk_widget_destroy(x->data);
    g_list_free(old);
    if (menu_model && PyList_Check(menu_model)) {
        for_each_in(menu_model, top) {
            GtkWidget *mi = gtk_menu_item_new_with_label(str_item(top, "title"));
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu_from_items(list_item(top, "items")));
            gtk_menu_shell_append(GTK_MENU_SHELL(bar), mi);
        }
    }
    gtk_widget_show_all(bar);
}
// }}}

// Native windows and their tabs {{{
#ifdef GDK_WINDOWING_X11
struct Page;
typedef struct HostWindow {
    GtkWidget *window, *topbar, *menubar, *notebook;
    bool fullscreen, maximized;
    // The find bar, at the end of the menu bar so that showing it does not resize the terminal. It shows the find
    // state of the current page, see find_bar(). find_keyboard is set while its entry has the keyboard.
    GtkWidget *find_revealer, *find_keys, *find_entry, *find_status, *find_up, *find_down, *find_case, *find_regex;
    bool find_keyboard, find_updating;
    // The size asked for is that of the terminal, to which the menu bar has to be added once it has content
    int wanted_width, wanted_height;
    bool initial_size_done;
    struct Page *shown_page; // Wayland: the page whose terminal is mapped
} HostWindow;

// The program status badge of a tab, see update_native_tab_status() in kitty/native_ui.py. Kept with the
// page as the widgets of its tab are made again whenever it moves to another window.
typedef struct Badge {
    char *state, *kind, *text, *tooltip, *accessibility; // state is NULL when there is no badge
} Badge;

typedef struct Page {
    unsigned long long id; // not a pointer, OS windows move in memory when others are added or removed
    Window xid;
    HostWindow *hw;
    GtkWidget *area, *label, *tab;
    int last_width, last_height;
    Badge badge;
    GtkWidget *badge_box, *badge_indicator, *badge_label;
    char *badge_indicator_for; // the state and kind that badge_indicator shows
    bool wants_gear; // the gear that opens the settings of the tab, see tab_settings_button()
    GtkWidget *gear;
    // The find bar of this tab, see find_bar(): the model to show and where to report, NULL when it is hidden
    PyObject *find_model, *find_callback;
} Page;

static struct OSWindow *
os_window_of_page(const Page *p) {
    return host->os_window_for_id(p->id);
}

// The open tab settings popover, only one is open at a time, see show_tab_settings()
static GtkWidget *tab_settings_popover = NULL;

static gboolean
swallow_key_event(GtkWidget *w, GdkEventKey *e, gpointer data) {
    // On Wayland the toolkit also gets the keys typed in the terminal, which is the keyboard focus of its window. They
    // are for the terminal: without this F10 would open the menu bar. Not while the tab settings popover is open, nor
    // while the find bar has the keyboard, then the terminal drops them, see handle_key() in kitty/find.py.
    (void)e;
    HostWindow *hw = data;
    if (hw && hw->find_keyboard) return FALSE;
    return !(tab_settings_popover && gtk_widget_get_toplevel(tab_settings_popover) == w);
}

static GHashTable *pages = NULL;   // of Page by OS window id
static GList *host_windows = NULL; // of HostWindow

static Display *
gdk_xdisplay(void) {
    return GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
}

static Page *
page_of(struct OSWindow *w) {
    if (!w || !pages) return NULL;
    unsigned long long id = host->os_window_id(w);
    return g_hash_table_lookup(pages, &id);
}

static Page *
page_at(HostWindow *hw, gint n) {
    GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(hw->notebook), n);
    return child ? g_object_get_data(G_OBJECT(child), "page") : NULL;
}

static Page *
current_page(HostWindow *hw) {
    return page_at(hw, gtk_notebook_get_current_page(GTK_NOTEBOOK(hw->notebook)));
}

static void
give_input_focus(Page *p) {
    // The windows of the terminal read keyboard, pointer and tablet events directly from the X
    // server, which must therefore have the input focus for them.
    GdkDisplay *d = gdk_display_get_default();
    Display *x = gdk_xdisplay();
    gdk_x11_display_error_trap_push(d);
    // Focusing a window that is not viewable is an error, for instance while its page is still being shown
    XWindowAttributes attrs;
    if (XGetWindowAttributes(x, p->xid, &attrs) && attrs.map_state == IsViewable) XSetInputFocus(x, p->xid, RevertToParent, CurrentTime);
    // Errors must arrive within the trap: glfw and GTK share the process wide X error handler, and when
    // glfw has last touched it the handler that an error gets is the one that exits the process
    XSync(x, False);
    gdk_x11_display_error_trap_pop_ignored(d);
}

static gboolean
focus_current_page(gpointer data) {
    HostWindow *hw = data;
    // The popover has the keyboard while it is open, it hands it back when it closes, and so does the find bar
    if (tab_settings_popover && gtk_widget_get_toplevel(tab_settings_popover) == hw->window) return G_SOURCE_REMOVE;
    if (g_list_find(host_windows, hw) && hw->find_keyboard) return G_SOURCE_REMOVE;
    if (g_list_find(host_windows, hw) && gtk_window_is_active(GTK_WINDOW(hw->window))) {
        Page *p = current_page(hw);
        if (p) give_input_focus(p);
    }
    return G_SOURCE_REMOVE;
}

static void
update_window_title(HostWindow *hw) {
    Page *p = current_page(hw);
    gtk_window_set_title(GTK_WINDOW(hw->window), p ? gtk_label_get_text(GTK_LABEL(p->label)) : "Silkie");
}

static void
update_chrome(HostWindow *hw) {
    const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(hw->notebook));
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(hw->notebook), n > 1 && !hw->fullscreen);
    gtk_widget_set_visible(hw->menubar, !hw->fullscreen);
    // in full screen the bar row is there only while the find bar is shown
    gtk_widget_set_visible(hw->topbar, !hw->fullscreen || gtk_revealer_get_reveal_child(GTK_REVEALER(hw->find_revealer)));
}

// Find bar {{{
// The find bar of kitty/find.py. It is at the end of the menu bar row: GTK cannot draw over the terminal, which
// is an X window or a Wayland subsurface of its own, and a bar above it would resize it whenever it is shown.
static void
find_emit(HostWindow *hw, const char *verb, PyObject *value) {
    if (hw->find_updating) return;
    Page *p = current_page(hw);
    if (!p || !p->find_callback) return;
    PyObject *cb = Py_NewRef(p->find_callback);  // the callback can hide the bar
    PyObject *ret = PyObject_CallFunction(cb, "sO", verb, value ? value : Py_None);
    if (!ret) PyErr_Print();
    Py_XDECREF(ret);
    Py_DECREF(cb);
}

// The terminal window has the X input focus. Giving it to the toplevel is not enough: X sends the keys to the
// window under the pointer when that is inside the focus window, which the terminal usually is. So the find bar
// has an X window of its own, beside the terminal, that gets the focus, once it is mapped. GTK sends the keys of
// any of its windows to the focus widget, the entry.
static void
focus_find_window(HostWindow *hw) {
    GdkWindow *gw = gtk_widget_get_window(hw->find_keys);
    if (!gw || !gdk_window_is_viewable(gw)) return;  // see on_find_mapped()
    GdkDisplay *d = gdk_display_get_default();
    gdk_x11_display_error_trap_push(d);
    XSetInputFocus(gdk_xdisplay(), gdk_x11_window_get_xid(gw), RevertToParent, CurrentTime);
    XSync(gdk_xdisplay(), False);
    gdk_x11_display_error_trap_pop_ignored(d);
}

// The bar has a window of its own, which is not painted by its parent: paint it as the menu bar beside it
static gboolean
on_find_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    HostWindow *hw = data;
    const int width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
    gtk_render_background(gtk_widget_get_style_context(hw->window), cr, 0, 0, width, height);
    gtk_render_background(gtk_widget_get_style_context(hw->menubar), cr, 0, 0, width, height);
    return FALSE;
}

static gboolean
on_find_mapped(GtkWidget *w, GdkEvent *e, gpointer data) {
    (void)w; (void)e;
    HostWindow *hw = data;
    if (hw->find_keyboard && !host->is_wayland) focus_find_window(hw);
    return FALSE;
}

static void
take_keyboard(HostWindow *hw) {
    hw->find_keyboard = true;
    if (!host->is_wayland) {
        gtk_widget_realize(hw->find_keys);
        GdkWindow *gw = gtk_widget_get_window(hw->find_keys);
        if (gw && gdk_window_ensure_native(gw)) focus_find_window(hw);
    }
    gtk_widget_grab_focus(hw->find_entry);
}

static void
give_keyboard_back(HostWindow *hw) {
    const bool had = hw->find_keyboard;
    hw->find_keyboard = false;
    if (had) find_emit(hw, "focus", Py_False);
    Page *p = current_page(hw);
    if (p && !host->is_wayland && gtk_window_is_active(GTK_WINDOW(hw->window))) give_input_focus(p);
}

static void on_find_changed(GtkEditable *e, gpointer data) {
    HostWindow *hw = data;
    PyObject *q = PyUnicode_FromString(gtk_entry_get_text(GTK_ENTRY(e)));
    if (q) find_emit(hw, "query", q);
    else PyErr_Print();
    Py_XDECREF(q);
}
static void on_find_up(GtkButton *b, gpointer data) { (void)b; find_emit(data, "next", NULL); }
static void on_find_down(GtkButton *b, gpointer data) { (void)b; find_emit(data, "previous", NULL); }
static void on_find_case(GtkToggleButton *b, gpointer data) { find_emit(data, "case_sensitive", gtk_toggle_button_get_active(b) ? Py_True : Py_False); }
static void on_find_regex(GtkToggleButton *b, gpointer data) { find_emit(data, "regex", gtk_toggle_button_get_active(b) ? Py_True : Py_False); }
static void on_find_close(GtkButton *b, gpointer data) { (void)b; find_emit(data, "close", NULL); }

static gboolean
on_find_key(GtkWidget *w, GdkEventKey *e, gpointer data) {
    (void)w;
    HostWindow *hw = data;
    const bool shift = (e->state & GDK_SHIFT_MASK) != 0;
    switch (e->keyval) {
        case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_F3: find_emit(hw, shift ? "previous" : "next", NULL); return TRUE;
        case GDK_KEY_Up: find_emit(hw, "next", NULL); return TRUE;
        case GDK_KEY_Down: find_emit(hw, "previous", NULL); return TRUE;
        case GDK_KEY_Escape: find_emit(hw, "close", NULL); return TRUE;
    }
    return FALSE;
}

static gboolean
on_find_focus(GtkWidget *w, GdkEvent *e, gpointer data) {
    (void)w;
    HostWindow *hw = data;
    if (e->type == GDK_FOCUS_CHANGE && e->focus_change.in && !hw->find_keyboard && !hw->find_updating) {
        // clicked: the keys must come here, not to the terminal
        take_keyboard(hw);
        find_emit(hw, "focus", Py_True);
    }
    return FALSE;
}

static GtkWidget *
find_button(const char *icon, const char *tip, GCallback cb, HostWindow *hw) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_can_focus(b, FALSE);
    g_signal_connect(b, "clicked", cb, hw);
    return b;
}

static GtkWidget *
find_toggle(const char *label, const char *tip, GCallback cb, HostWindow *hw) {
    GtkWidget *b = gtk_toggle_button_new_with_label(label);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_can_focus(b, FALSE);
    g_signal_connect(b, "toggled", cb, hw);
    return b;
}

static void
build_find_bar(HostWindow *hw) {
    hw->find_revealer = gtk_revealer_new();
    // At once: the window of the bar must be mapped when it takes the keyboard. With a horizontal transition the
    // row keeps the height of the bar while it is hidden, so that showing it does not resize the terminal.
    gtk_revealer_set_transition_type(GTK_REVEALER(hw->find_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(hw->find_revealer), 0);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 4);
    hw->find_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(hw->find_entry), "Find");
    gtk_entry_set_width_chars(GTK_ENTRY(hw->find_entry), 24);
    g_signal_connect(hw->find_entry, "changed", G_CALLBACK(on_find_changed), hw);
    g_signal_connect(hw->find_entry, "key-press-event", G_CALLBACK(on_find_key), hw);
    g_signal_connect(hw->find_entry, "focus-in-event", G_CALLBACK(on_find_focus), hw);
    hw->find_status = gtk_label_new("");
    gtk_label_set_width_chars(GTK_LABEL(hw->find_status), 11);
    gtk_label_set_xalign(GTK_LABEL(hw->find_status), 1);
    gtk_style_context_add_class(gtk_widget_get_style_context(hw->find_status), "dim-label");
    GtkWidget *arrows = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(arrows), "linked");
    hw->find_up = find_button("go-up-symbolic", "Find Next: the match above, older output", G_CALLBACK(on_find_up), hw);
    hw->find_down = find_button("go-down-symbolic", "Find Previous: the match below, newer output", G_CALLBACK(on_find_down), hw);
    gtk_box_pack_start(GTK_BOX(arrows), hw->find_up, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(arrows), hw->find_down, FALSE, FALSE, 0);
    hw->find_case = find_toggle("Aa", "Match case", G_CALLBACK(on_find_case), hw);
    hw->find_regex = find_toggle(".*", "Regular expression", G_CALLBACK(on_find_regex), hw);
    GtkWidget *close = find_button("window-close-symbolic", "Done (Escape)", G_CALLBACK(on_find_close), hw);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_box_pack_start(GTK_BOX(box), hw->find_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), hw->find_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), arrows, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hw->find_case, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hw->find_regex, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), close, FALSE, FALSE, 0);
    hw->find_keys = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(hw->find_keys), TRUE);  // an InputOnly window cannot have the focus
    gtk_widget_set_has_window(hw->find_keys, TRUE);
    gtk_container_add(GTK_CONTAINER(hw->find_keys), box);
    gtk_widget_add_events(hw->find_keys, GDK_STRUCTURE_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    g_signal_connect(hw->find_keys, "map-event", G_CALLBACK(on_find_mapped), hw);
    g_signal_connect(hw->find_keys, "draw", G_CALLBACK(on_find_draw), hw);
    gtk_container_add(GTK_CONTAINER(hw->find_revealer), hw->find_keys);
    gtk_widget_show_all(hw->find_revealer);
    gtk_revealer_set_reveal_child(GTK_REVEALER(hw->find_revealer), FALSE);
}

static const char *
model_str(PyObject *m, const char *key) {
    PyObject *v = PyDict_GetItemString(m, key);
    const char *ans = v && PyUnicode_Check(v) ? PyUnicode_AsUTF8(v) : NULL;
    if (!ans) PyErr_Clear();
    return ans ? ans : "";
}

static bool
model_bool(PyObject *m, const char *key) {
    PyObject *v = PyDict_GetItemString(m, key);
    return v && PyObject_IsTrue(v) == 1;
}

// Show the find bar of page p, which became the current page of hw, or hide it
static void
show_find_bar_of(HostWindow *hw, Page *p) {
    if (!hw->find_revealer) return;
    PyObject *m = p ? p->find_model : NULL;
    hw->find_updating = true;
    if (!m) {
        if (hw->find_keyboard && p) give_input_focus(p);
        hw->find_keyboard = false;
        gtk_revealer_set_reveal_child(GTK_REVEALER(hw->find_revealer), FALSE);
    } else {
        const char *q = model_str(m, "query");
        if (strcmp(gtk_entry_get_text(GTK_ENTRY(hw->find_entry)), q) != 0) gtk_entry_set_text(GTK_ENTRY(hw->find_entry), q);
        gtk_label_set_text(GTK_LABEL(hw->find_status), q[0] ? model_str(m, "status") : "");
        GtkStyleContext *sc = gtk_widget_get_style_context(hw->find_entry);
        const bool not_found = q[0] && (model_bool(m, "error") || strcmp(model_str(m, "status"), "Not found") == 0);
        if (not_found) gtk_style_context_add_class(sc, "error");
        else gtk_style_context_remove_class(sc, "error");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hw->find_case), model_bool(m, "case_sensitive"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hw->find_regex), model_bool(m, "regex"));
        PyObject *total = PyDict_GetItemString(m, "total");
        const bool any = total && PyLong_Check(total) && PyLong_AsLong(total) > 0;
        gtk_widget_set_sensitive(hw->find_up, any);
        gtk_widget_set_sensitive(hw->find_down, any);
        gtk_revealer_set_reveal_child(GTK_REVEALER(hw->find_revealer), TRUE);
    }
    hw->find_updating = false;
    update_chrome(hw);
}

static bool
find_bar(struct OSWindow *w, PyObject *model, PyObject *callback) {
    Page *p = page_of(w);
    // without a native tab the terminal draws a bar of its own
    if (!p || !p->hw) return false;
    HostWindow *hw = p->hw;
    const bool current = current_page(hw) == p;
    if (model == Py_None) {
        if (current && hw->find_keyboard) give_keyboard_back(hw);
        Py_CLEAR(p->find_model);
        Py_CLEAR(p->find_callback);
        if (current) show_find_bar_of(hw, p);
        return true;
    }
    Py_XSETREF(p->find_model, Py_NewRef(model));
    if (p->find_callback != callback) Py_XSETREF(p->find_callback, Py_NewRef(callback));
    if (!current) return true;
    show_find_bar_of(hw, p);
    if (model_bool(model, "focus")) {
        hw->find_updating = true;
        take_keyboard(hw);
        if (model_bool(model, "select_all")) gtk_editable_select_region(GTK_EDITABLE(hw->find_entry), 0, -1);
        hw->find_updating = false;
        find_emit(hw, "focus", Py_True);
    } else if (model_bool(model, "focus_terminal") && hw->find_keyboard) give_keyboard_back(hw);
    return true;
}

// For tests: draw the bar row, with the find bar, to a PNG file
static bool
snapshot_find_bar(struct OSWindow *w, const char *path) {
    Page *p = page_of(w);
    if (!p || !p->hw) return false;
    GtkWidget *bar = p->hw->find_revealer;
    const int width = gtk_widget_get_allocated_width(bar), height = gtk_widget_get_allocated_height(bar);
    if (width < 1 || height < 1) return false;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(s);
    gtk_render_background(gtk_widget_get_style_context(p->hw->window), cr, 0, 0, width, height);
    gtk_widget_draw(bar, cr);
    cairo_destroy(cr);
    const bool ok = cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(s);
    return ok;
}
// }}}

static void
apply_initial_size(HostWindow *hw) {
    if (hw->initial_size_done || !menu_model || !hw->wanted_width || !hw->wanted_height) return;
    hw->initial_size_done = true;
    const int scale = gtk_widget_get_scale_factor(hw->window);
    gint bar = 0;
    gtk_widget_get_preferred_height(hw->topbar, NULL, &bar);
    gtk_window_resize(GTK_WINDOW(hw->window), hw->wanted_width / scale, hw->wanted_height / scale + bar);
}

static void
on_area_allocate(GtkWidget *area, GdkRectangle *a, gpointer data) {
    Page *p = data;
    if (host->is_wayland) {
        // Terminal windows on Wayland are sized in the same units as the toolkit
        gint x = 0, y = 0;
        gtk_widget_translate_coordinates(area, gtk_widget_get_toplevel(area), 0, 0, &x, &y);
        host->wayland_set_position(os_window_of_page(p), x, y);
        if (a->width != p->last_width || a->height != p->last_height) {
            p->last_width = a->width; p->last_height = a->height;
            host->set_size(os_window_of_page(p), a->width, a->height);
        }
        // A position takes effect when the parent commits
        gtk_widget_queue_draw(gtk_widget_get_toplevel(area));
        return;
    }
    const int scale = gtk_widget_get_scale_factor(area), w = a->width * scale, h = a->height * scale;
    if (w == p->last_width && h == p->last_height) return;
    p->last_width = w; p->last_height = h;
    host->set_size(os_window_of_page(p), w, h);
}

static void
on_tab_close(GtkButton *b, gpointer data) {
    (void)b;
    Page *p = data;
    host->request_close(os_window_of_page(p));
}

static gboolean
on_window_delete(GtkWidget *w, GdkEvent *e, gpointer data) {
    (void)w; (void)e;
    HostWindow *hw = data;
    const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(hw->notebook));
    for (gint i = 0; i < n; i++) {
        Page *p = page_at(hw, i);
        if (p) host->request_close(os_window_of_page(p));
    }
    return TRUE; // destroyed when the last of its tabs is
}

static int xi_opcode = -1;

// A window manager that focuses the window again, as many do for every click in it, focuses the
// toplevel. GTK reports nothing then as for it the window has had the focus all along, so this looks at the X events.
// Handing the focus on to the window of the terminal does not cause another FocusIn on the toplevel.
static GdkFilterReturn
focus_event_filter(GdkXEvent *xevent, GdkEvent *event, gpointer data) {
    (void)event; (void)data;
    XEvent *e = xevent;
    Window target = None;
    if (e->type == FocusIn) {
        if (e->xfocus.mode != NotifyGrab && e->xfocus.mode != NotifyUngrab) target = e->xfocus.window;
    } else if (e->type == GenericEvent && e->xcookie.extension == xi_opcode && e->xcookie.data && e->xcookie.evtype == XI_FocusIn) {
        // GDK selects focus events through XInput 2 when that is available
        const XIFocusInEvent *fe = e->xcookie.data;
        if (fe->mode == XINotifyNormal || fe->mode == XINotifyWhileGrabbed) target = fe->event;
    }
    if (target == None) return GDK_FILTER_CONTINUE;
    GdkWindow *gw = gdk_x11_window_lookup_for_display(gdk_display_get_default(), target);
    if (!gw) return GDK_FILTER_CONTINUE;
    for (GList *x = host_windows; x; x = x->next) {
        HostWindow *hw = x->data;
        if (gtk_widget_get_window(hw->window) == gw) {
            g_idle_add(focus_current_page, hw);
            break;
        }
    }
    return GDK_FILTER_CONTINUE;
}

static gboolean
on_window_state(GtkWidget *w, GdkEventWindowState *e, gpointer data) {
    (void)w;
    HostWindow *hw = data;
    hw->fullscreen = (e->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
    hw->maximized = (e->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    update_chrome(hw);
    return FALSE;
}

static void
on_switch_page(GtkNotebook *nb, GtkWidget *child, guint n, gpointer data) {
    (void)nb; (void)n;
    HostWindow *hw = data;
    Page *p = g_object_get_data(G_OBJECT(child), "page");
    if (p) gtk_window_set_title(GTK_WINDOW(hw->window), gtk_label_get_text(GTK_LABEL(p->label)));
    show_find_bar_of(hw, p);
    if (host->is_wayland) {
        // A subsurface is shown whatever the toolkit thinks of the page it is on
        if (hw->shown_page && hw->shown_page != p) host->wayland_set_mapped(os_window_of_page(hw->shown_page), false);
        hw->shown_page = p;
        if (p) {
            host->wayland_set_mapped(os_window_of_page(p), true);
            host->wayland_activate(os_window_of_page(p));
            host->refresh(os_window_of_page(p));
        }
        return;
    }
    g_idle_add(focus_current_page, hw);
}

static void
on_new_tab_clicked(GtkButton *b, gpointer data) {
    (void)b; (void)data;
    host->run_action("new_tab");
}

static HostWindow *
new_host_window(struct OSWindow *size_from) {
    HostWindow *hw = g_new0(HostWindow, 1);
    hw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_icon_name(GTK_WINDOW(hw->window), "silkie");
    if (size_from) host->get_size(size_from, &hw->wanted_width, &hw->wanted_height);
    if (hw->wanted_width > 0 && hw->wanted_height > 0) {
        const int scale = gtk_widget_get_scale_factor(hw->window);
        gtk_window_set_default_size(GTK_WINDOW(hw->window), hw->wanted_width / scale, hw->wanted_height / scale);
    }
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    hw->topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    hw->menubar = gtk_menu_bar_new();
    fill_menubar(hw->menubar);
    gtk_box_pack_start(GTK_BOX(hw->topbar), hw->menubar, TRUE, TRUE, 0);
    build_find_bar(hw);
    gtk_box_pack_end(GTK_BOX(hw->topbar), hw->find_revealer, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hw->topbar, FALSE, FALSE, 0);
    hw->notebook = gtk_notebook_new();
    GtkNotebook *nb = GTK_NOTEBOOK(hw->notebook);
    gtk_notebook_set_scrollable(nb, TRUE);
    gtk_notebook_set_show_border(nb, FALSE);
    GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(add), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(add, "New Tab");
    g_signal_connect(add, "clicked", G_CALLBACK(on_new_tab_clicked), NULL);
    gtk_widget_show(add);
    gtk_notebook_set_action_widget(nb, add, GTK_PACK_END);
    gtk_box_pack_start(GTK_BOX(vbox), hw->notebook, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(hw->window), vbox);
    if (host->is_wayland) {
        g_signal_connect(hw->window, "key-press-event", G_CALLBACK(swallow_key_event), hw);
        g_signal_connect(hw->window, "key-release-event", G_CALLBACK(swallow_key_event), hw);
    }
    g_signal_connect(hw->window, "delete-event", G_CALLBACK(on_window_delete), hw);
    g_signal_connect(hw->window, "window-state-event", G_CALLBACK(on_window_state), hw);
    g_signal_connect(hw->notebook, "switch-page", G_CALLBACK(on_switch_page), hw);
    host_windows = g_list_prepend(host_windows, hw);
    return hw;
}

static void
destroy_host_window(HostWindow *hw) {
    host_windows = g_list_remove(host_windows, hw);
    gtk_widget_destroy(hw->window);
    g_free(hw);
}

// Program status badge {{{
static void
free_badge(Badge *b) {
    g_free(b->state); g_free(b->kind); g_free(b->text); g_free(b->tooltip); g_free(b->accessibility);
    *b = (Badge){0};
}

static void
ensure_badge_style(void) {
    static bool done = false;
    if (done) return;
    done = true;
    // The colors are those the theme names for success, warning and error. The fallbacks, from Adwaita, are for themes
    // that do not name them: a provider of lower priority than the theme is only asked for colors the theme lacks.
    GdkScreen *screen = gdk_screen_get_default();
    if (!screen) return;
    GtkCssProvider *fallback = gtk_css_provider_new(), *rules = gtk_css_provider_new();
    gtk_css_provider_load_from_data(fallback,
        "@define-color success_color #26a269;\n@define-color warning_color #c88800;\n@define-color error_color #e01b24;\n", -1, NULL);
    gtk_css_provider_load_from_data(rules,
        ".silkie-badge-done { color: @success_color; }\n"
        ".silkie-badge-blocked { color: @warning_color; }\n"
        ".silkie-badge-error { color: @error_color; }\n", -1, NULL);
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(fallback), GTK_STYLE_PROVIDER_PRIORITY_FALLBACK);
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(rules), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(fallback);
    g_object_unref(rules);
}

static bool
streq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static const char *
badge_icon_name(const Badge *b) {
    const char *name = "dialog-warning-symbolic", *fallback = name;
    if (streq(b->state, "done")) name = "emblem-ok-symbolic";
    else if (streq(b->state, "error")) name = fallback = "dialog-error-symbolic";
    else if (streq(b->kind, "permission")) name = "changes-prevent-symbolic";
    else if (streq(b->kind, "question")) name = "dialog-question-symbolic";
    else if (streq(b->kind, "auth")) name = "dialog-password-symbolic";
    // Icon themes other than Adwaita do not all have every one of these
    return gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), name) ? name : fallback;
}

static void
show_badge(Page *p) {
    if (!p->tab) return; // not in a window at the moment, shown when it is
    const Badge *b = &p->badge;
    if (!b->state) {
        if (p->badge_box) gtk_widget_destroy(p->badge_box);
        p->badge_box = p->badge_indicator = p->badge_label = NULL;
        g_clear_pointer(&p->badge_indicator_for, g_free);
        return;
    }
    ensure_badge_style();
    if (!p->badge_box) {
        p->badge_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
        p->badge_label = gtk_label_new(NULL);
        gtk_box_pack_end(GTK_BOX(p->badge_box), p->badge_label, FALSE, FALSE, 0);
        // After the title: the notebook names the tab for assistive technology after the first label in it
        gtk_box_pack_start(GTK_BOX(p->tab), p->badge_box, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(p->tab), p->badge_box, 1);
    }
    char *indicator_for = g_strconcat(b->state, ":", b->kind, NULL);
    if (!streq(indicator_for, p->badge_indicator_for)) {
        if (p->badge_indicator) gtk_widget_destroy(p->badge_indicator);
        if (streq(b->state, "working")) {
            p->badge_indicator = gtk_spinner_new();
            gtk_spinner_start(GTK_SPINNER(p->badge_indicator));
        } else {
            p->badge_indicator = gtk_image_new_from_icon_name(badge_icon_name(b), GTK_ICON_SIZE_MENU);
            char *cls = g_strconcat("silkie-badge-", b->state, NULL);
            gtk_style_context_add_class(gtk_widget_get_style_context(p->badge_indicator), cls);
            g_free(cls);
        }
        gtk_box_pack_start(GTK_BOX(p->badge_box), p->badge_indicator, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(p->badge_box), p->badge_indicator, 0);
        g_free(p->badge_indicator_for);
        p->badge_indicator_for = indicator_for;
    } else g_free(indicator_for);
    char *markup = g_markup_printf_escaped("<small>%s</small>", b->text);
    gtk_label_set_markup(GTK_LABEL(p->badge_label), markup);
    g_free(markup);
    gtk_widget_set_tooltip_text(p->badge_box, b->tooltip);
    gtk_widget_show_all(p->badge_box);
    gtk_widget_set_visible(p->badge_label, b->text[0] != 0);
}

static void
describe_page(Page *p) {
    // The widgets of a tab are not in the accessibility tree, the page tab of the notebook stands for them. It
    // keeps the title as its name, the status in words is its description.
    if (!p->hw || !p->area) return;
    AtkObject *nb = gtk_widget_get_accessible(p->hw->notebook);
    const gint idx = gtk_notebook_page_num(GTK_NOTEBOOK(p->hw->notebook), p->area);
    AtkObject *page = nb && idx >= 0 ? atk_object_ref_accessible_child(nb, idx) : NULL;
    if (!page) return;
    atk_object_set_description(page, p->badge.state ? p->badge.accessibility : "");
    g_object_unref(page);
}

static void
tab_status(struct OSWindow *w, PyObject *status) {
    Page *p = page_of(w);
    if (!p) return;
    free_badge(&p->badge);
    if (status != Py_None && str_item(status, "state")[0]) {
        p->badge = (Badge){
            .state = g_strdup(str_item(status, "state")), .kind = g_strdup(str_item(status, "kind")), .text = g_strdup(str_item(status, "text")),
            .tooltip = g_strdup(str_item(status, "tooltip")), .accessibility = g_strdup(str_item(status, "accessibility")),
        };
    }
    show_badge(p);
    describe_page(p);
}

static bool
snapshot_tab(struct OSWindow *w, const char *path) {
    Page *p = page_of(w);
    if (!p || !p->hw) return false;
    // Only what GTK draws: on X11 the terminal is a window of its own inside the page and is left out
    GtkWidget *win = p->hw->window;
    const int width = gtk_widget_get_allocated_width(win), height = gtk_widget_get_allocated_height(win);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(win, cr);
    cairo_destroy(cr);
    const bool ok = cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(s);
    return ok;
}
// }}}

// Tab settings {{{
static void
on_gear_clicked(GtkButton *b, gpointer data) {
    (void)b;
    Page *p = data;
    char action[64];  // the name of the action and a 64 bit number fit in 64 bytes
    snprintf(action, sizeof(action), "open_tab_settings %llu", p->id);
    host->run_action(action);
}

static void
show_gear(Page *p) {
    if (!p->tab) return; // made when the page is next embedded
    if (p->wants_gear && !p->gear) {
        p->gear = gtk_button_new_from_icon_name("emblem-system-symbolic", GTK_ICON_SIZE_MENU);
        gtk_button_set_relief(GTK_BUTTON(p->gear), GTK_RELIEF_NONE);
        gtk_widget_set_focus_on_click(p->gear, FALSE);
        gtk_widget_set_tooltip_text(p->gear, "Tab Settings");
        AtkObject *a = gtk_widget_get_accessible(p->gear);
        if (a) atk_object_set_name(a, "Tab Settings");
        g_signal_connect(p->gear, "clicked", G_CALLBACK(on_gear_clicked), p);
        // Before the close button, which is packed at the end
        gtk_box_pack_end(GTK_BOX(p->tab), p->gear, FALSE, FALSE, 0);
        gtk_widget_show(p->gear);
    } else if (!p->wants_gear && p->gear) {
        gtk_widget_destroy(p->gear);
        p->gear = NULL;
    }
}

static void
tab_settings_button(struct OSWindow *w, bool show) {
    Page *p = page_of(w);
    if (!p) return;
    p->wants_gear = show;
    show_gear(p);
}

// Each popover holds the Python callback that its fields report to

static void
tab_settings_emit(GtkWidget *source, const char *verb, const char *name, const char *value) {
    GtkWidget *popover = gtk_widget_get_ancestor(source, GTK_TYPE_POPOVER);
    PyObject *callback = popover ? g_object_get_data(G_OBJECT(popover), "callback") : NULL;
    GtkWidget *error = popover ? g_object_get_data(G_OBJECT(popover), "error-label") : NULL;
    if (!callback) return;
    PyObject *ret = PyObject_CallFunction(callback, "sss", verb, name, value);
    const char *err = "";
    if (!ret) PyErr_Print();
    else if (PyUnicode_Check(ret)) {
        err = PyUnicode_AsUTF8(ret);
        if (!err) { PyErr_Clear(); err = ""; }
    }
    if (error) {
        gtk_label_set_text(GTK_LABEL(error), err);
        gtk_widget_set_visible(error, err[0] != 0);
    }
    Py_XDECREF(ret);
}

static const char *
widget_field(gpointer w) {
    const char *n = g_object_get_data(G_OBJECT(w), "field-name");
    return n ? n : "";
}

static void
on_tab_choice_changed(GtkComboBox *c, gpointer data) {
    (void)data;
    const char *id = gtk_combo_box_get_active_id(c);
    if (id) tab_settings_emit(GTK_WIDGET(c), "set", widget_field(c), id);
}

static void
commit_tab_text(GtkWidget *w) {
    // An entry, or the entry of a combo box, which carries the name of the field
    GtkWidget *owner = g_object_get_data(G_OBJECT(w), "field-owner");
    tab_settings_emit(w, "set", widget_field(owner ? owner : w), gtk_entry_get_text(GTK_ENTRY(w)));
}

static void on_tab_entry_activate(GtkEntry *e, gpointer data) { (void)data; commit_tab_text(GTK_WIDGET(e)); }
static gboolean on_tab_entry_focus_out(GtkWidget *e, GdkEvent *ev, gpointer data) { (void)ev; (void)data; commit_tab_text(e); return FALSE; }

static void
on_tab_combo_changed(GtkComboBox *c, gpointer data) {
    (void)data;
    // Typing changes the text too, that is committed with Enter or when the field loses the focus
    if (gtk_combo_box_get_active(c) >= 0) commit_tab_text(gtk_bin_get_child(GTK_BIN(c)));
}

static void
on_tab_button_clicked(GtkButton *b, gpointer data) {
    (void)data;
    tab_settings_emit(GTK_WIDGET(b), "activate", widget_field(b), "");
}

static GtkWidget *
tab_settings_control(PyObject *f) {
    const char *kind = str_item(f, "kind"), *value = str_item(f, "value");
    GtkWidget *c = NULL;
    if (strcmp(kind, "choice") == 0 || strcmp(kind, "combo") == 0) {
        const bool editable = strcmp(kind, "combo") == 0;
        c = editable ? gtk_combo_box_text_new_with_entry() : gtk_combo_box_text_new();
        PyObject *choices = list_item(f, "choices");
        for (Py_ssize_t i = 0; choices && i < PyList_GET_SIZE(choices); i++) {
            PyObject *ch = PyList_GET_ITEM(choices, i);
            if (!PyList_Check(ch) || PyList_GET_SIZE(ch) < 2) continue;
            PyObject *v = PyList_GET_ITEM(ch, 0), *t = PyList_GET_ITEM(ch, 1);
            const char *vs = PyUnicode_Check(v) ? PyUnicode_AsUTF8(v) : NULL, *ts = PyUnicode_Check(t) ? PyUnicode_AsUTF8(t) : NULL;
            if (!vs || !ts) { PyErr_Clear(); continue; }
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), vs, ts);
        }
        if (editable) {
            GtkWidget *entry = gtk_bin_get_child(GTK_BIN(c));
            gtk_entry_set_text(GTK_ENTRY(entry), value);
            g_object_set_data(G_OBJECT(entry), "field-owner", c);
            g_signal_connect(entry, "activate", G_CALLBACK(on_tab_entry_activate), NULL);
            g_signal_connect(entry, "focus-out-event", G_CALLBACK(on_tab_entry_focus_out), NULL);
            g_signal_connect(c, "changed", G_CALLBACK(on_tab_combo_changed), NULL);
        } else {
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), value);
            g_signal_connect(c, "changed", G_CALLBACK(on_tab_choice_changed), NULL);
        }
    } else if (strcmp(kind, "text") == 0) {
        c = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(c), value);
        gtk_entry_set_placeholder_text(GTK_ENTRY(c), str_item(f, "placeholder"));
        gtk_entry_set_width_chars(GTK_ENTRY(c), 24);
        g_signal_connect(c, "activate", G_CALLBACK(on_tab_entry_activate), NULL);
        g_signal_connect(c, "focus-out-event", G_CALLBACK(on_tab_entry_focus_out), NULL);
    } else if (strcmp(kind, "button") == 0) {
        c = gtk_button_new_with_label(str_item(f, "title"));
        g_signal_connect(c, "clicked", G_CALLBACK(on_tab_button_clicked), NULL);
    } else return NULL;
    g_object_set_data_full(G_OBJECT(c), "field-name", g_strdup(str_item(f, "name")), g_free);
    gtk_widget_set_tooltip_text(c, str_item(f, "help"));
    AtkObject *a = gtk_widget_get_accessible(c);
    if (a) atk_object_set_name(a, str_item(f, "title"));
    PyObject *enabled = PyDict_GetItemString(f, "enabled");
    if (enabled && !PyObject_IsTrue(enabled)) gtk_widget_set_sensitive(c, FALSE);
    return c;
}

static gboolean
destroy_later(gpointer data) {
    // Holds a reference, as the widget can be destroyed with the tab in the meantime, which makes this do nothing
    gtk_widget_destroy(data);
    g_object_unref(data);
    return G_SOURCE_REMOVE;
}

static void
on_tab_settings_destroyed(GtkWidget *popover, gpointer data) {
    (void)data;
    if (tab_settings_popover == popover) tab_settings_popover = NULL;
}

static void
on_tab_settings_closed(GtkPopover *popover, gpointer data) {
    (void)data;
    // Commit what is being typed, closing does not always take the focus from the field first
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(popover))));
    if (focus && GTK_IS_ENTRY(focus) && gtk_widget_is_ancestor(focus, GTK_WIDGET(popover))) commit_tab_text(focus);
    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(popover));
    on_tab_settings_destroyed(GTK_WIDGET(popover), NULL);
    // Back to the terminal, which reads the keyboard itself
    for (GList *x = host_windows; x; x = x->next) {
        HostWindow *hw = x->data;
        if (hw->window == toplevel && !host->is_wayland) g_idle_add(focus_current_page, hw);
    }
    // Not from within its own signal
    g_idle_add(destroy_later, g_object_ref(popover));
}

static void
show_tab_settings(struct OSWindow *w, PyObject *model, PyObject *callback) {
    Page *p = page_of(w);
    if (!p || !p->hw) return;
    if (tab_settings_popover) gtk_popover_popdown(GTK_POPOVER(tab_settings_popover));
    // At the gear of the tab when it is visible, otherwise at the top of the terminal, as with a single tab
    const bool at_gear = p->gear && gtk_widget_get_mapped(p->gear) && gtk_notebook_get_show_tabs(GTK_NOTEBOOK(p->hw->notebook));
    GtkWidget *popover = gtk_popover_new(at_gear ? p->gear : p->area);
    if (!at_gear) {
        GtkAllocation a;
        gtk_widget_get_allocation(p->area, &a);
        gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){.x = a.width / 2, .y = 0, .width = 1, .height = 1});
    }
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    // On Wayland the terminal is a subsurface above the window of the toolkit, a popover drawn in that window would be under it
    if (host->is_wayland) gtk_popover_set_constrain_to(GTK_POPOVER(popover), GTK_POPOVER_CONSTRAINT_NONE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gint row = 0;
    GtkWidget *first = NULL;
    PyObject *fields = list_item(model, "fields");
    for (Py_ssize_t i = 0; fields && i < PyList_GET_SIZE(fields); i++) {
        PyObject *f = PyList_GET_ITEM(fields, i);
        if (!PyDict_Check(f)) continue;
        const char *kind = str_item(f, "kind");
        if (strcmp(kind, "section") == 0) {
            GtkWidget *l = gtk_label_new(NULL);
            char *markup = g_markup_printf_escaped("<b>%s</b>", str_item(f, "title"));
            gtk_label_set_markup(GTK_LABEL(l), markup);
            g_free(markup);
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            if (row) gtk_widget_set_margin_top(l, 6);
            gtk_grid_attach(GTK_GRID(grid), l, 0, row++, 2, 1);
            continue;
        }
        GtkWidget *c = tab_settings_control(f);
        if (!c) continue;
        gtk_widget_set_hexpand(c, TRUE);
        if (strcmp(kind, "button") == 0) {
            gtk_widget_set_halign(c, GTK_ALIGN_START);
            gtk_grid_attach(GTK_GRID(grid), c, 1, row++, 1, 1);
        } else {
            GtkWidget *l = gtk_label_new(str_item(f, "title"));
            gtk_widget_set_halign(l, GTK_ALIGN_END);
            // Names the control for assistive technology too
            gtk_label_set_mnemonic_widget(GTK_LABEL(l), c);
            gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), c, 1, row++, 1, 1);
        }
        if (!first && gtk_widget_get_sensitive(c)) first = c;
    }
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    GtkWidget *error = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(error), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(error), 40);
    gtk_style_context_add_class(gtk_widget_get_style_context(error), "error");
    gtk_box_pack_start(GTK_BOX(box), error, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(popover), "error-label", error);
    gtk_container_add(GTK_CONTAINER(popover), box);
    AtkObject *a = gtk_widget_get_accessible(popover);
    if (a) atk_object_set_name(a, str_item(model, "title"));
    tab_settings_popover = popover;
    g_object_set_data_full(G_OBJECT(popover), "callback", Py_NewRef(callback), drop_ref);
    g_signal_connect(popover, "closed", G_CALLBACK(on_tab_settings_closed), NULL);
    g_signal_connect(popover, "destroy", G_CALLBACK(on_tab_settings_destroyed), NULL);
    gtk_widget_show_all(box);
    gtk_widget_hide(error);
    gtk_window_present(GTK_WINDOW(p->hw->window));
    gtk_popover_popup(GTK_POPOVER(popover));
    if (first) gtk_widget_grab_focus(first);
    if (!host->is_wayland) {
        // The terminal has the X input focus, which the keys typed in the popover must go to the toolkit window for
        GdkDisplay *gd = gdk_display_get_default();
        gdk_x11_display_error_trap_push(gd);
        XSetInputFocus(gdk_xdisplay(), gdk_x11_window_get_xid(gtk_widget_get_window(p->hw->window)), RevertToParent, CurrentTime);
        XSync(gdk_xdisplay(), False);
        gdk_x11_display_error_trap_pop_ignored(gd);
    }
}
// }}}

static gint
insert_position(HostWindow *hw, const char *location) {
    const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(hw->notebook)), cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(hw->notebook));
    if (strcmp(location, "first") == 0) return 0;
    if (strcmp(location, "before") == 0) return MAX(cur, 0);
    if (strcmp(location, "after") == 0 || strcmp(location, "neighbor") == 0) return cur + 1;
    return n;
}

static void
embed_page(Page *p, HostWindow *hw, const char *location) {
    p->hw = hw;
    p->last_width = 0; p->last_height = 0;
    p->area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(p->area, TRUE);
    gtk_widget_set_vexpand(p->area, TRUE);
    gtk_widget_set_has_window(p->area, TRUE);
    gtk_widget_set_app_paintable(p->area, TRUE);
    g_object_set_data(G_OBJECT(p->area), "page", p);
    g_signal_connect(p->area, "size-allocate", G_CALLBACK(on_area_allocate), p);

    p->tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char *title = host->title(os_window_of_page(p));
    p->label = gtk_label_new(title && title[0] ? title : "Silkie");
    gtk_label_set_ellipsize(GTK_LABEL(p->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(p->label), 16);
    gtk_box_pack_start(GTK_BOX(p->tab), p->label, TRUE, TRUE, 0);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close, "Close Tab");
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close), p);
    gtk_box_pack_end(GTK_BOX(p->tab), close, FALSE, FALSE, 0);
    gtk_widget_show_all(p->tab);
    show_badge(p);
    show_gear(p);

    GtkNotebook *nb = GTK_NOTEBOOK(hw->notebook);
    const gint pos = gtk_notebook_insert_page(nb, p->area, p->tab, insert_position(hw, location));
    gtk_notebook_set_tab_reorderable(nb, p->area, TRUE);
    gtk_widget_show_all(hw->window);
    update_chrome(hw);
    apply_initial_size(hw);
    gtk_notebook_set_current_page(nb, pos);
    gtk_widget_realize(p->area);
    describe_page(p);
#ifdef GDK_WINDOWING_WAYLAND
    if (host->is_wayland) {
        host->wayland_embed(os_window_of_page(p), gdk_wayland_window_get_wl_surface(gtk_widget_get_window(hw->window)));
        host->wayland_set_mapped(os_window_of_page(p), true);
        host->wayland_activate(os_window_of_page(p));
        hw->shown_page = p;
        GtkAllocation a;
        gtk_widget_get_allocation(p->area, &a);
        on_area_allocate(p->area, &a, p);
        update_window_title(hw);
        return;
    }
#endif
    GdkWindow *gw = gtk_widget_get_window(p->area);
    gdk_window_ensure_native(gw);
    Display *d = gdk_xdisplay();
    GdkDisplay *gd = gdk_display_get_default();
    gdk_display_flush(gd);
    gdk_x11_display_error_trap_push(gd);
    XReparentWindow(d, p->xid, gdk_x11_window_get_xid(gw), 0, 0);
    XMapWindow(d, p->xid);
    XSync(d, False);
    gdk_x11_display_error_trap_pop_ignored(gd);
    update_window_title(hw);
    g_idle_add(focus_current_page, hw);
}

static void
unembed_page(Page *p) {
    HostWindow *hw = p->hw;
    if (host->is_wayland) {
        if (hw->shown_page == p) hw->shown_page = NULL;
        host->wayland_set_mapped(os_window_of_page(p), false);
        host->wayland_embed(os_window_of_page(p), NULL);
    } else {
        // The window must leave the page first, destroying the page would destroy all windows inside it
        Display *d = gdk_xdisplay();
        GdkDisplay *gd = gdk_display_get_default();
        gdk_x11_display_error_trap_push(gd);
        XUnmapWindow(d, p->xid);
        XReparentWindow(d, p->xid, DefaultRootWindow(d), 0, 0);
        XSync(d, False);
        gdk_x11_display_error_trap_pop_ignored(gd);
    }
    gtk_notebook_remove_page(GTK_NOTEBOOK(hw->notebook), gtk_notebook_page_num(GTK_NOTEBOOK(hw->notebook), p->area));
    p->hw = NULL; p->area = NULL; p->label = NULL; p->tab = NULL;
    // The badge went with the tab, it is made again from p->badge when the page is next embedded
    p->badge_box = p->badge_indicator = p->badge_label = NULL;
    p->gear = NULL;
    g_clear_pointer(&p->badge_indicator_for, g_free);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(hw->notebook)) == 0) destroy_host_window(hw);
    else {
        update_chrome(hw);
        update_window_title(hw);
        g_idle_add(focus_current_page, hw);
    }
}

static bool
tabs_supported(void) {
    // A subsurface can only go inside the window of a toolkit that uses the same connection to the compositor
    return !host->is_wayland || shares_wayland_display;
}

static bool
adopt_os_window(struct OSWindow *w, struct OSWindow *parent, const char *location) {
    if (!tabs_supported()) return false;
    Page *pp = page_of(parent);
    Page *p = g_new0(Page, 1);
    p->id = host->os_window_id(w);
    p->xid = host->x11_window(w);
    g_hash_table_insert(pages, &p->id, p);
    embed_page(p, pp ? pp->hw : new_host_window(w), location);
    return true;
}

static void
os_window_closing(struct OSWindow *w) {
    Page *p = page_of(w);
    if (!p) return;
    Py_CLEAR(p->find_model);
    Py_CLEAR(p->find_callback);
    unembed_page(p);
    g_hash_table_remove(pages, &p->id);
    free_badge(&p->badge);
    g_free(p);
}

static void
attach_tab(struct OSWindow *w, struct OSWindow *target, const char *location) {
    Page *p = page_of(w), *t = page_of(target);
    if (!p || !t || p->hw == t->hw) return;
    HostWindow *dest = t->hw;
    unembed_page(p);
    embed_page(p, dest, location);
}

static void
select_tab(struct OSWindow *w) {
    Page *p = page_of(w);
    if (!p) return;
    GtkNotebook *nb = GTK_NOTEBOOK(p->hw->notebook);
    gtk_notebook_set_current_page(nb, gtk_notebook_page_num(nb, p->area));
    gtk_window_present(GTK_WINDOW(p->hw->window));
}

static void
move_tab(struct OSWindow *w, int delta) {
    Page *p = page_of(w);
    if (!p) return;
    GtkNotebook *nb = GTK_NOTEBOOK(p->hw->notebook);
    const gint n = gtk_notebook_get_n_pages(nb);
    if (n < 2) return;
    gtk_notebook_reorder_child(nb, p->area, ((gtk_notebook_page_num(nb, p->area) + delta) % n + n) % n);
}

static void
detach_tab(struct OSWindow *w) {
    Page *p = page_of(w);
    if (!p || gtk_notebook_get_n_pages(GTK_NOTEBOOK(p->hw->notebook)) < 2) return;
    unembed_page(p);
    embed_page(p, new_host_window(w), "last");
}

static size_t
tab_members(struct OSWindow *w, unsigned long long *ids, size_t cap) {
    Page *p = page_of(w);
    if (!p) {
        if (cap) ids[0] = host->os_window_id(w);
        return 1;
    }
    const gint pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(p->hw->notebook));
    size_t n = 0;
    for (gint i = 0; i < pages; i++) {
        Page *q = page_at(p->hw, i);
        if (!q) continue;
        if (n < cap) ids[n] = q->id;
        n++;
    }
    return n;
}

static void
title_changed(struct OSWindow *w, const char *title) {
    Page *p = page_of(w);
    if (!p) return;
    gtk_label_set_text(GTK_LABEL(p->label), title);
    gtk_widget_set_tooltip_text(p->tab, title);
    if (current_page(p->hw) == p) gtk_window_set_title(GTK_WINDOW(p->hw->window), title);
}

static void
window_op(struct OSWindow *w, enum NativeWindowOp op) {
    Page *p = page_of(w);
    if (!p) return;
    HostWindow *hw = p->hw;
    GtkWindow *win = GTK_WINDOW(hw->window);
    switch (op) {
        case NATIVE_OP_TOGGLE_FULLSCREEN: if (hw->fullscreen) gtk_window_unfullscreen(win); else gtk_window_fullscreen(win); break;
        case NATIVE_OP_TOGGLE_MAXIMIZED: if (hw->maximized) gtk_window_unmaximize(win); else gtk_window_maximize(win); break;
        case NATIVE_OP_MAXIMIZE: gtk_window_maximize(win); break;
        case NATIVE_OP_MINIMIZE: gtk_window_iconify(win); break;
        case NATIVE_OP_RESTORE:
            if (hw->fullscreen) gtk_window_unfullscreen(win);
            else if (hw->maximized) gtk_window_unmaximize(win);
            break;
        case NATIVE_OP_FOCUS: select_tab(w); break;
    }
}

static int
saved_height_adjustment(struct OSWindow *w) {
    // Saved sizes are those of the terminal, with the tab strip only present while there are several tabs,
    // so that a window does not get shorter every time it is closed with several tabs.
    Page *p = page_of(w);
    if (!p) return 0;
    const int strip = gtk_widget_get_allocated_height(p->hw->notebook) - gtk_widget_get_allocated_height(p->area);
    return MAX(strip, 0) * gtk_widget_get_scale_factor(p->area);
}

static bool
window_query(struct OSWindow *w, enum NativeWindowQuery q) {
    Page *p = page_of(w);
    if (!p) return false;
    return q == NATIVE_QUERY_IS_FULLSCREEN ? p->hw->fullscreen : p->hw->maximized;
}

static void
update_all_menubars(void) {
    for (GList *x = host_windows; x; x = x->next) {
        HostWindow *hw = x->data;
        fill_menubar(hw->menubar);
        update_chrome(hw);
        apply_initial_size(hw);
    }
}
#else
static bool tabs_supported(void) { return false; }
static void update_all_menubars(void) {}
#endif

static void
update_menu(PyObject *model) {
    Py_XDECREF(menu_model);
    menu_model = Py_NewRef(model);
    update_all_menubars();
}
// }}}

// Settings window {{{
typedef struct {
    GtkWidget *window, *toolbar, *search, *sidebar, *sidebar_box, *profile_bar, *tab_bar, *rows, *scroll;
    PyObject *model, *callback;
    char *page_id;
    gint sidebar_index, profile_tab;
    GtkWidget *recording_button, *recording_entry;
    PyObject *recording_field;
    bool building;
} Settings;

static Settings settings = {0};
static void rebuild_all(void);

static char *
emit_event(const char *verb, const char *a, const char *b, const char *c) {
    if (!settings.callback) return NULL;
    PyObject *ret = c ? PyObject_CallFunction(settings.callback, "ssss", verb, a, b, c)
                      : (b ? PyObject_CallFunction(settings.callback, "sss", verb, a, b)
                           : (a ? PyObject_CallFunction(settings.callback, "ss", verb, a) : PyObject_CallFunction(settings.callback, "s", verb)));
    if (!ret) { PyErr_Print(); return NULL; }
    char *ans = NULL;
    if (PyUnicode_Check(ret)) {
        const char *s = PyUnicode_AsUTF8(ret);
        if (s && s[0]) ans = g_strdup(s);
    }
    Py_DECREF(ret);
    return ans;
}

static void
show_error(const char *message) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(settings.window), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "That value cannot be used");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", message);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(d);
}

static void
attach_field(gpointer widget, PyObject *field) {
    Py_INCREF(field);
    g_object_set_data_full(G_OBJECT(widget), "field", field, drop_ref);
}

static PyObject *
field_of(gpointer widget) {
    return g_object_get_data(G_OBJECT(widget), "field");
}

static bool
commit_field(gpointer widget, const char *value) {
    PyObject *f = field_of(widget);
    if (!f || settings.building) return false;
    char *err = emit_event("set", str_item(f, "scope"), str_item(f, "name"), value);
    if (err) {
        show_error(err);
        g_free(err);
        return false;
    }
    PyObject *v = PyUnicode_FromString(value);
    if (v) { PyDict_SetItemString(f, "value", v); Py_DECREF(v); } else PyErr_Clear();
    PyDict_SetItemString(f, "is_set", Py_True);
    GtkWidget *reset = g_object_get_data(G_OBJECT(widget), "reset");
    for (GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(widget)); !reset && parent; parent = gtk_widget_get_parent(parent))
        reset = g_object_get_data(G_OBJECT(parent), "reset");
    if (reset) gtk_widget_show(reset);
    return true;
}

static gboolean
rebuild_later(gpointer data) {
    (void)data;
    rebuild_all();
    return G_SOURCE_REMOVE;
}

static void
on_reset_clicked(GtkButton *b, gpointer data) {
    (void)data;
    PyObject *f = field_of(b);
    if (!f) return;
    Py_INCREF(f);
    char *err = emit_event("reset", str_item(f, "scope"), str_item(f, "name"), NULL);
    g_free(err);
    Py_DECREF(f);
}

static gboolean
on_switch_changed(GtkSwitch *s, gboolean state, gpointer data) {
    (void)data;
    commit_field(s, state ? "yes" : "no");
    return FALSE;
}

static void
on_choice_changed(GtkComboBoxText *c, gpointer data) {
    (void)data;
    char *t = gtk_combo_box_text_get_active_text(c);
    if (t) commit_field(c, t);
    g_free(t);
}

static void
commit_entry(GtkWidget *e) {
    PyObject *f = field_of(e);
    if (!f || settings.building) return;
    const char *text = gtk_entry_get_text(GTK_ENTRY(e));
    if (strcmp(text, str_item(f, "value")) == 0) return;
    char *copy = g_strstrip(g_strdup(text));
    if (!commit_field(e, copy)) gtk_entry_set_text(GTK_ENTRY(e), str_item(f, "value"));
    g_free(copy);
}

static void on_entry_activate(GtkEntry *e, gpointer data) { (void)data; commit_entry(GTK_WIDGET(e)); }
static gboolean on_entry_focus_out(GtkWidget *e, GdkEvent *ev, gpointer data) { (void)ev; (void)data; commit_entry(e); return FALSE; }

static void
on_number_adjust(GtkButton *button, gpointer data) {
    GtkWidget *entry = data;
    PyObject *f = field_of(entry);
    if (!f || settings.building) return;
    const char *direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction")) < 0 ? "-1" : "1";
    char *value = emit_event("adjust", str_item(f, "name"), gtk_entry_get_text(GTK_ENTRY(entry)), direction);
    if (value) {
        if (strcmp(value, str_item(f, "value")) && commit_field(entry, value)) gtk_entry_set_text(GTK_ENTRY(entry), value);
        g_free(value);
    }
}

#define HEX_COLOR_SZ sizeof("#rrggbb")

static void
hex_of(const GdkRGBA *c, char buf[HEX_COLOR_SZ]) {
#define C(x) (int)(CLAMP(c->x, 0.0, 1.0) * 255 + 0.5)
    g_snprintf(buf, HEX_COLOR_SZ, "#%02x%02x%02x", C(red), C(green), C(blue));
#undef C
}

static void
on_color_set(GtkColorButton *b, gpointer data) {
    (void)data;
    GdkRGBA c;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(b), &c);
    char buf[HEX_COLOR_SZ];
    hex_of(&c, buf);
    commit_field(b, buf);
}

static void
on_none_toggled(GtkToggleButton *t, gpointer data) {
    GtkWidget *color = data;
    const gboolean on = gtk_toggle_button_get_active(t);
    gtk_widget_set_sensitive(color, !on);
    if (on) { commit_field(t, "none"); return; }
    GdkRGBA c;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(color), &c);
    char buf[HEX_COLOR_SZ];
    hex_of(&c, buf);
    commit_field(t, buf);
}

static void
on_family_changed(GtkComboBox *c, gpointer data) {
    (void)data;
    if (gtk_combo_box_get_active(c) >= 0) commit_entry(gtk_bin_get_child(GTK_BIN(c)));
}

static gboolean
on_lines_focus_out(GtkWidget *tv, GdkEvent *e, gpointer data) {
    (void)e; (void)data;
    PyObject *f = field_of(tv);
    if (!f) return FALSE;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf, &a, &b);
    char *text = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
    if (strcmp(text, str_item(f, "value")) != 0 && !commit_field(tv, text)) gtk_text_buffer_set_text(buf, str_item(f, "value"), -1);
    g_free(text);
    return FALSE;
}

static void
stop_recording(void) {
    if (settings.recording_button) gtk_button_set_label(GTK_BUTTON(settings.recording_button), "Record");
    settings.recording_button = NULL;
    settings.recording_entry = NULL;
}

static void
on_record_clicked(GtkButton *b, gpointer entry) {
    stop_recording();
    settings.recording_button = GTK_WIDGET(b);
    settings.recording_entry = entry;
    gtk_button_set_label(b, "Press keys…");
}

static gboolean
on_settings_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w; (void)data;
    if (!settings.recording_entry) return FALSE;
    if (ev->is_modifier) return TRUE;
    GtkWidget *entry = settings.recording_entry;
    stop_recording();
    if (ev->keyval == GDK_KEY_Escape) return TRUE;
    GString *s = g_string_new("");
    if (ev->state & GDK_CONTROL_MASK) g_string_append(s, "ctrl+");
    if (ev->state & GDK_MOD1_MASK) g_string_append(s, "alt+");
    if (ev->state & GDK_SHIFT_MASK) g_string_append(s, "shift+");
    if (ev->state & GDK_SUPER_MASK) g_string_append(s, "super+");
    guint kv = gdk_keyval_to_lower(ev->keyval);
    const char *name = gdk_keyval_name(kv);
    char *lower = g_ascii_strdown(name ? name : "", -1);
    if (strcmp(lower, "return") == 0) g_string_append(s, "enter");
    else g_string_append(s, lower);
    g_free(lower);
    if (name && commit_field(entry, s->str)) {
        settings.building = true;
        gtk_entry_set_text(GTK_ENTRY(entry), s->str);
        settings.building = false;
    }
    g_string_free(s, TRUE);
    return TRUE;
}

static GtkWidget *
control_for_field(PyObject *f) {
    const char *kind = str_item(f, "kind"), *value = str_item(f, "value");
    if (strcmp(kind, "bool") == 0) {
        GtkWidget *s = gtk_switch_new();
        const bool on = g_ascii_strcasecmp(value, "yes") == 0 || g_ascii_strcasecmp(value, "y") == 0 || g_ascii_strcasecmp(value, "true") == 0;
        gtk_switch_set_active(GTK_SWITCH(s), on);
        gtk_widget_set_halign(s, GTK_ALIGN_START);
        attach_field(s, f);
        g_signal_connect(s, "state-set", G_CALLBACK(on_switch_changed), NULL);
        return s;
    }
    if (strcmp(kind, "choice") == 0) {
        GtkWidget *c = gtk_combo_box_text_new();
        PyObject *choices = list_item(f, "choices");
        gint active = -1;
        for (Py_ssize_t i = 0, n = choices ? PyList_GET_SIZE(choices) : 0; i < n; i++) {
            const char *t = PyUnicode_AsUTF8(PyList_GET_ITEM(choices, i));
            if (!t) { PyErr_Clear(); continue; }
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), t);
            if (strcmp(t, value) == 0) active = i;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(c), active);
        gtk_widget_set_halign(c, GTK_ALIGN_START);
        attach_field(c, f);
        g_signal_connect(c, "changed", G_CALLBACK(on_choice_changed), NULL);
        return c;
    }
    if (strcmp(kind, "color") == 0) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GdkRGBA c = {0, 0, 0, 1};
        const bool is_none = strcmp(value, "none") == 0;
        if (!is_none) gdk_rgba_parse(&c, value);
        GtkWidget *b = gtk_color_button_new_with_rgba(&c);
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(b), FALSE);
        attach_field(b, f);
        g_signal_connect(b, "color-set", G_CALLBACK(on_color_set), NULL);
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
        if (bool_item(f, "allow_none")) {
            GtkWidget *t = gtk_check_button_new_with_label("None");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t), is_none);
            gtk_widget_set_sensitive(b, !is_none);
            attach_field(t, f);
            g_signal_connect(t, "toggled", G_CALLBACK(on_none_toggled), b);
            gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
        }
        return box;
    }
    if (strcmp(kind, "font") == 0) {
        GtkWidget *c = gtk_combo_box_text_new_with_entry();
        for_each_in(list_item(settings.model, "families"), fam) {
            const char *t = PyUnicode_AsUTF8(fam);
            if (t) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), t);
            else PyErr_Clear();
        }
        GtkWidget *e = gtk_bin_get_child(GTK_BIN(c));
        gtk_entry_set_text(GTK_ENTRY(e), value);
        gtk_entry_set_width_chars(GTK_ENTRY(e), 30);
        attach_field(e, f);
        g_signal_connect(e, "activate", G_CALLBACK(on_entry_activate), NULL);
        g_signal_connect(e, "focus-out-event", G_CALLBACK(on_entry_focus_out), NULL);
        g_signal_connect(c, "changed", G_CALLBACK(on_family_changed), NULL);
        gtk_widget_set_halign(c, GTK_ALIGN_START);
        return c;
    }
    if (strcmp(kind, "int") == 0 || strcmp(kind, "float") == 0) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), value);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 10);
        attach_field(entry, f);
        g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), NULL);
        g_signal_connect(entry, "focus-out-event", G_CALLBACK(on_entry_focus_out), NULL);
        gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
        for (int direction = -1; direction <= 1; direction += 2) {
            GtkWidget *button = gtk_button_new_with_label(direction < 0 ? "−" : "+");
            g_object_set_data(G_OBJECT(button), "direction", GINT_TO_POINTER(direction));
            g_signal_connect(button, "clicked", G_CALLBACK(on_number_adjust), entry);
            gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
        }
        attach_field(box, f);
        gtk_widget_set_halign(box, GTK_ALIGN_START);
        return box;
    }
    if (strcmp(kind, "lines") == 0) {
        GtkWidget *tv = gtk_text_view_new();
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), value, -1);
        attach_field(tv, f);
        g_signal_connect(tv, "focus-out-event", G_CALLBACK(on_lines_focus_out), NULL);
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
        gtk_widget_set_size_request(sw, 360, 88);
        gtk_container_add(GTK_CONTAINER(sw), tv);
        return sw;
    }
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), value);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 30);
    attach_field(e, f);
    g_signal_connect(e, "activate", G_CALLBACK(on_entry_activate), NULL);
    g_signal_connect(e, "focus-out-event", G_CALLBACK(on_entry_focus_out), NULL);
    gtk_widget_set_halign(e, GTK_ALIGN_START);
    if (strcmp(kind, "shortcut") == 0) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(e), "No shortcut");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 0);
        GtkWidget *rec = gtk_button_new_with_label("Record");
        g_signal_connect(rec, "clicked", G_CALLBACK(on_record_clicked), e);
        gtk_box_pack_start(GTK_BOX(box), rec, FALSE, FALSE, 0);
        return box;
    }
    return e;
}

static char *
caption_of(PyObject *f, const char *prefix) {
    GString *s = g_string_new(prefix ? prefix : "");
    const char *help = str_item(f, "help");
    const char *end = strstr(help, ". "), *para = strchr(help, '\n');
    size_t cut = strlen(help);
    if (end) cut = MIN(cut, (size_t)(end - help) + 1);
    if (para) cut = MIN(cut, (size_t)(para - help));
    if (cut) {
        if (s->len) g_string_append_c(s, ' ');
        g_string_append_len(s, help, cut);
    }
    if (bool_item(f, "restart")) {
        if (s->len) g_string_append_c(s, ' ');
        g_string_append(s, "Applies after a restart.");
    }
    return g_string_free(s, FALSE);
}

static void
add_small_label(GtkGrid *grid, gint row, const char *text, bool is_notice) {
    GtkWidget *cap = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(cap), 0);
    gtk_label_set_line_wrap(GTK_LABEL(cap), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(cap), 70);
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(cap), attrs);
    pango_attr_list_unref(attrs);
    if (!is_notice) gtk_style_context_add_class(gtk_widget_get_style_context(cap), "dim-label");
    gtk_grid_attach(grid, cap, 1, row, 2, 1);
}

static gint
add_row(GtkGrid *grid, gint row, PyObject *f, const char *prefix) {
    GtkWidget *label = gtk_label_new(str_item(f, "title"));
    gtk_label_set_xalign(GTK_LABEL(label), 1);
    gtk_widget_set_valign(label, GTK_ALIGN_BASELINE);
    const char *help = str_item(f, "help");
    if (help[0]) gtk_widget_set_tooltip_text(label, help);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    GtkWidget *control = control_for_field(f);
    if (help[0]) gtk_widget_set_tooltip_text(control, help);
    gtk_grid_attach(grid, control, 1, row, 1, 1);
    GtkWidget *reset = gtk_button_new_from_icon_name("edit-undo-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(reset), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(reset, "Back to the default");
    gtk_widget_set_halign(reset, GTK_ALIGN_START);
    attach_field(reset, f);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_clicked), NULL);
    gtk_grid_attach(grid, reset, 2, row, 1, 1);
    gtk_widget_set_no_show_all(reset, !bool_item(f, "is_set"));
    // Controls that hold their value in a child report through the child, which needs to find the button
    g_object_set_data(G_OBJECT(control), "reset", reset);
    if (GTK_IS_BIN(control) && gtk_bin_get_child(GTK_BIN(control))) g_object_set_data(G_OBJECT(gtk_bin_get_child(GTK_BIN(control))), "reset", reset);
    row++;
    char *cap = caption_of(f, prefix);
    if (cap[0]) add_small_label(grid, row++, cap, false);
    g_free(cap);
    return row;
}

static PyObject *
current_settings_page(void) {
    PyObject *pages = list_item(settings.model, "pages");
    for_each_in(pages, p) if (settings.page_id && strcmp(str_item(p, "id"), settings.page_id) == 0) return p;
    return pages && PyList_GET_SIZE(pages) ? PyList_GET_ITEM(pages, 0) : NULL;
}

static bool
is_profiles_page(void) {
    return strcmp(str_item(current_settings_page(), "id"), "profiles") == 0;
}

static PyObject *
sidebar_items(void) {
    PyObject *p = current_settings_page();
    return list_item(p, is_profiles_page() ? "profiles" : "sections");
}

static PyObject *
current_profile(void) {
    if (!is_profiles_page()) return NULL;
    PyObject *items = sidebar_items();
    return items && settings.sidebar_index < PyList_GET_SIZE(items) ? PyList_GET_ITEM(items, settings.sidebar_index) : NULL;
}

static bool
field_matches(PyObject *f, const char *query) {
    for (const char **k = (const char *[]){"title", "name", "help", NULL}; *k; k++) {
        char *hay = g_utf8_casefold(str_item(f, *k), -1);
        const bool found = strstr(hay, query) != NULL;
        g_free(hay);
        if (found) return true;
    }
    return false;
}

static void
rebuild_rows(void) {
    GList *old = gtk_container_get_children(GTK_CONTAINER(settings.rows));
    for (GList *x = old; x; x = x->next) gtk_widget_destroy(x->data);
    g_list_free(old);
    settings.recording_button = NULL; settings.recording_entry = NULL;
    settings.building = true;
    GtkGrid *grid = GTK_GRID(settings.rows);
    gint row = 0;
    const char *notice = str_item(settings.model, "notice");
    if (notice[0]) add_small_label(grid, row++, notice, true);
    const char *query = gtk_entry_get_text(GTK_ENTRY(settings.search));
    if (query[0]) {
        char *q = g_utf8_casefold(query, -1);
        gint start = row;
        for_each_in(list_item(settings.model, "pages"), p) {
            for_each_in(list_item(p, "sections"), s) {
                for_each_in(list_item(s, "fields"), f) {
                    if (field_matches(f, q)) {
                        char *prefix = g_strdup_printf("%s › %s:", str_item(p, "title"), str_item(s, "title"));
                        row = add_row(grid, row, f, prefix);
                        g_free(prefix);
                    }
                }
            }
        }
        if (row == start) add_small_label(grid, row++, "Nothing matches", false);
        g_free(q);
    } else if (is_profiles_page()) {
        PyObject *tabs = list_item(current_profile(), "pages");
        if (tabs && settings.profile_tab < PyList_GET_SIZE(tabs)) {
            for_each_in(list_item(PyList_GET_ITEM(tabs, settings.profile_tab), "fields"), f) row = add_row(grid, row, f, NULL);
        }
    } else {
        PyObject *secs = sidebar_items();
        if (secs && settings.sidebar_index < PyList_GET_SIZE(secs)) {
            PyObject *s = PyList_GET_ITEM(secs, settings.sidebar_index);
            GtkWidget *h = gtk_label_new(NULL);
            char *markup = g_markup_printf_escaped("<b><big>%s</big></b>", str_item(s, "title"));
            gtk_label_set_markup(GTK_LABEL(h), markup);
            g_free(markup);
            gtk_label_set_xalign(GTK_LABEL(h), 0);
            gtk_grid_attach(grid, h, 0, row++, 3, 1);
            for_each_in(list_item(s, "fields"), f) row = add_row(grid, row, f, NULL);
        }
    }
    gtk_widget_show_all(settings.rows);
    settings.building = false;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(settings.scroll));
    gtk_adjustment_set_value(adj, 0);
}

static void
on_profile_tab_toggled(GtkToggleButton *b, gpointer data) {
    if (settings.building || !gtk_toggle_button_get_active(b)) return;
    settings.profile_tab = GPOINTER_TO_INT(data);
    rebuild_rows();
}

static void
rebuild_tab_bar(void) {
    GList *old = gtk_container_get_children(GTK_CONTAINER(settings.tab_bar));
    for (GList *x = old; x; x = x->next) gtk_widget_destroy(x->data);
    g_list_free(old);
    PyObject *tabs = is_profiles_page() && !gtk_entry_get_text_length(GTK_ENTRY(settings.search)) ? list_item(current_profile(), "pages") : NULL;
    gtk_widget_set_visible(settings.tab_bar, tabs != NULL);
    if (!tabs) return;
    if (settings.profile_tab >= PyList_GET_SIZE(tabs)) settings.profile_tab = 0;
    GtkWidget *group = NULL;
    const bool was_building = settings.building;
    settings.building = true;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(tabs); i++) {
        GtkWidget *b = gtk_radio_button_new_with_label_from_widget(group ? GTK_RADIO_BUTTON(group) : NULL, str_item(PyList_GET_ITEM(tabs, i), "title"));
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), i == settings.profile_tab);
        g_signal_connect(b, "toggled", G_CALLBACK(on_profile_tab_toggled), GINT_TO_POINTER((gint)i));
        gtk_box_pack_start(GTK_BOX(settings.tab_bar), b, FALSE, FALSE, 0);
        group = b;
    }
    settings.building = was_building;
    gtk_widget_show_all(settings.tab_bar);
}

static void
on_sidebar_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box; (void)data;
    if (settings.building || !row) return;
    settings.sidebar_index = gtk_list_box_row_get_index(row);
    rebuild_tab_bar();
    rebuild_rows();
}

static void
rebuild_sidebar(void) {
    const bool was_building = settings.building;
    settings.building = true;
    GList *old = gtk_container_get_children(GTK_CONTAINER(settings.sidebar));
    for (GList *x = old; x; x = x->next) gtk_widget_destroy(x->data);
    g_list_free(old);
    const bool profiles = is_profiles_page();
    const bool searching = gtk_entry_get_text_length(GTK_ENTRY(settings.search)) > 0;
    gtk_widget_set_visible(settings.profile_bar, profiles && !searching);
    if (!searching) {
        Py_ssize_t count = 0;
        for_each_in(sidebar_items(), d) {
            const char *title = str_item(d, profiles ? "name" : "title");
            char *text = bool_item(d, "is_default") ? g_strdup_printf("%s ★", title) : g_strdup(title);
            GtkWidget *l = gtk_label_new(text);
            g_free(text);
            gtk_label_set_xalign(GTK_LABEL(l), 0);
            gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
            g_object_set(l, "margin", 6, NULL);
            gtk_list_box_insert(GTK_LIST_BOX(settings.sidebar), l, -1);
            count++;
        }
        if (settings.sidebar_index >= count) settings.sidebar_index = count ? count - 1 : 0;
        GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(settings.sidebar), settings.sidebar_index);
        if (r) gtk_list_box_select_row(GTK_LIST_BOX(settings.sidebar), r);
    }
    gtk_widget_show_all(settings.sidebar);
    settings.building = was_building;
}

static void
rebuild_all(void) {
    if (!settings.window) return;
    rebuild_sidebar();
    rebuild_tab_bar();
    rebuild_rows();
}

static void
on_page_toggled(GtkRadioToolButton *b, gpointer data) {
    if (settings.building || !gtk_toggle_tool_button_get_active(GTK_TOGGLE_TOOL_BUTTON(b))) return;
    g_free(settings.page_id);
    settings.page_id = g_strdup(data);
    settings.sidebar_index = 0;
    settings.profile_tab = 0;
    gtk_entry_set_text(GTK_ENTRY(settings.search), "");
    rebuild_all();
}

static void
on_search_changed(GtkSearchEntry *e, gpointer data) {
    (void)e; (void)data;
    rebuild_all();
}

static const char *
icon_for_page(const char *id) {
    if (strcmp(id, "appearance") == 0) return "preferences-desktop-theme";
    if (strcmp(id, "profiles") == 0) return "system-users";
    if (strcmp(id, "keys") == 0) return "input-keyboard";
    if (strcmp(id, "pointer") == 0) return "input-mouse";
    if (strcmp(id, "advanced") == 0) return "applications-engineering";
    return "preferences-system";
}

static void
rebuild_toolbar(void) {
    GList *old = gtk_container_get_children(GTK_CONTAINER(settings.toolbar));
    for (GList *x = old; x; x = x->next) gtk_widget_destroy(x->data);
    g_list_free(old);
    GSList *group = NULL;
    const bool was_building = settings.building;
    settings.building = true;
    const PyObject *cur = current_settings_page();
    for_each_in(list_item(settings.model, "pages"), p) {
        GtkToolItem *b = gtk_radio_tool_button_new(group);
        group = gtk_radio_tool_button_get_group(GTK_RADIO_TOOL_BUTTON(b));
        gtk_tool_button_set_label(GTK_TOOL_BUTTON(b), str_item(p, "title"));
        gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(b), icon_for_page(str_item(p, "id")));
        gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(b), p == cur);
        g_signal_connect_data(b, "toggled", G_CALLBACK(on_page_toggled), g_strdup(str_item(p, "id")), free_closure_data, 0);
        gtk_toolbar_insert(GTK_TOOLBAR(settings.toolbar), b, -1);
    }
    GtkToolItem *sep = gtk_separator_tool_item_new();
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(sep), FALSE);
    gtk_tool_item_set_expand(sep, TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(settings.toolbar), sep, -1);
    if (!settings.search) {
        settings.search = gtk_search_entry_new();
        g_object_ref(settings.search);
        gtk_entry_set_placeholder_text(GTK_ENTRY(settings.search), "Search all settings");
        g_signal_connect(settings.search, "search-changed", G_CALLBACK(on_search_changed), NULL);
    } else {
        GtkWidget *parent = gtk_widget_get_parent(settings.search);
        if (parent) gtk_container_remove(GTK_CONTAINER(parent), settings.search);
    }
    GtkToolItem *holder = gtk_tool_item_new();
    gtk_container_add(GTK_CONTAINER(holder), settings.search);
    gtk_toolbar_insert(GTK_TOOLBAR(settings.toolbar), holder, -1);
    gtk_widget_show_all(settings.toolbar);
    settings.building = was_building;
}

static void
on_profile_new(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    g_free(emit_event("profile_new", "New Profile", NULL, NULL));
}

static void
on_profile_duplicate(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    g_free(emit_event("profile_duplicate", str_item(current_profile(), "id"), NULL, NULL));
}

static void
on_profile_default(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    g_free(emit_event("profile_set_default", str_item(current_profile(), "id"), NULL, NULL));
}

static void
on_delete_response(GtkDialog *dialog, gint response, gpointer data) {
    if (response == GTK_RESPONSE_ACCEPT) {
        char *err = emit_event("profile_delete", (const char *)data, NULL, NULL);
        if (err) { show_error(err); g_free(err); }
    }
    g_free(data);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
on_profile_delete(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    PyObject *p = current_profile();
    if (!bool_item(p, "can_delete")) { show_error("The default profile cannot be deleted"); return; }
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(settings.window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Delete the profile “%s”?", str_item(p, "name"));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "Windows using it keep running with the default profile.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL, "Delete", GTK_RESPONSE_ACCEPT, NULL);
    g_signal_connect(dialog, "response", G_CALLBACK(on_delete_response), g_strdup(str_item(p, "id")));
    gtk_widget_show(dialog);
}

static gboolean
on_settings_delete(GtkWidget *w, GdkEvent *e, gpointer d) {
    (void)e; (void)d;
    // Commits an edit in progress, and keeps the window for the next time it is opened
    gtk_window_set_focus(GTK_WINDOW(w), NULL);
    stop_recording();
    gtk_widget_hide(w);
    return TRUE;
}

static GtkWidget *
small_button(const char *icon, const char *text, const char *tip, GCallback cb) {
    GtkWidget *b = icon ? gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU) : gtk_button_new_with_label(text);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void
create_settings_window(void) {
    settings.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(settings.window), "Settings");
    gtk_window_set_default_size(GTK_WINDOW(settings.window), 900, 640);
    gtk_window_set_icon_name(GTK_WINDOW(settings.window), "silkie");
    g_signal_connect(settings.window, "delete-event", G_CALLBACK(on_settings_delete), NULL);
    g_signal_connect(settings.window, "key-press-event", G_CALLBACK(on_settings_key_press), NULL);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    settings.toolbar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(settings.toolbar), GTK_TOOLBAR_BOTH);
    gtk_box_pack_start(GTK_BOX(root), settings.toolbar, FALSE, FALSE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    settings.sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *side_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    settings.sidebar = gtk_list_box_new();
    g_signal_connect(settings.sidebar, "row-selected", G_CALLBACK(on_sidebar_selected), NULL);
    gtk_container_add(GTK_CONTAINER(side_scroll), settings.sidebar);
    gtk_box_pack_start(GTK_BOX(settings.sidebar_box), side_scroll, TRUE, TRUE, 0);
    settings.profile_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_pack_start(GTK_BOX(settings.profile_bar), small_button("list-add-symbolic", NULL, "New profile", G_CALLBACK(on_profile_new)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings.profile_bar), small_button("list-remove-symbolic", NULL, "Delete profile", G_CALLBACK(on_profile_delete)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings.profile_bar), small_button(NULL, "Duplicate", "Copy this profile", G_CALLBACK(on_profile_duplicate)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings.profile_bar), small_button(NULL, "Set as Default", "Use this profile for new windows and tabs", G_CALLBACK(on_profile_default)), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(settings.sidebar_box), settings.profile_bar, FALSE, FALSE, 4);
    gtk_widget_set_size_request(settings.sidebar_box, 200, -1);
    gtk_paned_pack1(GTK_PANED(paned), settings.sidebar_box, FALSE, FALSE);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    settings.tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(settings.tab_bar), "linked");
    gtk_widget_set_halign(settings.tab_bar, GTK_ALIGN_CENTER);
    g_object_set(settings.tab_bar, "margin-top", 8, NULL);
    gtk_box_pack_start(GTK_BOX(right), settings.tab_bar, FALSE, FALSE, 0);
    settings.scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(settings.scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    settings.rows = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(settings.rows), 6);
    gtk_grid_set_column_spacing(GTK_GRID(settings.rows), 10);
    g_object_set(settings.rows, "margin", 14, NULL);
    gtk_container_add(GTK_CONTAINER(settings.scroll), settings.rows);
    gtk_box_pack_start(GTK_BOX(right), settings.scroll, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(settings.window), root);
}

static void
take_model(PyObject *model) {
    Py_XDECREF(settings.model);
    settings.model = Py_NewRef(model);
    const char *page = str_item(model, "select_page");
    if (page[0]) {
        g_free(settings.page_id);
        settings.page_id = g_strdup(page);
        settings.sidebar_index = 0;
        settings.profile_tab = 0;
        // A profile asked for, such as the profile of a tab that its settings popover edits
        const char *wanted = str_item(model, "select_profile");
        PyObject *items = wanted[0] && is_profiles_page() ? sidebar_items() : NULL;
        for (Py_ssize_t i = 0; items && i < PyList_GET_SIZE(items); i++) {
            if (strcmp(str_item(PyList_GET_ITEM(items, i), "id"), wanted) == 0) settings.sidebar_index = (gint)i;
        }
    }
}

static void
show_settings(PyObject *model, PyObject *callback) {
    Py_XDECREF(settings.callback);
    settings.callback = Py_NewRef(callback);
    take_model(model);
    if (!settings.window) create_settings_window();
    gtk_widget_show_all(settings.window);
    rebuild_toolbar();
    rebuild_all();
    gtk_window_present(GTK_WINDOW(settings.window));
}

static void
update_settings(PyObject *model) {
    if (!settings.window || !gtk_widget_get_visible(settings.window)) return;
    take_model(model);
    // Can be called from the handler of a control that this destroys
    g_idle_add(rebuild_later, NULL);
}

static bool
snapshot_settings(const char *path) {
    if (!settings.window) return false;
    const int w = gtk_widget_get_allocated_width(settings.window), h = gtk_widget_get_allocated_height(settings.window);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(settings.window, cr);
    cairo_destroy(cr);
    const bool ok = cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(s);
    return ok;
}
// }}}

static void *
wayland_display(void) {
#ifdef GDK_WINDOWING_WAYLAND
    GdkDisplay *d = gdk_display_get_default();
    if (d && GDK_IS_WAYLAND_DISPLAY(d)) {
        shares_wayland_display = true;
        return gdk_wayland_display_get_wl_display(d);
    }
#endif
    return NULL;
}

__attribute__((visibility("default"))) const NativeUIBackend *
native_ui_gtk_backend(const NativeUIHost *h) {
    host = h;
    g_set_prgname("silkie");
    g_set_application_name("Silkie");
    gdk_set_allowed_backends(h->is_wayland ? "wayland" : "x11");
    if (!gtk_init_check(NULL, NULL)) return NULL;
    gdk_set_program_class("silkie");
    h->set_event_loop_hook(event_loop_hook);
#ifdef GDK_WINDOWING_X11
    pages = g_hash_table_new(g_int64_hash, g_int64_equal);
    if (!h->is_wayland) {
        int first_event, first_error;
        if (!XQueryExtension(gdk_xdisplay(), "XInputExtension", &xi_opcode, &first_event, &first_error)) xi_opcode = -1;
        gdk_window_add_filter(NULL, focus_event_filter, NULL);
    }
#endif
    static const NativeUIBackend b = {
        .tabs_supported = tabs_supported,
#ifdef GDK_WINDOWING_X11
        .adopt_os_window = adopt_os_window,
        .os_window_closing = os_window_closing,
        .attach_tab = attach_tab,
        .select_tab = select_tab,
        .move_tab = move_tab,
        .detach_tab = detach_tab,
        .tab_members = tab_members,
        .title_changed = title_changed,
        .tab_status = tab_status,
        .tab_settings_button = tab_settings_button,
        .show_tab_settings = show_tab_settings,
        .snapshot_tab = snapshot_tab,
        .window_op = window_op,
        .window_query = window_query,
        .saved_height_adjustment = saved_height_adjustment,
        .find_bar = find_bar,
        .snapshot_find_bar = snapshot_find_bar,
#endif
        .wayland_display = wayland_display,
        .show_settings = show_settings,
        .update_settings = update_settings,
        .update_menu = update_menu,
        .snapshot_settings = snapshot_settings,
    };
    return &b;
}

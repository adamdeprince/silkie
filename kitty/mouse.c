/*
 * mouse.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "state.h"
#include "screen.h"
#include "charsets.h"
#include "dnd.h"
#include <limits.h>
#include <math.h>
#include "glfw-wrapper.h"
#include "control-codes.h"
#include "base64.h"
#include "extension-fields.h"

extern PyTypeObject Screen_Type;

static MouseShape mouse_cursor_shape = TEXT_POINTER;
typedef enum MouseActions { PRESS, RELEASE, DRAG, MOVE, LEAVE } MouseAction;
#define debug debug_input

static void
finish_scroll_animation(Screen *screen) {
    if (screen->callbacks != Py_None) {
        PyObject *ret = PyObject_CallMethod(screen->callbacks, "finish_scroll_animation", NULL);
        if (ret == NULL) PyErr_Print();
        else Py_DECREF(ret);
    }
}

// Encoding of mouse events {{{
#define SHIFT_INDICATOR (1 << 2)
#define ALT_INDICATOR (1 << 3)
#define CONTROL_INDICATOR (1 << 4)
#define MOTION_INDICATOR (1 << 5)
#define SCROLL_BUTTON_INDICATOR (1 << 6)
#define EXTRA_BUTTON_INDICATOR (1 << 7)
#define LEAVE_INDICATOR (1 << 8)


static unsigned int
button_map(int button) {
    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT: return 1;
        case GLFW_MOUSE_BUTTON_RIGHT: return 3;
        case GLFW_MOUSE_BUTTON_MIDDLE: return 2;
        case GLFW_MOUSE_BUTTON_4:
        case GLFW_MOUSE_BUTTON_5:
        case GLFW_MOUSE_BUTTON_6:
        case GLFW_MOUSE_BUTTON_7:
        case GLFW_MOUSE_BUTTON_8: return button + 5;
        default: return UINT_MAX;
    }
}

static unsigned int
encode_button(unsigned int button) {
    if (button >= 8 && button <= 11) {
        return (button - 8) | EXTRA_BUTTON_INDICATOR;
    } else if (button >= 4 && button <= 7) {
        return (button - 4) | SCROLL_BUTTON_INDICATOR;
    } else if (button >= 1 && button <= 3) {
        return button - 1;
    } else {
        return UINT_MAX;
    }
}

static char mouse_event_buf[64];

static int
encode_mouse_event_impl(const MousePosition *mpos, int mouse_tracking_protocol, int button, MouseAction action, int mods) {
    unsigned int cb = encode_button(button);
    switch (action) {
        case MOVE:
            if (cb == UINT_MAX) cb = 3;
            cb += 32;
            break;
        case LEAVE:
            if (mouse_tracking_protocol != SGR_PIXEL_PROTOCOL) return 0;
            cb = LEAVE_INDICATOR | MOTION_INDICATOR;
            break;
        default:
            if (cb == UINT_MAX) return 0;
            break;
    }
    if (action == DRAG || action == MOVE) cb |= MOTION_INDICATOR;
    else if (action == RELEASE && mouse_tracking_protocol < SGR_PROTOCOL) cb = 3;
    if (mods & GLFW_MOD_SHIFT) cb |= SHIFT_INDICATOR;
    if (mods & GLFW_MOD_ALT) cb |= ALT_INDICATOR;
    if (mods & GLFW_MOD_CONTROL) cb |= CONTROL_INDICATOR;
    int x = mpos->cell_x + 1, y = mpos->cell_y + 1;
    switch (mouse_tracking_protocol) {
        case SGR_PIXEL_PROTOCOL:
            x = (int)round(mpos->global_x);
            y = (int)round(mpos->global_y);
            /* fallthrough */
        case SGR_PROTOCOL: return snprintf(mouse_event_buf, sizeof(mouse_event_buf), "<%d;%d;%d%s", cb, x, y, action == RELEASE ? "m" : "M"); break;
        case URXVT_PROTOCOL: return snprintf(mouse_event_buf, sizeof(mouse_event_buf), "%d;%d;%dM", cb + 32, x, y); break;
        case UTF8_PROTOCOL:
            mouse_event_buf[0] = 'M';
            mouse_event_buf[1] = cb + 32;
            unsigned int sz = 2;
            sz += encode_utf8(x + 32, mouse_event_buf + sz);
            sz += encode_utf8(y + 32, mouse_event_buf + sz);
            return sz;
            break;
        default:
            if (x > 223 || y > 223) return 0;
            else {
                mouse_event_buf[0] = 'M';
                mouse_event_buf[1] = cb + 32;
                mouse_event_buf[2] = x + 32;
                mouse_event_buf[3] = y + 32;
                return 4;
            }
            break;
    }
    return 0;
}

static int
encode_mouse_event(Window *w, int button, MouseAction action, int mods) {
    Screen *screen = w->render_data.screen;
    return encode_mouse_event_impl(&w->mouse_pos, screen->modes.mouse_tracking_protocol, button, action, mods);
}

static int
encode_mouse_button(Window *w, int button, MouseAction action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        switch (action) {
            case PRESS:
                global_state.tracked_drag_in_window = w->id;
                global_state.tracked_drag_button = button;
                break;
            case RELEASE:
                global_state.tracked_drag_in_window = 0;
                global_state.tracked_drag_button = -1;
                break;
            default: break;
        }
    }
    return encode_mouse_event(w, button_map(button), action, mods);
}

static int
encode_mouse_scroll(Window *w, int button, int mods) {
    return encode_mouse_event(w, button, PRESS, mods);
}

// }}}

static Window *
window_for_id(id_type window_id) {
    if (global_state.callback_os_window && global_state.callback_os_window->num_tabs) {
        Tab *t = global_state.callback_os_window->tabs + global_state.callback_os_window->active_tab;
        for (unsigned int i = 0; i < t->num_windows; i++) {
            Window *w = t->windows + i;
            if (w->id == window_id) return w;
        }
    }
    return window_for_window_id(window_id);
}

static void
update_scrollbar_hover_state(Window *w, bool hovering) {
    if (!w) return;
    bool changed = w->scrollbar.is_hovering != hovering;
    w->scrollbar.is_hovering = hovering;

    if (changed && global_state.callback_os_window) {
        global_state.callback_os_window->needs_render = true;
        request_tick_callback();
    }
}

static void
set_currently_hovered_window(id_type window_id, int modifiers, bool focus_follows) {
    if (global_state.mouse_hover_in_window != window_id) {
        Window *left_window = window_for_id(global_state.mouse_hover_in_window);
        global_state.mouse_hover_in_window = window_id;
        if (left_window) {
            if (left_window->scrollbar.is_hovering) update_scrollbar_hover_state(left_window, false);
            if (left_window->render_data.screen) screen_mark_url(left_window->render_data.screen, 0, 0, 0, 0);
            if (left_window->drop.hovered) drop_left_child(left_window);
            int sz = encode_mouse_event(left_window, 0, LEAVE, modifiers);
            if (sz > 0) {
                mouse_event_buf[sz] = 0;
                write_escape_code_to_child(left_window->render_data.screen, ESC_CSI, mouse_event_buf);
                debug("Sent mouse leave event to window: %llu currently hovering: %llu\n", left_window->id, window_id);
            }
        }
        if (focus_follows && window_id && OPT(focus_follows_mouse).on_cross && global_state.callback_os_window && global_state.callback_os_window->num_tabs) {
            Tab *t = global_state.callback_os_window->tabs + global_state.callback_os_window->active_tab;
            for (unsigned i = 0; i < t->num_windows; i++) {
                if (t->windows[i].id == window_id) {
                    if (i != t->active_window) call_boss(switch_focus_to_in_active_tab, "K", window_id);
                    break;
                }
            }
        }
    }
}

static bool
dispatch_mouse_event(Window *w, int button, int count, int modifiers, bool grabbed) {
    bool handled = false;
    if (w->render_data.screen && w->render_data.screen->callbacks != Py_None) {
        PyObject *callback_ret = PyObject_CallMethod(
            w->render_data.screen->callbacks,
            "on_mouse_event",
            "{si si si sO}",
            "button",
            button,
            "repeat_count",
            count,
            "mods",
            modifiers,
            "grabbed",
            grabbed ? Py_True : Py_False);
        if (callback_ret == NULL) PyErr_Print();
        else {
            handled = callback_ret == Py_True;
            Py_DECREF(callback_ret);
        }
        if (OPT(debug_keyboard)) {
            const char *evname = "move";
            switch (count) {
                case -3: evname = "doubleclick"; break;
                case -2: evname = "click"; break;
                case -1: evname = "release"; break;
                case 1: evname = "press"; break;
                case 2: evname = "doublepress"; break;
                case 3: evname = "triplepress"; break;
            }
            const char *bname = "unknown";
            switch (button) {
                case GLFW_MOUSE_BUTTON_LEFT: bname = "left"; break;
                case GLFW_MOUSE_BUTTON_MIDDLE: bname = "middle"; break;
                case GLFW_MOUSE_BUTTON_RIGHT: bname = "right"; break;
                case GLFW_MOUSE_BUTTON_4: bname = "b4"; break;
                case GLFW_MOUSE_BUTTON_5: bname = "b5"; break;
                case GLFW_MOUSE_BUTTON_6: bname = "b6"; break;
                case GLFW_MOUSE_BUTTON_7: bname = "b7"; break;
                case GLFW_MOUSE_BUTTON_8: bname = "b8"; break;
            }
            debug("\x1b[33mon_mouse_input\x1b[m: %s button: %s %sgrabbed: %d handled_in_kitty: %d\n", evname, bname, format_mods(modifiers), grabbed, handled);
        }
    }
    return handled;
}

static unsigned int
window_left(Window *w) {
    return w->render_data.geometry.left - w->padding.left - w->size_mismatch_padding.left;
}

static unsigned int
window_right(Window *w) {
    return w->render_data.geometry.right + w->padding.right + w->size_mismatch_padding.right;
}

static unsigned int
window_top(Window *w) {
    return w->render_data.geometry.top - w->padding.top - w->size_mismatch_padding.top;
}

static unsigned int
window_bottom(Window *w) {
    return w->render_data.geometry.bottom + w->padding.bottom + w->size_mismatch_padding.bottom;
}

static bool
contains_mouse(Window *w) {
    double x = global_state.callback_os_window->mouse_x, y = global_state.callback_os_window->mouse_y;
    return (w->visible && window_left(w) <= x && x < window_right(w) && window_top(w) <= y && y < window_bottom(w));
}

static bool
border_contains_mouse(BorderRect *br, double tolerance, Edge *edges) {
    bool ans = false;
    double x = global_state.callback_os_window->mouse_x, y = global_state.callback_os_window->mouse_y;
    if ((int)br->px.left - tolerance <= x && x < (int)br->px.right + tolerance && (int)br->px.top - tolerance <= y && y < (int)br->px.bottom + tolerance) {
        ans = true;
        if (!br->horizontal) *edges |= br->border_type < 0 ? LEFT_EDGE : RIGHT_EDGE;
        else *edges |= br->border_type < 0 ? TOP_EDGE : BOTTOM_EDGE;
    }
    return ans;
}


static bool clamp_to_window = false;

static bool
cell_for_pos(Window *w, unsigned int *x, unsigned int *y, bool *in_left_half_of_cell, OSWindow *os_window) {
    WindowGeometry *g = &w->render_data.geometry;
    Screen *screen = w->render_data.screen;
    if (!screen) return false;
    unsigned int qx = 0, qy = 0;
    bool in_left_half = true;
    double mouse_x = global_state.callback_os_window->mouse_x;
    double mouse_y = global_state.callback_os_window->mouse_y;
    double left = g->left, top = g->top, right = g->right, bottom = g->bottom;
    w->mouse_pos.global_x = mouse_x - left;
    w->mouse_pos.global_y = mouse_y - top;
    if (clamp_to_window) {
        mouse_x = MIN(MAX(mouse_x, left), right);
        mouse_y = MIN(MAX(mouse_y, top), bottom);
    }
    if (mouse_x < left || mouse_y < top || mouse_x > right || mouse_y > bottom) return false;
    if (mouse_x >= g->right) {
        qx = screen->columns - 1;
        in_left_half = false;
    } else if (mouse_x >= g->left) {
        double xval = (double)(mouse_x - g->left) / os_window->fonts_data->fcm.cell_width;
        double fxval = floor(xval);
        qx = (unsigned int)fxval;
        in_left_half = (xval - fxval <= 0.5) ? true : false;
    }
    if (mouse_y >= g->bottom) qy = screen->lines - 1;
    else if (mouse_y >= g->top) { qy = (unsigned int)((double)(mouse_y - g->top - screen->pixel_scroll_offset_y) / os_window->fonts_data->fcm.cell_height); }
    if (qx < screen->columns && qy < screen->lines) {
        *x = qx;
        *y = qy;
        *in_left_half_of_cell = in_left_half;
        return true;
    }
    return false;
}

#define HANDLER(name) static void name(Window UNUSED *w, int UNUSED button, int UNUSED modifiers, unsigned int UNUSED window_idx)

static void
set_mouse_cursor_when_dragging(Screen *screen) {
    MouseShape expected_shape = OPT(pointer_shape_when_dragging);
    if (screen && screen->selections.count && screen->selections.items[0].rectangle_select) expected_shape = OPT(pointer_shape_when_dragging_rectangle);
    if (mouse_cursor_shape != expected_shape) {
        mouse_cursor_shape = expected_shape;
        set_mouse_cursor(mouse_cursor_shape);
    }
}

static void
update_drag(Window *w) {
    Screen *screen = w->render_data.screen;
    if (screen && screen->selections.in_progress) {
        screen_update_selection(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y, w->mouse_pos.in_left_half_of_cell, (SelectionUpdate){0});
    }
    set_mouse_cursor_when_dragging(screen);
}

static bool
do_drag_scroll(Window *w, bool upwards) {
    Screen *screen = w->render_data.screen;
    if (screen->linebuf == screen->main_linebuf) {
        finish_scroll_animation(screen);
        screen_history_scroll(screen, SCROLL_LINE, upwards);
        update_drag(w);
        if (mouse_cursor_shape != DEFAULT_POINTER) {
            mouse_cursor_shape = DEFAULT_POINTER;
            set_mouse_cursor(mouse_cursor_shape);
        }
        return true;
    }
    return false;
}

bool
drag_scroll(Window *w, OSWindow *frame) {
    unsigned int margin = frame->fonts_data->fcm.cell_height / 2;
    double y = frame->mouse_y;
    bool upwards = y <= (w->render_data.geometry.top + margin);
    if (upwards || y >= w->render_data.geometry.bottom - margin) {
        if (do_drag_scroll(w, upwards)) {
            frame->last_mouse_activity_at = monotonic();
            return true;
        }
    }
    return false;
}

static void
extend_selection(Window *w, bool ended, bool extend_nearest) {
    Screen *screen = w->render_data.screen;
    if (screen_has_selection(screen)) {
        screen_update_selection(
            screen,
            w->mouse_pos.cell_x,
            w->mouse_pos.cell_y,
            w->mouse_pos.in_left_half_of_cell,
            (SelectionUpdate){.ended = ended, .set_as_nearest_extend = extend_nearest});
    }
}

static void
set_mouse_cursor_for_screen(Screen *screen) {
    MouseShape s = screen_pointer_shape(screen);
    if (s != INVALID_POINTER) {
        mouse_cursor_shape = s;
    } else {
        if (screen->modes.mouse_tracking_mode == NO_TRACKING) {
            mouse_cursor_shape = OPT(default_pointer_shape);
        } else {
            mouse_cursor_shape = OPT(pointer_shape_when_grabbed);
        }
    }
}

static void
handle_mouse_movement_in_kitty(Window *w, int button, bool mouse_cell_changed) {
    Screen *screen = w->render_data.screen;
    if (screen->selections.in_progress && (button == global_state.active_drag_button)) {
        monotonic_t now = monotonic();
        if ((now - w->last_drag_scroll_at) >= ms_to_monotonic_t(20ll) || mouse_cell_changed) {
            update_drag(w);
            w->last_drag_scroll_at = now;
        }
    }
}

static void
detect_url(Screen *screen, unsigned int x, unsigned int y) {
    int hid = screen_detect_url(screen, x, y);
    zero_at_ptr(&screen->current_hyperlink_under_mouse);
    if (hid != 0) {
        mouse_cursor_shape = POINTER_POINTER;
        screen->current_hyperlink_under_mouse.has_detected_url = hid < 0;
        if (hid > 0) {
            screen->current_hyperlink_under_mouse.id = (hyperlink_id_type)hid;
            screen->current_hyperlink_under_mouse.x = x;
            screen->current_hyperlink_under_mouse.y = y;
        }
    } else set_mouse_cursor_for_screen(screen);
}

static bool
should_handle_in_kitty(Window *w, Screen *screen, int button) {
    bool in_tracking_mode = (screen->modes.mouse_tracking_mode == ANY_MODE || (screen->modes.mouse_tracking_mode == MOTION_MODE && button >= 0));
    return !in_tracking_mode || global_state.active_drag_in_window == w->id;
}

static bool
set_mouse_position(Window *w, bool *mouse_cell_changed, bool *cell_half_changed) {
    unsigned int x = 0, y = 0;
    bool in_left_half_of_cell = false;
    if (!cell_for_pos(w, &x, &y, &in_left_half_of_cell, global_state.callback_os_window)) return false;
    *mouse_cell_changed = x != w->mouse_pos.cell_x || y != w->mouse_pos.cell_y;
    *cell_half_changed = in_left_half_of_cell != w->mouse_pos.in_left_half_of_cell;
    w->mouse_pos.cell_x = x;
    w->mouse_pos.cell_y = y;
    w->mouse_pos.in_left_half_of_cell = in_left_half_of_cell;
    return true;
}

// Scrollbar {{{
typedef enum { SCROLLBAR_HIT_NONE, SCROLLBAR_HIT_TRACK, SCROLLBAR_HIT_THUMB } ScrollbarHitType;

typedef struct {
    double left, right, top, bottom;
    double width, gap, hitbox_expansion;
} ScrollbarGeometry;


static bool
validate_scrollbar_state(const Window *w) {
    return w && w->render_data.screen && w->render_data.screen->historybuf && w->render_data.screen->historybuf->count > 0;
}

static ScrollbarGeometry
calculate_scrollbar_geometry(const Window *w) {
    ScrollbarGeometry geom = {0};
    if (!w || !w->render_data.screen) return geom;

    const WindowGeometry *g = &w->render_data.geometry;
    unsigned cell_width = w->render_data.screen->cell_size.width;
    geom.width = (double)OPT(scrollbar_width) * cell_width;
    if (w->scrollbar.is_hovering) geom.width = (double)OPT(scrollbar_hover_width) * cell_width;
    geom.gap = (double)OPT(scrollbar_gap) * cell_width;
    geom.hitbox_expansion = (double)OPT(scrollbar_hitbox_expansion) * cell_width;

    double right_edge = g->right + g->spaces.right;
    geom.left = right_edge - geom.gap - geom.width - geom.hitbox_expansion;
    geom.right = right_edge + geom.gap;
    geom.top = g->top - g->spaces.top;
    geom.bottom = g->bottom + g->spaces.bottom;

    return geom;
}

static ScrollbarHitType
get_scrollbar_hit_type(const Window *w, double mouse_x, double mouse_y) {
    if (!w || !validate_scrollbar_state(w)) return SCROLLBAR_HIT_NONE;

    ScrollbarGeometry geom = calculate_scrollbar_geometry(w);

    if (mouse_x < geom.left || mouse_x > geom.right || mouse_y < geom.top || mouse_y > geom.bottom) { return SCROLLBAR_HIT_NONE; }

    OSWindow *os_window = global_state.callback_os_window;
    if (!os_window) return SCROLLBAR_HIT_TRACK;
    double mouse_window_fraction = mouse_y / os_window->viewport_height;
    unsigned cell_width = w->render_data.screen->cell_size.width;
    double hitbox_expansion_fraction = (double)(OPT(scrollbar_hitbox_expansion) * cell_width) / os_window->viewport_height;

    if (mouse_window_fraction >= (w->scrollbar.thumb_top - hitbox_expansion_fraction) &&
        mouse_window_fraction <= (w->scrollbar.thumb_bottom + hitbox_expansion_fraction)) {
        return SCROLLBAR_HIT_THUMB;
    }

    return SCROLLBAR_HIT_TRACK;
}

static void
handle_scrollbar_track_click(Window *w, double mouse_y) {
    if (!w) return;
    Screen *screen = w->render_data.screen;
    if (!validate_scrollbar_state(w)) return;
    finish_scroll_animation(screen);

    if (OPT(scrollbar_jump_on_click)) {
        ScrollbarGeometry geom = calculate_scrollbar_geometry(w);
        double scrollbar_height = geom.bottom - geom.top;
        double mouse_pane_fraction = (mouse_y - geom.top) / scrollbar_height;
        double target_scrolled_by = screen->historybuf->count * (1.0 - mouse_pane_fraction);
        screen_history_scroll_to_absolute(screen, target_scrolled_by);
    } else {
        OSWindow *os_window = global_state.callback_os_window;
        if (!os_window) return;
        double mouse_window_fraction = mouse_y / os_window->viewport_height;
        bool click_above_thumb = mouse_window_fraction < w->scrollbar.thumb_top;
        screen_history_scroll(screen, SCROLL_PAGE, click_above_thumb);
    }
}

static void
end_drag(Window *w) {
    Screen *screen = w->render_data.screen;
    global_state.active_drag_in_window = 0;
    global_state.active_drag_button = -1;
    w->last_drag_scroll_at = 0;
    w->scrollbar.is_dragging = false;
    if (w->drag_source.potential_drag.type == POTENTIAL_DRAG_SELECTION) {
        // A press on selected text that never became a drag is an ordinary click.
        screen_start_selection(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y, w->mouse_pos.in_left_half_of_cell, false, EXTEND_CELL);
    }
    zero_at_ptr(&w->drag_source.potential_drag);
    zero_at_ptr(&w->drag_source.initial_left_press);

    if (global_state.callback_os_window &&
        get_scrollbar_hit_type(w, global_state.callback_os_window->mouse_x, global_state.callback_os_window->mouse_y) == SCROLLBAR_HIT_NONE) {
        mouse_cursor_shape = TEXT_POINTER;
        set_mouse_cursor(mouse_cursor_shape);
    }

    if (screen->selections.in_progress) {
        screen_update_selection(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y, w->mouse_pos.in_left_half_of_cell, (SelectionUpdate){.ended = true});
    }
}


static void
start_scrollbar_drag(Window *w, double mouse_y) {
    if (!w) return;
    Screen *screen = w->render_data.screen;
    if (!validate_scrollbar_state(w)) return;

    ScrollbarGeometry geom = calculate_scrollbar_geometry(w);
    double scrollbar_height = geom.bottom - geom.top;
    double mouse_pane_fraction = (mouse_y - geom.top) / scrollbar_height;
    w->scrollbar.is_dragging = true;
    w->scrollbar.drag_start_y = mouse_pane_fraction;
    w->scrollbar.drag_start_scrolled_by = screen->scrolled_by;
}

static void
handle_scrollbar_drag(Window *w, double mouse_y) {
    if (!w || !w->scrollbar.is_dragging || !validate_scrollbar_state(w)) return;
    Screen *screen = w->render_data.screen;
    finish_scroll_animation(screen);
    ScrollbarGeometry geom = calculate_scrollbar_geometry(w);
    double scrollbar_height = geom.bottom - geom.top;
    double mouse_pane_fraction = (mouse_y - geom.top) / scrollbar_height;
    double delta_y = mouse_pane_fraction - w->scrollbar.drag_start_y;
    double visible_fraction = (double)screen->lines / (screen->lines + screen->historybuf->count);
    unsigned cell_height = screen->cell_size.height;
    double min_thumb_height_fraction = ((double)OPT(scrollbar_min_handle_height) * cell_height) / scrollbar_height;
    double thumb_height = MAX(min_thumb_height_fraction, visible_fraction);
    double available_space = 1.0 - thumb_height;

    if (available_space > 0) {
        double scroll_fraction = delta_y / available_space;
        double target = w->scrollbar.drag_start_scrolled_by - scroll_fraction * screen->historybuf->count;
        double new_scrolled_by;
        if (target < 0) new_scrolled_by = 0;
        else if (target > screen->historybuf->count) new_scrolled_by = screen->historybuf->count;
        else new_scrolled_by = target;
        screen_history_scroll_to_absolute(screen, new_scrolled_by);
    }
}

static const MouseShape scrollbar_drag_mouse_cursor = NS_RESIZE_POINTER;

static bool
handle_scrollbar_mouse(Window *w, int button, MouseAction action, int modifiers UNUSED) {
    if (!w || !OPT(scrollbar_interactive) || !global_state.callback_os_window) return false;

    double mouse_x = global_state.callback_os_window->mouse_x;
    double mouse_y = global_state.callback_os_window->mouse_y;

    if (action == MOVE && w->scrollbar.is_dragging) {
        handle_scrollbar_drag(w, mouse_y);
        mouse_cursor_shape = scrollbar_drag_mouse_cursor;
        set_mouse_cursor(mouse_cursor_shape);
        return true;
    }
    if (global_state.active_drag_in_window == w->id || global_state.tracked_drag_in_window == w->id) return false;

    ScrollbarHitType hit_type = get_scrollbar_hit_type(w, mouse_x, mouse_y);
    bool hovering = (hit_type != SCROLLBAR_HIT_NONE);
    update_scrollbar_hover_state(w, hovering);

    if (!hovering) return false;

    mouse_cursor_shape = DEFAULT_POINTER;
    set_mouse_cursor(mouse_cursor_shape);

    if (button == GLFW_MOUSE_BUTTON_LEFT && action != MOVE) {
        bool is_release = (action == RELEASE);

        if (is_release) {
            if (w->scrollbar.is_dragging) {
                end_drag(w);
            } else if (hit_type == SCROLLBAR_HIT_TRACK) {
                handle_scrollbar_track_click(w, mouse_y);
            }
        } else {
            if (hit_type == SCROLLBAR_HIT_THUMB) {
                start_scrollbar_drag(w, mouse_y);
                global_state.active_drag_in_window = w->id;
                global_state.active_drag_button = button;
            } else if (hit_type == SCROLLBAR_HIT_TRACK && OPT(scrollbar_jump_on_click)) {
                handle_scrollbar_track_click(w, mouse_y);
                start_scrollbar_drag(w, mouse_y);
                global_state.active_drag_in_window = w->id;
                global_state.active_drag_button = button;
            }
        }
    }

    return true;
}
// }}}

static double
distance(double x1, double y1, double x2, double y2) {
    return sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
}

static void
clear_click_queue(Window *w, int button) {
    if (0 <= button && button < (ssize_t)arraysz(w->click_queues)) w->click_queues[button].length = 0;
}

// NB: This can call into Python, which can re-alloc the windows array, so w
// must not be used after it returns. Keep it last in its callers.
static void
handle_potential_drag(Window *w, int button) {
    if (button != GLFW_MOUSE_BUTTON_LEFT || !w->drag_source.initial_left_press.at || OPT(drag_threshold) <= 0) return;
    if (distance(w->mouse_pos.global_x, w->mouse_pos.global_y, w->drag_source.initial_left_press.x, w->drag_source.initial_left_press.y) <= OPT(drag_threshold))
        return;
    zero_at_ptr(&w->drag_source.initial_left_press);
    Screen *screen = w->render_data.screen;
    // A native drag consumes the release event, including when canceled. Stop
    // tracking the local gesture before handing it to the window system,
    // otherwise the press lingers and the next one counts as a double click.
    clear_click_queue(w, button);
    if (w->drag_source.potential_drag.type == POTENTIAL_DRAG_SELECTION) {
        zero_at_ptr(&w->drag_source.potential_drag);
        global_state.active_drag_in_window = 0;
        global_state.active_drag_button = -1;
        set_mouse_cursor_for_screen(screen);
        if (screen->callbacks != Py_None) {
            PyObject *ret = PyObject_CallMethod(screen->callbacks, "drag_selection", NULL);
            if (ret) Py_DECREF(ret);
            else PyErr_Print();
        }
    } else if (w->drag_source.can_offer) {
        drag_offer_start_to_child(w, w->mouse_pos.cell_x, w->mouse_pos.cell_y, (int)w->mouse_pos.global_x, (int)w->mouse_pos.global_y);
        debug("Sent drag start event to child\n");
    } else if (w->drag_source.potential_drag.type == POTENTIAL_DRAG_URL) {
        w->drag_source.potential_drag.type = POTENTIAL_DRAG_NONE;
        screen_detect_url(screen, w->drag_source.potential_drag.x, w->drag_source.potential_drag.y);
        if (screen->current_hyperlink_under_mouse.id || screen->current_hyperlink_under_mouse.has_detected_url) {
            screen_open_url(screen, "drag_url");
            debug("Started URL drag\n");
        }
    }
}

HANDLER(handle_move_event) {
    modifiers &= ~GLFW_LOCK_MASK;

    if (handle_scrollbar_mouse(w, -1, MOVE, modifiers)) return;

    bool mouse_cell_changed = false;
    bool cell_half_changed = false;
    if (!set_mouse_position(w, &mouse_cell_changed, &cell_half_changed)) {
        if (w->scrollbar.is_hovering) { update_scrollbar_hover_state(w, false); }
        return;
    }
    Screen *screen = w->render_data.screen;
    if (OPT(detect_urls)) detect_url(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y);
    if (should_handle_in_kitty(w, screen, button)) {
        handle_mouse_movement_in_kitty(w, button, mouse_cell_changed | cell_half_changed);
    } else {
        if (mouse_cell_changed || screen->modes.mouse_tracking_protocol == SGR_PIXEL_PROTOCOL) {
            int sz = encode_mouse_button(w, button, button >= 0 ? DRAG : MOVE, modifiers);
            if (sz > 0) {
                mouse_event_buf[sz] = 0;
                write_escape_code_to_child(screen, ESC_CSI, mouse_event_buf);
            }
        }
    }
    handle_potential_drag(w, button);
}

#define N(n) (q->clicks[q->length - n])

static double
radius_for_multiclick(void) {
    return 0.5 * (global_state.callback_os_window ? global_state.callback_os_window->fonts_data->fcm.cell_height : 8);
}

static bool
release_is_click(const Window *w, int button) {
    const ClickQueue *q = &w->click_queues[button];
    monotonic_t now = monotonic();
    return (
        q->length > 0 && distance(N(1).x, N(1).y, MAX(0, w->mouse_pos.global_x), MAX(0, w->mouse_pos.global_y)) <= radius_for_multiclick() &&
        now - N(1).at < OPT(click_interval));
}

static unsigned
multi_click_count(const Window *w, int button) {
    const ClickQueue *q = &w->click_queues[button];
    double multi_click_allowed_radius = radius_for_multiclick();
    if (q->length > 2) {
        // possible triple-click
        if (N(1).at - N(3).at <= 2 * OPT(click_interval) && distance(N(1).x, N(1).y, N(3).x, N(3).y) <= multi_click_allowed_radius) return 3;
    }
    if (q->length > 1) {
        // possible double-click
        if (N(1).at - N(2).at <= OPT(click_interval) && distance(N(1).x, N(1).y, N(2).x, N(2).y) <= multi_click_allowed_radius) return 2;
    }
    return q->length ? 1 : 0;
}


static void
add_press(Window *w, int button, int modifiers) {
    if (button < 0 || button >= (ssize_t)arraysz(w->click_queues)) return;
    modifiers &= ~GLFW_LOCK_MASK;
    ClickQueue *q = &w->click_queues[button];
    if (q->length == CLICK_QUEUE_SZ) {
        memmove(q->clicks, q->clicks + 1, sizeof(Click) * (CLICK_QUEUE_SZ - 1));
        q->length--;
    }
    monotonic_t now = monotonic();
    static unsigned long num = 0;
    N(0).at = now;
    N(0).button = button;
    N(0).modifiers = modifiers;
    N(0).x = MAX(0, w->mouse_pos.global_x);
    N(0).y = MAX(0, w->mouse_pos.global_y);
    N(0).num = ++num;
    q->length++;
    Screen *screen = w->render_data.screen;
    int count = multi_click_count(w, button);
    if (count > 1) {
        if (screen) dispatch_mouse_event(w, button, count, modifiers, screen->modes.mouse_tracking_mode != 0);
        if (count > 2) q->length = 0;
    }
}
#undef N

bool
mouse_open_url(Window *w) {
    Screen *screen = w->render_data.screen;
    detect_url(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y);
    return screen_open_url(screen, "open_url");
}

bool
mouse_set_last_visited_cmd_output(Window *w) {
    Screen *screen = w->render_data.screen;
    return screen_set_last_visited_prompt(screen, w->mouse_pos.cell_y);
}

bool
mouse_select_cmd_output(Window *w) {
    Screen *screen = w->render_data.screen;
    return screen_select_cmd_output(screen, w->mouse_pos.cell_y);
}

bool
move_cursor_to_mouse_if_at_shell_prompt(Window *w) {
    Screen *screen = w->render_data.screen;
    int y = screen_cursor_at_a_shell_prompt(screen);
    if (y < 0 || (unsigned)y > w->mouse_pos.cell_y) return false;

    bool is_relative;
    if (screen_prompt_supports_click_events(screen, &is_relative)) {
        MousePosition mpos = w->mouse_pos;
        if (is_relative) mpos.cell_y -= y;
        int sz = encode_mouse_event_impl(&mpos, SGR_PROTOCOL, 1, PRESS, 0);
        if (sz > 0) {
            mouse_event_buf[sz] = 0;
            write_escape_code_to_child(screen, ESC_CSI, mouse_event_buf);
            return true;
        }
        return false;
    } else {
        return screen_fake_move_cursor_to_position(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y);
    }
}


void
send_pending_click_to_window(Window *w, int i) {
    const id_type wid = w->id;
    if (i < 0) {
        while (true) {
            w = window_for_id(wid);
            if (!w || !w->pending_clicks.num) break;
            send_pending_click_to_window(w, w->pending_clicks.num - 1);
        }
        return;
    }
    PendingClick pc = w->pending_clicks.clicks[i];
    remove_i_from_array(w->pending_clicks.clicks, (unsigned)i, w->pending_clicks.num);
    const ClickQueue *q = &w->click_queues[pc.button];
    // only send click if no presses have happened since the release that triggered
    // the click or if the subsequent press is too far or too late for a double click
    if (!q->length) return;
#define press(n) q->clicks[q->length - n]
    if (press(1).at <= pc.at || // latest press is before click release
        (q->length > 1 && press(2).num == pc.press_num &&
         (                                                                                       // penultimate press is the press that belongs to this click
             press(1).at - press(2).at > OPT(click_interval) ||                                  // too long between the presses for it to be a double click
             distance(press(1).x, press(1).y, press(2).x, press(2).y) > pc.radius_for_multiclick // presses are too far apart
             ))) {
        MousePosition current_pos = w->mouse_pos;
        w->mouse_pos = pc.mouse_pos;
        dispatch_mouse_event(w, pc.button, pc.count, pc.modifiers, pc.grabbed);
        w = window_for_id(wid);
        if (w) w->mouse_pos = current_pos;
    }
#undef press
}

static void
dispatch_possible_click(Window *w, int button, int modifiers) {
    Screen *screen = w->render_data.screen;
    int count = multi_click_count(w, button);
    if (release_is_click(w, button)) {
        ensure_space_for(&(w->pending_clicks), clicks, PendingClick, w->pending_clicks.num + 1, capacity, 4, true);
        PendingClick *pc = w->pending_clicks.clicks + w->pending_clicks.num++;
        zero_at_ptr(pc);
        const ClickQueue *q = &w->click_queues[button];
        pc->press_num = q->length ? q->clicks[q->length - 1].num : 0;
        pc->window_id = w->id;
        pc->mouse_pos = w->mouse_pos;
        pc->at = monotonic();
        pc->button = button;
        pc->count = count == 2 ? -3 : -2;
        pc->modifiers = modifiers;
        pc->grabbed = screen->modes.mouse_tracking_mode != 0;
        pc->radius_for_multiclick = radius_for_multiclick();
        add_main_loop_timer(OPT(click_interval), false, dispatch_pending_clicks, NULL, NULL);
    }
}

// Must be called for every button event before any early return, otherwise a
// pending drag lingers and fires on a later mouse move with no button held.
static void
clear_potential_drag(Window *w, int button, bool is_release) {
    zero_at_ptr(&w->drag_source.potential_drag);
    if (is_release && button == GLFW_MOUSE_BUTTON_LEFT) zero_at_ptr(&w->drag_source.initial_left_press);
}

// Needs an up-to-date w->mouse_pos, as the drag threshold is measured from it.
static void
arm_potential_drag(Window *w, int button) {
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    w->drag_source.initial_left_press.x = w->mouse_pos.global_x;
    w->drag_source.initial_left_press.y = w->mouse_pos.global_y;
    w->drag_source.initial_left_press.at = monotonic();
}

HANDLER(handle_button_event) {
    modifiers &= ~GLFW_LOCK_MASK;
    OSWindow *osw = global_state.callback_os_window;
    if (!osw) return;

    Tab *t = osw->tabs + osw->active_tab;
    bool is_release = !osw->mouse_button_pressed[button];
    clear_potential_drag(w, button, is_release);

    if (handle_scrollbar_mouse(w, button, is_release ? RELEASE : PRESS, modifiers)) return;

    if (osw->is_focused && window_idx != t->active_window && !is_release) { call_boss(switch_focus_to_in_active_tab, "K", t->windows[window_idx].id); }

    Screen *screen = w->render_data.screen;
    if (!screen) return;

    bool a, b;
    if (!set_mouse_position(w, &a, &b)) return;
    if (!is_release) arm_potential_drag(w, button);
    if (button == GLFW_MOUSE_BUTTON_LEFT && !is_release) osw->shader_anim_event_registry |= (1u << SHADER_ANIM_EVENT_POINTER_LEFT_BUTTON_PRESS);
    id_type wid = w->id;
    if (!dispatch_mouse_event(w, button, is_release ? -1 : 1, modifiers, screen->modes.mouse_tracking_mode != 0)) {
        if (screen->modes.mouse_tracking_mode != 0) {
            int sz = encode_mouse_button(w, button, is_release ? RELEASE : PRESS, modifiers);
            if (sz > 0) {
                mouse_event_buf[sz] = 0;
                write_escape_code_to_child(screen, ESC_CSI, mouse_event_buf);
            }
        }
    }
    // the windows array might have been re-alloced in dispatch_mouse_event
    w = NULL;
    for (size_t i = 0; i < t->num_windows && !w; i++)
        if (t->windows[i].id == wid) w = t->windows + i;
    if (w) {
        if (is_release) dispatch_possible_click(w, button, modifiers);
        else add_press(w, button, modifiers);
    }
}

static int
currently_pressed_button(void) {
    for (int i = 0; i <= GLFW_MOUSE_BUTTON_LAST; i++) {
        if (global_state.callback_os_window->mouse_button_pressed[i]) return i;
    }
    return -1;
}

HANDLER(handle_event) {
    modifiers &= ~GLFW_LOCK_MASK;
    set_mouse_cursor_for_screen(w->render_data.screen);
    set_currently_hovered_window(w->id, modifiers, true);
    if (button == -1) {
        button = currently_pressed_button();
        handle_move_event(w, button, modifiers, window_idx);
    } else {
        handle_button_event(w, button, modifiers, window_idx);
    }
}

static void
handle_window_title_bar_mouse(Window *w, int button, int modifiers, int action) {
    OSWindow *osw = global_state.callback_os_window;
    if (!osw) return;
    if (button > -1 || global_state.window_being_dragged.id) {
        call_boss(handle_window_title_bar_mouse, "KKddiii", osw->id, w->id, osw->mouse_x, osw->mouse_y, button, modifiers, action);
    }
}

static void
handle_tab_bar_mouse(int button, int modifiers, int action) {
    set_currently_hovered_window(0, modifiers, false);
    OSWindow *w = global_state.callback_os_window;
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE && global_state.tab_being_dragged.id && global_state.tab_being_dragged.drag_started &&
        !global_state.drag_source.is_active) {
        // Once a system drag and drop is active the release is consumed by it
        // and never delivered to us, so getting one here means the drag never
        // became a system DND: either glfwStartDrag failed/was not called yet
        // or the compositor silently ignored it (Wayland with a stale serial).
        // Clear the drag state so mouse handling is not redirected to the tab
        // bar forever, and swallow the release as it ended an aborted drag.
        zero_at_ptr(&global_state.tab_being_dragged);
        // re-render the tab bar in case it was drawn without the dragged tab
        if (w) w->tab_bar_data_updated = false;
        return;
    }
    // dont report motion events, as they are expensive and useless
    if (w && (button > -1 || global_state.tab_being_dragged.id)) {
        call_boss(handle_tab_bar_mouse, "Kddiii", w->id, w->mouse_x, w->mouse_y, button, modifiers, action);
    }
}

static bool
mouse_in_region(Region *r) {
    if (r->left == r->right) return false;
    OSWindow *w = global_state.callback_os_window;
    if (!w) return false;
    if (w->mouse_y < r->top || w->mouse_y >= r->bottom) return false;
    if (w->mouse_x < r->left || w->mouse_x >= r->right) return false;
    return true;
}

static unsigned
num_visible_windows(Tab *t) {
    unsigned ans = t->num_windows;
    for (unsigned i = 0; i < t->num_windows; i++)
        if (!t->windows[i].visible) ans--;
    return ans;
}

typedef struct MouseRegion {
    unsigned window_idx;
    bool in_tab_bar;
    bool in_title_bar;
    Edge window_border;
    Window *window;
} MouseRegion;

static MouseRegion
mouse_region(bool detect_borders, bool detect_title_bar) {
    MouseRegion ans = {0};
    Region central, tab_bar;
    const OSWindow *w = global_state.callback_os_window;
    os_window_regions(w, &central, &tab_bar);
    const bool in_central = mouse_in_region(&central);
    if (!in_central) {
        if ((tab_bar.left < central.left && w->mouse_x < central.left) || (tab_bar.right > central.right && w->mouse_x >= central.right) ||
            (tab_bar.top < central.top && w->mouse_y < central.top) || (tab_bar.bottom > central.bottom && w->mouse_y >= central.bottom))
            ans.in_tab_bar = true;
    }
    if (in_central && w->num_tabs > 0) {
        Tab *t = global_state.callback_os_window->tabs + global_state.callback_os_window->active_tab;
        if (detect_borders && num_visible_windows(t) > 1) {
            id_type window_id = 0;
            double dpi = (w->fonts_data->logical_dpi_x + w->fonts_data->logical_dpi_y) / 2.;
            double tolerance = ((long)round((OPT(window_drag_tolerance) * (dpi / 72.0))));
            BorderRect *closest_vert = NULL, *closest_horiz = NULL;
            double closest_vert_dist = (double)UINT_MAX, closest_horiz_dist = (double)UINT_MAX;
            bool is_within_border_without_tolerance = false;
            for (unsigned i = 0; i < t->border_rects.num_border_rects; i++) {
                BorderRect *br = t->border_rects.rect_buf + i;
                if (!br->border_type) continue;
                Edge edges = 0;
                if (border_contains_mouse(br, 0, &edges)) {
                    ans.window_border |= edges;
                    is_within_border_without_tolerance = true;
                    if (edges & (LEFT_EDGE | RIGHT_EDGE)) {
                        closest_vert_dist = -1;
                        closest_vert = NULL;
                    } else {
                        closest_horiz_dist = -1;
                        closest_horiz = NULL;
                    }
                    window_id = br->border_type < 0 ? -br->border_type : br->border_type;
                } else if (border_contains_mouse(br, tolerance, &edges)) {
                    unsigned width = br->px.right - br->px.left, height = br->px.bottom - br->px.top;
                    if (!br->horizontal) {
                        double d = br->px.left + width / 2. - w->mouse_x;
                        d = d * d;
                        if (d < closest_vert_dist) {
                            closest_vert_dist = d;
                            closest_vert = br;
                        }
                    } else {
                        double d = br->px.top + height / 2. - w->mouse_y;
                        d = d * d;
                        if (d < closest_horiz_dist) {
                            closest_horiz_dist = d;
                            closest_horiz = br;
                        }
                    }
                }
            }
            if (!is_within_border_without_tolerance) {
                if (closest_vert && border_contains_mouse(closest_vert, tolerance, &ans.window_border) && !window_id)
                    window_id = closest_vert->border_type < 0 ? -closest_vert->border_type : closest_vert->border_type;
                if (closest_horiz && border_contains_mouse(closest_horiz, tolerance, &ans.window_border) && !window_id)
                    window_id = closest_horiz->border_type < 0 ? -closest_horiz->border_type : closest_horiz->border_type;
            }
            if (ans.window_border) {
                // If a hovered scrollbar overlaps this border region, the
                // scrollbar takes precedence to avoid flickering when the
                // expanded hover-width scrollbar straddles the border.
                bool scrollbar_takes_precedence = false;
                if (OPT(scrollbar_interactive)) {
                    for (unsigned int i = 0; i < t->num_windows; i++) {
                        Window *win = t->windows + i;
                        if (!win->visible || !win->render_data.screen) continue;
                        if (win->scrollbar.is_hovering && get_scrollbar_hit_type(win, w->mouse_x, w->mouse_y) != SCROLLBAR_HIT_NONE) {
                            scrollbar_takes_precedence = true;
                            break;
                        }
                    }
                }
                if (!scrollbar_takes_precedence) {
                    if (window_id) {
                        for (unsigned int i = 0; i < t->num_windows; i++)
                            if (t->windows[i].id == window_id) {
                                ans.window = t->windows + i;
                                ans.window_idx = i;
                                break;
                            }
                    }
                    return ans;
                }
                ans.window_border = 0;
            }
        }
        for (unsigned int i = 0; i < t->num_windows; i++) {
            Window *win = t->windows + i;
            if (contains_mouse(win) && win->render_data.screen) {
                ans.window_idx = i;
                ans.window = win;
                break;
            } else if (detect_title_bar && win->visible) {
                const WindowRenderData *trd = &win->window_title_render_data;
                if (trd->screen && trd->geometry.right > trd->geometry.left && trd->geometry.bottom > trd->geometry.top) {
                    if (w->mouse_x >= trd->geometry.left && w->mouse_x < trd->geometry.right && w->mouse_y >= trd->geometry.top &&
                        w->mouse_y < trd->geometry.bottom) {
                        ans.in_title_bar = true;
                        ans.window = win;
                        ans.window_idx = i;
                        break;
                    }
                }
            }
        }
        // If no window was found via contains_mouse, check if the mouse is in any
        // window's scrollbar hit area. The scrollbar may be drawn in the margin
        // which is outside the area covered by contains_mouse.
        if (!ans.window && OPT(scrollbar_interactive)) {
            for (unsigned int i = 0; i < t->num_windows; i++) {
                Window *win = t->windows + i;
                if (!win->visible || !win->render_data.screen) continue;
                if (get_scrollbar_hit_type(win, w->mouse_x, w->mouse_y) != SCROLLBAR_HIT_NONE) {
                    ans.window_idx = i;
                    ans.window = win;
                    break;
                }
            }
        }
    }
    return ans;
}

void
focus_in_event(void) {
    // Ensure that no URL is highlighted and the mouse cursor is in default shape
    mouse_cursor_shape = TEXT_POINTER;
    MouseRegion r = mouse_region(false, false);
    if (r.window && r.window->render_data.screen) {
        screen_mark_url(r.window->render_data.screen, 0, 0, 0, 0);
        set_mouse_cursor_for_screen(r.window->render_data.screen);
    }
    set_mouse_cursor(mouse_cursor_shape);
}

void
update_mouse_pointer_shape(void) {
    mouse_cursor_shape = TEXT_POINTER;
    MouseRegion r = mouse_region(false, true);
    if (r.in_tab_bar) {
        mouse_cursor_shape = POINTER_POINTER;
    } else if (r.in_title_bar) {
        mouse_cursor_shape = POINTER_POINTER;
    } else if (r.window) {
        if (handle_scrollbar_mouse(r.window, -1, MOVE, 0)) {
            mouse_cursor_shape = scrollbar_drag_mouse_cursor;
        } else if (r.window->render_data.screen) {
            screen_mark_url(r.window->render_data.screen, 0, 0, 0, 0);
            set_mouse_cursor_for_screen(r.window->render_data.screen);
        }
    }
    set_mouse_cursor(mouse_cursor_shape);
}

void
leave_event(int modifiers) {
    if (global_state.redirect_mouse_handling || global_state.active_drag_in_window || global_state.tracked_drag_in_window) return;
    set_currently_hovered_window(0, modifiers, false);
}

void
enter_event(int modifiers, bool cursor_moved) {
#ifdef __APPLE__
    // On cocoa there is no way to configure the window manager to
    // focus windows on mouse enter, so we do it ourselves
    if (OPT(focus_follows_mouse).on_cross && !global_state.callback_os_window->is_focused) {
        id_type wid = global_state.callback_os_window->id;
        focus_os_window(global_state.callback_os_window, false, NULL);
        if (!global_state.callback_os_window) {
            global_state.callback_os_window = os_window_for_id(wid);
            if (!global_state.callback_os_window) return;
        }
    }
#endif
    // If the mouse is grabbed send a move event to update the cursor position
    // since the last report.
    if (global_state.redirect_mouse_handling || global_state.active_drag_in_window || global_state.tracked_drag_in_window) return;
    MouseRegion r = mouse_region(false, false);
    Window *w = r.window;
    set_currently_hovered_window(w ? w->id : 0, modifiers, cursor_moved);
    if (!w || r.in_tab_bar || r.in_title_bar) return;

    if (handle_scrollbar_mouse(w, -1, MOVE, modifiers)) return;

    bool mouse_cell_changed = false, cell_half_changed = false;
    if (!set_mouse_position(w, &mouse_cell_changed, &cell_half_changed)) return;
    Screen *screen = w->render_data.screen;
    int button = currently_pressed_button();
    if (!screen || should_handle_in_kitty(w, screen, button)) return;
    int sz = encode_mouse_button(w, button, button >= 0 ? DRAG : MOVE, modifiers);
    if (sz > 0) {
        mouse_event_buf[sz] = 0;
        write_escape_code_to_child(screen, ESC_CSI, mouse_event_buf);
    }
}


typedef enum MouseSelectionType {
    MOUSE_SELECTION_DRAG_OR_NORMAL_SELECT = -1,
    MOUSE_SELECTION_NORMAL,
    MOUSE_SELECTION_EXTEND,
    MOUSE_SELECTION_RECTANGLE,
    MOUSE_SELECTION_WORD,
    MOUSE_SELECTION_LINE,
    MOUSE_SELECTION_LINE_FROM_BEGIN,
    MOUSE_SELECTION_LINE_FROM_POINT,
    MOUSE_SELECTION_WORD_AND_LINE_FROM_POINT,
    MOUSE_SELECTION_MOVE_END,
    MOUSE_SELECTION_UPTO_SURROUNDING_WHITESPACE,
} MouseSelectionType;


void
mouse_selection(Window *w, int code, int button) {
    zero_at_ptr(&w->drag_source.potential_drag);
    global_state.active_drag_in_window = w->id;
    global_state.active_drag_button = button;
    Screen *screen = w->render_data.screen;
    index_type start, end;
    unsigned int y1, y2;
#define S(mode)                                                                                                                                        \
    {                                                                                                                                                  \
        screen_start_selection(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y, w->mouse_pos.in_left_half_of_cell, false, mode);                      \
        screen_update_selection(                                                                                                                       \
            screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y, w->mouse_pos.in_left_half_of_cell, (SelectionUpdate){.start_extended_selection = true}); \
    }

    switch ((MouseSelectionType)code) {
        case MOUSE_SELECTION_DRAG_OR_NORMAL_SELECT:
            screen_pause_rendering(screen, false, 0);
            if (button == GLFW_MOUSE_BUTTON_LEFT && OPT(drag_threshold) > 0) {
                if (!screen->selections.in_progress && screen_is_cell_selected(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y)) {
                    // Like iTerm2's mouseDownOnSelection: defer changing the
                    // selection until we can distinguish a click from a drag.
                    w->drag_source.potential_drag.type = POTENTIAL_DRAG_SELECTION;
                    return;
                }
                if (screen->current_hyperlink_under_mouse.id || screen->current_hyperlink_under_mouse.has_detected_url) {
                    w->drag_source.potential_drag.type = POTENTIAL_DRAG_URL;
                    w->drag_source.potential_drag.x = w->mouse_pos.cell_x;
                    w->drag_source.potential_drag.y = w->mouse_pos.cell_y;
                    global_state.active_drag_in_window = 0;
                    global_state.active_drag_button = -1;
                    return;
                }
            }
            /* fallthrough */
        case MOUSE_SELECTION_NORMAL:
            screen_start_selection(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y, w->mouse_pos.in_left_half_of_cell, false, EXTEND_CELL);
            break;
        case MOUSE_SELECTION_RECTANGLE:
            screen_start_selection(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y, w->mouse_pos.in_left_half_of_cell, true, EXTEND_CELL);
            break;
        case MOUSE_SELECTION_WORD:
            if (screen_selection_range_for_word(screen, w->mouse_pos.cell_x, w->mouse_pos.cell_y, &y1, &y2, &start, &end, true)) S(EXTEND_WORD);
            break;
        case MOUSE_SELECTION_LINE:
            if (screen_selection_range_for_line(screen, w->mouse_pos.cell_y, &start, &end)) S(EXTEND_LINE);
            break;
        case MOUSE_SELECTION_LINE_FROM_BEGIN:
            if (screen_selection_range_for_line(screen, w->mouse_pos.cell_y, &start, &end)) S(EXTEND_LINE_FROM_BEGIN);
            break;
        case MOUSE_SELECTION_LINE_FROM_POINT:
            if (screen_selection_range_for_line(screen, w->mouse_pos.cell_y, &start, &end) && end > w->mouse_pos.cell_x) S(EXTEND_LINE_FROM_POINT);
            break;
        case MOUSE_SELECTION_WORD_AND_LINE_FROM_POINT:
            if (screen_selection_range_for_line(screen, w->mouse_pos.cell_y, &start, &end) && end > w->mouse_pos.cell_x) S(EXTEND_WORD_AND_LINE_FROM_POINT);
            break;
        case MOUSE_SELECTION_EXTEND: extend_selection(w, false, true); break;
        case MOUSE_SELECTION_MOVE_END: extend_selection(w, false, false); break;
        case MOUSE_SELECTION_UPTO_SURROUNDING_WHITESPACE:
            // TODO: Implement me for people migrating from urxvt
            break;
    }
    set_mouse_cursor_when_dragging(screen);
#undef S
}

static const char *
border_name(int edges) {
    switch (edges) {
        case 0: return "none";
        case LEFT_EDGE: return "left";
        case RIGHT_EDGE: return "right";
        case TOP_EDGE: return "top";
        case BOTTOM_EDGE: return "bottom";
        case LEFT_EDGE | TOP_EDGE: return "top-left";
        case LEFT_EDGE | BOTTOM_EDGE: return "bottom-left";
        case RIGHT_EDGE | TOP_EDGE: return "top-right";
        case RIGHT_EDGE | BOTTOM_EDGE: return "bottom-right";
    }
    return "unknown";
}

void
mouse_event(const int button, int modifiers, int action) {
    MouseShape old_cursor = mouse_cursor_shape;
    unsigned int window_idx = 0;
    Window *w = NULL;
    OSWindow *osw = global_state.callback_os_window;

    if (OPT(debug_keyboard)) {
        if (button < 0) {
            debug("%s x: %.1f y: %.1f ", "\x1b[36mMove\x1b[m", osw->mouse_x, osw->mouse_y);
        } else {
            debug("%s mouse_button: %d %s", action == GLFW_RELEASE ? "\x1b[32mRelease\x1b[m" : "\x1b[31mPress\x1b[m", button, format_mods(modifiers));
        }
    }
    if (global_state.redirect_mouse_handling) {
        MouseRegion r = mouse_region(false, false);
        w = r.window;
        call_boss(
            mouse_event,
            "OK iiii dd",
            (r.in_tab_bar ? Py_True : Py_False),
            (w ? w->id : 0),
            action,
            modifiers,
            button,
            currently_pressed_button(),
            osw->mouse_x,
            osw->mouse_y);
        debug("mouse handling redirected\n");
        return;
    }
    if (global_state.active_drag_in_window) {
        if (button == -1) { // drag move
            w = window_for_id(global_state.active_drag_in_window);
            if (w) {
                if (currently_pressed_button() == global_state.active_drag_button) {
                    clamp_to_window = true;
                    Tab *t = osw->tabs + osw->active_tab;
                    for (window_idx = 0; window_idx < t->num_windows && t->windows[window_idx].id != w->id; window_idx++);
                    handle_move_event(w, currently_pressed_button(), modifiers, window_idx);
                    clamp_to_window = false;
                    debug("handled as drag move\n");
                    return;
                }
            }
        } else if (action == GLFW_RELEASE && button == global_state.active_drag_button) {
            w = window_for_id(global_state.active_drag_in_window);
            if (w) {
                end_drag(w);
                debug("handled as drag end\n");
                dispatch_possible_click(w, button, modifiers);
                return;
            }
        }
    }
    if (global_state.tracked_drag_in_window) {
        if (button == -1) { // drag move
            w = window_for_id(global_state.tracked_drag_in_window);
            if (w) {
                if (currently_pressed_button() == GLFW_MOUSE_BUTTON_LEFT) {
                    if (w->render_data.screen->modes.mouse_tracking_mode >= MOTION_MODE &&
                        w->render_data.screen->modes.mouse_tracking_protocol == SGR_PIXEL_PROTOCOL) {
                        clamp_to_window = true;
                        Tab *t = osw->tabs + osw->active_tab;
                        for (window_idx = 0; window_idx < t->num_windows && t->windows[window_idx].id != w->id; window_idx++);
                        handle_move_event(w, global_state.tracked_drag_button, modifiers, window_idx);
                        clamp_to_window = false;
                        debug("sent to child as drag move\n");
                        return;
                    }
                }
            }
        } else if (action == GLFW_RELEASE && button == GLFW_MOUSE_BUTTON_LEFT) {
            w = window_for_id(global_state.tracked_drag_in_window);
            if (w) {
                zero_at_ptr(&w->drag_source.initial_left_press);
                if (w->render_data.screen->modes.mouse_tracking_mode >= BUTTON_MODE && w->render_data.screen->modes.mouse_tracking_protocol >= SGR_PROTOCOL) {
                    global_state.tracked_drag_in_window = 0;
                    clamp_to_window = true;
                    Tab *t = osw->tabs + osw->active_tab;
                    for (window_idx = 0; window_idx < t->num_windows && t->windows[window_idx].id != w->id; window_idx++);
                    debug("sent to child as drag end\n");
                    handle_button_event(w, button, modifiers, window_idx);
                    clamp_to_window = false;
                    return;
                }
            }
        }
    }
    if (global_state.active_drag_resize) {
        if (button < 0) {
            call_boss(drag_resize_update, "dd", osw->mouse_x, osw->mouse_y);
            debug("drag resize updated\n");
        } else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
            call_boss(drag_resize_end, "");
            global_state.active_drag_resize = 0;
            mouse_cursor_shape = DEFAULT_POINTER;
            set_mouse_cursor(mouse_cursor_shape);
            debug("drag resize ended\n");
        }
        return;
    }
    MouseRegion r = mouse_region(true, true);
    w = r.window;
    window_idx = r.window_idx;
    set_currently_hovered_window(w && !r.window_border && !r.in_title_bar ? w->id : 0, modifiers, true);

    if (r.in_tab_bar || global_state.tab_being_dragged.id) {
        mouse_cursor_shape = POINTER_POINTER;
        handle_tab_bar_mouse(button, modifiers, action);
        debug("handled by tab bar\n");
    } else if ((r.in_title_bar && r.window) || global_state.window_being_dragged.id) {
        mouse_cursor_shape = POINTER_POINTER;
        Window *tw = r.window;
        if (!tw && global_state.window_being_dragged.id) { tw = window_for_window_id(global_state.window_being_dragged.id); }
        if (tw) handle_window_title_bar_mouse(tw, button, modifiers, action);
        debug("handled by window title bar\n");
    } else if (r.window_border) {
        debug("window border: %s window id: %llu\n", border_name(r.window_border), w ? w->id : 0);
        if (r.window_border & LEFT_EDGE) {
            if (r.window_border & TOP_EDGE) mouse_cursor_shape = NWSE_RESIZE_POINTER;
            else if (r.window_border & BOTTOM_EDGE) mouse_cursor_shape = NESW_RESIZE_POINTER;
            else mouse_cursor_shape = EW_RESIZE_POINTER;
        } else if (r.window_border & RIGHT_EDGE) {
            if (r.window_border & TOP_EDGE) mouse_cursor_shape = NESW_RESIZE_POINTER;
            else if (r.window_border & BOTTOM_EDGE) mouse_cursor_shape = NWSE_RESIZE_POINTER;
            else mouse_cursor_shape = EW_RESIZE_POINTER;
        } else if (r.window_border & (TOP_EDGE | BOTTOM_EDGE)) mouse_cursor_shape = NS_RESIZE_POINTER;
        if (w && button == GLFW_MOUSE_BUTTON_LEFT && w->render_data.screen) {
            RAII_PyObject(
                retval,
                PyObject_CallMethod(
                    global_state.boss,
                    "drag_resize_start",
                    "iddKII",
                    r.window_border,
                    osw->mouse_x,
                    osw->mouse_y,
                    w->id,
                    w->render_data.screen->cell_size.width,
                    w->render_data.screen->cell_size.height));
            if (retval == NULL) {
                PyErr_Print();
                return;
            }
            if (PyObject_IsTrue(retval)) global_state.active_drag_resize = w->id;
        }
    } else if (w) {
        debug("grabbed: %d\n", w->render_data.screen->modes.mouse_tracking_mode != 0);
        // Button presses/releases in the padding around the cell area of the
        // window are mapped to the nearest cell, so that, for example,
        // selections can be started by pressing in the padding. Note that this
        // is deliberately not done for pure movement events so that merely
        // hovering over the padding does not cause URL detection and the like.
        // Also note that it does not apply to windows matched only because the
        // mouse is over their scrollbar, which can be drawn in the margin,
        // outside the window.
        clamp_to_window = button > -1 && contains_mouse(w);
        handle_event(w, button, modifiers, window_idx);
        clamp_to_window = false;
    } else {
        mouse_cursor_shape = DEFAULT_POINTER;
        debug("no window for event\n");
    }
    if (mouse_cursor_shape != old_cursor) set_mouse_cursor(mouse_cursor_shape);
}

static int
scale_scroll(
    MouseTrackingMode mouse_tracking_mode, double offset, GLFWOffsetType offset_type, double *pending_scroll_pixels, int cell_size, int *last_v120_dir) {
// scale the scroll by the multiplier unless the mouse is grabbed. If the mouse is grabbed only change direction.
#define SCALE_SCROLL(which)                            \
    {                                                  \
        double scale = OPT(which);                     \
        if (mouse_tracking_mode) scale /= fabs(scale); \
        offset *= scale;                               \
    }
    int s = 0;
    switch (offset_type) {
        case GLFW_SCROLL_OFFEST_HIGHRES: {
            SCALE_SCROLL(touch_scroll_multiplier);
            double pixels = *pending_scroll_pixels + offset;
            if (fabs(pixels) < cell_size) {
                *pending_scroll_pixels = pixels;
                return 0;
            }
            s = (int)round(pixels) / cell_size;
            *pending_scroll_pixels = pixels - s * cell_size;
        } break;
        case GLFW_SCROLL_OFFEST_V120: {
            SCALE_SCROLL(wheel_scroll_multiplier);
            // One physical detent arrives as several VALUE120 events, so the
            // fractional remainder is carried to the next event. Detents do not
            // reliably total 120 units, and that carry is what lets an
            // undersized detent still scroll a line. On a direction change the
            // residual has the wrong sign, and would be cancelled out by the
            // first detent of the new direction rather than scrolling it, so
            // discard it. Scrolling in a single direction is unaffected.
            const int dir = offset > 0 ? 1 : -1;
            if (*last_v120_dir && dir != *last_v120_dir) *pending_scroll_pixels = 0;
            *last_v120_dir = dir;
            const double offset_lines = offset / 120.;
            const double pixels = *pending_scroll_pixels + offset_lines * cell_size;
            if (fabs(pixels) < cell_size) {
                *pending_scroll_pixels = pixels;
                return 0;
            }
            s = (int)round(pixels) / cell_size;
            *pending_scroll_pixels = pixels - s * cell_size;
        } break;
        case GLFW_SCROLL_OFFSET_LINES: {
            SCALE_SCROLL(wheel_scroll_multiplier);
            s = (int)round(offset);
            if (offset != 0) {
                const int min_lines = mouse_tracking_mode ? 1 : OPT(wheel_scroll_min_lines);
                if (min_lines > 0 && abs(s) < min_lines) s = offset > 0 ? min_lines : -min_lines;
                // Always add the minimum number of lines when it is negative
                else if (min_lines < 0) s = offset > 0 ? s - min_lines : s + min_lines;
                // apparently on cocoa some mice generate really small yoffset values
                // when scrolling slowly https://github.com/kovidgoyal/kitty/issues/1238
                if (s == 0) s = offset > 0 ? 1 : -1;
            }
            *pending_scroll_pixels = 0;
        } break;
    }
    return s;
#undef SCALE_SCROLL
}

static const char *
scroll_offset_type(GLFWOffsetType t) {
    switch (t) {
        case GLFW_SCROLL_OFFSET_LINES: return "lines";
        case GLFW_SCROLL_OFFEST_V120: return "v120";
        case GLFW_SCROLL_OFFEST_HIGHRES: return "highres";
    }
    return "";
}

static const char *
scroll_phase(GLFWMomentumType t) {
    switch (t) {
        case GLFW_NO_MOMENTUM_DATA: return "none";
        case GLFW_MOMENTUM_PHASE_MAY_BEGIN: return "may_begin";
        case GLFW_MOMENTUM_PHASE_BEGAN: return "began";
        case GLFW_MOMENTUM_PHASE_ACTIVE: return "active";
        case GLFW_MOMENTUM_PHASE_STATIONARY: return "stationary";
        case GLFW_MOMENTUM_PHASE_CANCELED: return "cancelled";
        case GLFW_MOMENTUM_PHASE_ENDED: return "ended";
    }
    return "";
}


static inline bool
pixel_scroll_enabled_for_screen(const Screen *screen) {
    return OPT(pixel_scroll) && screen->linebuf == screen->main_linebuf;
}

void
scroll_event(const GLFWScrollEvent *ev) {
    debug(
        "\x1b[36mScroll\x1b[m type=%s x: %f y: %f momentum: %s modifiers: %s\n",
        scroll_offset_type(ev->offset_type),
        ev->x_offset,
        ev->y_offset,
        scroll_phase(ev->momentum_type),
        format_mods(ev->keyboard_modifiers));
    static id_type window_for_momentum_scroll = 0;
    static bool main_screen_for_momentum_scroll = false;
    // allow scroll events even if window is not currently focused (in
    // which case on some platforms such as macOS the mouse location is zeroed so
    // window_for_event() does not work).
    OSWindow *osw = global_state.callback_os_window;
    if (!osw->is_focused && osw->handle) {
        double mouse_x, mouse_y;
        glfwGetCursorPos((GLFWwindow *)osw->handle, &mouse_x, &mouse_y);
        osw->mouse_x = mouse_x * osw->viewport_x_ratio;
        osw->mouse_y = mouse_y * osw->viewport_y_ratio;
    }
    MouseRegion r = mouse_region(false, true);
    Window *w = r.window;
    if (!w && !r.in_tab_bar) {
        // fallback to last active window
        Tab *t = osw->tabs + osw->active_tab;
        if (t) w = t->windows + t->active_window;
    }
    if (!w) return;
    // Also update mouse cursor position while kitty OS window is not focused.
    // Allows scroll events to be delivered to the child with correct pointer coordinates even when
    // the window is not focused on macOS
    if (!osw->is_focused) {
        unsigned int x = 0, y = 0;
        bool in_left_half_of_cell;
        if (cell_for_pos(w, &x, &y, &in_left_half_of_cell, osw)) {
            w->mouse_pos.cell_x = x;
            w->mouse_pos.cell_y = y;
            w->mouse_pos.in_left_half_of_cell = in_left_half_of_cell;
        }
    }
    Screen *screen = w->render_data.screen;

    switch (ev->momentum_type) {
        case GLFW_NO_MOMENTUM_DATA: break;
        case GLFW_MOMENTUM_PHASE_BEGAN:
            window_for_momentum_scroll = w->id;
            main_screen_for_momentum_scroll = screen->linebuf == screen->main_linebuf;
            break;
        case GLFW_MOMENTUM_PHASE_STATIONARY:
        case GLFW_MOMENTUM_PHASE_ACTIVE:
            if (window_for_momentum_scroll != w->id || main_screen_for_momentum_scroll != (screen->linebuf == screen->main_linebuf)) return;
            break;
        case GLFW_MOMENTUM_PHASE_ENDED:
        case GLFW_MOMENTUM_PHASE_CANCELED: window_for_momentum_scroll = 0; break;
        case GLFW_MOMENTUM_PHASE_MAY_BEGIN: break;
    }
    finish_scroll_animation(screen);
    if (ev->y_offset != 0.0) {
        if (screen->modes.mouse_tracking_mode == NO_TRACKING && pixel_scroll_enabled_for_screen(screen) &&
            (ev->offset_type == GLFW_SCROLL_OFFEST_HIGHRES || ev->offset_type == GLFW_SCROLL_OFFEST_V120)) {
            double delta_pixels;
            if (ev->offset_type == GLFW_SCROLL_OFFEST_HIGHRES) {
                delta_pixels = ev->y_offset * OPT(touch_scroll_multiplier);
            } else {
                const double offset_lines = (ev->y_offset / 120.) * OPT(wheel_scroll_multiplier);
                delta_pixels = offset_lines * global_state.callback_os_window->fonts_data->fcm.cell_height;
            }
            osw->scroll.pending_pixels_y = 0.0;
            if (screen_apply_pixel_scroll(screen, delta_pixels) && screen->selections.in_progress) update_drag(w);
        } else {
            int s = scale_scroll(
                screen->modes.mouse_tracking_mode,
                ev->y_offset,
                ev->offset_type,
                &osw->scroll.pending_pixels_y,
                global_state.callback_os_window->fonts_data->fcm.cell_height,
                &osw->scroll.last_v120_dir_y);
            if (s) {
                bool upwards = s > 0;
                if (screen->modes.mouse_tracking_mode) {
                    int sz = encode_mouse_scroll(w, upwards ? 4 : 5, ev->keyboard_modifiers);
                    if (sz > 0) {
                        mouse_event_buf[sz] = 0;
                        for (s = abs(s); s > 0; s--) { write_escape_code_to_child(screen, ESC_CSI, mouse_event_buf); }
                    }
                } else {
                    if (screen->linebuf == screen->main_linebuf) {
                        screen_history_scroll(screen, abs(s), upwards);
                        if (screen->selections.in_progress) update_drag(w);
                    } else fake_scroll(w, abs(s), upwards);
                }
            }
        }
    }
    if (ev->x_offset != 0.0) {
        int s = scale_scroll(
            screen->modes.mouse_tracking_mode,
            ev->x_offset,
            ev->offset_type,
            &osw->scroll.pending_pixels_x,
            global_state.callback_os_window->fonts_data->fcm.cell_width,
            &osw->scroll.last_v120_dir_x);
        if (s) {
            if (screen->modes.mouse_tracking_mode) {
                int sz = encode_mouse_scroll(w, s > 0 ? 6 : 7, ev->keyboard_modifiers);
                if (sz > 0) {
                    mouse_event_buf[sz] = 0;
                    for (s = abs(s); s > 0; s--) { write_escape_code_to_child(screen, ESC_CSI, mouse_event_buf); }
                }
            }
        }
    }
}

// Tablet {{{
// The tablet pointer protocol, see docs/tablet-protocol.rst

static void
write_pointer_report(Screen *screen, const PointerReport *r) {
    char buf[1024];  // more than the longest report there can be
    int n = snprintf(buf, sizeof(buf), "777;tablet-pointer;v=1:op=event:kind=%d:phase=%d:device=%llu:t=%llu", r->kind, r->phase, (unsigned long long)r->device, r->t);
    if (r->has_tu) n += snprintf(buf + n, sizeof(buf) - n, ":tu=%u", r->tu);
    if (r->phase != POINTER_FRAME) {
        n += snprintf(buf + n, sizeof(buf) - n, ":contact=%d:x=%d:y=%d:buttons=%u:mods=%d:tool=%d:valid=%u", r->contact, r->x, r->y, r->buttons, r->mods, r->tool, r->valid);
#define F(bit, ...) if (r->valid & bit) n += snprintf(buf + n, sizeof(buf) - n, __VA_ARGS__)
        F(1, ":p=%d", r->p);
        F(2, ":tx=%d:ty=%d", r->tx, r->ty);
        F(4, ":twist=%d", r->twist);
        F(8, ":tangent=%d", r->tangent);
        F(16, ":z=%d", r->z);
        F(32, ":dist=%d", r->dist);
        F(64, ":wheel=%d:wclicks=%d", r->wheel, r->wclicks);
        F(128, ":major=%d:minor=%d:angle=%d", r->major, r->minor, r->angle);
        F(256, ":rx=%d:ry=%d", r->rx, r->ry);
        F(512, ":grip=%d", r->grip);
#undef F
        if (r->has_stage) n += snprintf(buf + n, sizeof(buf) - n, ":stage=%d", r->stage);
        if (r->has_conf) n += snprintf(buf + n, sizeof(buf) - n, ":conf=%d", r->conf);
        if (r->src) n += snprintf(buf + n, sizeof(buf) - n, ":src=%d", r->src);
        if (r->w > 0 && r->h > 0) snprintf(buf + n, sizeof(buf) - n, ":w=%d:h=%d", r->w, r->h);
    }
    write_escape_code_to_child(screen, ESC_OSC, buf);
}

// Reports a tool action to a program that asked for them
bool
screen_pointer_action(Screen *screen, const PointerAction *a) {
    if (!(screen->pointer.mode & POINTER_MODE_ACTIONS)) return false;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "777;tablet-pointer;v=1:op=event:kind=4:device=%llu:t=%llu", (unsigned long long)a->device, a->t);
    if (a->has_tu) n += snprintf(buf + n, sizeof(buf) - n, ":tu=%u", a->tu);
    n += snprintf(buf + n, sizeof(buf) - n, ":action=%d", a->action);
    if (a->state >= 0) n += snprintf(buf + n, sizeof(buf) - n, ":state=%d", a->state);
    if (a->state == 2) n += snprintf(buf + n, sizeof(buf) - n, ":dx=%d:dy=%d", MIN(MAX(a->dx, -32767), 32767), MIN(MAX(a->dy, -32767), 32767));
    snprintf(buf + n, sizeof(buf) - n, ":x=%d:y=%d:mods=%d", a->x, a->y, a->mods);
    write_escape_code_to_child(screen, ESC_OSC, buf);
    return true;
}

// The length of s cut to at most max bytes without splitting a UTF-8 sequence
static size_t
utf8_prefix_length(const char *s, size_t max) {
    size_t n = MIN(strlen(s), max), lead = n;
    while (lead > 0 && ((unsigned char)s[lead - 1] & 0xC0) == 0x80) lead--;
    if (lead > 0) {
        const unsigned char c = s[lead - 1];
        const size_t sequence = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1));
        if (lead - 1 + sequence > n) n = lead - 1;
    } else n = 0;
    return n;
}

// The most controls of one kind whose groups a device frame lists
#define POINTER_MAX_GROUPED 64

// Describes a tool or a pad to the program, which it is before the first of
// its reports and again when what was said of it changes. The hardware
// identifiers go only to a program that asked for them
bool
screen_pointer_device(Screen *screen, const PointerDevice *d) {
    if (!(screen->pointer.mode & (d->kind == POINTER_PAD ? POINTER_MODE_PADS : d->kind))) return false;
    char buf[2048];  // more than the longest frame there can be
    int n;
#define A(...) n += snprintf(buf + n, sizeof(buf) - n, __VA_ARGS__)
    if (d->kind == POINTER_PAD) {
        n = snprintf(
            buf, sizeof(buf), "777;tablet-pointer;v=1:op=device:device=%llu:kind=%d:buttons=%u:rings=%u:strips=%u:dials=%u:groups=%u",
            (unsigned long long)d->device, d->kind, d->num_buttons, d->num_rings, d->num_strips, d->num_dials, d->num_groups);
        const unsigned num_groups = MIN(d->num_groups, 16u);
        for (unsigned i = 0; i < num_groups && d->num_modes; i++) A("%s%u", i ? "," : ":modes=", MIN(d->num_modes[i], 9999u));
        for (unsigned i = 0; i < num_groups && d->modes; i++) A("%s%u", i ? "," : ":mode=", MIN(d->modes[i], 9999u));
        // The group of every control of a kind, or nothing: a list that stopped short would put the controls after it in no group.
        // A control in a group the frame does not describe is in none
#define G(key, groups, count)                                                                         \
    if (d->groups && d->count && d->count <= POINTER_MAX_GROUPED) {                                   \
        for (unsigned i = 0; i < d->count; i++) {                                                     \
            if (d->groups[i] < 0 || d->groups[i] >= (int)num_groups) A("%s-", i ? "," : ":" key "="); \
            else A("%s%d", i ? "," : ":" key "=", d->groups[i]);                                      \
        }                                                                                             \
    }
        G("bgroups", button_groups, num_buttons)
        G("rgroups", ring_groups, num_rings)
        G("sgroups", strip_groups, num_strips)
        G("dgroups", dial_groups, num_dials)
#undef G
    } else {
        n = snprintf(
            buf, sizeof(buf), "777;tablet-pointer;v=1:op=device:device=%llu:kind=%d:tool=%d:axes=%u", (unsigned long long)d->device, d->kind, d->tool, d->axes);
        if (d->w && d->h) A(":w=%u:h=%u", d->w, d->h);
        if (d->direct) A(":direct=%d", d->direct > 0);
        if (d->has_bat) A(":bat=%u", MIN(d->bat, 100u));
        if (d->has_pcolor) A(":pcolor=%06x", d->pcolor & 0xffffff);
        if (d->has_pwidth) A(":pwidth=%u", d->pwidth);
        if (d->pstyle) A(":pstyle=%u", d->pstyle);
    }
    if (d->tablet) A(":tablet=%llu", (unsigned long long)d->tablet);
    if (d->name && d->name[0]) {
        unsigned char name[192];
        size_t sz = sizeof(name) - 1;
        if (base64_encode8((const unsigned char *)d->name, utf8_prefix_length(d->name, 127), name, &sz, true)) {
            name[sz] = 0;
            A(":name=%s", name);
        }
    }
    if (screen->pointer.mode & POINTER_MODE_IDS) {
        if (d->serial) A(":serial=%llu", (unsigned long long)d->serial);
        if (d->hwid) A(":hwid=%llu", (unsigned long long)d->hwid);
        if (d->vid || d->pid) A(":vid=%u:pid=%u", d->vid, d->pid);
    }
#undef A
    write_escape_code_to_child(screen, ESC_OSC, buf);
    return true;
}

static void
write_pointer_pad(Screen *screen, const PointerPad *p) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "777;tablet-pointer;v=1:op=event:kind=3:device=%llu:t=%llu", (unsigned long long)p->device, p->t);
    if (p->has_tu) n += snprintf(buf + n, sizeof(buf) - n, ":tu=%u", p->tu);
    n += snprintf(buf + n, sizeof(buf) - n, ":mods=%d", p->mods);
#define C(name, value_name)                                                           \
    if (p->stop) n += snprintf(buf + n, sizeof(buf) - n, ":" name "=%d:stop=1", p->index); \
    else n += snprintf(buf + n, sizeof(buf) - n, ":" name "=%d:" value_name "=%d", p->index, p->value)
    switch (p->control) {
        case PAD_BUTTON: n += snprintf(buf + n, sizeof(buf) - n, ":button=%d:state=%d", p->index, p->value != 0); break;
        case PAD_RING: C("ring", "angle"); break;
        case PAD_STRIP: C("strip", "pos"); break;
        case PAD_DIAL: n += snprintf(buf + n, sizeof(buf) - n, ":dial=%d:delta=%d", p->index, p->value); break;
        case PAD_GROUP: n += snprintf(buf + n, sizeof(buf) - n, ":group=%d:mode=%d", p->index, p->value); break;
    }
#undef C
    if (p->from_finger) snprintf(buf + n, sizeof(buf) - n, ":src=1");
    write_escape_code_to_child(screen, ESC_OSC, buf);
}

// Reports a control of a pad to a program that asked for pads. The buttons that
// are down are remembered, so that they can be let go when the program can no
// longer be told, and a release whose press the program never saw is not sent
bool
screen_pointer_pad(Screen *screen, const PointerPad *p) {
    if (!(screen->pointer.mode & POINTER_MODE_PADS)) return false;
    if (p->control == PAD_BUTTON) {
        const PointerPad *down = screen->pointer.pad_buttons;
        unsigned i = 0;
        while (i < screen->pointer.num_pad_buttons && (down[i].device != p->device || down[i].index != p->index)) i++;
        const bool held = i < screen->pointer.num_pad_buttons;
        if (p->value) {
            if (!held) {
                ensure_space_for(&screen->pointer, pad_buttons, PointerPad, screen->pointer.num_pad_buttons + 1, pad_capacity, 8, false);
                screen->pointer.num_pad_buttons++;
            }
            screen->pointer.pad_buttons[i] = *p;
        } else {
            if (!held) return false;
            screen->pointer.pad_buttons[i] = screen->pointer.pad_buttons[--screen->pointer.num_pad_buttons];
        }
    }
    write_pointer_pad(screen, p);
    return true;
}

// Lets go of the held buttons of a pad, or of every pad when device is zero
void
screen_pointer_pad_release(Screen *screen, uint64_t device) {
    unsigned kept = 0;
    for (unsigned i = 0; i < screen->pointer.num_pad_buttons; i++) {
        PointerPad p = screen->pointer.pad_buttons[i];
        if (device && p.device != device) {
            screen->pointer.pad_buttons[kept++] = p;
            continue;
        }
        p.value = 0;
        write_pointer_pad(screen, &p);
    }
    screen->pointer.num_pad_buttons = kept;
}

static PointerReport *
pointer_contact(Screen *screen, const PointerReport *r, bool create) {
    for (unsigned i = 0; i < screen->pointer.num_contacts; i++) {
        PointerReport *c = screen->pointer.contacts + i;
        if (c->kind == r->kind && c->device == r->device && c->contact == r->contact) return c;
    }
    if (!create) return NULL;
    ensure_space_for(&screen->pointer, contacts, PointerReport, screen->pointer.num_contacts + 1, capacity, 16, false);
    return screen->pointer.contacts + screen->pointer.num_contacts++;
}

// Reports a tool or a finger to a program that asked for its kind, and returns
// whether anything was sent. A finger the system takes for an accident, a palm
// or a thumb at rest, is for a program that asked for those as well. To any
// other it is not there: one that lands as an accident is kept in the table of
// contacts as hidden, and begins with a down if the system comes to take it to
// be meant, and one that becomes an accident while it is being reported is
// cancelled and hidden. A frame closes the finger reports before it, and is
// not sent when there were none
bool
screen_pointer_report(Screen *screen, PointerReport *r) {
    if (!(screen->pointer.mode & r->kind)) return false;
    if (r->phase == POINTER_FRAME) {
        if (r->kind == POINTER_TOUCH && !screen->pointer.unframed) return false;
        screen->pointer.unframed = false;
        write_pointer_report(screen, r);
        return true;
    }
    const bool hide = r->kind == POINTER_TOUCH && r->has_conf && !r->conf && !(screen->pointer.mode & POINTER_MODE_ACCIDENTAL);
    PointerReport *c = pointer_contact(screen, r, false);
    const bool was_shown = c && !c->hidden, was_hidden = c && c->hidden;
    switch (r->phase) {
        case POINTER_DOWN:
            if (!c) c = pointer_contact(screen, r, true);
            /* fallthrough */
        case POINTER_MOVE:
            if (!c) return false;  // a move whose stroke was cancelled by a reset or resize
            *c = *r;
            c->hidden = hide;
            if (hide) {
                if (!was_shown) return false;
                // The program has it as a finger, and takes back what it made of it
                r->phase = POINTER_CANCEL;
                r->buttons = 0;
                r->p = 0;
            } else if (was_hidden) r->phase = c->phase = POINTER_DOWN;
            break;
        case POINTER_UP:
        case POINTER_CANCEL:
            if (!c) return false;
            *c = screen->pointer.contacts[--screen->pointer.num_contacts];
            if (was_hidden) return false;
            if (hide) r->phase = POINTER_CANCEL;  // a finger that lifts as an accident did not tap
            r->buttons = 0;
            r->p = 0;
            break;
        default: break;
    }
    if (r->kind == POINTER_TOUCH) screen->pointer.unframed = true;
    write_pointer_report(screen, r);
    return true;
}

// Ends the contacts of a kind, or of every kind when it is zero. The program is
// told of those it knows of, and the hidden ones are forgotten with them, so
// that what is left of a contact after this is dropped whatever it was
void
screen_pointer_cancel(Screen *screen, int kind) {
    unsigned kept = 0;
    for (unsigned i = 0; i < screen->pointer.num_contacts; i++) {
        PointerReport r = screen->pointer.contacts[i];
        if (kind && r.kind != kind) {
            screen->pointer.contacts[kept++] = r;
            continue;
        }
        if (r.hidden) continue;
        r.phase = POINTER_CANCEL;
        r.buttons = 0;
        r.p = 0;
        write_pointer_report(screen, &r);
    }
    screen->pointer.num_contacts = kept;
    if (!kind) screen->pointer.unframed = false;
    if (!kind || kind == POINTER_PAD) screen_pointer_pad_release(screen, 0);
}

// What this terminal can report: tools 1, fingers on a touchscreen 2, fingers
// on a touchpad 4, the shape of a finger contact 8, pads 16, tool actions 32,
// hardware identifiers 64, the fingers the system takes for accidents 256. The
// window system says which it has of all but the identifiers, and the bits of
// GLFWPointerInputClass are these bits. None of the window systems has the last
// one: AppKit has a mark for a touch at rest that no trackpad was seen to set
static unsigned
pointer_caps(void) {
#ifdef __APPLE__
    unsigned caps = 1 | 4 | 32;
#else
    unsigned caps = global_state.is_wayland ? 1 | 2 : 1;
#endif
    // No window system is loaded when the tests run
    if (glfwGetPointerInputClasses) caps = glfwGetPointerInputClasses();
    if (caps & (1 | 16)) caps |= 64;
    return caps;
}

typedef struct TabletTool {
    bool active, down;
    uint64_t device;
    id_type window_id;     // the window that captured the stroke
    id_type described_to;  // the window whose program was sent the device frame
    uint64_t described;    // the signature of what that frame said
} TabletTool;
static TabletTool tablet_tools[8];

typedef struct TabletPad {
    bool active;
    uint64_t device;
    id_type window_id;     // the window that got its last report, and so knows of its held buttons
    id_type described_to;  // the window whose program was sent the device frame
    uint64_t described;    // the signature of what that frame said
} TabletPad;
static TabletPad tablet_pads[8];

// The last label a program gave a control of a pad, for the tests
static struct {
    bool is_set;
    uint64_t device;
    int control, index;
    char *text;  // malloc'd
} last_pad_label;

// A label is passed to the window system, which may show it to the user, when
// it comes from the program the pad is reporting to
static void
pointer_pad_label(Screen *screen, uint64_t device, PointerPadControl control, int index, const char *text) {
    if (!(screen->pointer.mode & POINTER_MODE_PADS)) return;
    free(last_pad_label.text);
    last_pad_label.text = strdup(text);
    last_pad_label.is_set = last_pad_label.text != NULL;
    last_pad_label.device = device;
    last_pad_label.control = control;
    last_pad_label.index = index;
    const OSWindow *osw = os_window_for_kitty_window(screen->window_id);
    if (!osw || !osw->handle || !osw->num_tabs || !global_state.is_wayland || !glfwWaylandSetTabletPadLabel) return;
    const Tab *tab = osw->tabs + osw->active_tab;
    if (!tab->num_windows || tab->windows[tab->active_window].id != screen->window_id) return;
    static const int controls[] = {
        [PAD_BUTTON] = GLFW_TABLET_PAD_BUTTON, [PAD_RING] = GLFW_TABLET_PAD_RING, [PAD_STRIP] = GLFW_TABLET_PAD_STRIP, [PAD_DIAL] = GLFW_TABLET_PAD_DIAL,
    };
    glfwWaylandSetTabletPadLabel(osw->handle, device, controls[control], (unsigned)index, text);
}

// Decodes the text of a label: base64 of UTF-8 with no control characters.
// Returns NULL when it is anything else or there is no memory for it; the
// caller frees the text
static char *
decode_pad_label(const char *src) {
    const size_t src_sz = strlen(src);
    size_t sz = required_buffer_size_for_base64_decode(src_sz);
    uint8_t *decoded = malloc(sz + 1);
    if (!decoded) return NULL;
    if (!src_sz) sz = 0;
    else if (!base64_decode8_strict((const uint8_t *)src, src_sz, decoded, &sz)) goto fail;
    uint32_t state = 0, codep = 0;
    for (size_t i = 0; i < sz; i++) {
        if (decode_utf8(&state, &codep, decoded[i]) == 1) goto fail;
        if (state == 0 && (codep < 0x20 || codep == 0x7f)) goto fail;
    }
    if (state != 0) goto fail;
    decoded[sz] = 0;
    return (char *)decoded;
fail:
    free(decoded);
    return NULL;
}

// A decimal integer of no more than max that is the whole of val, with no sign
static bool
pointer_uint(const char *val, unsigned long long max, unsigned long long *ans) {
    if (!*val) return false;
    unsigned long long x = 0;
    for (const char *p = val; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        const unsigned d = *p - '0';
        if (x > (max - d) / 10) return false;
        x = x * 10 + d;
    }
    *ans = x;
    return true;
}

// A frame from a program, framed as docs/extension-framing.rst says: one with a
// duplicate key or a value out of range is rejected as a whole, and a key that
// is not known is ignored
void
screen_tablet_pointer(Screen *screen, const char *data, size_t sz) {
    EXTENSION_FIELDS(f);
    char reply[128];
    const char *op = "", *text = "";
    unsigned long long v = 0, x;
    int mode = -1, id = -1, num_controls = 0, control_index = -1;
    PointerPadControl control = PAD_BUTTON;
    unsigned long long device = 0;
    bool has_device = false;
    if (!extension_fields_parse(&f, data, sz)) return;
    for (unsigned i = 0; i < f.count; i++) {
        const char *tok = f.keys[i], *val = f.values[i];
#define CONTROL(name, which) \
    else if (!strcmp(tok, name)) { \
        if (!pointer_uint(val, 255, &x)) return; \
        control = which; \
        control_index = (int)x; \
        num_controls++; \
    }
        if (!strcmp(tok, "v")) { if (!pointer_uint(val, UINT32_MAX, &v)) return; }
        else if (!strcmp(tok, "op")) op = val;
        else if (!strcmp(tok, "mode")) { if (!pointer_uint(val, INT32_MAX, &x)) return; mode = (int)x; }
        else if (!strcmp(tok, "id")) { if (!pointer_uint(val, INT32_MAX, &x)) return; id = (int)x; }
        else if (!strcmp(tok, "device")) { if (!pointer_uint(val, UINT64_MAX, &device)) return; has_device = true; }
        else if (!strcmp(tok, "text")) text = val;
        CONTROL("button", PAD_BUTTON)
        CONTROL("ring", PAD_RING)
        CONTROL("strip", PAD_STRIP)
        CONTROL("dial", PAD_DIAL)
#undef CONTROL
    }
    if (v != 1) return;
    if (!strcmp(op, "label")) {
        // Says what one control of a pad does at the moment. A label that is not well formed is dropped as a whole
        if (!has_device || num_controls != 1 || control_index < 0 || control_index > 255) return;
        char *label = decode_pad_label(text);
        if (!label) return;
        pointer_pad_label(screen, device, control, control_index, label);
        free(label);
    } else if (!strcmp(op, "query") && id >= 0) {
        const unsigned cw = MIN(4096u, MAX(1u, screen->cell_size.width)), ch = MIN(4096u, MAX(1u, screen->cell_size.height));
        snprintf(
            reply, sizeof(reply), "777;tablet-pointer;v=1:op=cap:id=%d:caps=%u:mode=%u:w=%u:h=%u:cw=%u:ch=%u", id, pointer_caps(), screen->pointer.mode,
            MIN(65535u, screen->columns * cw), MIN(65535u, screen->lines * ch), cw, ch);
        write_escape_code_to_child(screen, ESC_OSC, reply);
    } else if (!strcmp(op, "mode")) {
        if (mode < 0) {  // a read
            snprintf(reply, sizeof(reply), "777;tablet-pointer;v=1:op=mode:mode=%u", screen->pointer.mode);
            write_escape_code_to_child(screen, ESC_OSC, reply);
            return;
        }
        screen_pointer_cancel(screen, 0);
        // Only the bits this terminal knows are kept, so a program that reads the mode sees what took. The user can
        // refuse the hardware identifiers, which tell whose pen it is
        unsigned granted = mode & POINTER_MODES;
        if (!OPT(tablet_hardware_ids)) granted &= ~POINTER_MODE_IDS;
        screen->pointer.mode = granted;
        // A program that changes what it wants is told about the tools in proximity and the pads again
        for (size_t i = 0; i < arraysz(tablet_tools); i++) {
            if (tablet_tools[i].described_to == screen->window_id) tablet_tools[i].described_to = 0;
        }
        for (size_t i = 0; i < arraysz(tablet_pads); i++) {
            if (tablet_pads[i].described_to == screen->window_id) tablet_pads[i].described_to = 0;
        }
    }
}

static id_type touch_pointer_window;  // the window that captured the fingers in progress
static bool touch_pointer_at_rest;    // those of them that are down are all ones the system takes for an accident

static TabletTool *
tablet_tool(uint64_t device) {
    TabletTool *free_slot = tablet_tools;
    for (size_t i = 0; i < arraysz(tablet_tools); i++) {
        if (tablet_tools[i].active && tablet_tools[i].device == device) return tablet_tools + i;
        if (!tablet_tools[i].active) free_slot = tablet_tools + i;
    }
    *free_slot = (TabletTool){.active = true, .device = device};
    return free_slot;
}

static int
pointer_mods(int mods) {
    return (mods & GLFW_MOD_SHIFT ? 1 : 0) | (mods & GLFW_MOD_ALT ? 2 : 0) | (mods & GLFW_MOD_CONTROL ? 4 : 0) | (mods & GLFW_MOD_SUPER ? 8 : 0);
}

// The window under the mouse, if it has this bit of the mode on
static Window *
pointer_window(int bit) {
    Window *w = mouse_region(false, false).window;
    return w && (w->render_data.screen->pointer.mode & bit) ? w : NULL;
}

#define pointer_coord(v) (int)round(MIN(MAX((v) * 256, -1073741823.), 1073741823.))
#define pointer_clamp(v, lo, hi) MIN(MAX(v, lo), hi)

// Sets the t and tu of a report or an action from the time of an event
#define pointer_time(r, timestamp, is_fine)                                \
    {                                                                      \
        const monotonic_t pointer_time_ = MAX(0, timestamp);               \
        (r)->t = (unsigned long long)(pointer_time_ / MONOTONIC_T_1e6);    \
        (r)->tu = (unsigned)(pointer_time_ / MONOTONIC_T_1e3 % 1000);      \
        (r)->has_tu = is_fine;                                             \
    }

// A number that is another one when what a device frame says of a device is.
// The modes the groups of a pad are in are left out, a change of mode being
// reported by itself
static uint64_t
pointer_device_signature(const PointerDevice *d) {
    uint64_t h = 0xcbf29ce484222325ull;
#define H(v) h = (h ^ (uint64_t)(v)) * 0x100000001b3ull
#define HG(groups, count) \
    H(d->count);          \
    for (unsigned i = 0; i < d->count && d->groups; i++) H(d->groups[i] + 2)
    H(d->kind), H(d->tool), H(d->axes), H(d->vid), H(d->pid), H(d->serial), H(d->hwid), H(d->tablet), H(d->w), H(d->h), H(d->direct + 1);
    H(d->has_bat ? d->bat + 1 : 0), H(d->has_pcolor ? d->pcolor + 1 : 0), H(d->has_pwidth ? d->pwidth + 1 : 0), H(d->pstyle);
    for (const char *c = d->name ? d->name : ""; *c; c++) H((unsigned char)*c);
    HG(num_modes, num_groups);
    HG(button_groups, num_buttons);
    HG(ring_groups, num_rings);
    HG(strip_groups, num_strips);
    HG(dial_groups, num_dials);
#undef HG
#undef H
    return h;
}

// An action goes to the window that has the stroke of its tool, or else to the
// window under the pointer
static bool
tablet_action(const GLFWTabletEvent *ev) {
    const OSWindow *osw = global_state.callback_os_window;
    Window *w = NULL;
    for (size_t i = 0; i < arraysz(tablet_tools) && ev->device; i++) {
        const TabletTool *t = tablet_tools + i;
        if (t->active && t->device == ev->device && t->window_id) w = window_for_window_id(t->window_id);
    }
    if (!w) w = pointer_window(POINTER_MODE_ACTIONS);
    if (!w) return false;
    const WindowGeometry *g = &w->render_data.geometry;
    PointerAction a = {
        .action = ev->action,
        .state = ev->action_state == GLFW_TABLET_ACTION_INSTANT ? -1 : ev->action_state == GLFW_TABLET_ACTION_BEGAN,
        .device = ev->device,
        .x = pointer_coord(osw->mouse_x - g->left),
        .y = pointer_coord(osw->mouse_y - g->top),
        .mods = pointer_mods(ev->keyboard_modifiers),
    };
    pointer_time(&a, ev->timestamp, ev->timestamp_is_fine);
    return screen_pointer_action(w->render_data.screen, &a);
}

bool
tablet_event(const GLFWTabletEvent *ev) {
    if (ev->type == GLFW_TABLET_ACTION) return tablet_action(ev);
    const OSWindow *osw = global_state.callback_os_window;
    TabletTool *t = tablet_tool(ev->device);
    Window *w = t->window_id ? window_for_window_id(t->window_id) : pointer_window(POINTER_PEN);
    if (!w && ev->type != GLFW_TABLET_POINT && t->described_to) {
        // A tool that goes away while it is over no window is still gone for the window that was last told of it
        w = window_for_window_id(t->described_to);
        if (w && !(w->render_data.screen->pointer.mode & POINTER_PEN)) w = NULL;
    }
    if (!w) {
        // The tool is remembered for as long as it is in proximity, even over no window
        if (ev->type != GLFW_TABLET_POINT) t->active = false;
        return false;
    }
    const WindowGeometry *g = &w->render_data.geometry;
    const bool has_size = ev->tablet_width > 0 && ev->tablet_height > 0;
    const PointerDevice d = {
        .kind = POINTER_PEN, .tool = ev->tool, .axes = ev->axes, .device = ev->device, .name = ev->name,
        .serial = ev->serial, .hwid = ev->hardware_id, .vid = ev->vendor_id, .pid = ev->product_id, .tablet = ev->tablet,
        .w = has_size ? (unsigned)pointer_clamp(round(ev->tablet_width * 256), 1., 1073741823.) : 0,
        .h = has_size ? (unsigned)pointer_clamp(round(ev->tablet_height * 256), 1., 1073741823.) : 0,
        .direct = ev->tablet_direct,
    };
    // A tool is described again when what was said of it is no longer so. Only
    // a point has all there is to say, the event of a tool that left may not
    const uint64_t signature = ev->type == GLFW_TABLET_POINT ? pointer_device_signature(&d) : t->described;
    if (t->described_to != w->id || t->described != signature) {
        if (screen_pointer_device(w->render_data.screen, &d)) {
            t->described_to = w->id;
            t->described = signature;
        }
    }
    PointerReport r = {
        .kind = POINTER_PEN,
        .device = ev->device,
        .x = pointer_coord(osw->mouse_x - g->left),
        .y = pointer_coord(osw->mouse_y - g->top),
        .buttons = ev->buttons,
        .mods = pointer_mods(ev->keyboard_modifiers),
        .tool = ev->tool,
        .valid = ev->axes,
        .p = pointer_clamp((int)round(ev->pressure * 65535), 0, 65535),
        .tx = pointer_clamp((int)round(ev->tilt_x * 100), -9000, 9000),
        .ty = pointer_clamp((int)round(ev->tilt_y * 100), -9000, 9000),
        .twist = ((int)round(ev->twist * 100) % 36000 + 36000) % 36000,
        .tangent = pointer_clamp((int)round(ev->tangent * 65535), -65535, 65535),
        .z = (int)pointer_clamp(ev->z, -1000000., 1000000.),
        .dist = pointer_clamp((int)round(ev->distance * 65535), 0, 65535),
        .wheel = (int)round(ev->wheel * 100),
        .wclicks = ev->wheel_clicks,
        .rx = pointer_clamp((int)round(ev->tablet_x * 65535), 0, 65535),
        .ry = pointer_clamp((int)round(ev->tablet_y * 65535), 0, 65535),
    };
    pointer_time(&r, ev->timestamp, ev->timestamp_is_fine);
    if (ev->type == GLFW_TABLET_POINT) {
        const bool tip = ev->buttons & GLFW_TABLET_BUTTON_TIP;
        r.phase = tip ? (t->down ? POINTER_MOVE : POINTER_DOWN) : (t->down ? POINTER_UP : POINTER_HOVER);
        t->down = tip;
        t->window_id = tip ? w->id : 0;
    } else {
        r.phase = t->down ? POINTER_CANCEL : POINTER_OUT;
        t->active = false;
        if (t->down && ev->type == GLFW_TABLET_OUT) {
            // The stroke is lost, and the system also said that the tool left
            screen_pointer_report(w->render_data.screen, &r);
            r.phase = POINTER_OUT;
        }
    }
    screen_pointer_report(w->render_data.screen, &r);
    return true;
}

// A pad reports to the window that has the keyboard focus, when it asked for pads
static Window *
pad_window(const OSWindow *osw) {
    if (!osw || !osw->num_tabs) return NULL;
    Tab *tab = osw->tabs + osw->active_tab;
    if (!tab->num_windows) return NULL;
    Window *w = tab->windows + tab->active_window;
    return w->render_data.screen && (w->render_data.screen->pointer.mode & POINTER_MODE_PADS) ? w : NULL;
}

bool
tablet_pad_event(const GLFWTabletPadEvent *ev) {
    TabletPad *pad = NULL, *free_slot = tablet_pads;
    for (size_t i = 0; i < arraysz(tablet_pads) && !pad; i++) {
        if (tablet_pads[i].active && tablet_pads[i].device == ev->device) pad = tablet_pads + i;
        else if (!tablet_pads[i].active) free_slot = tablet_pads + i;
    }
    if (!pad) {
        pad = free_slot;
        *pad = (TabletPad){.active = true, .device = ev->device};
    }
    Window *w = ev->type == GLFW_TABLET_PAD_LEAVE ? NULL : pad_window(global_state.callback_os_window);
    if (pad->window_id && (!w || w->id != pad->window_id)) {
        // The pad no longer reports to the window that knows of its held buttons
        Window *before = window_for_window_id(pad->window_id);
        if (before) screen_pointer_pad_release(before->render_data.screen, ev->device);
        pad->window_id = 0;
    }
    if (ev->type == GLFW_TABLET_PAD_LEAVE) pad->active = false;
    if (!w) return false;
    Screen *screen = w->render_data.screen;
    const PointerDevice d = {
        .kind = POINTER_PAD, .device = ev->device, .name = ev->name, .vid = ev->vendor_id, .pid = ev->product_id, .tablet = ev->tablet,
        .num_buttons = ev->num_buttons, .num_rings = ev->num_rings, .num_strips = ev->num_strips, .num_dials = ev->num_dials,
        .num_groups = ev->num_groups, .num_modes = ev->num_modes, .modes = ev->modes,
        .button_groups = ev->button_groups, .ring_groups = ev->ring_groups, .strip_groups = ev->strip_groups, .dial_groups = ev->dial_groups,
    };
    // A pad is described again when what was said of it is no longer so
    const uint64_t signature = pointer_device_signature(&d);
    if (pad->described_to != w->id || pad->described != signature) {
        if (screen_pointer_device(screen, &d)) {
            pad->described_to = w->id;
            pad->described = signature;
        }
    }
    PointerPad p = {
        .device = ev->device, .mods = pointer_mods(ev->keyboard_modifiers), .index = (int)ev->index, .stop = ev->stopped, .from_finger = ev->from_finger,
    };
    pointer_time(&p, ev->timestamp, ev->timestamp_is_fine);
    switch (ev->type) {
        case GLFW_TABLET_PAD_BUTTON:
            p.control = PAD_BUTTON;
            p.value = ev->pressed;
            break;
        case GLFW_TABLET_PAD_RING:
            p.control = PAD_RING;
            p.value = ((int)round(ev->value * 100) % 36000 + 36000) % 36000;
            break;
        case GLFW_TABLET_PAD_STRIP:
            p.control = PAD_STRIP;
            p.value = pointer_clamp((int)round(ev->value * 65535), 0, 65535);
            break;
        case GLFW_TABLET_PAD_DIAL:
            p.control = PAD_DIAL;
            p.value = ev->delta;
            break;
        case GLFW_TABLET_PAD_MODE:
            p.control = PAD_GROUP;
            p.value = (int)ev->mode;
            break;
        default: return true;  // the pad got the focus or changed, and has been described
    }
    pad->window_id = w->id;
    screen_pointer_pad(screen, &p);
    return true;
}

// Reports the fingers of a touch event to the window that captured them: the
// window under the first finger, or under the pointer for a touchpad. Returns
// true when a window captured the event, so that it is not also turned into
// mouse events
bool
touch_pointer_event(const GLFWTouchEvent *ev) {
    OSWindow *osw = global_state.callback_os_window;
    Window *w = NULL;
    // A contact the system takes for an accident, as a thumb resting on a touchpad, does not keep a sequence open: a
    // finger that lands when only such contacts are down begins a sequence of its own, with a window of its own. A
    // system that marks no contact as an accident has no such sequence, and there a sequence is what it always was
    const GLFWTouchPoint *meant = NULL;  // the first contact that is down and is not taken for an accident
    bool accident_down = false;
    for (size_t i = 0; i < ev->num_points; i++) {
        const GLFWTouchPoint *p = ev->points + i;
        if (p->state == GLFW_TOUCH_POINT_RELEASED) continue;
        if (p->confidence == GLFW_TOUCH_ACCIDENTAL) accident_down = true;
        else if (!meant) meant = p;
    }
    const bool begins_again = ev->type == GLFW_TOUCH_UPDATE && meant && touch_pointer_at_rest;
    bool moved_window = false;
    if (ev->type == GLFW_TOUCH_BEGIN || begins_again) {
        if (ev->indirect) w = pointer_window(POINTER_TOUCH);
        else if (ev->num_points) {
            // Locate the window under the finger without moving the mouse
            const GLFWTouchPoint *first = meant ? meant : ev->points;
            const double mouse_x = osw->mouse_x, mouse_y = osw->mouse_y;
            osw->mouse_x = first->x * osw->viewport_x_ratio;
            osw->mouse_y = first->y * osw->viewport_y_ratio;
            w = pointer_window(POINTER_TOUCH);
            osw->mouse_x = mouse_x;
            osw->mouse_y = mouse_y;
        }
        if (begins_again && touch_pointer_window != (w ? w->id : 0)) {
            // The contacts that are down go with the new sequence, and are gone for the window that had them
            Window *before = touch_pointer_window ? window_for_window_id(touch_pointer_window) : NULL;
            if (before) screen_pointer_cancel(before->render_data.screen, POINTER_TOUCH);
            moved_window = true;
        }
        touch_pointer_window = w ? w->id : 0;
    } else if (touch_pointer_window) w = window_for_window_id(touch_pointer_window);
    touch_pointer_at_rest = !meant && accident_down;
    if (ev->type == GLFW_TOUCH_END || ev->type == GLFW_TOUCH_CANCEL) touch_pointer_window = 0, touch_pointer_at_rest = false;
    if (!w) return false;
    Screen *screen = w->render_data.screen;
    const WindowGeometry *g = &w->render_data.geometry;
    const int mods = pointer_mods(ev->keyboard_modifiers);
    for (size_t i = 0; i < ev->num_points; i++) {
        const GLFWTouchPoint *p = ev->points + i;
        PointerReport r = {
            .kind = POINTER_TOUCH, .contact = p->id, .buttons = 1, .mods = mods, .src = ev->indirect,
            .w = pointer_coord(ev->device_width), .h = pointer_coord(ev->device_height),
        };
        pointer_time(&r, ev->timestamp, ev->timestamp_is_fine);
        switch (p->state) {
            case GLFW_TOUCH_POINT_PRESSED: r.phase = POINTER_DOWN; break;
            case GLFW_TOUCH_POINT_MOVED: r.phase = POINTER_MOVE; break;
            case GLFW_TOUCH_POINT_RELEASED: r.phase = ev->type == GLFW_TOUCH_CANCEL ? POINTER_CANCEL : POINTER_UP; break;
            default:
                if (!moved_window) continue;
                break;
        }
        // A window that takes over contacts that are down has not been told of them, so there they begin
        if (moved_window && p->state != GLFW_TOUCH_POINT_RELEASED) r.phase = POINTER_DOWN;
        if (p->confidence != GLFW_TOUCH_CONFIDENCE_UNKNOWN) r.conf = p->confidence == GLFW_TOUCH_INTENDED, r.has_conf = true;
        if (ev->indirect) {
            r.x = pointer_clamp((int)round(p->x * 65535), 0, 65535);
            r.y = pointer_clamp((int)round(p->y * 65535), 0, 65535);
            if (ev->has_pressure) {
                // The force on the pad as a whole, which every finger on it reports
                r.valid |= 1;
                r.p = pointer_clamp((int)round(ev->pressure * 65535), 0, 65535);
                r.stage = pointer_clamp(ev->stage, 0, 2);
                r.has_stage = true;
            }
        } else {
            r.x = pointer_coord(p->x * osw->viewport_x_ratio - g->left);
            r.y = pointer_coord(p->y * osw->viewport_y_ratio - g->top);
            if (p->major > 0) {
                r.valid = 128;
                r.major = pointer_coord(p->major * osw->viewport_x_ratio);
                r.minor = pointer_coord(p->minor * osw->viewport_y_ratio);
                r.angle = (int)round(p->angle * 100);
            }
        }
        screen_pointer_report(screen, &r);
    }
    if (ev->type == GLFW_TOUCH_CANCEL) screen_pointer_cancel(screen, POINTER_TOUCH);
    // The screen sends the frame only when it sent one of the reports
    PointerReport frame = {.kind = POINTER_TOUCH, .phase = POINTER_FRAME};
    pointer_time(&frame, ev->timestamp, ev->timestamp_is_fine);
    screen_pointer_report(screen, &frame);
    return true;
}
#undef pointer_coord
#undef pointer_clamp
#undef pointer_time

static PyObject *
send_pointer_report(PyObject *self UNUSED, PyObject *args, PyObject *kw) {
    Screen *screen;
    PointerReport r = {.tool = 1};
    int tu = -1;
    int stage = -1, conf = -1;
    static const char *kwlist[] = {"screen", "kind",  "phase",   "device", "contact", "x",    "y",     "buttons", "mods", "tool",  "valid", "p",     "tx", "ty",
                                   "twist",  "tangent", "z",     "dist",   "wheel",   "wclicks", "t",  "src",     "major", "minor", "angle", "w",     "h",  "tu",
                                   "stage",  "rx",      "ry",    "grip",   "conf",    NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args, kw, "O!iiKiii|IiiIiiiiiiiiiKiiiiiiiiiiii", (char **)kwlist, &Screen_Type, &screen, &r.kind, &r.phase, &r.device, &r.contact, &r.x, &r.y, &r.buttons,
            &r.mods, &r.tool, &r.valid, &r.p, &r.tx, &r.ty, &r.twist, &r.tangent, &r.z, &r.dist, &r.wheel, &r.wclicks, &r.t, &r.src, &r.major, &r.minor, &r.angle,
            &r.w, &r.h, &tu, &stage, &r.rx, &r.ry, &r.grip, &conf))
        return NULL;
    if (tu >= 0) r.tu = tu, r.has_tu = true;
    if (stage >= 0) r.stage = stage, r.has_stage = true;
    if (conf >= 0) r.conf = conf != 0, r.has_conf = true;
    if (screen_pointer_report(screen, &r)) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static PyObject *
send_pointer_action(PyObject *self UNUSED, PyObject *args, PyObject *kw) {
    Screen *screen;
    PointerAction a = {.state = -1};
    int tu = -1;
    static const char *kwlist[] = {"screen", "action", "device", "state", "x", "y", "mods", "t", "tu", "dx", "dy", NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args, kw, "O!i|KiiiiKiii", (char **)kwlist, &Screen_Type, &screen, &a.action, &a.device, &a.state, &a.x, &a.y, &a.mods, &a.t, &tu, &a.dx, &a.dy))
        return NULL;
    if (tu >= 0) a.tu = tu, a.has_tu = true;
    if (screen_pointer_action(screen, &a)) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

// Fills dest from a sequence of small numbers, returning how many there were or -1 with an exception set
static int
unsigned_sequence(PyObject *src, unsigned *dest, size_t max) {
    if (!src || src == Py_None) return 0;
    PyObject *fast = PySequence_Fast(src, "expected a sequence of numbers");
    if (!fast) return -1;
    const Py_ssize_t n = MIN(PySequence_Fast_GET_SIZE(fast), (Py_ssize_t)max);
    for (Py_ssize_t i = 0; i < n; i++) dest[i] = (unsigned)PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(fast, i));
    Py_DECREF(fast);
    return PyErr_Occurred() ? -1 : (int)n;
}

// Fills dest with the group of each control from a sequence in which None is a
// control in no group. Returns false with an exception set when it is anything
// else. *groups is left NULL for no sequence, which is groups that are not known
static bool
group_sequence(PyObject *src, int *dest, size_t max, const int **groups, unsigned count) {
    if (!src || src == Py_None) return true;
    PyObject *fast = PySequence_Fast(src, "expected a sequence of group numbers");
    if (!fast) return false;
    const Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    if (n != (Py_ssize_t)count || (size_t)n > max) {
        Py_DECREF(fast);
        PyErr_SetString(PyExc_ValueError, "a pad needs a group for every one of its controls");
        return false;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(fast, i);
        dest[i] = item == Py_None ? -1 : (int)PyLong_AsLong(item);
    }
    Py_DECREF(fast);
    if (PyErr_Occurred()) return false;
    *groups = dest;
    return true;
}

static PyObject *
send_pointer_device(PyObject *self UNUSED, PyObject *args, PyObject *kw) {
    Screen *screen;
    PointerDevice d = {.kind = POINTER_PEN, .tool = 1};
    PyObject *num_modes = NULL, *modes = NULL, *bgroups = NULL, *rgroups = NULL, *sgroups = NULL, *dgroups = NULL;
    unsigned num_modes_buf[16], modes_buf[16];
    int groups_buf[4][POINTER_MAX_GROUPED];
    int direct = -1, bat = -1, pcolor = -1, pwidth = -1;
    static const char *kwlist[] = {"screen", "device", "kind",  "tool",  "axes",      "name",  "serial", "hwid", "vid", "pid",    "buttons", "rings",
                                   "strips", "dials",  "num_modes", "modes", "tablet", "w",   "h",      "direct", "bat", "pcolor", "pwidth",  "pstyle",
                                   "bgroups", "rgroups", "sgroups", "dgroups", NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args, kw, "O!K|iiIzKKIIIIIIOOKIIiiiiIOOOO", (char **)kwlist, &Screen_Type, &screen, &d.device, &d.kind, &d.tool, &d.axes, &d.name, &d.serial, &d.hwid,
            &d.vid, &d.pid, &d.num_buttons, &d.num_rings, &d.num_strips, &d.num_dials, &num_modes, &modes, &d.tablet, &d.w, &d.h, &direct, &bat, &pcolor, &pwidth,
            &d.pstyle, &bgroups, &rgroups, &sgroups, &dgroups))
        return NULL;
    // A pad has as many groups as it is given numbers of modes
    const int num_groups = unsigned_sequence(num_modes, num_modes_buf, arraysz(num_modes_buf)), num = unsigned_sequence(modes, modes_buf, arraysz(modes_buf));
    if (num_groups < 0 || num < 0) return NULL;
    d.num_groups = num_groups;
    d.num_modes = num_modes_buf;
    if (num && num == num_groups) d.modes = modes_buf;
    if (!group_sequence(bgroups, groups_buf[0], POINTER_MAX_GROUPED, &d.button_groups, d.num_buttons)) return NULL;
    if (!group_sequence(rgroups, groups_buf[1], POINTER_MAX_GROUPED, &d.ring_groups, d.num_rings)) return NULL;
    if (!group_sequence(sgroups, groups_buf[2], POINTER_MAX_GROUPED, &d.strip_groups, d.num_strips)) return NULL;
    if (!group_sequence(dgroups, groups_buf[3], POINTER_MAX_GROUPED, &d.dial_groups, d.num_dials)) return NULL;
    // -1 is what is not known, and for direct 0 is a surface that is not a display
    d.direct = direct < 0 ? 0 : (direct ? 1 : -1);
    if (bat >= 0) d.bat = bat, d.has_bat = true;
    if (pcolor >= 0) d.pcolor = pcolor, d.has_pcolor = true;
    if (pwidth >= 0) d.pwidth = pwidth, d.has_pwidth = true;
    if (screen_pointer_device(screen, &d)) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static PyObject *
send_pointer_pad(PyObject *self UNUSED, PyObject *args, PyObject *kw) {
    Screen *screen;
    PointerPad p = {0};
    int control, tu = -1, stop = 0, src = 0;
    static const char *kwlist[] = {"screen", "device", "control", "index", "value", "stop", "src", "mods", "t", "tu", NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args, kw, "O!Kii|ippiKi", (char **)kwlist, &Screen_Type, &screen, &p.device, &control, &p.index, &p.value, &stop, &src, &p.mods, &p.t, &tu))
        return NULL;
    if (control < PAD_BUTTON || control > PAD_GROUP) {
        PyErr_SetString(PyExc_ValueError, "unknown pad control");
        return NULL;
    }
    p.control = control;
    p.stop = stop;
    p.from_finger = src;
    if (tu >= 0) p.tu = tu, p.has_tu = true;
    if (screen_pointer_pad(screen, &p)) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

// The last label a program gave a control of a pad as (device, control, index, text), then forgotten. None when there was none
static PyObject *
take_pointer_pad_label(PyObject *self UNUSED, PyObject *args UNUSED) {
    if (!last_pad_label.is_set) Py_RETURN_NONE;
    last_pad_label.is_set = false;
    return Py_BuildValue("Kiis", (unsigned long long)last_pad_label.device, last_pad_label.control, last_pad_label.index, last_pad_label.text);
}
// }}}

static PyObject *
send_mouse_event(PyObject *self UNUSED, PyObject *args, PyObject *kw) {
    Screen *screen;
    int x, y, px = 0, py = 0, in_left_half_of_cell = 0;

    int button, action, mods;
    static const char *kwlist[] = {"screen", "cell_x", "cell_y", "button", "action", "mods", "pixel_x", "pixel_y", "in_left_half_of_cell", NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args, kw, "O!iiiii|iip", (char **)kwlist, &Screen_Type, &screen, &x, &y, &button, &action, &mods, &px, &py, &in_left_half_of_cell))
        return NULL;

    MouseTrackingMode mode = screen->modes.mouse_tracking_mode;
    if (mode == ANY_MODE || (mode == MOTION_MODE && action != MOVE) || (mode == BUTTON_MODE && (action == PRESS || action == RELEASE))) {
        MousePosition mpos = {.cell_x = x, .cell_y = y, .global_x = px, .global_y = py, .in_left_half_of_cell = in_left_half_of_cell};
        int sz = encode_mouse_event_impl(&mpos, screen->modes.mouse_tracking_protocol, button, action, mods);
        if (sz > 0) {
            mouse_event_buf[sz] = 0;
            write_escape_code_to_child(screen, ESC_CSI, mouse_event_buf);
            Py_RETURN_TRUE;
        }
    }
    Py_RETURN_FALSE;
}

static PyObject *
test_encode_mouse(PyObject *self UNUSED, PyObject *args) {
    unsigned int x, y;
    int mouse_tracking_protocol, button, action, mods;
    if (!PyArg_ParseTuple(args, "IIiiii", &x, &y, &mouse_tracking_protocol, &button, &action, &mods)) return NULL;
    MousePosition mpos = {.cell_x = x - 1, .cell_y = y - 1};
    int sz = encode_mouse_event_impl(&mpos, mouse_tracking_protocol, button, action, mods);
    return PyUnicode_FromStringAndSize(mouse_event_buf, sz);
}

static PyObject *
mock_mouse_selection(PyObject *self UNUSED, PyObject *args) {
    PyObject *capsule;
    int button, code;
    if (!PyArg_ParseTuple(args, "O!ii", &PyCapsule_Type, &capsule, &button, &code)) return NULL;
    Window *w = PyCapsule_GetPointer(capsule, "Window");
    if (!w) return NULL;
    mouse_selection(w, code, button);
    Py_RETURN_NONE;
}

static PyObject *
send_mock_mouse_event_to_window(PyObject *self UNUSED, PyObject *args) {
    PyObject *capsule;
    int button, modifiers, is_release, clear_clicks, in_left_half_of_cell;
    unsigned int x, y;
    PyObject *pixel_x = Py_None, *pixel_y = Py_None;
    if (!PyArg_ParseTuple(
            args,
            "O!iipIIpp|OO",
            &PyCapsule_Type,
            &capsule,
            &button,
            &modifiers,
            &is_release,
            &x,
            &y,
            &clear_clicks,
            &in_left_half_of_cell,
            &pixel_x,
            &pixel_y))
        return NULL;
    Window *w = PyCapsule_GetPointer(capsule, "Window");
    if (!w) return NULL;
    if (clear_clicks) clear_click_queue(w, button);
    bool mouse_cell_changed = x != w->mouse_pos.cell_x || y != w->mouse_pos.cell_y || w->mouse_pos.in_left_half_of_cell != in_left_half_of_cell;
    // None means derive the pixel position from the cell, so that tests that do
    // not care about sub-cell positions stay terse. Negative values are legal.
    const double px = pixel_x == Py_None ? 10 * x : PyFloat_AsDouble(pixel_x);
    const double py = pixel_y == Py_None ? 20 * y : PyFloat_AsDouble(pixel_y);
    if (PyErr_Occurred()) return NULL;
    w->mouse_pos.global_x = px;
    w->mouse_pos.global_y = py;
    w->mouse_pos.cell_x = x;
    w->mouse_pos.cell_y = y;
    w->mouse_pos.in_left_half_of_cell = in_left_half_of_cell;
    static int last_button_pressed = GLFW_MOUSE_BUTTON_LEFT;
    if (button < 0) {
        if (button == -2) do_drag_scroll(w, true);
        else if (button == -3) do_drag_scroll(w, false);
        else {
            if (OPT(detect_urls)) detect_url(w->render_data.screen, x, y);
            handle_mouse_movement_in_kitty(w, last_button_pressed, mouse_cell_changed);
            handle_potential_drag(w, last_button_pressed);
        }
    } else {
        if (global_state.active_drag_in_window && is_release && button == global_state.active_drag_button) {
            end_drag(w);
        } else {
            clear_potential_drag(w, button, is_release);
            if (!is_release) arm_potential_drag(w, button);
            dispatch_mouse_event(w, button, is_release ? -1 : 1, modifiers, w->render_data.screen->modes.mouse_tracking_mode != NO_TRACKING);
            if (!is_release) {
                last_button_pressed = button;
                add_press(w, button, modifiers);
            }
        }
    }
    Py_RETURN_NONE;
}

static PyObject *
test_scale_scroll(PyObject *self UNUSED, PyObject *args) {
    PyObject *values;
    int cell_size;
    if (!PyArg_ParseTuple(args, "O!i", &PyList_Type, &values, &cell_size)) return NULL;
    double pending = 0;
    int last_dir = 0;
    PyObject *ans = PyList_New(0);
    if (!ans) return NULL;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(values); i++) {
        const double v120 = PyFloat_AsDouble(PyList_GET_ITEM(values, i));
        if (PyErr_Occurred()) {
            Py_DECREF(ans);
            return NULL;
        }
        // ANY_MODE so that wheel_scroll_multiplier is reduced to its sign
        const int s = scale_scroll(ANY_MODE, v120, GLFW_SCROLL_OFFEST_V120, &pending, cell_size, &last_dir);
        PyObject *t = PyLong_FromLong(s);
        if (!t || PyList_Append(ans, t) != 0) {
            Py_XDECREF(t);
            Py_DECREF(ans);
            return NULL;
        }
        Py_DECREF(t);
    }
    return ans;
}

static PyMethodDef module_methods[] = {
    {"send_mouse_event", (PyCFunction)(void (*)(void))(send_mouse_event), METH_VARARGS | METH_KEYWORDS, NULL},
    {"send_pointer_report", (PyCFunction)(void (*)(void))(send_pointer_report), METH_VARARGS | METH_KEYWORDS, NULL},
    {"send_pointer_device", (PyCFunction)(void (*)(void))(send_pointer_device), METH_VARARGS | METH_KEYWORDS, NULL},
    {"send_pointer_action", (PyCFunction)(void (*)(void))(send_pointer_action), METH_VARARGS | METH_KEYWORDS, NULL},
    {"send_pointer_pad", (PyCFunction)(void (*)(void))(send_pointer_pad), METH_VARARGS | METH_KEYWORDS, NULL},
    {"take_pointer_pad_label", (PyCFunction)take_pointer_pad_label, METH_NOARGS, NULL},
    METHODB(test_encode_mouse, METH_VARARGS),
    METHODB(test_scale_scroll, METH_VARARGS),
    METHODB(send_mock_mouse_event_to_window, METH_VARARGS),
    METHODB(mock_mouse_selection, METH_VARARGS),
    {NULL, NULL, 0, NULL} /* Sentinel */
};

bool
init_mouse(PyObject *module) {
    PyModule_AddIntMacro(module, PRESS);
    PyModule_AddIntMacro(module, RELEASE);
    PyModule_AddIntMacro(module, DRAG);
    PyModule_AddIntMacro(module, MOVE);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_DRAG_OR_NORMAL_SELECT);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_NORMAL);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_EXTEND);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_RECTANGLE);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_WORD);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_LINE);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_LINE_FROM_BEGIN);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_LINE_FROM_POINT);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_WORD_AND_LINE_FROM_POINT);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_MOVE_END);
    PyModule_AddIntMacro(module, MOUSE_SELECTION_UPTO_SURROUNDING_WHITESPACE);
    if (PyModule_AddFunctions(module, module_methods) != 0) return false;
    return true;
}

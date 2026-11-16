/*
 * linux_session_monitor.c
 * Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

// Monitors the state of the desktop session using the Inhibit desktop portal, so that the application can save its state
// before the user is logged out. When the session is about to end, the portal changes the state to Query End and the
// application must acknowledge it within a second. The callback is run before acknowledging, so that the application can save
// its state before the session proceeds to the Ending state.
//
// Creating the monitor is a portal request: the CreateMonitor method returns a request object and the result is reported later by
// the Response signal of that object. Signals are accepted only from the portal that replied to CreateMonitor and only for the
// monitor session created by it, so that other clients on the session bus cannot fake a session ending.
//
// The monitor session ends if the portal exits, for example, because it crashed, so the monitor is created again when that happens.
// This is done only if the monitor was created successfully, so that a portal that crashes when creating it is not restarted in a
// loop by DBUS activation.

#include "linux_session_monitor.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORTAL_SERVICE "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define INHIBIT_INTERFACE "org.freedesktop.portal.Inhibit"
#define REQUEST_INTERFACE "org.freedesktop.portal.Request"
// Object paths include the unique name of the connection, so a fixed token is unique, as there is only one monitor per connection
#define HANDLE_TOKEN "glfw_session_monitor"

static GLFWDBussessionstatefun session_state_callback = NULL;
static uint32_t current_session_state = GLFW_SESSION_STATE_RUNNING;
static bool monitoring = false;
// Whether the portal reported that the monitor was created
static bool established = false;
// The unique name of the portal that replied to CreateMonitor, NULL until it replies. These strings are allocated to fit what the
// portal sends, as object paths have no maximum length.
static char *portal_name = NULL;
static char *request_path = NULL, *session_path = NULL;

static bool
streq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static bool
set_string(char **dest, const char *src) {
    // Replaces *dest with a copy of src, leaving *dest unchanged if there is not enough memory.
    // Not strdup(), which strict C11 does not declare.
    const size_t sz = strlen(src) + 1;
    char *copy = malloc(sz);
    if (!copy) return false;
    memcpy(copy, src, sz);
    free(*dest);
    *dest = copy;
    return true;
}

static char *
join3(const char *a, const char *b, const char *c) {
    // Returns a newly allocated a + b + c, or NULL if there is not enough memory
    const size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    if (lb > SIZE_MAX - 1 - la || lc > SIZE_MAX - 1 - la - lb) return NULL;
    char *ans = malloc(la + lb + lc + 1);
    if (!ans) return NULL;
    memcpy(ans, a, la);
    memcpy(ans + la, b, lb);
    memcpy(ans + la + lb, c, lc + 1);
    return ans;
}

static char *
portal_object_path(DBusConnection *conn, const char *prefix) {
    // The portal creates objects at PORTAL_PATH/kind/SENDER/TOKEN where SENDER is the unique name of the connection without
    // the leading colon and with dots replaced by underscores. prefix is PORTAL_PATH/kind/. Returns a newly allocated path, or NULL.
    const char *unique_name = dbus_bus_get_unique_name(conn);
    if (!unique_name || unique_name[0] != ':') return NULL;
    char *ans = join3(prefix, unique_name + 1, "/" HANDLE_TOKEN);
    if (!ans) return NULL;
    for (char *p = ans + strlen(prefix); *p && *p != '/'; p++) {
        if (*p == '.') *p = '_';
    }
    return ans;
}

static bool
set_portal_object_path(DBusConnection *conn, const char *prefix, char **dest) {
    char *path = portal_object_path(conn, prefix);
    if (!path) return false;
    free(*dest);
    *dest = path;
    return true;
}

static bool
add_response_match(DBusConnection *conn) {
    char *rule = join3("type='signal',interface='" REQUEST_INTERFACE "',member='Response',path='", request_path, "'");
    if (!rule) return false;
    dbus_bus_add_match(conn, rule, NULL);
    free(rule);
    return true;
}

static bool
is_from_portal(DBusMessage *msg) {
    return streq(dbus_message_get_sender(msg), portal_name);
}

static bool
read_session_state(DBusMessageIter *dict, uint32_t *state) {
    DBusMessageIter entry, value;
    while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
        dbus_message_iter_recurse(dict, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            const char *key;
            dbus_message_iter_get_basic(&entry, &key);
            if (strcmp(key, "session-state") == 0 && dbus_message_iter_next(&entry) && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&entry, &value);
                if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_UINT32) return false;
                dbus_message_iter_get_basic(&value, state);
                return true;
            }
        }
        if (!dbus_message_iter_next(dict)) break;
    }
    // The state dictionary does not have a valid session state
    return false;
}

static void
on_state_changed(DBusConnection *conn, DBusMessage *msg) {
    DBusMessageIter iter, dict;
    const char *session_handle = NULL;
    if (!dbus_message_iter_init(msg, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: StateChanged signal does not have a session handle");
        return;
    }
    dbus_message_iter_get_basic(&iter, &session_handle);
    if (!streq(session_handle, session_path)) return;
    if (!dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: StateChanged signal does not have a state dictionary");
        return;
    }
    dbus_message_iter_recurse(&iter, &dict);
    uint32_t state;
    if (!read_session_state(&dict, &state)) return;
    // The signal is also sent when only the screensaver state changes
    const bool changed = state != current_session_state;
    current_session_state = state;
    if (changed && session_state_callback) session_state_callback(state);
    if (state == GLFW_SESSION_STATE_QUERY_END) {
        if (!glfw_dbus_call_method_no_reply(
                conn, PORTAL_SERVICE, PORTAL_PATH, INHIBIT_INTERFACE, "QueryEndResponse", DBUS_TYPE_OBJECT_PATH, &session_handle, DBUS_TYPE_INVALID))
            _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: Failed to respond to the session ending");
    }
}

static void
on_monitor_response(DBusMessage *msg) {
    DBusMessageIter iter;
    uint32_t response;
    if (!dbus_message_iter_init(msg, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32) {
        monitoring = false;
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: The response to CreateMonitor does not have a response code");
        return;
    }
    dbus_message_iter_get_basic(&iter, &response);
    if (response != 0) {
        monitoring = false;
        _glfwInputError(
            GLFW_PLATFORM_ERROR,
            "Inhibit portal: Failed to monitor the session state, make sure your desktop portal supports version 3 of the Inhibit "
            "portal. Response code: %u",
            response);
        return;
    }
    established = true;
    // Use the session handle from the results, in case the portal ignored session_handle_token
    DBusMessageIter results, entry, value;
    if (!dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&iter, &results);
    for (; dbus_message_iter_get_arg_type(&results) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&results)) {
        const char *key, *handle;
        dbus_message_iter_recurse(&results, &entry);
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING) continue;
        dbus_message_iter_get_basic(&entry, &key);
        if (strcmp(key, "session_handle") != 0 || !dbus_message_iter_next(&entry) || dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) continue;
        dbus_message_iter_recurse(&entry, &value);
        const int type = dbus_message_iter_get_arg_type(&value);
        if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH) continue;
        dbus_message_iter_get_basic(&value, &handle);
        // Keeps the expected session path if there is not enough memory, which is correct unless the portal ignored the token
        if (!set_string(&session_path, handle)) _glfwInputError(GLFW_OUT_OF_MEMORY, "Inhibit portal: Not enough memory to store the session handle");
        break;
    }
}

static bool start_monitoring(DBusConnection *session_bus);

static void
on_name_owner_changed(DBusConnection *conn, DBusMessage *msg) {
    const char *name, *old_owner, *new_owner;
    if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner, DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID)) return;
    if (strcmp(name, PORTAL_SERVICE) != 0 || !streq(old_owner, portal_name)) return;
    // The portal that created the monitor has exited, which ends the monitor session
    const bool restart = monitoring && established;
    monitoring = false;
    established = false;
    free(portal_name);
    portal_name = NULL;
    if (restart) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: The desktop portal exited, monitoring the session state again");
        start_monitoring(conn);
    }
}

static DBusHandlerResult
message_filter(DBusConnection *conn, DBusMessage *msg, void *user_data UNUSED) {
    if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged") && dbus_message_has_sender(msg, DBUS_SERVICE_DBUS)) {
        on_name_owner_changed(conn, msg);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (!monitoring || !is_from_portal(msg)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (dbus_message_is_signal(msg, INHIBIT_INTERFACE, "StateChanged")) on_state_changed(conn, msg);
    else if (dbus_message_is_signal(msg, REQUEST_INTERFACE, "Response") && request_path && dbus_message_has_path(msg, request_path)) on_monitor_response(msg);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
monitor_created(DBusMessage *msg, const DBusError *err, void *data UNUSED) {
    if (err) {
        monitoring = false;
        _glfwInputError(
            GLFW_PLATFORM_ERROR,
            "Inhibit portal: Failed to monitor the session state, make sure you have the desktop portal running. Error: %s: %s",
            err->name,
            err->message);
        return;
    }
    const char *sender = dbus_message_get_sender(msg), *handle = NULL;
    if (!sender || !dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &handle, DBUS_TYPE_INVALID)) {
        monitoring = false;
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: Invalid reply to CreateMonitor");
        return;
    }
    if (!set_string(&portal_name, sender)) {
        monitoring = false;
        _glfwInputError(GLFW_OUT_OF_MEMORY, "Inhibit portal: Not enough memory to store the name of the portal");
        return;
    }
    if (!streq(handle, request_path)) {
        // Portals that ignore handle_token use a different request path, its Response signal could be missed if it was sent before
        // the match below is added
        if (!set_string(&request_path, handle)) {
            monitoring = false;
            _glfwInputError(GLFW_OUT_OF_MEMORY, "Inhibit portal: Not enough memory to store the request path returned by CreateMonitor");
            return;
        }
        if (!add_response_match(glfw_dbus_session_bus())) {
            monitoring = false;
            _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: Failed to watch for the response to CreateMonitor");
        }
    }
}

static bool
append_string_option(DBusMessageIter *options, const char *key, const char *value) {
    DBusMessageIter entry, variant;
    if (!dbus_message_iter_open_container(options, DBUS_TYPE_DICT_ENTRY, NULL, &entry)) return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key)) return false;
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant)) return false;
    if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value)) return false;
    if (!dbus_message_iter_close_container(&entry, &variant)) return false;
    return dbus_message_iter_close_container(options, &entry);
}

static bool
create_monitor(DBusConnection *session_bus) {
    RAII_MSG(msg, dbus_message_new_method_call(PORTAL_SERVICE, PORTAL_PATH, INHIBIT_INTERFACE, "CreateMonitor"));
    if (!msg) return false;
    DBusMessageIter iter, options;
    const char *window = "";
    dbus_message_iter_init_append(msg, &iter);
    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &window)) return false;
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options)) return false;
    // Some versions of xdg-desktop-portal abort if the session handle token is missing
    if (!append_string_option(&options, "handle_token", HANDLE_TOKEN)) return false;
    if (!append_string_option(&options, "session_handle_token", HANDLE_TOKEN)) return false;
    if (!dbus_message_iter_close_container(&iter, &options)) return false;
    return call_method_with_msg(session_bus, msg, DBUS_TIMEOUT_USE_DEFAULT, monitor_created, NULL, false);
}

static bool
start_monitoring(DBusConnection *session_bus) {
    static bool filter_added = false;
    if (!set_portal_object_path(session_bus, PORTAL_PATH "/request/", &request_path) ||
        !set_portal_object_path(session_bus, PORTAL_PATH "/session/", &session_path)) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: Failed to get the unique name of the DBUS connection or out of memory");
        return false;
    }
    if (!filter_added) {
        // Added before calling CreateMonitor, so that the Response signal cannot be missed
        if (!add_response_match(session_bus)) return false;
        dbus_bus_add_match(session_bus, "type='signal',interface='" INHIBIT_INTERFACE "',member='StateChanged'", NULL);
        dbus_bus_add_match(
            session_bus,
            "type='signal',sender='" DBUS_SERVICE_DBUS "',interface='" DBUS_INTERFACE_DBUS "',member='NameOwnerChanged',arg0='" PORTAL_SERVICE "'",
            NULL);
        if (!dbus_connection_add_filter(session_bus, message_filter, NULL, NULL)) {
            _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: Failed to add a DBUS message filter");
            return false;
        }
        filter_added = true;
    }
    free(portal_name);
    portal_name = NULL;
    established = false;
    // Reset without running the callback, so that state the application saved for a logout that was in progress when the portal
    // exited is not discarded
    current_session_state = GLFW_SESSION_STATE_RUNNING;
    monitoring = create_monitor(session_bus);
    if (!monitoring) _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: Failed to call CreateMonitor");
    return monitoring;
}

bool
glfw_dbus_monitor_session_state(GLFWDBussessionstatefun callback) {
    session_state_callback = callback;
    if (monitoring) return true;
    DBusConnection *session_bus = glfw_dbus_session_bus();
    if (!session_bus) return false;
    return start_monitoring(session_bus);
}

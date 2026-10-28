/*
 * osc777.c
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#include "screen.h"
#include "control-codes.h"
#include "extension-fields.h"

// Every OSC 777 selector this terminal speaks, with the version of its
// protocol, see docs/capabilities-protocol.rst. A selector handled in
// dispatch_osc() must also be listed here.
static const struct { const char *selector; unsigned version; } extensions[] = {
    {"caps", 1},
};
// The graphics protocol transmission media (the t key) and pixel formats (the
// f key) this terminal accepts. A branch that adds one adds it here too.
static const char *const media[] = {"d", "f", "t", "s"};
static const char *const formats[] = {"24", "32", "100"};

static int
append_list(char *reply, size_t cap, int n, const char *key, const char *const *items, size_t count) {
    if (n < (int)cap) n += snprintf(reply + n, cap - n, ":%s=", key);
    for (size_t i = 0; i < count && n < (int)cap; i++) n += snprintf(reply + n, cap - n, "%s%s", i ? "," : "", items[i]);
    return n;
}

// Turns off every extension mode of the window, as if each selector had been
// sent mode=0, with the cancels that sends. It runs on the caps reset request
// and at every shell prompt, so it must cost nothing when the modes are off.
// An extension that has a mode adds its lines here.
void
screen_reset_extension_modes(Screen *self UNUSED) {
}

// A frame from the program, see docs/extension-framing.rst for what is rejected
void
screen_caps_query(Screen *self, const char *data, size_t sz) {
    EXTENSION_FIELDS(f);
    char reply[512];
    const char *op;
    long long id = -1;
    if (!extension_fields_parse(&f, data, sz) || !extension_frame_header(&f, 1, &op)) return;
    if (!extension_field_int(&f, "id", 0, INT32_MAX, &id)) return;
    if (!strcmp(op, "reset")) {
        screen_reset_extension_modes(self);
        return;
    }
    if (strcmp(op, "query") || id < 0) return;
    // caps: the listing 1, the reset request 2
    int n = snprintf(reply, sizeof(reply), "777;caps;v=1:op=cap:id=%lld:caps=3:ext=", id);
    for (size_t i = 0; i < arraysz(extensions) && n < (int)sizeof(reply); i++)
        n += snprintf(reply + n, sizeof(reply) - n, "%s%s.%u", i ? "," : "", extensions[i].selector, extensions[i].version);
    n = append_list(reply, sizeof(reply), n, "media", media, arraysz(media));
    append_list(reply, sizeof(reply), n, "format", formats, arraysz(formats));
    write_escape_code_to_child(self, ESC_OSC, reply);
}

/*
 * monitor_info.h
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What a monitor's EDID says, see parse_edid()
typedef struct EdidInfo {
    // The manufacturer ID, three letters A to Z: its size is fixed by the EDID format, which packs it in 15 bits.
    // Empty when the bits do not spell three letters.
    char mfg[4];
    unsigned product;        // the product code
    uint32_t serial_number;  // the serial number field, 0 when not used
    int year;                // of manufacture, or of the model when model_year is set, 0 when not given
    bool model_year;
    unsigned version, revision;
    // The display descriptors, printable ASCII without the padding, NULL when absent: the name, and the text of the
    // serial descriptor, which some monitors fill with their model rather than a serial number. That is used only to
    // recognize a model, never stored or reported, as serial numbers say whose monitor it is.
    char *name, *serial;
    // The EDID says the display shows only shades of gray, which it can say only for an analog display or before
    // EDID 1.4. Digital displays of 1.4 and later state their color encodings instead.
    bool grayscale;
} EdidInfo;

bool parse_edid(const uint8_t *edid, size_t size, EdidInfo *out);
void free_edid_info(EdidInfo *info);

// A monitor as Silkie knows it. Interned, so two windows on one monitor share one of these, and a change of monitor
// is a change of pointer. Never freed, as DisplayInfo of any window can point to it: there is one for each monitor
// seen while Silkie runs, and one for each change of what the system says about one.
typedef struct MonitorIdentity {
    // Printable UTF-8, NULL when not known. make and model are those of the system, else from the EDID: the
    // manufacturer ID and the name descriptor. No serial number is kept.
    char *connector, *make, *model, *description;
    char mfg[4];   // the EDID manufacturer ID, empty when not known
    int product;   // the EDID product code, -1 when not known
    bool edid_grayscale;
    // Where the EDID came from: "system" (X11), "sysfs" (Linux, for a Wayland output), "" without one
    const char *edid_source;
    // The entry of the built-in table of e-paper monitors this is, NULL when none, see monitor_info.c
    const struct KnownMonitor *known;
    // What the system said, which is how an identity is found again without reading an EDID
    struct {
        char *connector, *make, *model, *description;
        uint8_t *edid;
        size_t edid_size;
        unsigned vendor_id, product_id;
        bool has_ids;
    } raw;
} MonitorIdentity;

typedef struct KnownMonitor {
    const char *name;           // for people
    const char *mfg;            // EDID manufacturer ID
    int product;                // EDID product code, -1 to match on the strings only
    const char *const *strings; // name or serial descriptors that identify the model, NULL terminated
    int kind;                   // a GLFWdisplaykind
    int color;                  // a GLFWdisplaycolor, -1 when not known
    int gray_bits;              // 0 when not known
} KnownMonitor;

struct GLFWmonitoridentity;
const MonitorIdentity *intern_monitor_identity(const struct GLFWmonitoridentity *g);
// Whether a monitor_info match string selects the monitor, see docs
bool monitor_matches(const MonitorIdentity *m, const char *match);

// For tests: an identity to use for every window instead of what the system says, see set_test_monitor_identity()
bool test_monitor_identity(struct GLFWmonitoridentity *g, bool *has_identity);
#ifdef Py_PYTHON_H
PyObject *monitor_identity_as_dict(const MonitorIdentity *m);
#endif

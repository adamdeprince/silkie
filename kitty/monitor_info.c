/*
 * monitor_info.c
 * Distributed under terms of the GPL3 license.
 *
 * Which monitor a window is on: what the system says about it, its EDID, the
 * built-in table of e-paper monitors and the monitor_info option, see
 * docs/display-protocol.rst.
 */

#include "state.h"
#include "monitor_info.h"
#include "glfw-wrapper.h"
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

// EDID {{{
// Layout from VESA E-EDID Release A, Revision 2 (EDID 1.4), which 1.3 shares for everything read here

#define EDID_BLOCK_SIZE 128  // the base block, fixed by the format
#define EDID_DESCRIPTOR_SIZE 18
#define EDID_DESCRIPTOR_TEXT_SIZE 13  // the text of a display descriptor, bytes 5 to 17

// The text of a display descriptor: ASCII ended by a line feed and padded with spaces. Only printable characters are
// kept, so the text is safe to show and to send to programs.
static char *
descriptor_text(const uint8_t *d) {
    char *ans = malloc(EDID_DESCRIPTOR_TEXT_SIZE + 1);
    if (!ans) return NULL;
    size_t n = 0;
    for (size_t i = 5; i < EDID_DESCRIPTOR_SIZE && d[i] != '\n'; i++) {
        if (d[i] >= 0x20 && d[i] < 0x7f) ans[n++] = (char)d[i];
    }
    while (n && ans[n - 1] == ' ') n--;
    ans[n] = 0;
    if (!n) {
        free(ans);
        return NULL;
    }
    return ans;
}

void
free_edid_info(EdidInfo *info) {
    free(info->name);
    free(info->serial);
    info->name = info->serial = NULL;
}

bool
parse_edid(const uint8_t *e, size_t size, EdidInfo *out) {
    static const uint8_t header[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    memset(out, 0, sizeof(*out));
    if (!e || size < EDID_BLOCK_SIZE || memcmp(e, header, sizeof(header)) != 0) return false;
    // Three letters of five bits each, 1 for A, big endian
    const unsigned id = (unsigned)e[8] << 8 | e[9];
    const unsigned letters[3] = {(id >> 10) & 31, (id >> 5) & 31, id & 31};
    if (!(id & 0x8000) && letters[0] >= 1 && letters[0] <= 26 && letters[1] >= 1 && letters[1] <= 26 && letters[2] >= 1 && letters[2] <= 26) {
        for (int i = 0; i < 3; i++) out->mfg[i] = (char)('A' + letters[i] - 1);
    }
    out->product = (unsigned)e[10] | (unsigned)e[11] << 8;
    out->serial_number = (uint32_t)e[12] | (uint32_t)e[13] << 8 | (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;
    // The week 0xff says the year is that of the model
    out->model_year = e[16] == 0xff;
    if (e[17]) out->year = 1990 + e[17];
    out->version = e[18];
    out->revision = e[19];
    // Bits 4 and 3 of byte 24 are the color type of an analog display, and of any display before EDID 1.4:
    // 00 says monochrome or grayscale
    const bool digital = e[20] & 0x80, v14 = out->version > 1 || (out->version == 1 && out->revision >= 4);
    if ((!digital || !v14) && ((e[24] >> 3) & 3) == 0) out->grayscale = true;
    for (size_t i = 0; i < 4; i++) {
        const uint8_t *d = e + 54 + i * EDID_DESCRIPTOR_SIZE;
        // A display descriptor rather than a detailed timing starts with a zero pixel clock
        if (d[0] || d[1] || d[2]) continue;
        char **dest = d[3] == 0xfc ? &out->name : d[3] == 0xff ? &out->serial : NULL;
        if (dest && !*dest) *dest = descriptor_text(d);
    }
    return true;
}
// }}}

// Strings from the system {{{

// A copy without control characters, NULL for NULL, an empty string or no memory
static char *
clean_string(const char *s) {
    if (!s || !*s) return NULL;
    const size_t len = strlen(s);
    char *ans = malloc(len + 1);
    if (!ans) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != 0x7f) ans[n++] = (char)c;
    }
    while (n && ans[n - 1] == ' ') n--;
    ans[n] = 0;
    if (!n) {
        free(ans);
        return NULL;
    }
    return ans;
}

static bool
same_string(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

static bool
same_string_ci(const char *a, const char *b) {
    return a && b && strcasecmp(a, b) == 0;
}

static bool
contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return false;
    const size_t n = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, n) == 0) return true;
    }
    return false;
}

static char *
dup_or_null(const char *s, bool *failed) {
    if (!s) return NULL;
    char *ans = strdup(s);
    if (!ans) *failed = true;
    return ans;
}
// }}}

// The EDID of a Wayland output, from sysfs {{{
// No Wayland protocol gives the EDID. A compositor that drives the hardware itself names outputs after the kernel's
// connectors, card1-DP-7 being DP-7, and the kernel lets anyone read their EDID. That is used only when exactly one
// connector has the name and its EDID agrees with what the compositor said, so that a nested or remote compositor,
// whose outputs merely have similar names, never gets the EDID of a monitor it is not on.

static char *sysfs_root = NULL;  // for tests, NULL for /sys/class/drm

static uint8_t *
read_whole_file(const char *path, size_t *size) {
    *size = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    // sysfs reports a size of zero for these files, so read until the end
    size_t capacity = 4096, used = 0;
    uint8_t *buf = malloc(capacity);
    while (buf) {
        if (used == capacity) {
            if (capacity > SIZE_MAX / 2) {
                free(buf);
                buf = NULL;
                break;
            }
            uint8_t *bigger = realloc(buf, capacity * 2);
            if (!bigger) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = bigger;
            capacity *= 2;
        }
        const ssize_t n = read(fd, buf + used, capacity - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            buf = NULL;
            break;
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    close(fd);
    if (buf) *size = used;
    return buf;
}


static bool
edid_agrees_with_system(const EdidInfo *info, const char *make, const char *model) {
    // The model is the name descriptor in mutter and wlroots, or the product code when there is none
    if (model) {
        if (same_string(info->name, model)) return true;
        char code[16];
        snprintf(code, sizeof(code), "0x%04X", info->product);
        return strcasecmp(code, model) == 0 && make && info->mfg[0] && strcmp(make, info->mfg) == 0;
    }
    return false;
}

static uint8_t *
sysfs_edid_for_connector(const char *connector, const char *make, const char *model, size_t *size) {
    *size = 0;
    if (!connector || !*connector || strchr(connector, '/') || !model) return NULL;
    const char *root = sysfs_root ? sysfs_root : "/sys/class/drm";
    DIR *dir = opendir(root);
    if (!dir) return NULL;
    const size_t clen = strlen(connector);
    char *found = NULL;
    bool ambiguous = false;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        // card<N>-<connector>
        const char *n = ent->d_name;
        if (strncmp(n, "card", 4) != 0) continue;
        const char *p = n + 4;
        if (!isdigit((unsigned char)*p)) continue;
        while (isdigit((unsigned char)*p)) p++;
        if (*p != '-' || strlen(p + 1) != clen || strcmp(p + 1, connector) != 0) continue;
        if (found) {
            ambiguous = true;
            break;
        }
        found = strdup(n);
        if (!found) break;
    }
    closedir(dir);
    if (!found || ambiguous) {
        free(found);
        return NULL;
    }
    const size_t plen = strlen(root) + strlen(found) + sizeof("//edid");
    RAII_ALLOC(char, path, malloc(plen));
    if (!path) {
        free(found);
        return NULL;
    }
    snprintf(path, plen, "%s/%s/edid", root, found);
    free(found);
    size_t n;
    uint8_t *edid = read_whole_file(path, &n);
    EdidInfo info;
    if (!edid || !parse_edid(edid, n, &info)) {
        free(edid);
        return NULL;
    }
    const bool agrees = edid_agrees_with_system(&info, make, model);
    free_edid_info(&info);
    if (!agrees) {
        free(edid);
        return NULL;
    }
    *size = n;
    return edid;
}
// }}}

// The built-in table of e-paper monitors {{{
// An entry matches a monitor with its EDID manufacturer ID and either its product code or one of its strings, the name
// or serial descriptor, or the model the system gives. The serial descriptor is looked at only here, for monitors
// that put their model in it, and is not kept. Where the manufacturer ID is not known, as from a compositor
// that gives a name for the manufacturer rather than its ID, the strings alone. Each entry says where its IDs and
// properties come from; none is guessed. kind and color are independent: there is color e-paper.

static const char *const boox_mira_pro_strings[] = {"MIRA253", "BOOX MIRA253", NULL}, *const boox_mira_strings[] = {"Mira133", NULL},
                         *const paperlike_hd_strings[] = {"Paperlike H D", NULL}, *const paperlike_253_strings[] = {"DASUNG", "Paperlike253", NULL},
                         *const paperlike_13k_strings[] = {"Paperlike13K", NULL}, *const glider_strings[] = {"Paper Monitor", NULL};

// A product code of -1 matches on the strings alone, for a manufacturer ID that other products share: ONX is
// registered to another company, and Dasung's DSC is on an LCD too, and DSC 0x0001 on a monitor named ChuangKe.
// Where an EDID has two name descriptors only the first is the model, as wlroots gives it, so both are listed.
static const KnownMonitor known_monitors[] = {
    // ONYX BOOX Mira Pro, 25.3 inch. IDs: the EDID of one, kitty_tests/edid/boox-mira-pro.edid, the same as
    // https://github.com/linuxhw/EDID/blob/master/Digital/OBX/OBX0011/62D7FF99FC04 (manufacturer OBX, product 0x0011,
    // name descriptor MIRA253, serial descriptor BOOX MIRA253). The EDID claims an RGB color display. Sixteen levels of
    // gray: "16 Grey Level (monochrome)" at https://shop.boox.com/products/boox-mira, and confirmed by its owner.
    // The color version, with a Kaleido panel, has no EDID published.
    {"ONYX BOOX Mira Pro", "OBX", 0x0011, boox_mira_pro_strings, GLFW_DISPLAY_KIND_EINK, GLFW_DISPLAY_COLOR_GRAY, 4},
    // ONYX BOOX Mira, 13.3 inch. IDs: https://github.com/linuxhw/EDID/blob/master/Digital/ONX/ONX0001/FC153BC3D9F6
    // (ONX, 0x0001, name descriptor Mira133). Sixteen levels of gray: "16 Grey Level (monochrome)" in
    // https://blog.the-ebook-reader.com/2021/09/30/onyx-releasing-boox-mira-series-e-ink-monitors/
    {"ONYX BOOX Mira", "ONX", -1, boox_mira_strings, GLFW_DISPLAY_KIND_EINK, GLFW_DISPLAY_COLOR_GRAY, 4},
    // DASUNG Paperlike HD, HD-F and HD-FT, 13.3 inch, grayscale. IDs:
    // https://github.com/linuxhw/EDID/blob/master/Digital/Others/DSC0001/CB44944F2225 (DSC, 0x0001, Paperlike H D).
    // The number of levels is not published by Dasung.
    {"DASUNG Paperlike HD", "DSC", -1, paperlike_hd_strings, GLFW_DISPLAY_KIND_EINK, GLFW_DISPLAY_COLOR_GRAY, 0},
    // DASUNG Paperlike 253, 25.3 inch, grayscale. IDs: https://github.com/linuxhw/EDID/blob/master/Digital/Others/DSC0000/C9EA7BCF5E7C
    // (DSC, 0x0000, name descriptors DASUNG and Paperlike253). The number of levels is not published by Dasung.
    {"DASUNG Paperlike 253", "DSC", -1, paperlike_253_strings, GLFW_DISPLAY_KIND_EINK, GLFW_DISPLAY_COLOR_GRAY, 0},
    // DASUNG Paperlike 13K. IDs: https://github.com/linuxhw/EDID/blob/master/Digital/Others/DSC133C/5F768B7324B1
    // (DSC, 0x133C, Paperlike13K). It is sold with a grayscale and with a Kaleido color panel, which the EDID does not
    // tell apart, so its color is not known.
    {"DASUNG Paperlike 13K", "DSC", -1, paperlike_13k_strings, GLFW_DISPLAY_KIND_EINK, -1, 0},
    // Modos Glider, the open e-paper monitor. IDs: its firmware, https://github.com/Modos-Labs/Glider/blob/
    // 9f8d46bea7f02c2efe97f73c71ce49e5e9a5db8b/fw/User/edid.c (ZPR, 0x0001, Paper Monitor). It drives grayscale and
    // color panels, so its color is not known.
    {"Modos Glider", "ZPR", 0x0001, glider_strings, GLFW_DISPLAY_KIND_EINK, -1, 0},
};

static bool
known_monitor_matches(const KnownMonitor *k, const MonitorIdentity *m, const char *serial_text) {
    bool strings = false;
    for (const char *const *s = k->strings; s && *s && !strings; s++) strings = same_string_ci(*s, m->model) || same_string_ci(*s, serial_text);
    if (m->mfg[0]) return strcmp(m->mfg, k->mfg) == 0 && ((k->product >= 0 && m->product == k->product) || strings);
    // A make of three capital letters is the manufacturer ID, as mutter and wlroots give it for a manufacturer they have
    // no name for, and then it has to agree
    if (m->make && strlen(m->make) == 3 && isupper((unsigned char)m->make[0]) && isupper((unsigned char)m->make[1]) &&
        isupper((unsigned char)m->make[2]) && strcmp(m->make, k->mfg) != 0)
        return false;
    return strings;
}

static const KnownMonitor *
known_monitor_for(const MonitorIdentity *m, const char *serial_text) {
    for (size_t i = 0; i < arraysz(known_monitors); i++) {
        if (known_monitor_matches(known_monitors + i, m, serial_text)) return known_monitors + i;
    }
    return NULL;
}
// }}}

// Interning {{{

static struct {
    MonitorIdentity **items;
    size_t count, capacity;
} identities = {0};

static bool
same_raw(const MonitorIdentity *m, const GLFWmonitoridentity *g) {
    return same_string(m->raw.connector, g->connector) && same_string(m->raw.make, g->make) && same_string(m->raw.model, g->model) &&
           same_string(m->raw.description, g->description) && m->raw.has_ids == g->has_ids && m->raw.vendor_id == g->vendor_id &&
           m->raw.product_id == g->product_id && m->raw.edid_size == (g->edid ? g->edid_size : 0) &&
           (!m->raw.edid_size || memcmp(m->raw.edid, g->edid, m->raw.edid_size) == 0);
}

static void
free_identity(MonitorIdentity *m) {
    if (!m) return;
    free(m->connector); free(m->make); free(m->model); free(m->description);
    free(m->raw.connector); free(m->raw.make); free(m->raw.model); free(m->raw.description); free(m->raw.edid);
    free(m);
}

static MonitorIdentity *
build_identity(const GLFWmonitoridentity *g) {
    MonitorIdentity *m = calloc(1, sizeof(MonitorIdentity));
    if (!m) return NULL;
    bool failed = false;
    m->product = -1;
    m->edid_source = "";
    m->raw.connector = dup_or_null(g->connector, &failed);
    m->raw.make = dup_or_null(g->make, &failed);
    m->raw.model = dup_or_null(g->model, &failed);
    m->raw.description = dup_or_null(g->description, &failed);
    m->raw.has_ids = g->has_ids;
    m->raw.vendor_id = g->vendor_id;
    m->raw.product_id = g->product_id;
    if (g->edid && g->edid_size) {
        if ((m->raw.edid = malloc(g->edid_size))) {
            memcpy(m->raw.edid, g->edid, g->edid_size);
            m->raw.edid_size = g->edid_size;
        } else failed = true;
    }
#define CLEAN(dest, src) if (src && !(dest = clean_string(src)) && *src) failed = true;
    CLEAN(m->connector, g->connector);
    CLEAN(m->make, g->make);
    CLEAN(m->model, g->model);
    CLEAN(m->description, g->description);
#undef CLEAN
    if (failed) {
        free_identity(m);
        return NULL;
    }
    const uint8_t *edid = m->raw.edid;
    size_t edid_size = m->raw.edid_size;
    RAII_ALLOC(uint8_t, sysfs_edid, NULL);
    if (edid) m->edid_source = "system";
    else if ((global_state.is_wayland || sysfs_root) && !g->has_ids) {
        sysfs_edid = sysfs_edid_for_connector(m->connector, m->make, m->model, &edid_size);
        if (sysfs_edid) {
            edid = sysfs_edid;
            m->edid_source = "sysfs";
        }
    }
    EdidInfo info;
    RAII_ALLOC(char, serial_text, NULL);
    if (edid && parse_edid(edid, edid_size, &info)) {
        memcpy(m->mfg, info.mfg, sizeof(m->mfg));
        m->product = (int)info.product;
        m->edid_grayscale = info.grayscale;
        // What the system says comes first, the EDID fills in what it does not
        if (!m->make && info.mfg[0] && !(m->make = strdup(info.mfg))) failed = true;
        if (!m->model && info.name) {
            m->model = info.name;
            info.name = NULL;
        }
        // Only to recognize the model, see known_monitor_matches()
        serial_text = info.serial;
        info.serial = NULL;
        free_edid_info(&info);
    } else if (g->has_ids && g->vendor_id <= 0xffff && g->product_id <= 0xffff) {
        // macOS: the same 16 bits as EDID bytes 8 and 9
        uint8_t fake[EDID_BLOCK_SIZE] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
        fake[8] = (uint8_t)(g->vendor_id >> 8);
        fake[9] = (uint8_t)(g->vendor_id & 0xff);
        if (parse_edid(fake, sizeof(fake), &info)) {
            memcpy(m->mfg, info.mfg, sizeof(m->mfg));
            free_edid_info(&info);
        }
        if (m->mfg[0]) m->product = (int)g->product_id;
    }
    if (failed) {
        free_identity(m);
        return NULL;
    }
    m->known = known_monitor_for(m, serial_text);
    return m;
}

const MonitorIdentity *
intern_monitor_identity(const GLFWmonitoridentity *g) {
    for (size_t i = 0; i < identities.count; i++) {
        if (same_raw(identities.items[i], g)) return identities.items[i];
    }
    MonitorIdentity *m = build_identity(g);
    if (!m) {
        log_error("Out of memory recording the identity of a monitor");
        return NULL;
    }
    // A monitor seen before under another raw description, as when a compositor names it in a second step, is still
    // its own entry: the raw description is what is looked up. Distinct descriptions are few.
    if (identities.count >= identities.capacity) {
        const size_t cap = identities.capacity ? identities.capacity * 2 : 8;
        if (cap < identities.capacity || cap > SIZE_MAX / sizeof(identities.items[0])) {
            free_identity(m);
            return NULL;
        }
        MonitorIdentity **items = realloc(identities.items, cap * sizeof(identities.items[0]));
        if (!items) {
            log_error("Out of memory recording the identity of a monitor");
            free_identity(m);
            return NULL;
        }
        identities.items = items;
        identities.capacity = cap;
    }
    identities.items[identities.count++] = m;
    return m;
}
// }}}

// monitor_info matching {{{

// MFG:PRODUCT, MFG being three letters and PRODUCT a number, decimal or hex with 0x, or * for any
static bool
parse_id_match(const char *match, char mfg[4], int *product) {
    if (strlen(match) < 5 || match[3] != ':') return false;
    for (int i = 0; i < 3; i++) {
        if (!isalpha((unsigned char)match[i])) return false;
        mfg[i] = (char)toupper((unsigned char)match[i]);
    }
    mfg[3] = 0;
    const char *p = match + 4;
    if (strcmp(p, "*") == 0) {
        *product = -1;
        return true;
    }
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    if (!*p) return false;
    unsigned long v = 0;
    for (; *p; p++) {
        int digit;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (base == 16 && isxdigit((unsigned char)*p)) digit = tolower((unsigned char)*p) - 'a' + 10;
        else return false;
        v = v * (unsigned)base + (unsigned)digit;
        if (v > 0xffff) return false;  // product codes are 16 bits
    }
    *product = (int)v;
    return true;
}

bool
monitor_matches(const MonitorIdentity *m, const char *match) {
    if (!m || !match || !*match) return false;
    char mfg[4];
    int product;
    if (parse_id_match(match, mfg, &product)) {
        const char *have = m->mfg[0] ? m->mfg : (m->make && strlen(m->make) == 3 ? m->make : NULL);
        if (!have || strcasecmp(have, mfg) != 0) return false;
        return product < 0 || m->product == product;
    }
    return same_string_ci(m->model, match) || same_string_ci(m->connector, match) ||
           same_string_ci(m->make, match) || contains_ci(m->description, match);
}
// }}}

// Python {{{

static PyObject *
identity_as_dict(const MonitorIdentity *m) {
    if (!m) Py_RETURN_NONE;
    char product[16] = {0};
    if (m->product >= 0) snprintf(product, sizeof(product), "0x%04X", m->product);
    return Py_BuildValue("{sz sz sz sz sz sz sO sz sz}", "connector", m->connector, "make", m->make, "model", m->model,
        "description", m->description, "mfg", m->mfg[0] ? m->mfg : NULL, "product", m->product >= 0 ? product : NULL,
        "edid_grayscale", m->edid_grayscale ? Py_True : Py_False, "edid_source", m->edid_source[0] ? m->edid_source : NULL, "known",
        m->known ? m->known->name : NULL);
}

PyObject *
monitor_identity_as_dict(const MonitorIdentity *m) {
    return identity_as_dict(m);
}

// Testing hooks: an identity to use for every window instead of what the system says
static struct {
    bool active, has_identity;
    GLFWmonitoridentity g;
    PyObject *keep;  // the objects the strings of g point into
} test_identity = {0};

bool
test_monitor_identity(GLFWmonitoridentity *g, bool *has_identity) {
    if (!test_identity.active) return false;
    *g = test_identity.g;
    *has_identity = test_identity.has_identity;
    return true;
}

static PyObject *
set_test_monitor_identity(PyObject *self UNUSED, PyObject *args) {
    PyObject *spec = Py_None;
    int active = 1;
    if (!PyArg_ParseTuple(args, "|Op", &spec, &active)) return NULL;
    Py_CLEAR(test_identity.keep);
    zero_at_ptr(&test_identity.g);
    test_identity.active = active;
    test_identity.has_identity = spec != Py_None;
    if (spec == Py_None) Py_RETURN_NONE;
    if (!PyDict_Check(spec)) {
        PyErr_SetString(PyExc_TypeError, "The identity must be a dict or None");
        return NULL;
    }
    test_identity.keep = PyDict_Copy(spec);
    if (!test_identity.keep) return NULL;
#define S(name)                                                                       \
    {                                                                                 \
        PyObject *v = PyDict_GetItemString(test_identity.keep, #name);                \
        if (v && v != Py_None) {                                                      \
            if (!(test_identity.g.name = PyUnicode_AsUTF8(v))) return NULL;           \
        }                                                                             \
    }
    S(connector) S(make) S(model) S(description)
#undef S
    PyObject *edid = PyDict_GetItemString(test_identity.keep, "edid");
    if (edid && edid != Py_None) {
        if (!PyBytes_Check(edid)) {
            PyErr_SetString(PyExc_TypeError, "edid must be bytes");
            return NULL;
        }
        test_identity.g.edid = (const unsigned char *)PyBytes_AS_STRING(edid);
        test_identity.g.edid_size = (size_t)PyBytes_GET_SIZE(edid);
    }
    PyObject *ids = PyDict_GetItemString(test_identity.keep, "ids");
    if (ids && ids != Py_None) {
        if (!PyArg_ParseTuple(ids, "II", &test_identity.g.vendor_id, &test_identity.g.product_id)) return NULL;
        test_identity.g.has_ids = true;
    }
    Py_RETURN_NONE;
}

static PyObject *
set_monitor_sysfs_root(PyObject *self UNUSED, PyObject *args) {
    const char *root = NULL;
    if (!PyArg_ParseTuple(args, "|z", &root)) return NULL;
    free(sysfs_root);
    sysfs_root = NULL;
    if (root && !(sysfs_root = strdup(root))) return PyErr_NoMemory();
    Py_RETURN_NONE;
}

static PyObject *
py_parse_edid(PyObject *self UNUSED, PyObject *args) {
    const char *data;
    Py_ssize_t sz;
    if (!PyArg_ParseTuple(args, "y#", &data, &sz)) return NULL;
    EdidInfo info;
    if (!parse_edid((const uint8_t *)data, (size_t)sz, &info)) Py_RETURN_NONE;
    PyObject *ans = Py_BuildValue("{sz sI sk si sO sI sI sz sO}", "mfg", info.mfg[0] ? info.mfg : NULL, "product", info.product,
        "serial_number", (unsigned long)info.serial_number, "year", info.year, "model_year", info.model_year ? Py_True : Py_False,
        "version", info.version, "revision", info.revision, "name", info.name, "grayscale",
        info.grayscale ? Py_True : Py_False);
    free_edid_info(&info);
    return ans;
}

// test only: the identity Silkie makes of what a system says, as from a GLFWmonitoridentity
static PyObject *
py_intern_monitor_identity(PyObject *self UNUSED, PyObject *args) {
    PyObject *spec;
    if (!PyArg_ParseTuple(args, "O!", &PyDict_Type, &spec)) return NULL;
    const bool was_active = test_identity.active;
    RAII_PyObject(saved, test_identity.keep ? Py_NewRef(test_identity.keep) : NULL);
    const GLFWmonitoridentity saved_g = test_identity.g;
    const bool saved_has = test_identity.has_identity;
    test_identity.keep = NULL;
    RAII_PyObject(a, Py_BuildValue("(O)", spec));
    if (!a) return NULL;
    PyObject *r = set_test_monitor_identity(NULL, a);
    PyObject *ans = NULL;
    if (r) {
        Py_DECREF(r);
        const MonitorIdentity *m = intern_monitor_identity(&test_identity.g);
        ans = m ? identity_as_dict(m) : PyErr_NoMemory();
    }
    Py_CLEAR(test_identity.keep);
    test_identity.keep = saved ? Py_NewRef(saved) : NULL;
    test_identity.g = saved_g;
    test_identity.has_identity = saved_has;
    test_identity.active = was_active;
    return ans;
}

static PyObject *
py_monitor_matches(PyObject *self UNUSED, PyObject *args) {
    PyObject *spec;
    const char *match;
    if (!PyArg_ParseTuple(args, "O!s", &PyDict_Type, &spec, &match)) return NULL;
    RAII_PyObject(saved, test_identity.keep ? Py_NewRef(test_identity.keep) : NULL);
    const GLFWmonitoridentity saved_g = test_identity.g;
    const bool saved_has = test_identity.has_identity, was_active = test_identity.active;
    test_identity.keep = NULL;
    RAII_PyObject(a, Py_BuildValue("(O)", spec));
    if (!a) return NULL;
    PyObject *r = set_test_monitor_identity(NULL, a);
    PyObject *ans = NULL;
    if (r) {
        Py_DECREF(r);
        const MonitorIdentity *m = intern_monitor_identity(&test_identity.g);
        ans = m ? Py_NewRef(monitor_matches(m, match) ? Py_True : Py_False) : PyErr_NoMemory();
    }
    Py_CLEAR(test_identity.keep);
    test_identity.keep = saved ? Py_NewRef(saved) : NULL;
    test_identity.g = saved_g;
    test_identity.has_identity = saved_has;
    test_identity.active = was_active;
    return ans;
}

static PyObject *
known_e_paper_monitors(PyObject *self UNUSED, PyObject *args UNUSED) {
    static const char *kinds[] = {"unknown", "lcd", "oled", "eink"}, *colors[] = {"rgb", "gray", "mono"};
    RAII_PyObject(ans, PyList_New(0));
    if (!ans) return NULL;
    for (size_t i = 0; i < arraysz(known_monitors); i++) {
        const KnownMonitor *k = known_monitors + i;
        RAII_PyObject(strings, PyTuple_New(0));
        for (const char *const *s = k->strings; s && *s; s++) {
            RAII_PyObject(one, Py_BuildValue("(s)", *s));
            if (!one) return NULL;
            PyObject *joined = PySequence_Concat(strings, one);
            if (!joined) return NULL;
            Py_SETREF(strings, joined);
        }
        RAII_PyObject(e, Py_BuildValue("{ss ss si sO ss sz si}", "name", k->name, "mfg", k->mfg, "product", k->product, "strings", strings,
            "kind", kinds[k->kind], "color", k->color >= 0 ? colors[k->color] : NULL, "gray_bits", k->gray_bits));
        if (!e || PyList_Append(ans, e) != 0) return NULL;
    }
    return Py_NewRef(ans);
}

static PyMethodDef module_methods[] = {
    {"set_test_monitor_identity", set_test_monitor_identity, METH_VARARGS, ""},
    {"set_monitor_sysfs_root", set_monitor_sysfs_root, METH_VARARGS, ""},
    {"parse_edid", py_parse_edid, METH_VARARGS, ""},
    {"intern_monitor_identity", py_intern_monitor_identity, METH_VARARGS, ""},
    {"monitor_matches", py_monitor_matches, METH_VARARGS, ""},
    {"known_e_paper_monitors", known_e_paper_monitors, METH_NOARGS, ""},
    {NULL, NULL, 0, NULL} /* Sentinel */
};

bool
init_monitor_info(PyObject *module) {
    return PyModule_AddFunctions(module, module_methods) == 0;
}
// }}}

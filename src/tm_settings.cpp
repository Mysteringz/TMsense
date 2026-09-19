#include "tm_settings.h"

#include <Arduino.h>
#include <IPAddress.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include "tm_transport.h"

TmSettings g_settings;

static Preferences s_prefs;
static const char* NS = "tmnode";

typedef struct {
    const char* name;
    int32_t min;
    int32_t max;
    const char* unit;
} ParamSpec;

// Order is TM_PARAM_* order.
static const ParamSpec SPECS[TM_PARAM_COUNT] = {
    {"min_contrast", 10, 2000, "centi-C"},
    {"min_peak", 10, 3000, "centi-C"},
    {"noise_k", 10, 200, "x10"},
    {"min_area", 1, 200, "px"},
    {"max_area", 1, TM_GRID_SIZE, "px"},
    {"bg_tau", 5, 20000, "frames"},
    {"bg_frames", 3, 600, "frames"},
    {"raw_every", 0, 3600, "frames"},
    {"refresh", 1, 5, "MLX code"},
    {"split_sep", 5, 100, "px x10"},
};

const char* tm_param_name(uint8_t id) { return id < TM_PARAM_COUNT ? SPECS[id].name : "?"; }

static void default_params(int32_t* out) {
    TmDetectorParams d;
    tm_detector_default_params(&d);
    out[TM_PARAM_MIN_CONTRAST] = (int32_t) lroundf(d.min_contrast_c * 100.0f);
    out[TM_PARAM_MIN_PEAK] = (int32_t) lroundf(d.min_peak_c * 100.0f);
    out[TM_PARAM_NOISE_K] = (int32_t) lroundf(d.noise_k * 10.0f);
    out[TM_PARAM_MIN_AREA] = d.min_area;
    out[TM_PARAM_MAX_AREA] = d.max_area;
    out[TM_PARAM_BG_TAU] = d.bg_tau_frames;
    out[TM_PARAM_BG_FRAMES] = d.bg_learn_frames;
    // Every frame while the site is being commissioned: the console and the
    // calibration recorder want it. A settled campus sets this to 0 and sends
    // only the 30-212 byte REPORT.
    out[TM_PARAM_RAW_EVERY] = 1;
    out[TM_PARAM_REFRESH] = 2;   // 2 Hz subpages = 1 full frame per second
    out[TM_PARAM_SPLIT_SEP_X10] = (int32_t) lroundf(d.split_sep_px * 10.0f);
}

static void parse_edges(const char* list) {
    memset(g_settings.edges, 0, sizeof(g_settings.edges));
    int slot = 0;
    const char* p = list;
    while (*p && slot < TM_MAX_EDGES) {
        while (*p == ' ' || *p == ',') ++p;
        size_t n = 0;
        while (p[n] && p[n] != ',' && p[n] != ' ') ++n;
        if (n > 0 && n < sizeof(g_settings.edges[0])) {
            memcpy(g_settings.edges[slot], p, n);
            ++slot;
        }
        p += n;
    }
}

static void load_defaults() {
    memset(&g_settings, 0, sizeof(g_settings));
    strncpy(g_settings.ssid, TM_DEFAULT_SSID, sizeof(g_settings.ssid) - 1);
    strncpy(g_settings.password, TM_DEFAULT_PASSWORD, sizeof(g_settings.password) - 1);
    strncpy(g_settings.key, TM_DEFAULT_KEY, sizeof(g_settings.key) - 1);
    parse_edges(TM_DEFAULT_EDGES);
    g_settings.mode = TM_MODE_WIFI;
    default_params(g_settings.params);
}

static bool is_ipv4(const char* s) {
    IPAddress ip;
    return ip.fromString(s);
}

void tm_settings_begin() {
    load_defaults();
    s_prefs.begin(NS, false);
    if (s_prefs.isKey("ssid")) s_prefs.getString("ssid", g_settings.ssid, sizeof(g_settings.ssid));
    if (s_prefs.isKey("pass")) s_prefs.getString("pass", g_settings.password, sizeof(g_settings.password));
    if (s_prefs.isKey("key")) s_prefs.getString("key", g_settings.key, sizeof(g_settings.key));
    if (s_prefs.isKey("edges")) {
        char list[64] = {0};
        s_prefs.getString("edges", list, sizeof(list));
        parse_edges(list);
    }
    g_settings.node_id = s_prefs.getUShort("node_id", 0);
    const uint8_t mode = s_prefs.getUChar("mode", TM_MODE_WIFI);
    g_settings.mode = mode == TM_MODE_LORA ? TM_MODE_LORA : TM_MODE_WIFI;
    if (s_prefs.isKey("lora_gw")) s_prefs.getString("lora_gw", g_settings.lora_gw, sizeof(g_settings.lora_gw));
    if (s_prefs.isKey("params")) {
        int32_t stored[TM_PARAM_COUNT];
        const size_t n = s_prefs.getBytes("params", stored, sizeof(stored));
        // Older firmware may have stored fewer params; keep defaults for the rest.
        for (size_t i = 0; i < n / sizeof(int32_t) && i < TM_PARAM_COUNT; ++i) {
            if (stored[i] >= SPECS[i].min && stored[i] <= SPECS[i].max) g_settings.params[i] = stored[i];
        }
    }
    g_settings.last_cmd = s_prefs.getUInt("last_cmd", 0);
    // The boot counter makes (boot, seq) strictly increasing across power
    // cycles, which is what lets the edge reject a replayed packet without
    // having to guess whether a node rebooted.
    g_settings.boot = (uint16_t) (s_prefs.getUShort("boot", 0) + 1);
    s_prefs.putUShort("boot", g_settings.boot);
}

bool tm_settings_save() {
    char list[64] = {0};
    for (int i = 0; i < TM_MAX_EDGES; ++i) {
        if (!g_settings.edges[i][0]) continue;
        if (list[0]) strncat(list, ",", sizeof(list) - strlen(list) - 1);
        strncat(list, g_settings.edges[i], sizeof(list) - strlen(list) - 1);
    }
    bool ok = s_prefs.putString("ssid", g_settings.ssid) >= 0;
    ok = s_prefs.putString("pass", g_settings.password) >= 0 && ok;
    ok = s_prefs.putString("key", g_settings.key) >= 0 && ok;
    ok = s_prefs.putString("edges", list) >= 0 && ok;
    ok = s_prefs.putUShort("node_id", g_settings.node_id) == sizeof(uint16_t) && ok;
    ok = s_prefs.putUChar("mode", g_settings.mode) == sizeof(uint8_t) && ok;
    ok = s_prefs.putString("lora_gw", g_settings.lora_gw) >= 0 && ok;
    ok = s_prefs.putBytes("params", g_settings.params, sizeof(g_settings.params)) == sizeof(g_settings.params) && ok;
    return ok;
}

void tm_settings_persist_last_cmd() { s_prefs.putUInt("last_cmd", g_settings.last_cmd); }

void tm_settings_factory_reset() {
    const uint16_t boot = g_settings.boot;
    s_prefs.clear();
    // Keep the boot counter: resetting it would make this node's next packets
    // look like replays to every edge that has heard it before.
    s_prefs.putUShort("boot", boot);
    load_defaults();
    g_settings.boot = boot;
}

bool tm_settings_set_param(uint8_t id, int32_t value) {
    if (id >= TM_PARAM_COUNT || value < SPECS[id].min || value > SPECS[id].max) return false;
    g_settings.params[id] = value;
    return true;
}

void tm_settings_to_detector(TmDetectorParams* out) {
    const int32_t* p = g_settings.params;
    tm_detector_default_params(out);
    out->min_contrast_c = p[TM_PARAM_MIN_CONTRAST] / 100.0f;
    out->min_peak_c = p[TM_PARAM_MIN_PEAK] / 100.0f;
    out->noise_k = p[TM_PARAM_NOISE_K] / 10.0f;
    out->min_area = (uint16_t) p[TM_PARAM_MIN_AREA];
    out->max_area = (uint16_t) p[TM_PARAM_MAX_AREA];
    out->bg_tau_frames = (uint16_t) p[TM_PARAM_BG_TAU];
    out->bg_learn_frames = (uint16_t) p[TM_PARAM_BG_FRAMES];
    out->split_sep_px = p[TM_PARAM_SPLIT_SEP_X10] / 10.0f;
}

// --- Serial console ----------------------------------------------------------

static char s_line[160];
static size_t s_len = 0;

static void show() {
    // Secrets are never echoed: whether they are set, and the key's length,
    // is all anyone debugging a node needs.
    uint8_t uid[6];
    tm_transport_uid(uid);
    // TMflash reads these `name : value` lines to verify a node after
    // provisioning; keep the names stable.
    Serial.printf("uid       : %02x:%02x:%02x:%02x:%02x:%02x\n", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
    Serial.printf("fw        : %s\n", TM_FW_VERSION);
    if (g_settings.node_id) Serial.printf("node_id   : %u\n", (unsigned) g_settings.node_id);
    else Serial.println("node_id   : (unset)");
    Serial.printf("mode      : %s\n", g_settings.mode == TM_MODE_LORA ? "lora" : "wifi");
    Serial.printf("lora_gw   : %s\n", g_settings.lora_gw[0] ? g_settings.lora_gw : "(none)");
    Serial.printf("ssid      : %s\n", g_settings.ssid[0] ? g_settings.ssid : "(unset)");
    Serial.printf("password  : %s\n", g_settings.password[0] ? "(set)" : "(unset)");
    Serial.printf("edges     : %s %s\n", g_settings.edges[0][0] ? g_settings.edges[0] : "(none)", g_settings.edges[1]);
    Serial.printf("key       : %s\n", g_settings.key[0] ? "(set)" : "(unset - telemetry unsigned)");
    Serial.printf("boot      : %u   last_cmd: %lu\n", (unsigned) g_settings.boot, (unsigned long) g_settings.last_cmd);
    for (uint8_t i = 0; i < TM_PARAM_COUNT; ++i) {
        Serial.printf("param %-12s = %ld %s\n", SPECS[i].name, (long) g_settings.params[i], SPECS[i].unit);
    }
}

static void help() {
    Serial.println(
        "commands:\n"
        "  show                      settings (secrets are not printed)\n"
        "  set ssid <name>           Wi-Fi network (2.4 GHz)\n"
        "  set pass <password>\n"
        "  set edges <ip>[,<ip>]     where to send over Wi-Fi: TMWAccess or TMedge\n"
        "  set id <1-65535>          node ID (a label; identity stays the MAC)\n"
        "  set mode <wifi|lora>      uplink transport\n"
        "  set lora_gw <ip>          TMLAccess address (LoRa mode)\n"
        "  set key <string>          shared signing key (TMedge TM_KEY)\n"
        "  param <name> <value>      detector/telemetry parameter, see `show`\n"
        "  save                      keep settings across reboots\n"
        "  reset-bg                  relearn the background\n"
        "  factory                   erase saved settings\n"
        "  reboot");
}

static uint8_t execute(char* line) {
    char* cmd = strtok(line, " ");
    if (!cmd) return 0;
    if (!strcmp(cmd, "help")) { help(); return 0; }
    if (!strcmp(cmd, "show")) { show(); return 0; }
    if (!strcmp(cmd, "save")) {
        Serial.println(tm_settings_save() ? "saved" : "SAVE FAILED");
        return 0;
    }
    if (!strcmp(cmd, "reboot")) return TM_CONSOLE_REBOOT;
    if (!strcmp(cmd, "reset-bg")) { Serial.println("relearning background"); return TM_CONSOLE_RESET_BG; }
    if (!strcmp(cmd, "factory")) {
        tm_settings_factory_reset();
        Serial.println("settings erased; `reboot` to apply");
        return 0;
    }
    if (!strcmp(cmd, "set")) {
        char* what = strtok(NULL, " ");
        char* value = strtok(NULL, "");   // rest of line: SSIDs may contain spaces
        if (!what || !value) { Serial.println("usage: set <ssid|pass|edges|key|id|mode|lora_gw> <value>"); return 0; }
        if (!strcmp(what, "id")) {
            char* end = NULL;
            const long v = strtol(value, &end, 10);
            if (end == value || *end || v < 1 || v > 65535) { Serial.println("invalid: id must be 1..65535"); return 0; }
            g_settings.node_id = (uint16_t) v;
            Serial.println("id updated (not saved)");
            return 0;   // a label: nothing to restart
        }
        if (!strcmp(what, "mode")) {
            if (!strcmp(value, "wifi")) g_settings.mode = TM_MODE_WIFI;
            else if (!strcmp(value, "lora")) g_settings.mode = TM_MODE_LORA;
            else { Serial.println("invalid: mode must be wifi or lora"); return 0; }
            Serial.println("mode updated (not saved)");
            return TM_CONSOLE_NETWORK_CHANGED;
        }
        if (!strcmp(what, "lora_gw")) {
            if (!is_ipv4(value)) { Serial.println("invalid: lora_gw must be an IPv4 address"); return 0; }
            strncpy(g_settings.lora_gw, value, sizeof(g_settings.lora_gw) - 1);
            Serial.println("lora_gw updated (not saved)");
            return 0;
        }
        if (!strcmp(what, "edges")) {
            // Checked here too, so a typo is refused at the console instead of
            // only being logged at the next boot.
            char copy[64];
            strncpy(copy, value, sizeof(copy) - 1);
            copy[sizeof(copy) - 1] = 0;
            for (char* t = strtok(copy, ", "); t; t = strtok(NULL, ", ")) {
                if (!is_ipv4(t)) { Serial.printf("invalid: edge \"%s\" is not an IPv4 address\n", t); return 0; }
            }
        }
        if (!strcmp(what, "ssid")) strncpy(g_settings.ssid, value, sizeof(g_settings.ssid) - 1);
        else if (!strcmp(what, "pass")) strncpy(g_settings.password, value, sizeof(g_settings.password) - 1);
        else if (!strcmp(what, "key")) strncpy(g_settings.key, value, sizeof(g_settings.key) - 1);
        else if (!strcmp(what, "edges")) parse_edges(value);
        else { Serial.println("unknown setting"); return 0; }
        Serial.printf("%s updated (not saved)\n", what);
        return TM_CONSOLE_NETWORK_CHANGED;
    }
    if (!strcmp(cmd, "param")) {
        char* name = strtok(NULL, " ");
        char* value = strtok(NULL, " ");
        if (!name || !value) { Serial.println("usage: param <name> <value>"); return 0; }
        for (uint8_t i = 0; i < TM_PARAM_COUNT; ++i) {
            if (strcmp(name, SPECS[i].name)) continue;
            const long v = strtol(value, NULL, 10);
            if (!tm_settings_set_param(i, (int32_t) v)) {
                Serial.printf("%s must be %ld..%ld\n", name, (long) SPECS[i].min, (long) SPECS[i].max);
                return 0;
            }
            Serial.printf("%s = %ld (not saved)\n", name, v);
            return TM_CONSOLE_PARAMS_CHANGED;
        }
        Serial.println("unknown param; `show` lists them");
        return 0;
    }
    Serial.println("unknown command; try `help`");
    return 0;
}

uint8_t tm_console_poll() {
    uint8_t actions = 0;
    while (Serial.available()) {
        const int c = Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            s_line[s_len] = '\0';
            if (s_len > 0) actions |= execute(s_line);
            s_len = 0;
        } else if (s_len < sizeof(s_line) - 1) {
            s_line[s_len++] = (char) c;
        } else {
            s_len = 0;   // overlong line: drop it rather than act on half of it
            Serial.println("line too long, ignored");
        }
    }
    return actions;
}

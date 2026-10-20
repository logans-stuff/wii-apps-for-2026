// sd:/apps/meshtasticd/config.txt -> firmware settings, for a node with no
// phone attached. Each setting becomes the AdminMessage a Meshtastic client
// would send (set_owner / set_config / set_module_config / set_channel /
// set_fixed_position), wrapped in begin/commit_edit_settings, and goes in
// through the in-process PhoneAPI - so AdminModule's own validation, minimums
// and save logic apply exactly as they would for the phone app.
//
// Applied once per distinct file content (a hash is kept in oem/config.applied),
// one message per loop tick so the router handles them in order.
#include "Channels.h"
#include "NodeDB.h"
#include "mesh-pb-constants.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WII_CONFIG_FILE "sd:/apps/meshtasticd/config.txt"
#define WII_CONFIG_STAMP "sd:/apps/meshtasticd/oem/config.applied"

extern "C" int wii_api_to_radio(const uint8_t *buf, size_t len);

static std::vector<std::vector<uint8_t>> g_pending; // encoded ToRadio, in order
static char g_stamp[16];                             // written once the commit is sent

struct EnumName {
    const char *name;
    int value;
};

static const EnumName REGIONS[] = {
    {"US", 1},      {"EU_433", 2},  {"EU_868", 3},  {"CN", 4},      {"JP", 5},      {"ANZ", 6},     {"KR", 7},
    {"TW", 8},      {"RU", 9},      {"IN", 10},     {"NZ_865", 11}, {"TH", 12},     {"LORA_24", 13}, {"UA_433", 14},
    {"UA_868", 15}, {"MY_433", 16}, {"MY_919", 17}, {"SG_923", 18}, {"PH_433", 19}, {"PH_868", 20}, {"PH_915", 21},
    {"ANZ_433", 22}, {"KZ_433", 23}, {"KZ_863", 24}, {"NP_865", 25}, {"BR_902", 26}, {"EU_866", 29}, {"EU_874", 30},
    {"EU_917", 31}, {"EU_N_868", 32}, {nullptr, 0}};
static const EnumName PRESETS[] = {{"LONG_FAST", 0},     {"LONG_SLOW", 1},    {"VERY_LONG_SLOW", 2}, {"MEDIUM_SLOW", 3},
                                   {"MEDIUM_FAST", 4},   {"SHORT_SLOW", 5},   {"SHORT_FAST", 6},     {"LONG_MODERATE", 7},
                                   {"SHORT_TURBO", 8},   {"LONG_TURBO", 9},   {"MEDIUM_TURBO", 16},  {nullptr, 0}};
static const EnumName ROLES[] = {{"CLIENT", 0},  {"CLIENT_MUTE", 1}, {"ROUTER", 2},          {"TRACKER", 5}, {"SENSOR", 6},
                                 {"TAK", 7},     {"CLIENT_HIDDEN", 8}, {"LOST_AND_FOUND", 9}, {"TAK_TRACKER", 10},
                                 {"ROUTER_LATE", 11}, {"CLIENT_BASE", 12}, {nullptr, 0}};
static const EnumName REBROADCAST[] = {{"ALL", 0},  {"ALL_SKIP_DECODING", 1}, {"LOCAL_ONLY", 2},
                                       {"KNOWN_ONLY", 3}, {"NONE", 4},         {"CORE_PORTNUMS_ONLY", 5},
                                       {nullptr, 0}};

// Firmware enum spelling, case-insensitive, '-' or ' ' for '_' (medium_fast,
// Medium-Fast, MEDIUM FAST).
static bool lookup(const EnumName *t, const std::string &v, int &out)
{
    std::string u;
    for (char c : v)
        u += (c == '-' || c == ' ') ? '_' : (char)toupper((unsigned char)c);
    for (; t->name; t++)
        if (u == t->name) {
            out = t->value;
            return true;
        }
    printf("config.txt: unknown value '%s'\n", v.c_str());
    return false;
}

static bool on(const std::string &v)
{
    return v == "1" || v == "on" || v == "true" || v == "yes";
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

static size_t b64decode(const std::string &in, uint8_t *out, size_t max)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (char c : in) {
        int v = b64val(c);
        if (v < 0)
            continue; // '=' padding, whitespace
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8 && n < max) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

static void queue_admin(const meshtastic_AdminMessage &a)
{
    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    tr.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket &p = tr.packet;
    p.to = nodeDB->getNodeNum();
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    p.decoded.payload.size =
        pb_encode_to_bytes(p.decoded.payload.bytes, sizeof(p.decoded.payload.bytes), &meshtastic_AdminMessage_msg, &a);
    std::vector<uint8_t> buf(meshtastic_ToRadio_size);
    size_t n = pb_encode_to_bytes(buf.data(), buf.size(), &meshtastic_ToRadio_msg, &tr);
    if (n == 0 || p.decoded.payload.size == 0) {
        printf("config.txt: could not encode admin message %u\n", (unsigned)a.which_payload_variant);
        return;
    }
    buf.resize(n);
    g_pending.push_back(buf);
}

static uint32_t fnv1a(const std::string &s)
{
    uint32_t h = 2166136261u ^ 2u; // format version: bump to re-apply every config
    for (unsigned char c : s)
        h = (h ^ c) * 16777619u;
    return h;
}

// Parse config.txt into admin messages. Called once, after setup().
extern "C" void wii_config_load()
{
    FILE *f = fopen(WII_CONFIG_FILE, "r");
    if (!f)
        return;
    std::string text;
    char line[256];
    while (fgets(line, sizeof(line), f))
        text += line;
    fclose(f);

    char *stamp = g_stamp;
    snprintf(g_stamp, sizeof(g_stamp), "%08lx", (unsigned long)fnv1a(text));
    if (FILE *s = fopen(WII_CONFIG_STAMP, "r")) {
        char old[16] = {0};
        bool same = fgets(old, sizeof(old), s) && strncmp(old, stamp, 8) == 0;
        fclose(s);
        if (same)
            return; // already applied; the firmware has kept it in prefs/
    }

    meshtastic_Config_DeviceConfig device = config.device;
    meshtastic_Config_LoRaConfig lora = config.lora;
    meshtastic_Config_PositionConfig position = config.position;
    meshtastic_ModuleConfig_NeighborInfoConfig neighbor = moduleConfig.neighbor_info;
    meshtastic_ModuleConfig_TelemetryConfig telemetry = moduleConfig.telemetry;
    meshtastic_ModuleConfig_TrafficManagementConfig traffic = moduleConfig.traffic_management;
    meshtastic_Channel primary = channels.getByIndex(channels.getPrimaryIndex());
    meshtastic_User user = owner;
    bool dDevice = false, dLora = false, dPosition = false, dNeighbor = false, dTelemetry = false, dTraffic = false,
         dChannel = false, dOwner = false, dFixed = false;
    double fixedLat = 0, fixedLon = 0;
    int fixedAlt = 0;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string l = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        size_t hash = l.find('#');
        if (hash != std::string::npos)
            l.resize(hash);
        size_t eq = l.find('=');
        if (eq == std::string::npos)
            continue;
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        for (auto &c : k)
            c = (char)tolower((unsigned char)c);
        if (v.empty())
            continue;
        int e;
        long n = strtol(v.c_str(), nullptr, 0);

        if (k == "name" || k == "long_name") {
            snprintf(user.long_name, sizeof(user.long_name), "%s", v.c_str());
            dOwner = true;
        } else if (k == "short" || k == "short_name") {
            snprintf(user.short_name, sizeof(user.short_name), "%s", v.c_str());
            dOwner = true;
        } else if (k == "region") {
            if (lookup(REGIONS, v, e))
                lora.region = (meshtastic_Config_LoRaConfig_RegionCode)e, dLora = true;
        } else if (k == "preset" || k == "modem_preset") {
            if (lookup(PRESETS, v, e))
                lora.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)e, lora.use_preset = true, dLora = true;
        } else if (k == "slot" || k == "channel_num") {
            lora.channel_num = (uint16_t)n, dLora = true;
        } else if (k == "hop_limit" || k == "hops") {
            lora.hop_limit = (uint32_t)n, dLora = true;
        } else if (k == "tx_power" || k == "power") {
            lora.tx_power = (int8_t)n, dLora = true;
        } else if (k == "override_duty_cycle") {
            lora.override_duty_cycle = on(v), dLora = true;
        } else if (k == "ignore_mqtt") {
            lora.ignore_mqtt = on(v), dLora = true;
        } else if (k == "ok_to_mqtt") {
            lora.config_ok_to_mqtt = on(v), dLora = true;
        } else if (k == "role") {
            if (lookup(ROLES, v, e))
                device.role = (meshtastic_Config_DeviceConfig_Role)e, dDevice = true;
        } else if (k == "rebroadcast" || k == "rebroadcast_mode") {
            if (lookup(REBROADCAST, v, e))
                device.rebroadcast_mode = (meshtastic_Config_DeviceConfig_RebroadcastMode)e, dDevice = true;
        } else if (k == "node_info_interval" || k == "node_info_broadcast_secs") {
            device.node_info_broadcast_secs = (uint32_t)n, dDevice = true;
        } else if (k == "position_interval" || k == "position_broadcast_secs") {
            position.position_broadcast_secs = (uint32_t)n, dPosition = true;
        } else if (k == "position_precision") {
            primary.settings.has_module_settings = true;
            primary.settings.module_settings.position_precision = (uint32_t)n;
            dChannel = true;
        } else if (k == "fixed_lat") {
            fixedLat = atof(v.c_str()), dFixed = true;
        } else if (k == "fixed_lon") {
            fixedLon = atof(v.c_str()), dFixed = true;
        } else if (k == "fixed_alt") {
            fixedAlt = (int)n, dFixed = true;
        } else if (k == "telemetry" || k == "device_telemetry_enabled") {
            telemetry.device_telemetry_enabled = on(v), dTelemetry = true;
        } else if (k == "telemetry_interval" || k == "device_update_interval") {
            telemetry.device_update_interval = (uint32_t)n, dTelemetry = true;
        } else if (k == "neighbor_info") {
            neighbor.enabled = on(v), dNeighbor = true;
        } else if (k == "neighbor_interval" || k == "neighbor_info_interval") {
            neighbor.update_interval = (uint32_t)n, dNeighbor = true;
        } else if (k == "neighbor_lora" || k == "transmit_over_lora") {
            neighbor.transmit_over_lora = on(v), dNeighbor = true;
        } else if (k == "position_dedup" || k == "position_min_interval_secs") {
            traffic.position_min_interval_secs = (uint32_t)n, dTraffic = true;
        } else if (k == "channel_name") {
            snprintf(primary.settings.name, sizeof(primary.settings.name), "%s", v.c_str());
            dChannel = true;
        } else if (k == "psk" || k == "key") {
            primary.settings.psk.size = (pb_size_t)b64decode(v, primary.settings.psk.bytes, sizeof(primary.settings.psk.bytes));
            dChannel = true;
        } else {
            printf("config.txt: unknown key '%s'\n", k.c_str());
        }
    }

    meshtastic_AdminMessage a;
    a = meshtastic_AdminMessage_init_zero;
    a.which_payload_variant = meshtastic_AdminMessage_begin_edit_settings_tag;
    a.begin_edit_settings = true;
    queue_admin(a);
    if (dOwner) {
        a = meshtastic_AdminMessage_init_zero;
        a.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
        a.set_owner = user;
        queue_admin(a);
    }
    if (dDevice) {
        a = meshtastic_AdminMessage_init_zero;
        a.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        a.set_config.which_payload_variant = meshtastic_Config_device_tag;
        a.set_config.payload_variant.device = device;
        queue_admin(a);
    }
    if (dLora) {
        a = meshtastic_AdminMessage_init_zero;
        a.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        a.set_config.which_payload_variant = meshtastic_Config_lora_tag;
        a.set_config.payload_variant.lora = lora;
        queue_admin(a);
    }
    if (dPosition) {
        a = meshtastic_AdminMessage_init_zero;
        a.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        a.set_config.which_payload_variant = meshtastic_Config_position_tag;
        a.set_config.payload_variant.position = position;
        queue_admin(a);
    }
    if (dChannel) {
        a = meshtastic_AdminMessage_init_zero;
        a.which_payload_variant = meshtastic_AdminMessage_set_channel_tag;
        a.set_channel = primary;
        queue_admin(a);
    }
    if (dTelemetry) {
        a = meshtastic_AdminMessage_init_zero;
        a.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
        a.set_module_config.which_payload_variant = meshtastic_ModuleConfig_telemetry_tag;
        a.set_module_config.payload_variant.telemetry = telemetry;
        queue_admin(a);
    }
    if (dNeighbor) {
        a = meshtastic_AdminMessage_init_zero;
        a.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
        a.set_module_config.which_payload_variant = meshtastic_ModuleConfig_neighbor_info_tag;
        a.set_module_config.payload_variant.neighbor_info = neighbor;
        queue_admin(a);
    }
    if (dTraffic) {
        a = meshtastic_AdminMessage_init_zero;
        a.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
        a.set_module_config.which_payload_variant = meshtastic_ModuleConfig_traffic_management_tag;
        a.set_module_config.payload_variant.traffic_management = traffic;
        queue_admin(a);
    }
    if (dFixed) {
        a = meshtastic_AdminMessage_init_zero;
        a.which_payload_variant = meshtastic_AdminMessage_set_fixed_position_tag;
        a.set_fixed_position.has_latitude_i = true;
        a.set_fixed_position.latitude_i = (int32_t)(fixedLat * 1e7);
        a.set_fixed_position.has_longitude_i = true;
        a.set_fixed_position.longitude_i = (int32_t)(fixedLon * 1e7);
        a.set_fixed_position.has_altitude = true;
        a.set_fixed_position.altitude = fixedAlt;
        queue_admin(a);
    }
    a = meshtastic_AdminMessage_init_zero;
    a.which_payload_variant = meshtastic_AdminMessage_commit_edit_settings_tag;
    a.commit_edit_settings = true;
    queue_admin(a);

    printf("config.txt: applying %u settings messages\n", (unsigned)g_pending.size());
}

// One queued admin message per loop tick. (Nothing drains the client queue
// yet: without a want_config handshake PhoneAPI never empties it, and
// MeshService already drops the oldest entry when it is full.)
extern "C" void wii_config_tick()
{
    if (g_pending.empty())
        return;
    const std::vector<uint8_t> &m = g_pending.front();
    if (!wii_api_to_radio(m.data(), m.size()))
        printf("config.txt: settings message rejected\n");
    g_pending.erase(g_pending.begin());
    if (g_pending.empty()) {
        // Only now is it applied: an interrupted run tries again next boot.
        if (FILE *s = fopen(WII_CONFIG_STAMP, "w")) {
            fputs(g_stamp, s);
            fclose(s);
        }
        printf("config.txt: applied\n");
    }
}

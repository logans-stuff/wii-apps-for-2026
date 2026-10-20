/* Meshtoad config loader. See config.h. Reads simple key=value lines from
 * sd:/apps/meshtoad/config.txt; anything not set keeps its default. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <fat.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/coding.h>

#include "config.h"

/* Meshtastic modem presets -> (spreading factor, bandwidth kHz). */
static const struct {
    const char *name;
    int sf, bw;
} PRESETS[] = {
    { "shortturbo", 7, 500 }, { "shortfast", 7, 250 }, { "shortslow", 8, 250 },
    { "mediumfast", 9, 250 }, { "mediumslow", 10, 250 },
    { "longfast", 11, 250 }, { "longmoderate", 11, 125 }, { "longslow", 12, 125 },
};

static void defaults(struct mesh_config *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->long_name, sizeof(c->long_name), "Wii");
    snprintf(c->short_name, sizeof(c->short_name), "Wii");
    c->node_id = 0x57494921u;
    c->lora.freq_hz = 906375000u;
    c->lora.sf = 9;
    c->lora.bw_khz = 250;
    c->lora.cr = 5;
    c->lora.sync = 0x2B;
    c->lora.preamble = 16;
    c->channel_hash = 0x1f;
    c->keylen = 0; /* public default key */
    snprintf(c->source, sizeof(c->source), "defaults");
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

/* Lowercase, keep only a-z0-9 (so "Medium Fast" == "mediumfast"). */
static void normalize(const char *in, char *out, int n)
{
    int i = 0;
    for (; *in && i < n - 1; in++)
        if (isalnum((unsigned char)*in))
            out[i++] = (char)tolower((unsigned char)*in);
    out[i] = '\0';
}

static uint32_t parse_id(const char *v)
{
    if (v[0] == '!' || v[0] == '#')
        v++;
    return (uint32_t)strtoul(v, NULL, 16);
}

void config_load(struct mesh_config *c)
{
    FILE *f;
    char line[160];
    int slot = 0, bw_set_by_preset = 0;

    defaults(c);
    if (!fatInitDefault())
        return; /* no SD: keep defaults */

    /* Make sure our app folder exists, so the config and NodeDB can be saved
     * even when the app was sent over wiiload rather than installed. */
    mkdir("sd:/apps", 0777);
    mkdir("sd:/apps/meshtoad", 0777);

    f = fopen("sd:/apps/meshtoad/config.txt", "r");
    if (f == NULL)
        f = fopen("/apps/meshtoad/config.txt", "r");
    if (f == NULL)
        return;
    snprintf(c->source, sizeof(c->source), "config.txt");

    while (fgets(line, sizeof(line), f) != NULL) {
        char *hash = strchr(line, '#');
        char *eq, *key, *val, kn[24];
        if (hash)
            *hash = '\0';
        eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        key = trim(line);
        val = trim(eq + 1);
        normalize(key, kn, sizeof(kn));
        if (val[0] == '\0')
            continue;

        if (!strcmp(kn, "name") || !strcmp(kn, "longname"))
            snprintf(c->long_name, sizeof(c->long_name), "%s", val);
        else if (!strcmp(kn, "short") || !strcmp(kn, "shortname"))
            snprintf(c->short_name, sizeof(c->short_name), "%s", val);
        else if (!strcmp(kn, "id") || !strcmp(kn, "nodeid"))
            c->node_id = parse_id(val);
        else if (!strcmp(kn, "preset")) {
            char pv[24];
            size_t i;
            normalize(val, pv, sizeof(pv));
            for (i = 0; i < sizeof(PRESETS) / sizeof(PRESETS[0]); i++)
                if (!strcmp(pv, PRESETS[i].name)) {
                    c->lora.sf = PRESETS[i].sf;
                    c->lora.bw_khz = PRESETS[i].bw;
                    bw_set_by_preset = 1;
                    break;
                }
        } else if (!strcmp(kn, "slot") || !strcmp(kn, "channelnum"))
            slot = atoi(val);
        else if (!strcmp(kn, "freq") || !strcmp(kn, "frequency"))
            c->lora.freq_hz = (uint32_t)strtoul(val, NULL, 0);
        else if (!strcmp(kn, "sf"))
            c->lora.sf = atoi(val);
        else if (!strcmp(kn, "bw") || !strcmp(kn, "bandwidth"))
            c->lora.bw_khz = atoi(val);
        else if (!strcmp(kn, "cr"))
            c->lora.cr = atoi(val);
        else if (!strcmp(kn, "sync"))
            c->lora.sync = (uint8_t)strtoul(val, NULL, 0);
        else if (!strcmp(kn, "channel") || !strcmp(kn, "chan") || !strcmp(kn, "channelhash"))
            c->channel_hash = (uint8_t)strtoul(val, NULL, 0);
        else if (!strcmp(kn, "key") || !strcmp(kn, "psk")) {
            byte out[48];
            word32 outlen = sizeof(out);
            if (Base64_Decode((const byte *)val, (word32)strlen(val), out, &outlen) == 0) {
                if (outlen == 16 || outlen == 32) {
                    memcpy(c->key, out, outlen);
                    c->keylen = (int)outlen;
                } else {
                    c->keylen = 0; /* "AQ==" (default) or unknown length */
                }
            }
        }
    }
    fclose(f);

    /* Frequency slot overrides freq when given (US 902-928 spacing by BW). */
    if (slot > 0) {
        uint32_t bw = (uint32_t)c->lora.bw_khz * 1000u;
        c->lora.freq_hz = 902000000u + bw / 2u + (uint32_t)(slot - 1) * bw;
    }
    (void)bw_set_by_preset;
}

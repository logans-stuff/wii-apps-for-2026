// Wii-side portduino glue (ARCH_PORTDUINO_WII, built together with
// ARCH_PORTDUINO_WASM so the firmware takes its single-threaded/no-YAML paths).
// Derived from wasm/portduino_glue_wasm.cpp: same hooks, with the browser bits
// replaced by libogc ones - the FS is the SD card (libfat), the node identity
// comes from the console's unique device ID, and reboot returns to the loader.
//
// Wiring in the firmware tree (all #ifdef ARCH_PORTDUINO_WASM, unchanged):
//   - PortduinoGlue.cpp portduinoSetup(): calls wasm_config_apply() and
//     constructs Ch341Hal, then returns before the YAML path.
//   - exec() short-circuits to "".
// Downstream: Ch341Hal -> libpinedio_ogc.c -> libogc USB.

#include "MeshRadio.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PhoneAPI.h"
#include "PortduinoFS.h"
#include "PortduinoGlue.h"
#include "RadioInterface.h"
#include <SHA256.h>
#include <cerrno>
#include <reent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#define WII_DATA_DIR "sd:/apps/meshtasticd"

void wasm_fs_mount()
{
    mkdir("sd:/apps", 0777);
    mkdir(WII_DATA_DIR, 0777);
    mkdir(WII_DATA_DIR "/prefs", 0777);
    mkdir(WII_DATA_DIR "/oem", 0777);
    portduinoVFS->mountpoint(WII_DATA_DIR);
}

// MAC (12 hex chars). The lower 4 bytes become the NodeNum, so every Wii needs
// its own: use the console's unique ES device ID, locally administered.
static std::string wii_resolve_mac()
{
    u32 devid = 0;
    if (ES_GetDeviceID(&devid) < 0 || devid == 0)
        devid = (u32)gettime() ^ 0x57494921u; // no ES: fall back to a random-ish id
    char hex[13];
    snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X", 0x02, 0x57, (unsigned)(devid >> 24) & 0xff,
             (unsigned)(devid >> 16) & 0xff, (unsigned)(devid >> 8) & 0xff, (unsigned)devid & 0xff);
    return std::string(hex, 12);
}

// MeshToad E22/SX1262 over CH341 (bin/config.d/lora-usb-meshtoad-e22.yaml).
void wasm_config_apply()
{
    if (portduino_config.lora_module == use_simradio) {
        portduino_config.lora_module = use_sx1262;
        portduino_config.lora_usb_vid = 0x1A86;
        portduino_config.lora_usb_pid = 0x5512;
        portduino_config.lora_usb_serial_num = "";
        portduino_config.dio2_as_rf_switch = true;
        portduino_config.dio3_tcxo_voltage = 1800;
        portduino_config.spiSpeed = 2000000;
        auto setPin = [](pinMapping &p, int n) {
            p.pin = n;
            p.enabled = true;
        };
        setPin(portduino_config.lora_cs_pin, 0);    // CS  = D0
        setPin(portduino_config.lora_irq_pin, 6);   // IRQ = D6
        setPin(portduino_config.lora_busy_pin, 4);  // BUSY = D4
        setPin(portduino_config.lora_reset_pin, 2); // RESET = D2
        setPin(portduino_config.lora_rxen_pin, 1);  // RXen = D1
    }
    portduino_config.lora_spi_dev = "ch341";
    portduino_config.displayPanel = no_screen;
    portduino_config.has_gps = false;
    portduino_config.MaxNodes = 200;
    portduino_config.mac_address = wii_resolve_mac();
}

extern "C" volatile bool g_wasm_in_firmware = false;

extern "C" int wii_set_region(int region);

// A fresh node has no region and keeps TX off until one is set: default to US
// (the MeshToad's band). The API can change it later.
extern "C" void wii_default_region()
{
    if (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
        wii_set_region(meshtastic_Config_LoRaConfig_RegionCode_US);
}

// Set the LoRa region the way AdminModule's set_config does (validate, mint
// the PKI identity on first region, initRegion, reloadConfig). 0 = ok.
extern "C" int wii_set_region(int region)
{
    auto newRegion = (meshtastic_Config_LoRaConfig_RegionCode)region;
    if (config.lora.region == newRegion)
        return 0;
    meshtastic_Config_LoRaConfig validated = config.lora;
    validated.region = newRegion;
    if (!(RadioInterface::validateConfigRegion(validated) && RadioInterface::validateConfigLora(validated)))
        return -1;
    int changes = SEGMENT_CONFIG;
    bool wasUnset = (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET);
    if (wasUnset && newRegion > meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
        if (nodeDB && nodeDB->ensurePkiIdentity())
            changes |= SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE;
#endif
        validated.tx_enabled = true;
    }
    if (!wasUnset && newRegion == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
        validated.tx_enabled = false;
    config.lora = validated;
    initRegion();
    if (service)
        service->reloadConfig(changes);
    return 0;
}

// In-process client API: the UI (or a TCP bridge) feeds ToRadio protobufs in and
// drains FromRadio out, unframed, exactly like the wasm_api_* seam.
class WiiPhoneAPI : public PhoneAPI
{
  public:
    virtual bool checkIsConnected() override { return true; }
};

static WiiPhoneAPI *g_phone = nullptr;

static WiiPhoneAPI *phone()
{
    if (!g_phone && service)
        g_phone = new WiiPhoneAPI();
    return g_phone;
}

extern "C" int wii_api_to_radio(const uint8_t *buf, size_t len)
{
    WiiPhoneAPI *p = phone();
    return (p && p->handleToRadio((uint8_t *)buf, len)) ? 1 : 0;
}

extern "C" int wii_api_from_radio(uint8_t *out, size_t max)
{
    WiiPhoneAPI *p = phone();
    if (!p || max < 512 || !p->available())
        return 0;
    return (int)p->getFromRadio(out);
}

extern "C" int wii_api_available()
{
    WiiPhoneAPI *p = phone();
    return (p && p->available()) ? 1 : 0;
}

// --- framework symbols normally from linux/LinuxCommon.cpp (excluded) ---
void delay(unsigned long ms)
{
    if (ms)
        usleep((useconds_t)ms * 1000);
}
void yield(void) {}
long random(long howbig)
{
    return howbig > 0 ? (::rand() % howbig) : 0;
}
long random(long howsmall, long howbig)
{
    return howsmall >= howbig ? howsmall : howsmall + random(howbig - howsmall);
}
void randomSeed(unsigned long seed)
{
    ::srand((unsigned)seed);
}
// No in-process restart: return to the Homebrew Channel. Power::reboot() has
// already let modules persist their state.
// Quitting with Home/Reset skips the firmware's reboot path, which is where
// positions, telemetry and last-heard of the NodeDB get written. Do the same.
extern "C" void wii_save_state(void)
{
    if (nodeDB)
        nodeDB->saveToDisk(); // config, module config, device state, channels, NodeDB
}

extern "C" void wii_debuglog_stop();
void reboot()
{
    printf("reboot requested: returning to the loader\n");
    wii_debuglog_stop(); // leave Wi-Fi down so HBC can bring it up again
    exit(0);
}
void tone(unsigned char, unsigned int, unsigned long) {}
void noTone(unsigned char) {}
void delayMicroseconds(unsigned int us)
{
    if (us)
        usleep(us);
}
bool shouldWakeOnReceivedMessage()
{
    return false;
}
void initApiServer(int) {}
void deInitApiServer() {}

// No serial ports on the Wii (framework LinuxSerial's FIONREAD lands here).
extern "C" int ioctl(int, unsigned long, ...)
{
    errno = ENOTTY;
    return -1;
}

// newlib's getentropy()/arc4random backend. The Wii has no OS RNG, so hash
// PowerPC timebase jitter (bus/cache timing around a memory walk and a
// microsecond sleep). HardwareRNG XORs the SX1262's RF-noise RNG over this
// once the radio is up, which is when the PKI keypair gets generated.
extern "C" int _getentropy_r(struct _reent *r, void *buf, size_t len)
{
    static volatile uint32_t walk[256];
    static uint32_t counter;
    uint8_t *out = (uint8_t *)buf;
    (void)r;
    while (len > 0) {
        SHA256 sha;
        uint8_t digest[32];
        counter++;
        sha.update(&counter, sizeof(counter));
        for (int i = 0; i < 64; i++) {
            uint64_t t0 = gettime();
            for (int j = 0; j < 256; j++)
                walk[(j * 37 + i) & 255] += (uint32_t)t0 ^ (uint32_t)j;
            if ((i & 15) == 0)
                usleep(1);
            uint64_t t1 = gettime();
            uint32_t d = (uint32_t)(t1 - t0);
            sha.update(&d, sizeof(d));
            sha.update(&t1, sizeof(t1));
        }
        sha.finalize(digest, sizeof(digest));
        size_t n = len < sizeof(digest) ? len : sizeof(digest);
        memcpy(out, digest, n);
        out += n;
        len -= n;
    }
    return 0;
}

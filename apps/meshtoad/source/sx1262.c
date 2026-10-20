/* SX1262 LoRa radio driver over the CH341 USB-SPI bridge.
 * Command codes and register addresses: Semtech SX1261/2 datasheet. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "ch341.h"
#include "sx1262.h"
#include "net.h" /* dbg() */

/* CH341 pins, wired as on the Meshtoad/MeshTadpole (E22 module). */
#define PIN_CS 0
#define PIN_RXEN 1
#define PIN_RESET 2
#define PIN_BUSY 4
#define PIN_IRQ 6 /* DIO1 */

/* SX126x commands. */
#define CMD_SET_STANDBY 0x80
#define CMD_SET_RX 0x82
#define CMD_SET_TX 0x83
#define CMD_WRITE_BUFFER 0x0E
#define CMD_SET_TX_PARAMS 0x8E
#define CMD_SET_PA_CONFIG 0x95
#define CMD_SET_PACKET_TYPE 0x8A
#define CMD_SET_RF_FREQ 0x86
#define CMD_SET_BUF_BASE 0x8F
#define CMD_SET_MOD_PARAMS 0x8B
#define CMD_SET_PACKET_PARAMS 0x8C
#define CMD_SET_DIO_IRQ 0x08
#define CMD_GET_IRQ 0x12
#define CMD_CLR_IRQ 0x02
#define CMD_GET_RX_BUF_STATUS 0x13
#define CMD_READ_BUFFER 0x1E
#define CMD_GET_PACKET_STATUS 0x14
#define CMD_SET_REGULATOR 0x96
#define CMD_CALIBRATE 0x89
#define CMD_CALIBRATE_IMAGE 0x98
#define CMD_SET_DIO3_TCXO 0x97
#define CMD_SET_DIO2_RFSW 0x9D

#define REG_LORA_SYNC_MSB 0x0740

/* IRQ bits. */
#define IRQ_TX_DONE 0x0001
#define IRQ_RX_DONE 0x0002
#define IRQ_CRC_ERR 0x0040
#define IRQ_HEADER_ERR 0x0020
#define IRQ_TIMEOUT 0x0200

static struct lora_cfg cur_cfg; /* remembered for re-arming RX after a TX */
static int cur_power = 22;       /* transmit power in dBm */

void sx_set_tx_power(int dbm)
{
    cur_power = dbm < -9 ? -9 : dbm > 22 ? 22 : dbm;
}

static unsigned long long ms(void)
{
    return ticks_to_millisecs(gettime());
}

static int wait_busy(int timeout_ms)
{
    unsigned long long t0 = ms();

    for (;;) {
        int b = ch341_read_pin(PIN_BUSY);
        if (b == 0)
            return 0;
        if (b < 0 || ms() - t0 > (unsigned long long)timeout_ms)
            return -1;
        usleep(200);
    }
}

/* Send opcode + args, then read nresp bytes after them (NOPs). */
static int cmd(uint8_t op, const uint8_t *args, int nargs, uint8_t *resp, int nresp)
{
    uint8_t tx[300], rx[300];
    int n = 1 + nargs + nresp;

    if (n > (int)sizeof(tx) || wait_busy(100) != 0)
        return -1;
    tx[0] = op;
    if (nargs > 0)
        memcpy(tx + 1, args, (size_t)nargs);
    memset(tx + 1 + nargs, 0, (size_t)nresp);
    if (ch341_spi(tx, rx, n) != 0)
        return -1;
    if (resp != NULL)
        memcpy(resp, rx + 1 + nargs, (size_t)nresp);
    return 0;
}

static int write_reg(uint16_t addr, const uint8_t *data, int n)
{
    uint8_t a[16];

    a[0] = (uint8_t)(addr >> 8);
    a[1] = (uint8_t)addr;
    memcpy(a + 2, data, (size_t)n);
    return cmd(0x0D, a, 2 + n, NULL, 0);
}

/* Kept for register reads a later bring-up step will need. */
__attribute__((unused)) static int read_reg(uint16_t addr, uint8_t *data, int n)
{
    uint8_t a[2] = { (uint8_t)(addr >> 8), (uint8_t)addr }, r[17];

    /* After the address the chip sends one status byte, then the data. */
    if (cmd(0x1D, a, 2, r, n + 1) != 0)
        return -1;
    memcpy(data, r + 1, (size_t)n);
    return 0;
}

/* Pulse the reset pin and wait for the chip to come out of busy. */
static int hw_reset(void)
{
    if (ch341_write_pin(PIN_RESET, 0) != 0)
        return -1;
    usleep(2000);
    if (ch341_write_pin(PIN_RESET, 1) != 0)
        return -1;
    usleep(5000);
    return wait_busy(100);
}

int sx_open(char *info, size_t infolen, char *err, size_t errlen)
{
    char chinfo[80] = "";
    uint8_t st = 0;
    int mode;
    uint8_t stdby = 0x00;       /* STDBY_RC */
    uint8_t reg = 0x01;         /* DC-DC + LDO */
    uint8_t tcxo[4] = { 0x02, 0x00, 0x01, 0x40 }; /* 1.8 V, 5 ms settle */
    uint8_t rfsw = 0x01;

    if (ch341_open(PIN_CS, (1 << PIN_RXEN) | (1 << PIN_RESET),
                   chinfo, sizeof(chinfo), err, errlen) != 0)
        return -1;

    if (hw_reset() != 0) {
        snprintf(err, errlen, "radio reset: BUSY never cleared");
        ch341_close();
        return -1;
    }
    if (cmd(CMD_SET_STANDBY, &stdby, 1, NULL, 0) != 0 ||
        cmd(CMD_SET_REGULATOR, &reg, 1, NULL, 0) != 0 ||
        cmd(CMD_SET_DIO3_TCXO, tcxo, 4, NULL, 0) != 0 ||
        cmd(CMD_SET_DIO2_RFSW, &rfsw, 1, NULL, 0) != 0) {
        snprintf(err, errlen, "radio standby setup failed (no SPI reply?)");
        ch341_close();
        return -1;
    }

    /* Confirm the chip answers: GetStatus must report it in standby (STDBY_RC
     * = chip-mode 2), not 0x00/0xFF which mean a dead SPI line. */
    if (cmd(0xC0, NULL, 0, &st, 1) != 0) {
        snprintf(err, errlen, "radio GetStatus failed (no SPI reply)");
        ch341_close();
        return -1;
    }
    mode = (st >> 4) & 0x7;
    /* 0x00 and 0xFF mean a dead/stuck MISO line; any other value means the
     * chip is clocking real status back. (GetStatus's exact mode bits are
     * unreliable over the CH341, so don't gate on them.) */
    if (st == 0x00 || st == 0xFF) {
        snprintf(err, errlen, "radio not answering: status %02x", st);
        ch341_close();
        return -1;
    }
    (void)mode;
    snprintf(info, infolen, "SX1262 up (status %02x) on %s", st, chinfo);
    return 0;
}

int sx_start_rx(const struct lora_cfg *c)
{
    uint8_t ptype = 0x01; /* LoRa */
    uint8_t freq[4], mod[4], pkt[6], sync[2], base[2] = { 0, 0 };
    uint8_t irq[8], rx[3] = { 0xFF, 0xFF, 0xFF }; /* continuous */
    uint8_t calimg[2];
    uint8_t cal = 0x7F;
    uint32_t fr;
    int bwc, ldro;

    cur_cfg = *c;

    /* RX path on (E22 RXen), TX path off. */
    if (ch341_write_pin(PIN_RXEN, 1) != 0)
        return -1;

    if (cmd(CMD_CALIBRATE, &cal, 1, NULL, 0) != 0)
        return -1;
    usleep(5000);
    if (cmd(CMD_SET_PACKET_TYPE, &ptype, 1, NULL, 0) != 0)
        return -1;

    /* Frequency: f_reg = f_hz * 2^25 / 32 MHz. */
    fr = (uint32_t)(((uint64_t)c->freq_hz << 25) / 32000000ULL);
    freq[0] = (uint8_t)(fr >> 24);
    freq[1] = (uint8_t)(fr >> 16);
    freq[2] = (uint8_t)(fr >> 8);
    freq[3] = (uint8_t)fr;
    /* Image calibration for the band around this frequency. */
    if (c->freq_hz >= 902000000U) {
        calimg[0] = 0xE1;
        calimg[1] = 0xE9; /* 902-928 MHz */
    } else if (c->freq_hz >= 863000000U) {
        calimg[0] = 0xD7;
        calimg[1] = 0xDB; /* 863-870 MHz */
    } else {
        calimg[0] = 0x6B;
        calimg[1] = 0x6F; /* 470-510 MHz */
    }
    if (cmd(CMD_CALIBRATE_IMAGE, calimg, 2, NULL, 0) != 0 ||
        cmd(CMD_SET_RF_FREQ, freq, 4, NULL, 0) != 0)
        return -1;

    /* Modulation: SF, BW, CR, low-data-rate optimize. */
    bwc = c->bw_khz == 500 ? 0x06 : c->bw_khz == 250 ? 0x05 : 0x04;
    ldro = ((long)(1 << c->sf) * 1000 / c->bw_khz) > 16000 ? 1 : 0;
    mod[0] = (uint8_t)c->sf;
    mod[1] = (uint8_t)bwc;
    mod[2] = (uint8_t)(c->cr - 4); /* 5->1 (4/5) .. 8->4 (4/8) */
    mod[3] = (uint8_t)ldro;
    if (cmd(CMD_SET_MOD_PARAMS, mod, 4, NULL, 0) != 0)
        return -1;

    /* Packet: preamble, explicit header, max length, CRC on, standard IQ. */
    pkt[0] = (uint8_t)(c->preamble >> 8);
    pkt[1] = (uint8_t)c->preamble;
    pkt[2] = 0x00; /* variable length (explicit header) */
    pkt[3] = 0xFF; /* max payload we will accept */
    pkt[4] = 0x01; /* CRC on */
    pkt[5] = 0x00; /* standard IQ */
    if (cmd(CMD_SET_PACKET_PARAMS, pkt, 6, NULL, 0) != 0)
        return -1;

    /* LoRa sync word, as RadioLib writes it (control bits 0x44). */
    sync[0] = (uint8_t)((c->sync & 0xF0) | 0x04);
    sync[1] = (uint8_t)(((c->sync & 0x0F) << 4) | 0x04);
    if (write_reg(REG_LORA_SYNC_MSB, sync, 2) != 0)
        return -1;

    if (cmd(CMD_SET_BUF_BASE, base, 2, NULL, 0) != 0)
        return -1;

    /* Route RX-done, CRC and header errors to the IRQ bits (and DIO1). */
    {
        uint16_t mask = IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_HEADER_ERR;
        irq[0] = (uint8_t)(mask >> 8);
        irq[1] = (uint8_t)mask; /* IrqMask */
        irq[2] = irq[0];
        irq[3] = irq[1]; /* DIO1 */
        irq[4] = 0;
        irq[5] = 0; /* DIO2 */
        irq[6] = 0;
        irq[7] = 0; /* DIO3 */
    }
    if (cmd(CMD_SET_DIO_IRQ, irq, 8, NULL, 0) != 0)
        return -1;

    return cmd(CMD_SET_RX, rx, 3, NULL, 0);
}

int sx_poll(uint8_t *buf, int max, int *rssi, float *snr)
{
    uint8_t st[3], rb[3], hdr, off, tmp[258], ps[4];
    uint16_t irq;
    int len;

    if (cmd(CMD_GET_IRQ, NULL, 0, st, 3) != 0)
        return 0;
    irq = (uint16_t)((st[1] << 8) | st[2]);
    if (!(irq & (IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_HEADER_ERR)))
        return 0;

    /* Clear everything we routed before touching the buffer. */
    {
        uint8_t clr[2] = { st[1], st[2] };
        cmd(CMD_CLR_IRQ, clr, 2, NULL, 0);
    }
    if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR))
        return -1;
    if (!(irq & IRQ_RX_DONE))
        return 0;

    if (cmd(CMD_GET_RX_BUF_STATUS, NULL, 0, rb, 3) != 0)
        return 0;
    len = rb[1];
    off = rb[2];
    if (len <= 0)
        return 0;
    if (len > max)
        len = max;

    /* ReadBuffer: offset arg, then one status byte, then the payload. */
    hdr = off;
    if (cmd(CMD_READ_BUFFER, &hdr, 1, tmp, len + 1) != 0)
        return 0;
    memcpy(buf, tmp + 1, (size_t)len);

    if (cmd(CMD_GET_PACKET_STATUS, NULL, 0, ps, 4) == 0) {
        if (rssi != NULL)
            *rssi = -(int)ps[1] / 2;
        if (snr != NULL)
            *snr = (float)((int8_t)ps[2]) / 4.0f;
    }
    return len;
}

int sx_send(const uint8_t *buf, int len)
{
    uint8_t stdby = 0x00, base[2] = { 0, 0 }, pkt[6], irq[8], tx[3] = { 0, 0, 0 };
    uint8_t pa[4] = { 0x04, 0x07, 0x00, 0x01 };   /* +22 dBm PA config (SX1262) */
    uint8_t txp[2] = { (uint8_t)(int8_t)cur_power, 0x04 }; /* power dBm, 200 us ramp */
    uint8_t wb[257], clr[2] = { 0xFF, 0xFF };
    uint16_t mask;
    unsigned long long t0;
    int done = 0;

    if (len <= 0 || len > 255)
        return -1;

    if (cmd(CMD_SET_STANDBY, &stdby, 1, NULL, 0) != 0 ||
        cmd(CMD_SET_PA_CONFIG, pa, 4, NULL, 0) != 0 ||
        cmd(CMD_SET_TX_PARAMS, txp, 2, NULL, 0) != 0)
        return -1;

    /* Packet params for TX: same preamble, explicit header, this length, CRC. */
    pkt[0] = (uint8_t)(cur_cfg.preamble >> 8);
    pkt[1] = (uint8_t)cur_cfg.preamble;
    pkt[2] = 0x00;
    pkt[3] = (uint8_t)len;
    pkt[4] = 0x01;
    pkt[5] = 0x00;
    if (cmd(CMD_SET_PACKET_PARAMS, pkt, 6, NULL, 0) != 0 ||
        cmd(CMD_SET_BUF_BASE, base, 2, NULL, 0) != 0)
        return -1;

    wb[0] = 0x00; /* write offset */
    memcpy(wb + 1, buf, (size_t)len);
    if (cmd(CMD_WRITE_BUFFER, wb, len + 1, NULL, 0) != 0)
        return -1;

    mask = IRQ_TX_DONE | IRQ_TIMEOUT;
    irq[0] = (uint8_t)(mask >> 8);
    irq[1] = (uint8_t)mask;
    irq[2] = irq[0];
    irq[3] = irq[1];
    irq[4] = 0;
    irq[5] = 0;
    irq[6] = 0;
    irq[7] = 0;
    if (cmd(CMD_SET_DIO_IRQ, irq, 8, NULL, 0) != 0 ||
        cmd(CMD_CLR_IRQ, clr, 2, NULL, 0) != 0)
        return -1;

    /* E22: RX path off, TX path on (DIO2 drives the switch). */
    ch341_write_pin(PIN_RXEN, 0);
    if (cmd(CMD_SET_TX, tx, 3, NULL, 0) != 0) {
        ch341_write_pin(PIN_RXEN, 1);
        return -1;
    }

    t0 = ms();
    while (ms() - t0 < 8000) {
        uint8_t st[3];
        if (cmd(CMD_GET_IRQ, NULL, 0, st, 3) == 0) {
            uint16_t s = (uint16_t)((st[1] << 8) | st[2]);
            if (s & (IRQ_TX_DONE | IRQ_TIMEOUT)) {
                done = (s & IRQ_TX_DONE) != 0;
                break;
            }
        }
        usleep(2000);
    }
    cmd(CMD_CLR_IRQ, clr, 2, NULL, 0);

    /* Back to listening regardless, so RX keeps working. */
    sx_start_rx(&cur_cfg);
    return done ? 0 : -1;
}

int sx_rssi_inst(void)
{
    uint8_t r[2];

    if (cmd(0x15, NULL, 0, r, 2) != 0) /* GetRssiInst */
        return 1;
    return -(int)r[1] / 2;
}

void sx_debug_reads(void)
{
    uint8_t tx[8], rx[8];

    /* GetStatus (0xC0): wide read so we see which byte holds the status. */
    memset(tx, 0, sizeof(tx));
    tx[0] = 0xC0;
    if (ch341_spi(tx, rx, 5) == 0)
        dbg("sx dbg GetStatus raw: %02x %02x %02x %02x %02x",
            rx[0], rx[1], rx[2], rx[3], rx[4]);

    /* GetRssiInst (0x15). */
    memset(tx, 0, sizeof(tx));
    tx[0] = 0x15;
    if (ch341_spi(tx, rx, 5) == 0)
        dbg("sx dbg GetRssiInst raw: %02x %02x %02x %02x %02x",
            rx[0], rx[1], rx[2], rx[3], rx[4]);

    /* ReadRegister sync word 0x0740 (we wrote 0x24 0xB4). Find the offset. */
    memset(tx, 0, sizeof(tx));
    tx[0] = 0x1D;
    tx[1] = 0x07;
    tx[2] = 0x40;
    if (ch341_spi(tx, rx, 8) == 0)
        dbg("sx dbg ReadReg 0740 raw (want 24 b4): %02x %02x %02x %02x %02x %02x %02x %02x",
            rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);
}

void sx_close(void)
{
    ch341_close();
}

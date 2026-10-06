/******************************************************************************
 *
 * rfdc_ctl.c  -  Host CLI for configuring the ZU47DR RFSoC over PCIe
 *
 * Layering:
 *     rfdc_ctl.c      CLI / user interface
 *       -> cmd_*()    command manager: builds a ring word, sends, waits ACK
 *          -> pcie_*  access layer (pcie_access.c)
 *
 * Handshake implemented (the only one the register map supports):
 *     1. write RFDC_configuration  (payload)
 *     2. write ring_register with NEW_CMD set
 *     3. poll ring_register until NEW_CMD clears  == device acknowledged
 *     4. timeout -> report failure
 *
 * NOTE: the map defines no status or error register, so a cleared NEW_CMD
 * means "processed", not "succeeded". The device reports rejection reasons on
 * its own console only. See the GAPS section of pcie_regs.h.
 *
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>

#include "pcie_access.h"
#include "pcie_regs.h"

#define ACK_TIMEOUT_MS_DEFAULT  2000
#define ACK_POLL_INTERVAL_US    200

static PcieDev  g_dev;
static int      g_timeout_ms = ACK_TIMEOUT_MS_DEFAULT;
static int      g_verbose    = 0;

/*---------------------------------------------------------------------------
 * Small helpers
 *---------------------------------------------------------------------------*/
static const char *pcie_err_str(unsigned e)
{
    switch (e) {
    case 0x0: return "no error";
    case 0x1: return "invalid TARGET field";
    case 0x2: return "invalid TILE field";
    case 0x3: return "invalid CHANNEL field";
    case 0x4: return "unknown EVENT code";
    case 0x5: return "payload out of range";
    case 0x6: return "EVENT not valid for this TARGET";
    case 0x7: return "tile/block not enabled in this design";
    case 0x8: return "driver rejected the setting";
    case 0x9: return "back-to-back tile encoding unsupported";
    case 0xA: return "manager not initialised";
    case 0xB: return "known code, not wired up";
    default:  return "unknown error";
    }
}

static void msleep_us(long us)
{
    struct timespec ts = { us / 1000000L, (us % 1000000L) * 1000L };
    nanosleep(&ts, NULL);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*---------------------------------------------------------------------------
 * Command manager
 *---------------------------------------------------------------------------*/

/* Build the ring word. NEW_CMD is added by cmd_send(). */
static uint32_t ring_build(uint32_t target, uint32_t tile, int last_tile,
                           uint32_t channel, uint32_t event)
{
    uint32_t r = 0;
    r |= RING_SET(TARGET,  target);
    r |= RING_SET(TILE,    tile);
    if (last_tile) r |= RING_LASTTILE_MASK;
    r |= RING_SET(CHANNEL, channel);
    r |= RING_SET(EVENT,   event);
    return r;
}

/* Full write-then-wait transaction. Returns 0 on ACK, -ETIMEDOUT on timeout. */
static int cmd_send(uint32_t ring_word, uint32_t payload)
{
    int rc;

    /* Refuse to start if the device has not consumed the previous command. */
    uint32_t cur = 0;
    rc = pcie_read_register(&g_dev, PCIE_REG_RING, &cur);
    if (rc) return rc;
    if (cur & RING_NEWCMD_MASK) {
        fprintf(stderr,
                "error: device still has NEW_CMD set (ring=0x%08x).\n"
                "       Previous command not yet processed, or firmware is not "
                "polling.\n", cur);
        return -EBUSY;
    }

    /* 1. payload first - the device latches it before acknowledging. */
    rc = pcie_write_register(&g_dev, PCIE_REG_CFG, payload);
    if (rc) return rc;

    /* 2. command word with NEW_CMD */
    rc = pcie_write_register(&g_dev, PCIE_REG_RING, ring_word | RING_NEWCMD_MASK);
    if (rc) return rc;

    if (g_verbose)
        fprintf(stderr, "sent: ring=0x%08x cfg=0x%08x\n",
                ring_word | RING_NEWCMD_MASK, payload);

    /* 3. wait for the device to clear NEW_CMD */
    long deadline = now_ms() + g_timeout_ms;
    for (;;) {
        uint32_t v = 0;
        rc = pcie_read_register(&g_dev, PCIE_REG_RING, &v);
        if (rc) return rc;

        if ((v & RING_NEWCMD_MASK) == 0) {
            if (g_verbose) fprintf(stderr, "ack: ring=0x%08x\n", v);

            /* The target may embed a result code in the acknowledge word.
             * Older firmware just clears NEW_CMD - then the marker is absent
             * and we fall back to "processed, outcome unknown". */
            if (RING_ACK_VALID(v)) {
                unsigned e = RING_ACK_ERR(v);
                if (e != 0) {
                    fprintf(stderr, "device REJECTED the command: %s (0x%X)\n",
                            pcie_err_str(e), e);
                    return -EIO;
                }
                if (g_verbose) fprintf(stderr, "device reported success\n");
            } else if (g_verbose) {
                fprintf(stderr, "note: no status marker in ACK; "
                                "command processed, outcome unknown\n");
            }
            return 0;
        }
        if (now_ms() > deadline) {
            fprintf(stderr,
                    "error: timeout after %d ms waiting for ACK (ring=0x%08x).\n"
                    "       The firmware did not clear NEW_CMD. Check that the\n"
                    "       bare-metal application is running and polling.\n",
                    g_timeout_ms, v);
            return -ETIMEDOUT;
        }
        msleep_us(ACK_POLL_INTERVAL_US);
    }
}

/*---------------------------------------------------------------------------
 * Payload encoders (exactly as specified in the register document)
 *---------------------------------------------------------------------------*/

/* NCO frequency: magnitude in kHz, bit31 = sign */
static uint32_t enc_nco_freq_mhz(double mhz)
{
    int neg = (mhz < 0.0);
    double khz = (neg ? -mhz : mhz) * 1000.0;
    uint32_t mag = (uint32_t)(khz + 0.5) & PCIE_PAYLOAD_MAG_MASK;
    return neg ? (mag | PCIE_PAYLOAD_SIGN_MASK) : mag;
}

/* NCO phase: integer degrees, bit31 = sign */
static uint32_t enc_nco_phase_deg(int deg)
{
    int neg = (deg < 0);
    uint32_t mag = (uint32_t)(neg ? -deg : deg) & PCIE_PAYLOAD_MAG_MASK;
    return neg ? (mag | PCIE_PAYLOAD_SIGN_MASK) : mag;
}

/* DDS: Hz */
static uint32_t enc_dds_hz(double mhz)
{
    return (uint32_t)(mhz * 1000000.0 + 0.5);
}

static int factor_ok(uint32_t f)
{
    static const uint32_t ok[] = { 1,2,3,4,5,6,8,10,12,16,20,24,40 };
    for (size_t i = 0; i < sizeof(ok)/sizeof(ok[0]); i++)
        if (ok[i] == f) return 1;
    return 0;
}

/*---------------------------------------------------------------------------
 * Argument parsing helpers
 *---------------------------------------------------------------------------*/
static int parse_target(const char *s, uint32_t *out)
{
    if (!strcasecmp(s, "adc"))      { *out = PCIE_TARGET_ADC; return 0; }
    if (!strcasecmp(s, "dac"))      { *out = PCIE_TARGET_DAC; return 0; }
    if (!strcasecmp(s, "dds"))      { *out = PCIE_TARGET_DDS; return 0; }
    if (!strcasecmp(s, "both"))     { *out = PCIE_TARGET_ADC_AND_DAC; return 0; }
    fprintf(stderr, "error: target must be adc | dac | dds | both\n");
    return -EINVAL;
}

static int parse_tile(const char *s, uint32_t *out)
{
    if (!strcasecmp(s, "all")) { *out = PCIE_TILE_ALL; return 0; }
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (*end || v < 0 || v > 3) {
        fprintf(stderr, "error: tile must be 0..3 or 'all'\n");
        return -EINVAL;
    }
    *out = (uint32_t)v;
    return 0;
}

static int parse_channel(const char *s, uint32_t *out)
{
    if (!strcasecmp(s, "both")) { *out = PCIE_CHAN_BOTH; return 0; }
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (*end || v < 0 || v > 1) {
        fprintf(stderr, "error: channel must be 0, 1 or 'both'\n");
        return -EINVAL;
    }
    *out = (uint32_t)v;
    return 0;
}

/*---------------------------------------------------------------------------
 * Usage
 *---------------------------------------------------------------------------*/
static void usage(const char *me)
{
    printf(
"ZU47DR RFSoC PCIe control tool\n"
"\n"
"Usage: %s [connection options] <command> [args]\n"
"\n"
"Connection options:\n"
"  -b, --bdf <BDF>        sysfs backend, e.g. 0000:01:00.0\n"
"  -r, --bar <n>          BAR index for the sysfs backend (default 0)\n"
"  -c, --chardev <path>   character-device backend, e.g. /dev/xdma0_user\n"
"  -o, --regbase <hex>    offset of the control block inside the BAR (default 0)\n"
"  -t, --timeout <ms>     ACK timeout (default %d)\n"
"  -v, --verbose          print every register transaction\n"
"\n"
"Commands:\n"
"  list                                  list PCIe endpoints\n"
"  dump                                  read the two control registers\n"
"  raw-read  <off_hex>                   read one register\n"
"  raw-write <off_hex> <val_hex>         write one register\n"
"\n"
"  nco-freq    <adc|dac|both> <tile|all> <ch|both> <MHz>\n"
"  nco-phase   <adc|dac|both> <tile|all> <ch|both> <deg -180..180>\n"
"  decim       <tile|all> <ch|both> <factor>          (ADC)\n"
"  interp      <tile|all> <ch|both> <factor>          (DAC)\n"
"  dds-freq    <MHz>                                  (PL DDS compiler)\n"
"\n"
"Extended commands (EVENT codes beyond Register_mapping.docx - see docs):\n"
"  sample-rate <adc|dac> <MHz>       reprogram PLL, re-align MTS, restore NCO\n"
"  nyquist     <adc|dac> <1|2>       force Nyquist zone\n"
"  dsa         <tile|all> <ch|both> <dB 0..27>        ADC attenuation\n"
"  qmc-gain    <adc|dac|both> <tile|all> <ch|both> <0..1.9999>\n"
"  dac-vop     <tile|all> <ch|both> <mA 2.25..40.5>\n"
"  axis-route  <ch0> <ch1> <ch2> <ch3>                each 0..7\n"
"  gpio-route  <1|2|3>               PL channel-select logic\n"
"  dac-source  <dds|host|0|1>        0/dds = DDS tone, 1/host = H2C stream\n"
"  mts                               re-align MTS + restore NCOs\n"
"  start                             enable all FIFOs\n"
"  stop                              disable all FIFOs\n"
"  reset                             soft reset (MTS + NCO + routing)\n"
"  ping                              handshake test, changes nothing\n"
"\n"
"  Valid decim/interp factors: 1 2 3 4 5 6 8 10 12 16 20 24 40\n"
"\n"
"Everything the UART menu offers is now reachable over PCIe. Commands above\n"
"the divider use the documented EVENT codes; the rest are EXTENSIONS that\n"
"reuse the same two registers and handshake, and must be agreed with the\n"
"FPGA team. See docs/PCIE_MIGRATION.md.\n"
"\n"
"Examples:\n"
"  %s -b 0000:3c:00.0 -r 2 ping\n"
"  %s -b 0000:3c:00.0 -r 2 nco-freq adc all both 3100\n"
"  %s -b 0000:3c:00.0 -r 2 sample-rate adc 4000\n"
"\n", me, ACK_TIMEOUT_MS_DEFAULT, me, me, me);
}

/*---------------------------------------------------------------------------
 * main
 *---------------------------------------------------------------------------*/
int main(int argc, char **argv)
{
    const char *bdf = NULL, *chardev = NULL;
    int bar = 0;
    uint64_t regbase = 0;

    static struct option lo[] = {
        {"bdf",     required_argument, 0, 'b'},
        {"bar",     required_argument, 0, 'r'},
        {"chardev", required_argument, 0, 'c'},
        {"regbase", required_argument, 0, 'o'},
        {"timeout", required_argument, 0, 't'},
        {"verbose", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "b:r:c:o:t:vh", lo, NULL)) != -1) {
        switch (opt) {
        case 'b': bdf = optarg; break;
        case 'r': bar = atoi(optarg); break;
        case 'c': chardev = optarg; break;
        case 'o': regbase = strtoull(optarg, NULL, 0); break;
        case 't': g_timeout_ms = atoi(optarg); break;
        case 'v': g_verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    if (optind >= argc) { usage(argv[0]); return 2; }
    const char *cmd = argv[optind++];
    int rem = argc - optind;
    char **a = &argv[optind];

    /* ---- commands that need no device ---- */
    if (!strcmp(cmd, "list")) {
        char found[32][32];
        int n = pcie_find_devices(-1, -1, found, 32);
        if (n < 0) { fprintf(stderr, "error: %s\n", strerror(-n)); return 1; }
        printf("PCIe endpoints (%d):\n", n);
        for (int i = 0; i < n; i++) printf("  %s\n", found[i]);
        printf("\nIdentify yours with:  lspci -D -nn\n");
        return 0;
    }

    /* ---- open ---- */
    int rc;
    if (chardev)      rc = pcie_open_chardev(&g_dev, chardev, regbase);
    else if (bdf)     rc = pcie_open_sysfs(&g_dev, bdf, bar, regbase);
    else {
        fprintf(stderr, "error: specify --bdf or --chardev (see --help)\n");
        return 2;
    }
    if (rc) return 1;
    g_dev.verbose = g_verbose;

    int ret = 0;
    uint32_t ring = 0, payload = 0, target = 0, tile = 0, chan = 0;

    if (!strcmp(cmd, "dump")) {
        uint32_t r = 0, c = 0;
        if (pcie_read_register(&g_dev, PCIE_REG_RING, &r) ||
            pcie_read_register(&g_dev, PCIE_REG_CFG,  &c)) { ret = 1; goto out; }
        printf("ring_register      (0x%02X) = 0x%08X\n", PCIE_REG_RING, r);
        printf("  NEW_CMD  [31]    = %u\n", (unsigned)RING_GET(r, NEWCMD));
        printf("  TARGET   [30:28] = %u\n", (unsigned)RING_GET(r, TARGET));
        printf("  LASTTILE [27]    = %u\n", (unsigned)RING_GET(r, LASTTILE));
        printf("  TILE     [26:24] = %u\n", (unsigned)RING_GET(r, TILE));
        printf("  CHANNEL  [23:20] = %u\n", (unsigned)RING_GET(r, CHANNEL));
        printf("  EVENT    [19:0]  = 0x%05X\n", (unsigned)RING_GET(r, EVENT));
        printf("RFDC_configuration (0x%02X) = 0x%08X\n", PCIE_REG_CFG, c);
    }
    else if (!strcmp(cmd, "raw-read") && rem == 1) {
        uint32_t off = (uint32_t)strtoul(a[0], NULL, 0), v = 0;
        if (pcie_read_register(&g_dev, off, &v)) { ret = 1; goto out; }
        printf("[0x%04X] = 0x%08X\n", off, v);
    }
    else if (!strcmp(cmd, "raw-write") && rem == 2) {
        uint32_t off = (uint32_t)strtoul(a[0], NULL, 0);
        uint32_t val = (uint32_t)strtoul(a[1], NULL, 0);
        if (pcie_write_register(&g_dev, off, val)) { ret = 1; goto out; }
        printf("wrote [0x%04X] = 0x%08X\n", off, val);
    }
    else if (!strcmp(cmd, "nco-freq") && rem == 4) {
        if (parse_target(a[0], &target) || parse_tile(a[1], &tile) ||
            parse_channel(a[2], &chan)) { ret = 2; goto out; }
        if (target == PCIE_TARGET_DDS) {
            fprintf(stderr, "error: use dds-freq for the DDS target\n");
            ret = 2; goto out;
        }
        double mhz = atof(a[3]);
        payload = enc_nco_freq_mhz(mhz);
        ring = ring_build(target, tile, 1, chan, PCIE_EVT_NCO_FREQ);
        printf("NCO frequency -> %.3f MHz (payload 0x%08X)\n", mhz, payload);
        ret = cmd_send(ring, payload) ? 1 : 0;
    }
    else if (!strcmp(cmd, "nco-phase") && rem == 4) {
        if (parse_target(a[0], &target) || parse_tile(a[1], &tile) ||
            parse_channel(a[2], &chan)) { ret = 2; goto out; }
        int deg = atoi(a[3]);
        if (deg < -PCIE_PHASE_MAX_DEG || deg > PCIE_PHASE_MAX_DEG) {
            fprintf(stderr, "error: phase must be -180..180 degrees\n");
            ret = 2; goto out;
        }
        payload = enc_nco_phase_deg(deg);
        ring = ring_build(target, tile, 1, chan, PCIE_EVT_NCO_PHASE);
        printf("NCO phase -> %d deg (payload 0x%08X)\n", deg, payload);
        ret = cmd_send(ring, payload) ? 1 : 0;
    }
    else if (!strcmp(cmd, "decim") && rem == 3) {
        if (parse_tile(a[0], &tile) || parse_channel(a[1], &chan)) { ret = 2; goto out; }
        uint32_t f = (uint32_t)strtoul(a[2], NULL, 0);
        if (!factor_ok(f)) { fprintf(stderr, "error: unsupported factor %u\n", f); ret = 2; goto out; }
        payload = f;
        ring = ring_build(PCIE_TARGET_ADC, tile, 1, chan, PCIE_EVT_DECIMATION);
        printf("ADC decimation -> x%u\n", f);
        ret = cmd_send(ring, payload) ? 1 : 0;
    }
    else if (!strcmp(cmd, "interp") && rem == 3) {
        if (parse_tile(a[0], &tile) || parse_channel(a[1], &chan)) { ret = 2; goto out; }
        uint32_t f = (uint32_t)strtoul(a[2], NULL, 0);
        if (!factor_ok(f)) { fprintf(stderr, "error: unsupported factor %u\n", f); ret = 2; goto out; }
        payload = f;
        ring = ring_build(PCIE_TARGET_DAC, tile, 1, chan, PCIE_EVT_INTERPOLATION);
        printf("DAC interpolation -> x%u\n", f);
        ret = cmd_send(ring, payload) ? 1 : 0;
    }
    else if (!strcmp(cmd, "dds-freq") && rem == 1) {
        double mhz = atof(a[0]);
        if (mhz <= 0.0 || mhz > 75.0) {
            fprintf(stderr, "error: DDS frequency must be 0 < f <= 75 MHz\n");
            ret = 2; goto out;
        }
        payload = enc_dds_hz(mhz);
        ring = ring_build(PCIE_TARGET_DDS, 0, 1, 0, PCIE_EVT_DDS_PHASE_INC);
        printf("DDS frequency -> %.6f MHz (payload 0x%08X Hz)\n", mhz, payload);
        ret = cmd_send(ring, payload) ? 1 : 0;
    }
    else if (!strcmp(cmd, "sample-rate") && rem == 2) {
        if (parse_target(a[0], &target)) { ret = 2; goto out; }
        if (target != PCIE_TARGET_ADC && target != PCIE_TARGET_DAC) {
            fprintf(stderr, "error: sample-rate takes adc or dac (one domain)\n");
            ret = 2; goto out;
        }
        unsigned long mhz = strtoul(a[1], NULL, 0);
        payload = (uint32_t)mhz;
        ring = ring_build(target, PCIE_TILE_ALL, 1, PCIE_CHAN_BOTH,
                          PCIE_EVT_SAMPLING_RATE);
        printf("Sampling rate -> %lu MHz (%s)\n", mhz, a[0]);
        printf("NOTE: this reprograms the PLL, re-aligns MTS and restores NCOs.\n"
               "      Allow several seconds.\n");
        ret = cmd_send(ring, payload) ? 1 : 0;
    }
    else if (!strcmp(cmd, "nyquist") && rem == 2) {
        if (parse_target(a[0], &target)) { ret = 2; goto out; }
        uint32_t z = (uint32_t)strtoul(a[1], NULL, 0);
        if (z != 1 && z != 2) { fprintf(stderr, "error: zone must be 1 or 2\n"); ret = 2; goto out; }
        ring = ring_build(target, PCIE_TILE_ALL, 1, PCIE_CHAN_BOTH, PCIE_EVT_NYQUIST_ZONE);
        printf("Nyquist zone -> %u\n", z);
        ret = cmd_send(ring, z) ? 1 : 0;
    }
    else if (!strcmp(cmd, "dsa") && rem == 3) {
        if (parse_tile(a[0], &tile) || parse_channel(a[1], &chan)) { ret = 2; goto out; }
        double db = atof(a[2]);
        if (db < 0.0 || db > 27.0) { fprintf(stderr, "error: DSA must be 0..27 dB\n"); ret = 2; goto out; }
        payload = (uint32_t)(db * 10.0 + 0.5);
        ring = ring_build(PCIE_TARGET_ADC, tile, 1, chan, PCIE_EVT_ADC_DSA);
        printf("ADC DSA -> %.1f dB\n", db);
        ret = cmd_send(ring, payload) ? 1 : 0;
    }
    else if (!strcmp(cmd, "qmc-gain") && rem == 4) {
        if (parse_target(a[0], &target) || parse_tile(a[1], &tile) ||
            parse_channel(a[2], &chan)) { ret = 2; goto out; }
        double g = atof(a[3]);
        if (g < 0.0 || g > 1.9999) { fprintf(stderr, "error: gain must be 0..1.9999\n"); ret = 2; goto out; }
        payload = (uint32_t)(g * 10000.0 + 0.5);
        ring = ring_build(target, tile, 1, chan, PCIE_EVT_QMC_GAIN);
        printf("QMC gain -> %.4f\n", g);
        ret = cmd_send(ring, payload) ? 1 : 0;
    }
    else if (!strcmp(cmd, "dac-vop") && rem == 3) {
        if (parse_tile(a[0], &tile) || parse_channel(a[1], &chan)) { ret = 2; goto out; }
        double ma = atof(a[2]);
        payload = (uint32_t)(ma * 1000.0 + 0.5);
        if (payload < 2250 || payload > 40500) {
            fprintf(stderr, "error: VOP must be 2.25..40.5 mA\n"); ret = 2; goto out;
        }
        ring = ring_build(PCIE_TARGET_DAC, tile, 1, chan, PCIE_EVT_DAC_VOP);
        printf("DAC VOP -> %.2f mA\n", ma);
        ret = cmd_send(ring, payload) ? 1 : 0;
    }
    else if (!strcmp(cmd, "axis-route") && rem == 4) {
        uint32_t c0 = (uint32_t)strtoul(a[0], NULL, 0);
        uint32_t c1 = (uint32_t)strtoul(a[1], NULL, 0);
        uint32_t c2 = (uint32_t)strtoul(a[2], NULL, 0);
        uint32_t c3 = (uint32_t)strtoul(a[3], NULL, 0);
        if (c0 > 7 || c1 > 7 || c2 > 7 || c3 > 7) {
            fprintf(stderr, "error: each channel must be 0..7\n"); ret = 2; goto out;
        }
        payload = c0 | (c1 << 4) | (c2 << 8) | (c3 << 12);
        ring = ring_build(PCIE_TARGET_ADC, PCIE_TILE_ALL, 1, PCIE_CHAN_BOTH,
                          PCIE_EVT_AXIS_ROUTING);
        printf("AXIS routing -> MI0<-SI%u MI1<-SI%u MI2<-SI%u MI3<-SI%u\n", c0, c1, c2, c3);
        ret = cmd_send(ring, payload) ? 1 : 0;
    }
    else if (!strcmp(cmd, "gpio-route") && rem == 1) {
        uint32_t m = (uint32_t)strtoul(a[0], NULL, 0);
        if (m < 1 || m > 3) { fprintf(stderr, "error: mode must be 1..3\n"); ret = 2; goto out; }
        ring = ring_build(PCIE_TARGET_ADC, PCIE_TILE_ALL, 1, PCIE_CHAN_BOTH,
                          PCIE_EVT_PL_GPIO_ROUTING);
        printf("PL GPIO routing -> mode %u\n", m);
        ret = cmd_send(ring, m) ? 1 : 0;
    }
    else if (!strcmp(cmd, "dac-source") && rem == 1) {
        /* Logical values (pcie_regs.h PCIE_DAC_SRC_*); the firmware maps them to
         * the switch port. Words are accepted so the numbering never has to be
         * remembered. */
        uint32_t v;
        if      (!strcmp(a[0], "dds")  || !strcmp(a[0], "0")) v = PCIE_DAC_SRC_DDS;
        else if (!strcmp(a[0], "host") || !strcmp(a[0], "1")) v = PCIE_DAC_SRC_HOST;
        else { fprintf(stderr, "error: dac-source takes dds (0) or host (1)\n"); ret = 2; goto out; }
        ring = ring_build(PCIE_TARGET_DAC, PCIE_TILE_ALL, 1, PCIE_CHAN_BOTH,
                          PCIE_EVT_DAC_SOURCE);
        printf("DAC input source -> %s\n",
               v == PCIE_DAC_SRC_HOST ? "host / GNU Radio stream" : "DDS compiler");
        ret = cmd_send(ring, v) ? 1 : 0;
    }
    else if (!strcmp(cmd, "mts") && rem == 0) {
        ring = ring_build(PCIE_TARGET_ADC, PCIE_TILE_ALL, 1, PCIE_CHAN_BOTH,
                          PCIE_EVT_MTS_REALIGN);
        printf("MTS re-align...\n");
        ret = cmd_send(ring, 0) ? 1 : 0;
    }
    else if (!strcmp(cmd, "start") && rem == 0) {
        ring = ring_build(PCIE_TARGET_ADC_AND_DAC, PCIE_TILE_ALL, 1, PCIE_CHAN_BOTH,
                          PCIE_EVT_START);
        printf("START (enable FIFOs)\n");
        ret = cmd_send(ring, 0) ? 1 : 0;
    }
    else if (!strcmp(cmd, "stop") && rem == 0) {
        ring = ring_build(PCIE_TARGET_ADC_AND_DAC, PCIE_TILE_ALL, 1, PCIE_CHAN_BOTH,
                          PCIE_EVT_STOP);
        printf("STOP (disable FIFOs)\n");
        ret = cmd_send(ring, 0) ? 1 : 0;
    }
    else if (!strcmp(cmd, "reset") && rem == 0) {
        ring = ring_build(PCIE_TARGET_ADC_AND_DAC, PCIE_TILE_ALL, 1, PCIE_CHAN_BOTH,
                          PCIE_EVT_RESET);
        printf("Soft reset (MTS re-align + NCO restore + routing)\n");
        ret = cmd_send(ring, 0) ? 1 : 0;
    }
    else if (!strcmp(cmd, "ping") && rem == 0) {
        ring = ring_build(PCIE_TARGET_ADC, PCIE_TILE_ALL, 1, PCIE_CHAN_BOTH, PCIE_EVT_PING);
        printf("Ping...\n");
        ret = cmd_send(ring, 0) ? 1 : 0;
    }
    else {
        fprintf(stderr, "error: unknown command or wrong argument count: %s\n", cmd);
        usage(argv[0]);
        ret = 2;
    }

    if (ret == 0 && strcmp(cmd, "dump") && strncmp(cmd, "raw-", 4))
        printf("OK - device acknowledged.\n");

out:
    pcie_close_device(&g_dev);
    return ret;
}

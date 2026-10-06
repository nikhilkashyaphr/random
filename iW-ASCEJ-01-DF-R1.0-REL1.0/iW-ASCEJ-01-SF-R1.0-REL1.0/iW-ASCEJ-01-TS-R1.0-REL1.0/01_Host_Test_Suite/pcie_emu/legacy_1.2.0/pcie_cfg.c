/******************************************************************************
 *
 * pcie_cfg.c  -  PCIe register based configuration manager (bare-metal side)
 *
 * Flow, once per PcieCfg_Poll() call:
 *
 *     read ring_register
 *          |
 *     NEW_CMD set?  --no--> return 0
 *          | yes
 *     read RFDC_configuration        (payload FIRST, before clearing NEW_CMD)
 *          v
 *     decode -> validate -> apply    (apply reuses existing app functions)
 *          v
 *     clear NEW_CMD                  (this is the ACK the map defines)
 *
 * Ordering rationale: the payload is captured BEFORE the acknowledgement is
 * written, so the host cannot overwrite RFDC_configuration for the next
 * command while this one is still being read. That closes the only race the
 * two-register handshake exposes.
 *
 ******************************************************************************/

#include <stdio.h>
#include "pcie_cfg.h"
#include "main.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xstatus.h"

#if ENABLE_PCIE_CONFIG

/*---------------------------------------------------------------------------
 * Functions already implemented by the application. Reused as-is: this module
 * changes the transport, not the processing.
 *---------------------------------------------------------------------------*/
extern int  Update_Datapath_Rate(XRFdc *RFdcInstPtr, int is_adc, int target_tile,
                                 int target_block, u32 New_Factor);
extern void Set_NCO_Freq_MTS_Safe(XRFdc *RFdcInstPtr, int is_adc, double freq_mhz);
extern void Set_Dds_Frequency(double target_freq_mhz);
extern double current_adc_nco;
extern double current_dac_nco;
extern int    Change_Sampling_Rate_All_Tiles(XRFdc *p, int is_adc, double mhz);
extern void   Set_Nyquist_Zone_Manual(XRFdc *p, int is_adc, int zone);
extern void   Set_Adc_Channel_Routing(int c0, int c1, int c2, int c3);
extern void   Set_PL_Gpio_Routing(int mode);
extern void   Set_Dds_H2c_Routing(int si_port);
extern int    Run_MTS_Procedure(XRFdc *p);
extern void   Save_Gain_State(XRFdc *p, int is_adc);
extern void   Restore_Gain_State(XRFdc *p, int is_adc);
extern int    force_adc_zone;
extern double global_adc_gsps;
extern double global_dac_gsps;
extern double current_dds_freq_mhz;
extern double current_dac_stream_clk_mhz;
extern int    force_dac_zone;

/*---------------------------------------------------------------------------
 * Module state
 *---------------------------------------------------------------------------*/
static XRFdc       *g_RFdc      = NULL;
static UINTPTR      g_base      = (UINTPTR)PCIE_CTRL_BASEADDR;
static int          g_ready     = 0;
static PcieCfgStats g_stats;

/* The RFDC driver exposes no getter for these four, so the last applied value
 * is shadowed. Seeded with the boot defaults so a readback issued before any
 * command still reports the truth rather than zero. */
static u32 g_last_vop_ua       = PCIE_DEF_DAC_VOP_UA;
static u32 g_last_dac_source   = PCIE_DEF_DAC_SOURCE;
static u32 g_last_gpio_mode    = PCIE_DEF_PL_GPIO_MODE;
static u32 g_last_axis_routing = PCIE_DEF_AXIS_ROUTING;

/*---------------------------------------------------------------------------
 * Register access
 *---------------------------------------------------------------------------*/
static inline u32 pcie_rd(u32 off)
{
    return Xil_In32(g_base + off);
}

static inline void pcie_wr(u32 off, u32 val)
{
    Xil_Out32(g_base + off, val);
}

/*---------------------------------------------------------------------------
 * Status reporting. With the register map as supplied there is nowhere to put
 * a status word, so this degrades to a console message. If the optional
 * registers are added later, only this function changes.
 *---------------------------------------------------------------------------*/
static void pcie_report(PcieCfgError err, u32 event)
{
    g_stats.last_error = err;
    g_stats.last_event = event;

#if PCIE_CFG_HAVE_STATUS_REGS
    u32 st = (err == PCIE_ERR_NONE) ? PCIE_STATUS_DONE : PCIE_STATUS_ERROR;
    st |= (event & 0xFFFFFU) << 8;
    pcie_wr(PCIE_REG_STATUS, st);
    pcie_wr(PCIE_REG_ERRCODE, (u32)err);
#endif

    if (err != PCIE_ERR_NONE) {
        xil_printf("[pcie] REJECTED event 0x%05lx: %s\r\n",
                   (unsigned long)event, PcieCfg_ErrorString(err));
    }
}

const char *PcieCfg_ErrorString(PcieCfgError e)
{
    switch (e) {
    case PCIE_ERR_NONE:             return "no error";
    case PCIE_ERR_BAD_TARGET:       return "invalid TARGET field";
    case PCIE_ERR_BAD_TILE:         return "invalid TILE field";
    case PCIE_ERR_BAD_CHANNEL:      return "invalid CHANNEL field";
    case PCIE_ERR_BAD_EVENT:        return "unknown EVENT code";
    case PCIE_ERR_BAD_PAYLOAD:      return "payload out of range";
    case PCIE_ERR_TARGET_EVENT_MIX: return "EVENT not valid for this TARGET";
    case PCIE_ERR_BLOCK_DISABLED:   return "tile/block not enabled in this design";
    case PCIE_ERR_APPLY_FAILED:     return "driver rejected the setting";
    case PCIE_ERR_TILE_B2B_UNSUPP:  return "back-to-back tile encoding unsupported";
    case PCIE_ERR_NOT_INITIALISED:  return "manager not initialised";
    default:                        return "unknown error";
    }
}

/*---------------------------------------------------------------------------
 * Decode
 *---------------------------------------------------------------------------*/
static void pcie_decode(u32 ring, u32 cfg, PcieCfgCmd *c)
{
    c->raw_ring  = ring;
    c->raw_cfg   = cfg;
    c->target    = (u32)RING_GET(ring, TARGET);
    c->tile      = (u32)RING_GET(ring, TILE);
    c->last_tile = (u32)RING_GET(ring, LASTTILE);
    c->channel   = (u32)RING_GET(ring, CHANNEL);
    c->event     = (u32)RING_GET(ring, EVENT);
    c->negative  = (cfg & PCIE_PAYLOAD_SIGN_MASK) ? 1 : 0;
    c->magnitude = cfg & PCIE_PAYLOAD_MAG_MASK;
}

/*---------------------------------------------------------------------------
 * Validate. Rejects rather than clamps: a silently altered setting is worse
 * than a refused one.
 *---------------------------------------------------------------------------*/
static int factor_supported(u32 f)
{
    switch (f) {
    case PCIE_FACTOR_X1:  case PCIE_FACTOR_X2:  case PCIE_FACTOR_X3:
    case PCIE_FACTOR_X4:  case PCIE_FACTOR_X5:  case PCIE_FACTOR_X6:
    case PCIE_FACTOR_X8:  case PCIE_FACTOR_X10: case PCIE_FACTOR_X12:
    case PCIE_FACTOR_X16: case PCIE_FACTOR_X20: case PCIE_FACTOR_X24:
    case PCIE_FACTOR_X40:
        return 1;
    default:
        return 0;
    }
}

static PcieCfgError pcie_validate(const PcieCfgCmd *c)
{
    /* --- TARGET --- */
    switch (c->target) {
    case PCIE_TARGET_ADC:
    case PCIE_TARGET_DAC:
    case PCIE_TARGET_DDS:
    case PCIE_TARGET_ADC_AND_DAC:
        break;
    default:
        return PCIE_ERR_BAD_TARGET;
    }

    /* --- EVENT --- */
    switch (c->event) {
    case PCIE_EVT_NCO_FREQ:
    case PCIE_EVT_NCO_PHASE:
    case PCIE_EVT_INTERPOLATION:
    case PCIE_EVT_DECIMATION:
    case PCIE_EVT_DDS_PHASE_INC:
    case PCIE_EVT_SAMPLING_RATE:
    case PCIE_EVT_NYQUIST_ZONE:
    case PCIE_EVT_ADC_DSA:
    case PCIE_EVT_QMC_GAIN:
    case PCIE_EVT_AXIS_ROUTING:
    case PCIE_EVT_PL_GPIO_ROUTING:
    case PCIE_EVT_DAC_SOURCE:
    case PCIE_EVT_MTS_REALIGN:
    case PCIE_EVT_DAC_VOP:
    case PCIE_EVT_START:
    case PCIE_EVT_STOP:
    case PCIE_EVT_RESET:
    case PCIE_EVT_PING:
    case PCIE_EVT_READBACK:
    case PCIE_EVT_DEFAULTS:
        break;
    default:
        return PCIE_ERR_BAD_EVENT;
    }

    /* --- TARGET / EVENT consistency ---
     * Decimation only exists on ADCs, interpolation only on DACs, and the DDS
     * event only makes sense for the PL DDS compiler. Enforcing this stops a
     * malformed command from being applied to the wrong converter. */
    if (c->event == PCIE_EVT_DDS_PHASE_INC && c->target != PCIE_TARGET_DDS) {
        return PCIE_ERR_TARGET_EVENT_MIX;
    }
    if (c->target == PCIE_TARGET_DDS &&
        c->event != PCIE_EVT_DDS_PHASE_INC &&
        c->event != PCIE_EVT_PING) {
        return PCIE_ERR_TARGET_EVENT_MIX;
    }
    if (c->event == PCIE_EVT_DECIMATION &&
        !(c->target == PCIE_TARGET_ADC || c->target == PCIE_TARGET_ADC_AND_DAC)) {
        return PCIE_ERR_TARGET_EVENT_MIX;
    }
    if (c->event == PCIE_EVT_INTERPOLATION &&
        !(c->target == PCIE_TARGET_DAC || c->target == PCIE_TARGET_ADC_AND_DAC)) {
        return PCIE_ERR_TARGET_EVENT_MIX;
    }

    /* --- TILE / CHANNEL (not meaningful for the DDS target) --- */
    int global_evt = (c->event == PCIE_EVT_AXIS_ROUTING   ||
                      c->event == PCIE_EVT_PL_GPIO_ROUTING ||
                      c->event == PCIE_EVT_DAC_SOURCE      ||
                      c->event == PCIE_EVT_MTS_REALIGN     ||
                      c->event == PCIE_EVT_START           ||
                      c->event == PCIE_EVT_STOP            ||
                      c->event == PCIE_EVT_RESET           ||
                      c->event == PCIE_EVT_PING            ||
                      c->event == PCIE_EVT_DEFAULTS        ||
                      c->event == PCIE_EVT_SAMPLING_RATE   ||
                      c->event == PCIE_EVT_NYQUIST_ZONE);

    if (c->target != PCIE_TARGET_DDS && !global_evt) {
        if (c->tile == PCIE_TILE_BACK_TO_BACK) {
            /* Encoding is ambiguous in the source document - see pcie_regs.h
             * GAPS note 6. Refuse rather than guess. */
            return PCIE_ERR_TILE_B2B_UNSUPP;
        }
        if (c->tile > PCIE_TILE_3 && c->tile != PCIE_TILE_ALL) {
            return PCIE_ERR_BAD_TILE;
        }
        if (c->channel != PCIE_CHAN_0 &&
            c->channel != PCIE_CHAN_1 &&
            c->channel != PCIE_CHAN_BOTH) {
            return PCIE_ERR_BAD_CHANNEL;
        }
    }

    /* --- PAYLOAD --- */
    switch (c->event) {
    case PCIE_EVT_NCO_FREQ:
        /* magnitude is kHz; 0 is legal (NCO bypass/DC) */
        break;

    case PCIE_EVT_NCO_PHASE:
        if (c->magnitude > (u32)PCIE_PHASE_MAX_DEG) {
            return PCIE_ERR_BAD_PAYLOAD;
        }
        break;

    case PCIE_EVT_INTERPOLATION:
    case PCIE_EVT_DECIMATION:
        if (!factor_supported(c->magnitude)) {
            return PCIE_ERR_BAD_PAYLOAD;
        }
        break;

    case PCIE_EVT_DDS_PHASE_INC:
        /* Payload is Hz. The document notes the 32-bit path represents up to
         * ~75 MHz of DDS output; reject anything above that. */
        if (c->raw_cfg == 0U || c->raw_cfg > 75000000UL) {
            return PCIE_ERR_BAD_PAYLOAD;
        }
        break;

    case PCIE_EVT_SAMPLING_RATE:
        /* Gen 3 ZU47DR: ADC <= 5 GSPS, DAC <= 10 GSPS. Refuse anything the
         * silicon cannot reach rather than letting the PLL fail mid-teardown. */
        if (c->magnitude < 500U ||
            c->magnitude > ((c->target == PCIE_TARGET_ADC) ? 5000U : 10000U)) {
            return PCIE_ERR_BAD_PAYLOAD;
        }
        if (c->target != PCIE_TARGET_ADC && c->target != PCIE_TARGET_DAC) {
            return PCIE_ERR_TARGET_EVENT_MIX;   /* one domain at a time */
        }
        break;

    case PCIE_EVT_NYQUIST_ZONE:
        if (c->magnitude != 1U && c->magnitude != 2U) return PCIE_ERR_BAD_PAYLOAD;
        if (c->target != PCIE_TARGET_ADC && c->target != PCIE_TARGET_DAC)
            return PCIE_ERR_TARGET_EVENT_MIX;
        break;

    case PCIE_EVT_ADC_DSA:
        if (c->magnitude > 270U) return PCIE_ERR_BAD_PAYLOAD;   /* 27.0 dB */
        if (c->target != PCIE_TARGET_ADC) return PCIE_ERR_TARGET_EVENT_MIX;
        break;

    case PCIE_EVT_QMC_GAIN:
        if (c->magnitude > 19999U) return PCIE_ERR_BAD_PAYLOAD;  /* 1.9999 */
        break;

    case PCIE_EVT_DAC_VOP:
        /* Gen 3 DAC VOP range 2.25 mA .. 40.5 mA, in microamps. */
        if (c->magnitude < 2250U || c->magnitude > 40500U)
            return PCIE_ERR_BAD_PAYLOAD;
        if (c->target != PCIE_TARGET_DAC) return PCIE_ERR_TARGET_EVENT_MIX;
        break;

    case PCIE_EVT_AXIS_ROUTING: {
        for (int i = 0; i < 4; i++) {
            if (((c->raw_cfg >> (i * 4)) & 0xFU) > 7U) return PCIE_ERR_BAD_PAYLOAD;
        }
        break;
    }

    case PCIE_EVT_PL_GPIO_ROUTING:
        if (c->magnitude < 1U || c->magnitude > 3U) return PCIE_ERR_BAD_PAYLOAD;
        break;

    case PCIE_EVT_DAC_SOURCE:
        if (c->magnitude > 1U) return PCIE_ERR_BAD_PAYLOAD;
        break;

    case PCIE_EVT_MTS_REALIGN:
    case PCIE_EVT_START:
    case PCIE_EVT_STOP:
    case PCIE_EVT_RESET:
    case PCIE_EVT_PING:
    case PCIE_EVT_DEFAULTS:
        break;   /* payload ignored */

    case PCIE_EVT_READBACK:
        /* payload carries the tagged request id; the resolver range-checks */
        break;

    default:
        return PCIE_ERR_BAD_EVENT;
    }

    return PCIE_ERR_NONE;
}

/*---------------------------------------------------------------------------
 * Apply helpers
 *---------------------------------------------------------------------------*/

/* Map CHANNEL to the block indices this design actually implements.
 * ADC: blocks 0..3 enabled, channel 0/1 -> block 0/1.
 * DAC: only blocks 0 and 2 exist, channel 0/1 -> block 0/2. */
static int chan_to_block(int is_adc, u32 channel, int which)
{
    if (is_adc) {
        return (which == 0) ? 0 : 1;
    }
    (void)channel;
    return (which == 0) ? 0 : 2;
}

static int block_enabled(int is_adc, int tile, int block)
{
    return is_adc ? XRFdc_IsADCBlockEnabled(g_RFdc, tile, block)
                  : XRFdc_IsDACBlockEnabled(g_RFdc, tile, block);
}

/* Per-block NCO update, using the same freeze/unfreeze discipline as the
 * application's Set_NCO_Freq_MTS_Safe(). That function operates on ALL tiles,
 * so it is reused verbatim when the command targets all tiles and both
 * channels; this routine covers the per-tile / per-channel cases the register
 * map allows and the UART menu never exposed. */
static int nco_set_block(int is_adc, int tile, int block, double freq_mhz)
{
    XRFdc_Mixer_Settings mix;
    u32 type = is_adc ? XRFDC_ADC_TILE : XRFDC_DAC_TILE;

    if (XRFdc_GetMixerSettings(g_RFdc, type, tile, block, &mix) != XRFDC_SUCCESS) {
        return XST_FAILURE;
    }
    mix.Freq = freq_mhz;

    if (is_adc) {
        XRFdc_Cal_Freeze_Settings fz = { 0, 0, 0 };
        fz.DisableFreezePin  = 1;
        fz.FreezeCalibration = 1;
        XRFdc_SetCalFreeze(g_RFdc, tile, block, &fz);

        if (XRFdc_SetMixerSettings(g_RFdc, type, tile, block, &mix) != XRFDC_SUCCESS) {
            fz.FreezeCalibration = 0;
            XRFdc_SetCalFreeze(g_RFdc, tile, block, &fz);
            return XST_FAILURE;
        }
        XRFdc_UpdateEvent(g_RFdc, type, tile, block, XRFDC_EVENT_MIXER);

        fz.FreezeCalibration = 0;
        XRFdc_SetCalFreeze(g_RFdc, tile, block, &fz);
    } else {
        if (XRFdc_SetMixerSettings(g_RFdc, type, tile, block, &mix) != XRFDC_SUCCESS) {
            return XST_FAILURE;
        }
        XRFdc_UpdateEvent(g_RFdc, type, tile, block, XRFDC_EVENT_MIXER);
    }
    return XST_SUCCESS;
}

static int nco_set_phase_block(int is_adc, int tile, int block, double deg)
{
    XRFdc_Mixer_Settings mix;
    u32 type = is_adc ? XRFDC_ADC_TILE : XRFDC_DAC_TILE;

    if (XRFdc_GetMixerSettings(g_RFdc, type, tile, block, &mix) != XRFDC_SUCCESS) {
        return XST_FAILURE;
    }
    mix.PhaseOffset = deg;
    if (XRFdc_SetMixerSettings(g_RFdc, type, tile, block, &mix) != XRFDC_SUCCESS) {
        return XST_FAILURE;
    }
    XRFdc_UpdateEvent(g_RFdc, type, tile, block, XRFDC_EVENT_MIXER);
    return XST_SUCCESS;
}

/* Iterate the tile/channel selection and invoke fn() on each enabled block. */
typedef int (*BlockFn)(int is_adc, int tile, int block, double arg);

static PcieCfgError for_each_selected(const PcieCfgCmd *c, int is_adc,
                                      BlockFn fn, double arg)
{
    int tile_lo = (c->tile == PCIE_TILE_ALL) ? 0 : (int)c->tile;
    int tile_hi = (c->tile == PCIE_TILE_ALL) ? 3 : (int)c->tile;
    int touched = 0;

    for (int t = tile_lo; t <= tile_hi; t++) {
        int which_lo = (c->channel == PCIE_CHAN_1)    ? 1 : 0;
        int which_hi = (c->channel == PCIE_CHAN_BOTH) ? 1
                     : (c->channel == PCIE_CHAN_1)    ? 1 : 0;

        for (int w = which_lo; w <= which_hi; w++) {
            int blk = chan_to_block(is_adc, c->channel, w);
            if (!block_enabled(is_adc, t, blk)) {
                continue;   /* silently skip blocks the design does not build */
            }
            if (fn(is_adc, t, blk, arg) != XST_SUCCESS) {
                return PCIE_ERR_APPLY_FAILED;
            }
            touched++;
        }
    }

    return (touched == 0) ? PCIE_ERR_BLOCK_DISABLED : PCIE_ERR_NONE;
}

static int dsa_set_block(int is_adc, int tile, int block, double db)
{
    XRFdc_DSA_Settings d;
    (void)is_adc;                      /* DSA is ADC-only */
    if (XRFdc_GetDSA(g_RFdc, tile, block, &d) != XRFDC_SUCCESS) return XST_FAILURE;
    d.Attenuation = db;
    return (XRFdc_SetDSA(g_RFdc, tile, block, &d) == XRFDC_SUCCESS)
           ? XST_SUCCESS : XST_FAILURE;
}

static int qmc_set_block(int is_adc, int tile, int block, double gain)
{
    XRFdc_QMC_Settings q;
    u32 type = is_adc ? XRFDC_ADC_TILE : XRFDC_DAC_TILE;
    if (XRFdc_GetQMCSettings(g_RFdc, type, tile, block, &q) != XRFDC_SUCCESS)
        return XST_FAILURE;
    q.EnableGain           = 1;
    q.GainCorrectionFactor = gain;
    q.EventSource          = XRFDC_EVNT_SRC_TILE;
    if (XRFdc_SetQMCSettings(g_RFdc, type, tile, block, &q) != XRFDC_SUCCESS)
        return XST_FAILURE;
    XRFdc_UpdateEvent(g_RFdc, type, tile, block, XRFDC_EVNT_SRC_TILE);
    return XST_SUCCESS;
}

static int vop_set_block(int is_adc, int tile, int block, double microamps)
{
    (void)is_adc;                      /* VOP is DAC-only */
    return (XRFdc_SetDACVOP(g_RFdc, tile, block, (u32)microamps) == XRFDC_SUCCESS)
           ? XST_SUCCESS : XST_FAILURE;
}

/*---------------------------------------------------------------------------
 * READBACK resolver.
 *
 * Returns the live value for a parameter id so the host can display what the
 * hardware IS, not what it was last asked for. Values come from the driver
 * wherever a getter exists; the four without one are shadowed above.
 *---------------------------------------------------------------------------*/
static u32 pcie_readback(u32 id, int tile, int blk_adc, int blk_dac)
{
    u32 f = 0;
    XRFdc_Mixer_Settings mix;
    XRFdc_DSA_Settings   dsa;
    XRFdc_QMC_Settings   qmc;

    switch (id) {
    case PCIE_RB_ADC_FS_MHZ:  return (u32)(global_adc_gsps * 1000.0 + 0.5);
    case PCIE_RB_DAC_FS_MHZ:  return (u32)(global_dac_gsps * 1000.0 + 0.5);

    case PCIE_RB_DECIMATION:
        if (XRFdc_GetDecimationFactor(g_RFdc, tile, blk_adc, &f) != XRFDC_SUCCESS) return 0;
        return f;

    case PCIE_RB_INTERPOLATION:
        if (XRFdc_GetInterpolationFactor(g_RFdc, tile, blk_dac, &f) != XRFDC_SUCCESS) return 0;
        return f;

    case PCIE_RB_ADC_NCO_KHZ:
        if (XRFdc_GetMixerSettings(g_RFdc, XRFDC_ADC_TILE, tile, blk_adc, &mix) != XRFDC_SUCCESS)
            return 0;
        return (u32)(s32)(mix.Freq * 1000.0);

    case PCIE_RB_DAC_NCO_KHZ:
        if (XRFdc_GetMixerSettings(g_RFdc, XRFDC_DAC_TILE, tile, blk_dac, &mix) != XRFDC_SUCCESS)
            return 0;
        return (u32)(s32)(mix.Freq * 1000.0);

    case PCIE_RB_DDS_HZ:  return (u32)(current_dds_freq_mhz * 1000000.0 + 0.5);

    case PCIE_RB_ADC_DSA_DB10:
        if (XRFdc_GetDSA(g_RFdc, tile, blk_adc, &dsa) != XRFDC_SUCCESS) return 0;
        return (u32)(dsa.Attenuation * 10.0 + 0.5);

    case PCIE_RB_QMC_GAIN_10000:
        if (XRFdc_GetQMCSettings(g_RFdc, XRFDC_ADC_TILE, tile, blk_adc, &qmc) != XRFDC_SUCCESS)
            return 0;
        return (u32)(qmc.GainCorrectionFactor * 10000.0 + 0.5);

    case PCIE_RB_NYQUIST_ADC:  return (u32)force_adc_zone;
    case PCIE_RB_NYQUIST_DAC:  return (u32)force_dac_zone;

    case PCIE_RB_ADC_STREAM_KHZ:
        if (XRFdc_GetDecimationFactor(g_RFdc, tile, blk_adc, &f) != XRFDC_SUCCESS || f == 0)
            return 0;
        return (u32)((global_adc_gsps * 1000000.0) / (double)f + 0.5);

    case PCIE_RB_DAC_STREAM_KHZ:
        return (u32)(current_dac_stream_clk_mhz * 1000.0 + 0.5);

    case PCIE_RB_DAC_VOP_UA:    return g_last_vop_ua;
    case PCIE_RB_DAC_SOURCE:    return g_last_dac_source;
    case PCIE_RB_PL_GPIO_MODE:  return g_last_gpio_mode;
    case PCIE_RB_AXIS_ROUTING:  return g_last_axis_routing;

    /* Firmware identity, so a host can diagnose a version mismatch itself
     * instead of the operator having to read the serial console. */
    case PCIE_RB_ABI:           return (u32)PCIE_CFG_ABI;
    case PCIE_RB_CAPS:          return (u32)PCIE_CFG_CAPABILITIES;

    default:                    return 0;
    }
}

/* Adapter so Update_Datapath_Rate() fits the BlockFn signature. */
static int rate_set_block(int is_adc, int tile, int block, double arg)
{
    return Update_Datapath_Rate(g_RFdc, is_adc, tile, block, (u32)arg);
}

/*---------------------------------------------------------------------------
 * Apply
 *---------------------------------------------------------------------------*/
static PcieCfgError pcie_apply(const PcieCfgCmd *c)
{
    PcieCfgError e = PCIE_ERR_NONE;
    int all_blocks = (c->tile == PCIE_TILE_ALL) && (c->channel == PCIE_CHAN_BOTH);

    switch (c->event) {

    case PCIE_EVT_NCO_FREQ: {
        double mhz = PCIE_NCO_KHZ_TO_MHZ(c->magnitude);
        if (c->negative) mhz = -mhz;

        if (c->target == PCIE_TARGET_ADC || c->target == PCIE_TARGET_ADC_AND_DAC) {
            if (all_blocks) {
                /* exact reuse of the application's existing routine */
                Set_NCO_Freq_MTS_Safe(g_RFdc, 1, mhz);
                current_adc_nco = mhz;
            } else {
                e = for_each_selected(c, 1, nco_set_block, mhz);
                if (e != PCIE_ERR_NONE) return e;
            }
        }
        if (c->target == PCIE_TARGET_DAC || c->target == PCIE_TARGET_ADC_AND_DAC) {
            if (all_blocks) {
                Set_NCO_Freq_MTS_Safe(g_RFdc, 0, mhz);
                current_dac_nco = mhz;
            } else {
                e = for_each_selected(c, 0, nco_set_block, mhz);
                if (e != PCIE_ERR_NONE) return e;
            }
        }
        break;
    }

    case PCIE_EVT_NCO_PHASE: {
        double deg = (double)c->magnitude;
        if (c->negative) deg = -deg;

        if (c->target == PCIE_TARGET_ADC || c->target == PCIE_TARGET_ADC_AND_DAC) {
            e = for_each_selected(c, 1, nco_set_phase_block, deg);
            if (e != PCIE_ERR_NONE) return e;
        }
        if (c->target == PCIE_TARGET_DAC || c->target == PCIE_TARGET_ADC_AND_DAC) {
            e = for_each_selected(c, 0, nco_set_phase_block, deg);
            if (e != PCIE_ERR_NONE) return e;
        }
        break;
    }

    case PCIE_EVT_DECIMATION:
        e = for_each_selected(c, 1, rate_set_block, (double)c->magnitude);
        if (e != PCIE_ERR_NONE) return e;
        break;

    case PCIE_EVT_INTERPOLATION:
        e = for_each_selected(c, 0, rate_set_block, (double)c->magnitude);
        if (e != PCIE_ERR_NONE) return e;
        break;

    case PCIE_EVT_DDS_PHASE_INC:
        /* payload is Hz; the application function takes MHz */
        Set_Dds_Frequency(PCIE_DDS_HZ_TO_MHZ(c->raw_cfg));
        break;

    /* ------------------------- extended events ------------------------- */

    case PCIE_EVT_SAMPLING_RATE: {
        int is_adc = (c->target == PCIE_TARGET_ADC);
        /* Same sequence the UART menu uses: a PLL reprogram wipes QMC gain,
         * and MTS/NCO must be re-established afterwards. */
        Save_Gain_State(g_RFdc, is_adc);
        if (Change_Sampling_Rate_All_Tiles(g_RFdc, is_adc,
                                           (double)c->magnitude) != XST_SUCCESS) {
            return PCIE_ERR_APPLY_FAILED;
        }
        Restore_Gain_State(g_RFdc, is_adc);
        if (is_adc) Set_Adc_Channel_Routing(7, 3, 2, 1);
        Run_MTS_Procedure(g_RFdc);
        Set_NCO_Freq_MTS_Safe(g_RFdc, is_adc,
                              is_adc ? current_adc_nco : current_dac_nco);
        break;
    }

    case PCIE_EVT_NYQUIST_ZONE: {
        int is_adc = (c->target == PCIE_TARGET_ADC);
        int zone   = (int)c->magnitude;
        Set_Nyquist_Zone_Manual(g_RFdc, is_adc, zone);
        if (is_adc) force_adc_zone = zone; else force_dac_zone = zone;
        break;
    }

    case PCIE_EVT_ADC_DSA:
        e = for_each_selected(c, 1, dsa_set_block, (double)c->magnitude / 10.0);
        if (e != PCIE_ERR_NONE) return e;
        break;

    case PCIE_EVT_QMC_GAIN: {
        double g = (double)c->magnitude / 10000.0;
        if (c->target == PCIE_TARGET_ADC || c->target == PCIE_TARGET_ADC_AND_DAC) {
            e = for_each_selected(c, 1, qmc_set_block, g);
            if (e != PCIE_ERR_NONE) return e;
        }
        if (c->target == PCIE_TARGET_DAC || c->target == PCIE_TARGET_ADC_AND_DAC) {
            e = for_each_selected(c, 0, qmc_set_block, g);
            if (e != PCIE_ERR_NONE) return e;
        }
        break;
    }

    case PCIE_EVT_DAC_VOP:
        e = for_each_selected(c, 0, vop_set_block, (double)c->magnitude);
        if (e != PCIE_ERR_NONE) return e;
        g_last_vop_ua = c->magnitude;
        break;

    case PCIE_EVT_AXIS_ROUTING:
        Set_Adc_Channel_Routing((int)( c->raw_cfg        & 0xF),
                                (int)((c->raw_cfg >>  4) & 0xF),
                                (int)((c->raw_cfg >>  8) & 0xF),
                                (int)((c->raw_cfg >> 12) & 0xF));
        g_last_axis_routing = c->raw_cfg & 0xFFFFU;
        break;

    case PCIE_EVT_PL_GPIO_ROUTING:
        Set_PL_Gpio_Routing((int)c->magnitude);
        g_last_gpio_mode = c->magnitude;
        break;

    case PCIE_EVT_DAC_SOURCE:
        Set_Dds_H2c_Routing((int)c->magnitude);
        g_last_dac_source = c->magnitude;
        break;

    case PCIE_EVT_MTS_REALIGN:
    case PCIE_EVT_RESET:
        /* RESET is deliberately NOT a converter power cycle - that would drop
         * the clock tree and need the full LMK/LMX sequence again. Re-align
         * and restore is what recovery actually needs. */
        Run_MTS_Procedure(g_RFdc);
        Set_NCO_Freq_MTS_Safe(g_RFdc, 1, current_adc_nco);
        Set_NCO_Freq_MTS_Safe(g_RFdc, 0, current_dac_nco);
        if (c->event == PCIE_EVT_RESET) {
            Set_Adc_Channel_Routing(7, 3, 2, 1);
            xil_printf("[pcie] soft reset complete\r\n");
        }
        break;

    case PCIE_EVT_START:
    case PCIE_EVT_STOP: {
        u32 on = (c->event == PCIE_EVT_START) ? 1U : 0U;
        for (int t = 0; t < 4; t++) {
            if (XRFdc_IsADCBlockEnabled(g_RFdc, t, 0))
                XRFdc_SetupFIFO(g_RFdc, XRFDC_ADC_TILE, t, on);
            if (XRFdc_IsDACBlockEnabled(g_RFdc, t, 0))
                XRFdc_SetupFIFO(g_RFdc, XRFDC_DAC_TILE, t, on);
        }
        xil_printf("[pcie] datapath %s\r\n", on ? "STARTED" : "STOPPED");
        break;
    }

    case PCIE_EVT_PING:
        xil_printf("[pcie] ping\r\n");
        break;

    case PCIE_EVT_READBACK: {
        /* The request tag arrives in the payload; the answer OVERWRITES that
         * register. If the PL makes RFDC_configuration read-only to the PS the
         * write is dropped, the host sees its own tag come back, and reports
         * "readback unsupported" rather than trusting a stale value. */
        u32 id  = c->raw_cfg & 0xFFFFU;
        int t   = (c->tile == PCIE_TILE_ALL) ? 0 : (int)c->tile;
        int ba  = (c->channel == PCIE_CHAN_1) ? 1 : 0;   /* ADC block */
        int bd  = (c->channel == PCIE_CHAN_1) ? 2 : 0;   /* DAC block */
        pcie_wr(PCIE_REG_CFG, pcie_readback(id, t, ba, bd));
        break;
    }

    case PCIE_EVT_DEFAULTS: {
        /* Restore what main() establishes at boot, so "reset to defaults" and
         * a power cycle agree. The sampling rates are only reprogrammed when
         * they have actually drifted: a PLL cycle is disruptive and slow. */
        xil_printf("[pcie] restoring boot defaults...\r\n");

        if ((u32)(global_adc_gsps * 1000.0 + 0.5) != PCIE_DEF_ADC_FS_MHZ) {
            Save_Gain_State(g_RFdc, 1);
            Change_Sampling_Rate_All_Tiles(g_RFdc, 1, (double)PCIE_DEF_ADC_FS_MHZ);
            Restore_Gain_State(g_RFdc, 1);
        }
        if ((u32)(global_dac_gsps * 1000.0 + 0.5) != PCIE_DEF_DAC_FS_MHZ) {
            Save_Gain_State(g_RFdc, 0);
            Change_Sampling_Rate_All_Tiles(g_RFdc, 0, (double)PCIE_DEF_DAC_FS_MHZ);
            Restore_Gain_State(g_RFdc, 0);
        }

        for (int t = 0; t < 4; t++) {
            for (int b = 0; b < 4; b++) {
                if (XRFdc_IsADCBlockEnabled(g_RFdc, t, b)) {
                    Update_Datapath_Rate(g_RFdc, 1, t, b, PCIE_DEF_DECIMATION);
                    dsa_set_block(1, t, b, (double)PCIE_DEF_ADC_DSA_DB10 / 10.0);
                }
                if (XRFdc_IsDACBlockEnabled(g_RFdc, t, b)) {
                    Update_Datapath_Rate(g_RFdc, 0, t, b, PCIE_DEF_INTERPOLATION);
                    vop_set_block(0, t, b, (double)PCIE_DEF_DAC_VOP_UA);
                }
            }
        }
        g_last_vop_ua = PCIE_DEF_DAC_VOP_UA;

        Set_Adc_Channel_Routing((PCIE_DEF_AXIS_ROUTING      ) & 0xF,
                                (PCIE_DEF_AXIS_ROUTING >>  4) & 0xF,
                                (PCIE_DEF_AXIS_ROUTING >>  8) & 0xF,
                                (PCIE_DEF_AXIS_ROUTING >> 12) & 0xF);
        g_last_axis_routing = PCIE_DEF_AXIS_ROUTING;

        Set_PL_Gpio_Routing(PCIE_DEF_PL_GPIO_MODE);
        g_last_gpio_mode = PCIE_DEF_PL_GPIO_MODE;

        Set_Dds_H2c_Routing(PCIE_DEF_DAC_SOURCE);
        g_last_dac_source = PCIE_DEF_DAC_SOURCE;

        force_adc_zone = 0;      /* back to automatic Nyquist selection */
        force_dac_zone = 0;

        Run_MTS_Procedure(g_RFdc);
        current_adc_nco = (double)PCIE_DEF_NCO_MHZ;
        current_dac_nco = (double)PCIE_DEF_NCO_MHZ;
        Set_NCO_Freq_MTS_Safe(g_RFdc, 1, current_adc_nco);
        Set_NCO_Freq_MTS_Safe(g_RFdc, 0, current_dac_nco);
        Set_Dds_Frequency((double)PCIE_DEF_DDS_MHZ);

        xil_printf("[pcie] defaults restored.\r\n");
        break;
    }

    default:
        return PCIE_ERR_BAD_EVENT;
    }

    return PCIE_ERR_NONE;
}

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
int PcieCfg_Init(XRFdc *RFdcInstPtr)
{
    if (RFdcInstPtr == NULL) {
        return XST_FAILURE;
    }
    g_RFdc = RFdcInstPtr;

    g_stats.commands_seen     = 0;
    g_stats.commands_applied  = 0;
    g_stats.commands_rejected = 0;
    g_stats.last_error        = PCIE_ERR_NONE;
    g_stats.last_event        = 0;

    /* Clear any NEW_CMD left set by a previous run, so we do not replay a
     * stale command at startup. */
    u32 ring = pcie_rd(PCIE_REG_RING);
    if (ring & RING_NEWCMD_MASK) {
        pcie_wr(PCIE_REG_RING, ring & ~RING_NEWCMD_MASK);
        xil_printf("[pcie] cleared stale NEW_CMD (ring was 0x%08lx)\r\n",
                   (unsigned long)ring);
    }

#if PCIE_CFG_HAVE_STATUS_REGS
    pcie_wr(PCIE_REG_STATUS, 0);
    pcie_wr(PCIE_REG_ERRCODE, PCIE_ERR_NONE);
#endif

    g_ready = 1;
    xil_printf("[pcie] config manager ready @ 0x%08lx "
               "(ring +0x%02x, cfg +0x%02x)\r\n",
               (unsigned long)g_base, PCIE_REG_RING, PCIE_REG_CFG);

    /* State what this module actually is. main.c and pcie_cfg.c can be
     * flashed out of step, and nothing else distinguishes the combinations. */
    xil_printf("[pcie] pcie_cfg ABI %d, built %s %s, caps 0x%02lx: "
               "basic%s%s%s%s\r\n",
               PCIE_CFG_ABI, __DATE__, __TIME__,
               (unsigned long)PCIE_CFG_CAPABILITIES,
               (PCIE_CFG_CAPABILITIES & PCIE_CAP_EXTENDED)  ? " extended" : "",
               (PCIE_CFG_CAPABILITIES & PCIE_CAP_STARTSTOP) ? " start/stop" : "",
               (PCIE_CFG_CAPABILITIES & PCIE_CAP_READBACK)  ? " READBACK(0x0F1)" : "",
               (PCIE_CFG_CAPABILITIES & PCIE_CAP_DEFAULTS)  ? " DEFAULTS(0x023)" : "");
    return XST_SUCCESS;
}

int PcieCfg_Poll(void)
{
    PcieCfgCmd   cmd;
    PcieCfgError err;
    u32 ring, cfg;

    if (!g_ready) {
        return 0;
    }

    ring = pcie_rd(PCIE_REG_RING);
    if ((ring & RING_NEWCMD_MASK) == 0U) {
        return 0;                       /* nothing pending - common path */
    }

    /* Capture the payload BEFORE acknowledging, so the host cannot race us. */
    cfg = pcie_rd(PCIE_REG_CFG);

#if PCIE_CFG_HAVE_STATUS_REGS
    pcie_wr(PCIE_REG_STATUS, PCIE_STATUS_BUSY);
#endif

    g_stats.commands_seen++;
    pcie_decode(ring, cfg, &cmd);

#if PCIE_CFG_VERBOSE
    xil_printf("[pcie] cmd ring=0x%08lx cfg=0x%08lx "
               "| tgt=%lu tile=%lu ch=%lu evt=0x%05lx\r\n",
               (unsigned long)ring, (unsigned long)cfg,
               (unsigned long)cmd.target, (unsigned long)cmd.tile,
               (unsigned long)cmd.channel, (unsigned long)cmd.event);
#endif

    err = pcie_validate(&cmd);
    if (err == PCIE_ERR_NONE) {
        err = pcie_apply(&cmd);
    }

    if (err == PCIE_ERR_NONE) {
        g_stats.commands_applied++;
#if PCIE_CFG_VERBOSE
        xil_printf("[pcie] applied.\r\n");
#endif
    } else {
        g_stats.commands_rejected++;
    }

    pcie_report(err, cmd.event);

    /* ACK: clear NEW_CMD. Re-read first so we do not clobber a field the host
     * may have updated, and preserve every other bit. */
#if PCIE_CFG_STATUS_IN_RING
    /* NEW_CMD clearing remains the contract; the result code rides along in
     * the same write, costing no extra hardware. */
    pcie_wr(PCIE_REG_RING, RING_ACK_BUILD(err, cmd.event));
#else
    ring = pcie_rd(PCIE_REG_RING);
    pcie_wr(PCIE_REG_RING, ring & ~RING_NEWCMD_MASK);
#endif

    return 1;
}

void PcieCfg_GetStats(PcieCfgStats *out)
{
    if (out != NULL) {
        *out = g_stats;
    }
}

UINTPTR PcieCfg_GetBase(void)
{
    return g_base;
}

void PcieCfg_SetBase(UINTPTR base)
{
    g_base = base;
    xil_printf("[pcie] base set to 0x%08lx\r\n", (unsigned long)base);
}

void PcieCfg_Diagnostics(void)
{
    u32 ring = pcie_rd(PCIE_REG_RING);
    u32 cfg  = pcie_rd(PCIE_REG_CFG);

    xil_printf("\r\n--- PCIe configuration interface ---\r\n");
    xil_printf("  pcie_cfg ABI    : %d  (built %s %s)\r\n",
               PCIE_CFG_ABI, __DATE__, __TIME__);
    xil_printf("  capabilities    : 0x%02lx%s%s\r\n",
               (unsigned long)PCIE_CFG_CAPABILITIES,
               (PCIE_CFG_CAPABILITIES & PCIE_CAP_READBACK) ? "  READBACK(0x0F1)" : "  no readback",
               (PCIE_CFG_CAPABILITIES & PCIE_CAP_DEFAULTS) ? "  DEFAULTS(0x023)" : "  no defaults");
    xil_printf("  base            : 0x%08lx\r\n", (unsigned long)g_base);
    xil_printf("  ring  (+0x%02x)   : 0x%08lx\r\n",
               PCIE_REG_RING, (unsigned long)ring);

    /* After a command completes the ring holds an ACKNOWLEDGEMENT, not a
     * command. Decoding it with the command field layout (as this used to)
     * printed nonsense like TARGET=2 TILE=2 CHANNEL=4 for a status word. */
    if (RING_ACK_VALID(ring)) {
        xil_printf("      (this is an ACK word, not a command)\r\n");
        xil_printf("      marker  [30:24]= 0x%02lx\r\n",
                   (unsigned long)((ring & RING_ACKMARK_MASK) >> RING_ACKMARK_SHIFT));
        xil_printf("      error   [23:20]= 0x%lx  %s\r\n",
                   (unsigned long)RING_ACK_ERR(ring),
                   PcieCfg_ErrorString((PcieCfgError)RING_ACK_ERR(ring)));
        xil_printf("      event   [19:0] = 0x%05lx  (the command reported on)\r\n",
                   (unsigned long)RING_GET(ring, EVENT));
    } else {
        xil_printf("      NEW_CMD [31]  = %lu\r\n", (unsigned long)RING_GET(ring, NEWCMD));
        xil_printf("      TARGET  [30:28]= %lu\r\n", (unsigned long)RING_GET(ring, TARGET));
        xil_printf("      TILE    [26:24]= %lu\r\n", (unsigned long)RING_GET(ring, TILE));
        xil_printf("      CHANNEL [23:20]= %lu\r\n", (unsigned long)RING_GET(ring, CHANNEL));
        xil_printf("      EVENT   [19:0] = 0x%05lx\r\n", (unsigned long)RING_GET(ring, EVENT));
    }
    xil_printf("  cfg   (+0x%02x)   : 0x%08lx\r\n",
               PCIE_REG_CFG, (unsigned long)cfg);
    xil_printf("  seen/applied/rejected : %lu / %lu / %lu\r\n",
               (unsigned long)g_stats.commands_seen,
               (unsigned long)g_stats.commands_applied,
               (unsigned long)g_stats.commands_rejected);
    xil_printf("  last error      : %s\r\n", PcieCfg_ErrorString(g_stats.last_error));
    xil_printf("------------------------------------\r\n");
}

#else /* !ENABLE_PCIE_CONFIG */

int PcieCfg_Init(XRFdc *RFdcInstPtr)
{
    (void)RFdcInstPtr;
    xil_printf("[pcie] disabled at build time (ENABLE_PCIE_CONFIG=0)\r\n");
    return XST_SUCCESS;
}

int  PcieCfg_Poll(void) { return 0; }

void PcieCfg_GetStats(PcieCfgStats *out)
{
    if (out != NULL) {
        PcieCfgStats z = { 0, 0, 0, PCIE_ERR_NOT_INITIALISED, 0 };
        *out = z;
    }
}

const char *PcieCfg_ErrorString(PcieCfgError e)
{
    (void)e;
    return "pcie config disabled";
}

UINTPTR PcieCfg_GetBase(void)          { return (UINTPTR)PCIE_CTRL_BASEADDR; }
void    PcieCfg_SetBase(UINTPTR base)  { (void)base; }

void PcieCfg_Diagnostics(void)
{
    xil_printf("\r\nPCIe configuration disabled at build time "
               "(ENABLE_PCIE_CONFIG=0).\r\n");
}

#endif /* ENABLE_PCIE_CONFIG */

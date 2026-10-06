/******************************************************************************
 *
 * pcie_regs.h  -  PCIe control register map (SHARED: bare-metal AND host)
 *
 * Transcribed verbatim from "Register_mapping.docx". Nothing here is invented;
 * anything the document did not define is listed in the GAPS section at the
 * bottom rather than guessed at.
 *
 * This file is compiled by BOTH the Zynq bare-metal application and the Linux
 * host tool. Keep it free of platform headers - fixed-width types only.
 *
 *============================================================================
 * REGISTER TABLE
 *============================================================================
 * Offset | Name               | Access (PS view) | Description
 * -------|--------------------|------------------|--------------------------
 * 0xA8   | ring_register      | R/W              | Command + target selection
 * 0xAC   | RFDC_configuration | RO               | Command payload
 *
 * ring_register bit fields
 * ------------------------
 *  [31]     NEW_CMD     1 = host has written a new command.
 *                       PS clears it to 0 after reading. This clear IS the
 *                       acknowledgement - the map defines no other ACK.
 *  [30:28]  TARGET      000 ADC, 001 DAC, 010 PL DDS compiler, 100 ADC+DAC
 *  [27]     LAST_TILE   see TILE note below
 *  [26:24]  TILE        000..011 = Tile 0..3, 111 = all tiles,
 *                       100 = back-to-back tile select
 *  [23:20]  CHANNEL     0000 Ch0, 0001 Ch1, 0010 both
 *  [19:0]   EVENT       1 NCO freq, 2 NCO phase, 3 interpolation,
 *                       4 decimation, 5 DDS phase increment
 *
 * RFDC_configuration payload encoding depends on EVENT:
 *   EVENT 1 (NCO freq)  : magnitude in kHz, bit[31] = sign (1 = negative)
 *                         e.g. 4.5 GHz -> 4500000 kHz -> 0x0044AA20
 *   EVENT 2 (NCO phase) : integer degrees 0..180, bit[31] = sign
 *                         e.g. +180 -> 0x000000B4, -180 -> 0x800000B4
 *   EVENT 3 (interp)    : factor value, see PCIE_FACTOR_* below
 *   EVENT 4 (decim)     : factor value, see PCIE_FACTOR_* below
 *   EVENT 5 (DDS freq)  : frequency in Hz
 *                         e.g. 10 MHz -> 10000000 -> 0x00989680
 *
 ******************************************************************************/

#ifndef PCIE_REGS_H_
#define PCIE_REGS_H_

#include <stdint.h>

/*---------------------------------------------------------------------------
 * Base address
 *
 * *** VERIFY BEFORE FIRST HARDWARE RUN ***
 * The register document gives OFFSETS only; it does not name the base. In the
 * block design the only PCIe control aperture is M00_PCIe_control:
 *
 *     XPAR_M00_PCIE_CONTROL_BASEADDR = 0xA00A0000  (64 KB window)
 *
 * NOTE on the block design as supplied: S00_PCIe_control and M00_PCIe_control
 * are EXTERNAL PORTS of the BD, wired to smartconnect_1. There is no register
 * IP at 0xA00A0000 inside this XSA - the address is a pass-through to logic
 * outside the block design (the QDMA / wrapper RTL). Confirm that the block
 * implementing ring_register and RFDC_configuration is reachable by the PS at
 * this base before relying on it. Override with -DPCIE_CTRL_BASEADDR=0x... if
 * your integration differs.
 *---------------------------------------------------------------------------*/
#ifndef PCIE_CTRL_BASEADDR
#define PCIE_CTRL_BASEADDR          0xA00A0000UL
#endif

/*---------------------------------------------------------------------------
 * Register offsets
 *---------------------------------------------------------------------------*/
#define PCIE_REG_RING               0x00A8U   /* ring_register             */
#define PCIE_REG_CFG                0x00ACU   /* RFDC_configuration        */

/*---------------------------------------------------------------------------
 * ring_register field masks / shifts
 *---------------------------------------------------------------------------*/
#define RING_NEWCMD_SHIFT           31U
#define RING_NEWCMD_MASK            (1U << RING_NEWCMD_SHIFT)

#define RING_TARGET_SHIFT           28U
#define RING_TARGET_MASK            (0x7U << RING_TARGET_SHIFT)

#define RING_LASTTILE_SHIFT         27U
#define RING_LASTTILE_MASK          (1U << RING_LASTTILE_SHIFT)

#define RING_TILE_SHIFT             24U
#define RING_TILE_MASK              (0x7U << RING_TILE_SHIFT)

/* Convenience: the document repeatedly treats [27:24] as one 4-bit field. */
#define RING_TILEFIELD_SHIFT        24U
#define RING_TILEFIELD_MASK         (0xFU << RING_TILEFIELD_SHIFT)

#define RING_CHANNEL_SHIFT          20U
#define RING_CHANNEL_MASK           (0xFU << RING_CHANNEL_SHIFT)

#define RING_EVENT_SHIFT            0U
#define RING_EVENT_MASK             (0xFFFFFU << RING_EVENT_SHIFT)

#define RING_GET(reg, name)         ((uint32_t)(((reg) & RING_##name##_MASK) >> RING_##name##_SHIFT))
#define RING_SET(name, val)         ((uint32_t)((((uint32_t)(val)) << RING_##name##_SHIFT) & RING_##name##_MASK))

/*---------------------------------------------------------------------------
 * TARGET  ring_register[30:28]
 *---------------------------------------------------------------------------*/
#define PCIE_TARGET_ADC             0x0U
#define PCIE_TARGET_DAC             0x1U
#define PCIE_TARGET_DDS             0x2U
#define PCIE_TARGET_ADC_AND_DAC     0x4U

/*---------------------------------------------------------------------------
 * TILE  ring_register[26:24], with [27] = LAST_TILE
 *
 * Document wording: "Tile 0: 0000 ... Tile 3: 0011, Back to back tile select:
 * 0100, ... on the last tile select 27th bit should be high, if your last tile
 * is Tile 2 the 4 bit register should be 1010. All tiles: 111".
 *
 * Reading 1010 as bit[27]=1 plus tile=010 (Tile 2) is self-consistent, so this
 * header treats [27] as a LAST marker over a 3-bit tile index. The value 0100
 * ("back to back tile select") does not fit that scheme - it would be tile 4,
 * which does not exist - so it is exposed as its own constant and rejected by
 * the bare-metal validator rather than guessed at. See GAPS note 6.
 *---------------------------------------------------------------------------*/
#define PCIE_TILE_0                 0x0U
#define PCIE_TILE_1                 0x1U
#define PCIE_TILE_2                 0x2U
#define PCIE_TILE_3                 0x3U
#define PCIE_TILE_BACK_TO_BACK      0x4U   /* ambiguous - see note above */
#define PCIE_TILE_ALL               0x7U

/*---------------------------------------------------------------------------
 * CHANNEL  ring_register[23:20]
 *---------------------------------------------------------------------------*/
#define PCIE_CHAN_0                 0x0U
#define PCIE_CHAN_1                 0x1U
#define PCIE_CHAN_BOTH              0x2U

/*---------------------------------------------------------------------------
 * EVENT  ring_register[19:0]
 *---------------------------------------------------------------------------*/
#define PCIE_EVT_NCO_FREQ           0x00001U
#define PCIE_EVT_NCO_PHASE          0x00002U
#define PCIE_EVT_INTERPOLATION      0x00003U
#define PCIE_EVT_DECIMATION         0x00004U
#define PCIE_EVT_DDS_PHASE_INC      0x00005U

/*---------------------------------------------------------------------------
 * Payload helpers
 *---------------------------------------------------------------------------*/
#define PCIE_PAYLOAD_SIGN_MASK      0x80000000U
#define PCIE_PAYLOAD_MAG_MASK       0x7FFFFFFFU

/* NCO frequency: magnitude in kHz. Returns MHz as a double. */
#define PCIE_NCO_KHZ_TO_MHZ(mag)    (((double)(mag)) / 1000.0)

/* NCO phase: integer degrees, valid magnitude 0..180 */
#define PCIE_PHASE_MAX_DEG          180

/* DDS: payload is Hz. Returns MHz. */
#define PCIE_DDS_HZ_TO_MHZ(hz)      (((double)(hz)) / 1000000.0)

/*---------------------------------------------------------------------------
 * Interpolation / decimation factors accepted by the map (exact list)
 *---------------------------------------------------------------------------*/
#define PCIE_FACTOR_X1              0x01U
#define PCIE_FACTOR_X2              0x02U
#define PCIE_FACTOR_X3              0x03U
#define PCIE_FACTOR_X4              0x04U
#define PCIE_FACTOR_X5              0x05U
#define PCIE_FACTOR_X6              0x06U
#define PCIE_FACTOR_X8              0x08U
#define PCIE_FACTOR_X10             0x0AU
#define PCIE_FACTOR_X12             0x0CU
#define PCIE_FACTOR_X16             0x10U
#define PCIE_FACTOR_X20             0x14U
#define PCIE_FACTOR_X24             0x18U
#define PCIE_FACTOR_X40             0x28U

/*===========================================================================
 * EXTENDED EVENTS  --  NOT part of Register_mapping.docx
 *
 * The supplied map defines 5 events in a 20-bit field. Everything below is an
 * EXTENSION that reuses the SAME two registers and the SAME handshake; no new
 * register address is invented. They exist so the host can reach the remaining
 * UART menu functions.
 *
 * *** THE FPGA / HOST TEAMS MUST AGREE THESE CODES. *** They are additions,
 * not documented hardware. Codes 0x001-0x005 above are untouched and remain
 * exactly as specified.
 *===========================================================================*/
#define PCIE_EVT_SAMPLING_RATE      0x00010U  /* payload = MHz                */
#define PCIE_EVT_NYQUIST_ZONE       0x00011U  /* payload = 1 or 2             */
#define PCIE_EVT_ADC_DSA            0x00012U  /* payload = dB x 10 (0..270)   */
#define PCIE_EVT_QMC_GAIN           0x00013U  /* payload = gain x 10000       */
#define PCIE_EVT_AXIS_ROUTING       0x00014U  /* payload = ch0|ch1<<4|ch2<<8|ch3<<12 */
#define PCIE_EVT_PL_GPIO_ROUTING    0x00015U  /* payload = 1..3               */
#define PCIE_EVT_DAC_SOURCE         0x00016U  /* payload = PCIE_DAC_SRC_*     */
#define PCIE_EVT_MTS_REALIGN        0x00017U  /* payload ignored              */
#define PCIE_EVT_DAC_VOP            0x00018U  /* payload = microamps          */

#define PCIE_EVT_START              0x00020U  /* enable FIFOs                 */
#define PCIE_EVT_STOP               0x00021U  /* disable FIFOs                */
#define PCIE_EVT_RESET              0x00022U  /* MTS re-align + restore NCOs  */

#define PCIE_EVT_PING               0x000F0U  /* no-op; handshake test        */
#define PCIE_EVT_READBACK           0x000F1U  /* payload = PCIE_RB_* id       */
#define PCIE_EVT_DEFAULTS           0x00023U  /* restore boot configuration   */

/*---------------------------------------------------------------------------
 * DAC input source  (PCIE_EVT_DAC_SOURCE payload, PCIE_RB_DAC_SOURCE value)
 *
 * These are LOGICAL values, deliberately not AXI4-Stream switch port numbers.
 * The bitstream wires the DDS_H2C_switch as
 *
 *     S00_AXIS  <-  H2C_data_in      (host / GNU Radio stream over PCIe)
 *     S01_AXIS  <-  dds_compiler_2   (internal DDS tone)
 *
 * and the firmware translates logical -> port in exactly one place
 * (platform_ids.h, HW_DAC_SW_SI_*). Firmware before 1.2.1 passed the value
 * straight through as a port number, which swapped DDS and host everywhere:
 * choosing "DDS" played the host stream and choosing "host" played the DDS.
 *---------------------------------------------------------------------------*/
#define PCIE_DAC_SRC_DDS            0U
#define PCIE_DAC_SRC_HOST           1U
#define PCIE_DAC_SRC_NONE           0xFFU  /* readback only: no input routed */

/*---------------------------------------------------------------------------
 * READBACK  (EVENT 0x0F1)
 *
 * The map has no read path, so the firmware answers in place: the host writes
 * a tagged request into RFDC_configuration, and the PS OVERWRITES that register
 * with the 32-bit result before acknowledging.
 *
 * Self-detecting: the request carries the tag 0x5242 ("RB") in [31:16]. If the
 * host reads that tag back unchanged, the PS could not write the register on
 * this hardware and readback is unsupported - as opposed to silently believing
 * a stale value.
 *
 *   host  -> cfg  = PCIE_RB_REQUEST(id)
 *   host  -> ring = NEW_CMD | EVENT 0x0F1   (tile/channel select the block)
 *   PS    -> cfg  = value
 *   PS    -> ring = ack
 *   host  <- cfg  = value   (or the tag still, meaning unsupported)
 *---------------------------------------------------------------------------*/
#define PCIE_RB_TAG                 0x5242U
#define PCIE_RB_REQUEST(id)         ((uint32_t)((PCIE_RB_TAG << 16) | ((id) & 0xFFFFU)))
#define PCIE_RB_IS_UNANSWERED(v)    ((((uint32_t)(v)) >> 16) == PCIE_RB_TAG)

#define PCIE_RB_ADC_FS_MHZ          0x01U  /* ADC sampling rate, MHz           */
#define PCIE_RB_DAC_FS_MHZ          0x02U  /* DAC sampling rate, MHz           */
#define PCIE_RB_DECIMATION          0x03U  /* factor, for the selected block   */
#define PCIE_RB_INTERPOLATION       0x04U  /* factor, for the selected block   */
#define PCIE_RB_ADC_NCO_KHZ         0x05U  /* signed, two's complement         */
#define PCIE_RB_DAC_NCO_KHZ         0x06U  /* signed, two's complement         */
#define PCIE_RB_DDS_HZ              0x07U
#define PCIE_RB_ADC_DSA_DB10        0x08U  /* dB x 10                          */
#define PCIE_RB_DAC_VOP_UA          0x09U  /* microamps                        */
#define PCIE_RB_NYQUIST_ADC         0x0AU
#define PCIE_RB_NYQUIST_DAC         0x0BU
#define PCIE_RB_DAC_SOURCE          0x0CU
#define PCIE_RB_QMC_GAIN_10000      0x0DU  /* gain x 10000                     */
#define PCIE_RB_ADC_STREAM_KHZ      0x0EU  /* ADC stream clock, kHz            */
#define PCIE_RB_DAC_STREAM_KHZ      0x0FU  /* DAC stream clock, kHz            */
#define PCIE_RB_PL_GPIO_MODE        0x10U
#define PCIE_RB_AXIS_ROUTING        0x11U  /* ch0|ch1<<4|ch2<<8|ch3<<12        */

/* Firmware identity, readable over PCIe.
 *
 * The ABI and capability set were announced on the SERIAL CONSOLE only, so a
 * host had no way to ask what it was talking to. Every version mismatch then
 * surfaced as a generic command failure, and the only way to diagnose it was
 * to go and read the UART — which is how a partially-flashed board went
 * unnoticed more than once.
 *
 * Reading these is itself version-safe: firmware too old to know them rejects
 * the request with "unknown EVENT code" (0x4), and that rejection is ITSELF
 * the answer — it means ABI < 4. */
#define PCIE_RB_ABI                 0x12U  /* PCIE_CFG_ABI                     */
#define PCIE_RB_CAPS                0x13U  /* PCIE_CFG_CAPABILITIES bitmask    */

/* The ABI a host must find in order to use readback and defaults. */
#define PCIE_ABI_REQUIRED           4U

/* Capability bit reported in PCIE_RB_CAPS by firmware 1.2.1 and later: the
 * DAC_SOURCE payload and readback are LOGICAL (PCIE_DAC_SRC_*) and the
 * readback comes from the switch register itself. A host that finds this bit
 * CLEAR is talking to firmware with the DDS/host swap, and must invert the
 * value in both directions to obtain the routing it intends. */
#define PCIE_CAP_DAC_SOURCE_MAPPED  (1U << 6)

/*---------------------------------------------------------------------------
 * Boot defaults, restored by PCIE_EVT_DEFAULTS. These mirror what main()
 * establishes at startup, so "Reset to defaults" and a power cycle agree.
 *---------------------------------------------------------------------------*/
#define PCIE_DEF_ADC_FS_MHZ         4800U
#define PCIE_DEF_DAC_FS_MHZ         9600U
#define PCIE_DEF_DECIMATION         24U
#define PCIE_DEF_INTERPOLATION      24U
#define PCIE_DEF_NCO_MHZ            3100
#define PCIE_DEF_DDS_MHZ            10
#define PCIE_DEF_ADC_DSA_DB10       0U
#define PCIE_DEF_DAC_VOP_UA         32000U
#define PCIE_DEF_DAC_SOURCE         PCIE_DAC_SRC_DDS
#define PCIE_DEF_PL_GPIO_MODE       3U
#define PCIE_DEF_AXIS_ROUTING       0x1237U   /* ch0=7 ch1=3 ch2=2 ch3=1 */

/*---------------------------------------------------------------------------
 * STATUS-IN-RING acknowledgement  --  EXTENSION, requires no new hardware.
 *
 * The map gives no status register. But the PS must already be able to WRITE
 * ring_register (that is how it clears NEW_CMD), so the acknowledge write can
 * carry a result code at no hardware cost.
 *
 * On acknowledge the PS writes:
 *     [31]    = 0            NEW_CMD cleared (unchanged, still the ACK)
 *     [30:24] = 0x2A         marker, so the host can tell a status word from
 *                            its own command word with bit31 cleared
 *     [23:20] = error code   PcieCfgError, 0 = success
 *     [19:0]  = event echo   the command being reported on
 *
 * A host that ignores this still works: NEW_CMD clearing remains the contract.
 * Disable with PCIE_CFG_STATUS_IN_RING=0 if your PL makes those bits read-only
 * from the PS side.
 *---------------------------------------------------------------------------*/
#define RING_ACK_MARKER             0x2AU
#define RING_ACKMARK_SHIFT          24U
#define RING_ACKMARK_MASK           (0x7FU << RING_ACKMARK_SHIFT)
#define RING_ACKERR_SHIFT           20U
#define RING_ACKERR_MASK            (0xFU << RING_ACKERR_SHIFT)

#define RING_ACK_BUILD(err, evt) \
    ((uint32_t)(((RING_ACK_MARKER) << RING_ACKMARK_SHIFT) | \
                ((((uint32_t)(err)) & 0xFU) << RING_ACKERR_SHIFT) | \
                (((uint32_t)(evt)) & RING_EVENT_MASK)))

#define RING_ACK_VALID(r) \
    (((((uint32_t)(r)) & RING_ACKMARK_MASK) >> RING_ACKMARK_SHIFT) == RING_ACK_MARKER)

#define RING_ACK_ERR(r) \
    ((uint32_t)((((uint32_t)(r)) & RING_ACKERR_MASK) >> RING_ACKERR_SHIFT))

/*---------------------------------------------------------------------------
 * Error codes.
 *
 * The register map defines NO error register, so these are reported over the
 * bare-metal console only. They become externally visible if the optional
 * STATUS/ERROR registers described in GAPS are added to the hardware; see
 * PCIE_CFG_HAVE_STATUS_REGS in pcie_cfg.h.
 *---------------------------------------------------------------------------*/
typedef enum {
    PCIE_ERR_NONE               = 0x00,
    PCIE_ERR_BAD_TARGET         = 0x01,
    PCIE_ERR_BAD_TILE           = 0x02,
    PCIE_ERR_BAD_CHANNEL        = 0x03,
    PCIE_ERR_BAD_EVENT          = 0x04,
    PCIE_ERR_BAD_PAYLOAD        = 0x05,
    PCIE_ERR_TARGET_EVENT_MIX   = 0x06,  /* e.g. decimation aimed at a DAC   */
    PCIE_ERR_BLOCK_DISABLED     = 0x07,
    PCIE_ERR_APPLY_FAILED       = 0x08,
    PCIE_ERR_TILE_B2B_UNSUPP    = 0x09,  /* 0100 encoding ambiguous          */
    PCIE_ERR_NOT_INITIALISED    = 0x0A,
    PCIE_ERR_UNSUPPORTED        = 0x0B   /* known code, not wired up */
} PcieCfgError;   /* must stay <= 0x0F to fit RING_ACKERR_MASK */

/*============================================================================
 * GAPS - fields the register map does not define
 *
 * These are NOT implemented. They are recorded so the hardware/host teams can
 * decide. The current design is workable without them; each simply removes a
 * class of blindness.
 *
 * 1. STATUS register (RO by host).  Minimum useful: BUSY, DONE, ERROR flags
 *    plus an echo of the last EVENT executed. Today the host learns only that
 *    ring[31] went to 0 - which happens on both success and failure.
 *
 * 2. ERROR_CODE register (RO by host). Would surface PcieCfgError above.
 *
 * 3. COMMAND SEQUENCE / ACK COUNTER. ring[31] clearing is a single-bit
 *    handshake. It is adequate provided the host always waits for 0 before
 *    issuing the next command, but it cannot distinguish "command N done"
 *    from "command N+1 already done".
 *
 * 4. DEVICE ID / VERSION register. The host cannot currently confirm it is
 *    talking to the right device or a compatible firmware build.
 *
 * 5. START / STOP / RESET commands. The map defines no such events, so the
 *    host tool exposes none. (Requested in the brief; not implementable.)
 *
 * 6. TILE field encoding 0100 "back to back tile select" is ambiguous - it
 *    collides with a 3-bit tile index. Recommend either reserving [27] as the
 *    LAST marker over a 2-bit tile index, or defining an explicit 4-bit tile
 *    BITMASK (bit per tile), which removes the multi-write sequence entirely.
 *
 * 7. No PCIe equivalent exists for these existing UART menu options:
 *    sampling-rate change (13), Nyquist zone (14), ADC DSA (15), QMC gain (12),
 *    AXIS routing (9), PL GPIO routing (10), DAC input source (16).
 *    They remain reachable over UART only. Suggest extending the EVENT field -
 *    it has 20 bits and uses 5 values.
 *
 * Suggested minimum addition (2 registers, contiguous with the existing pair):
 *    0xB0  STATUS      RO  [0] BUSY, [1] DONE, [2] ERROR, [19:0]<<8 last EVENT
 *    0xB4  ERROR_CODE  RO  PcieCfgError of the most recent command
 *============================================================================*/

#endif /* PCIE_REGS_H_ */

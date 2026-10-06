/******************************************************************************
 *
 * pcie_cfg.h  -  PCIe register based configuration manager (bare-metal side)
 *
 * Replaces UART as the CONFIGURATION TRANSPORT. The application processing
 * logic is unchanged: this module decodes a command from the PCIe registers
 * and calls the same functions the UART menu already calls.
 *
 * Usage from main():
 *
 *     PcieCfg_Init(&RFdcInst);          // once, after RFDC bring-up
 *     for (;;) {
 *         PcieCfg_Poll();               // non-blocking, call often
 *         ... existing UART menu servicing ...
 *     }
 *
 ******************************************************************************/

#ifndef PCIE_CFG_H_
#define PCIE_CFG_H_

#include "xrfdc.h"
#include "xil_types.h"
#include "pcie_regs.h"

/*---------------------------------------------------------------------------
 * FIRMWARE CAPABILITY REPORT
 *
 * A board running a NEW main.c with an OLD pcie_cfg.c looks identical to a
 * fully updated one: the boot banner, the init trace and the menu all come
 * from main.c, while the event handlers live here. That ambiguity cost real
 * bench time, so the module now states its own version and capabilities at
 * startup and in the diagnostics dump.
 *
 * Bump PCIE_CFG_ABI whenever an EVENT is added or its meaning changes.
 *---------------------------------------------------------------------------*/
#define PCIE_CFG_ABI            4   /* 4: readback + defaults are IMPLEMENTED,
                                     * not merely accepted by the validator */   /* 1: base events  2: +extended  3: +readback/defaults */

#define PCIE_CAP_BASIC          (1U << 0)  /* events 0x001-0x005            */
#define PCIE_CAP_EXTENDED       (1U << 1)  /* 0x010-0x018 rate/zone/dsa/... */
#define PCIE_CAP_STARTSTOP      (1U << 2)  /* 0x020-0x022                   */
#define PCIE_CAP_READBACK       (1U << 3)  /* 0x0F1                         */
#define PCIE_CAP_DEFAULTS       (1U << 4)  /* 0x023                         */
#define PCIE_CAP_STATUS_IN_ACK  (1U << 5)  /* result code in the ACK word    */

#define PCIE_CFG_CAPABILITIES  (PCIE_CAP_BASIC | PCIE_CAP_EXTENDED |  \
                                PCIE_CAP_STARTSTOP | PCIE_CAP_READBACK | \
                                PCIE_CAP_DEFAULTS | PCIE_CAP_STATUS_IN_ACK)

/*---------------------------------------------------------------------------
 * Build-time options
 *---------------------------------------------------------------------------*/

/* The supplied register map defines no STATUS/ERROR registers. Set this to 1
 * ONLY after the hardware actually implements the optional registers described
 * in the GAPS section of pcie_regs.h, otherwise the writes go nowhere. */
#ifndef PCIE_CFG_HAVE_STATUS_REGS
#define PCIE_CFG_HAVE_STATUS_REGS   0
#endif

#if PCIE_CFG_HAVE_STATUS_REGS
#define PCIE_REG_STATUS             0x00B0U
#define PCIE_REG_ERRCODE            0x00B4U
#define PCIE_STATUS_BUSY            (1UL << 0)
#define PCIE_STATUS_DONE            (1UL << 1)
#define PCIE_STATUS_ERROR           (1UL << 2)
#endif

/* Write a result code into the acknowledge (see RING_ACK_* in pcie_regs.h).
 * Costs no hardware: the PS already writes ring_register to clear NEW_CMD.
 * Set to 0 if your PL makes bits [30:20] read-only from the PS. */
#ifndef PCIE_CFG_STATUS_IN_RING
#define PCIE_CFG_STATUS_IN_RING     1
#endif

/* Print a line for every command processed. Useful during bring-up; set to 0
 * for production if the console is shared with an operator. */
#ifndef PCIE_CFG_VERBOSE
#define PCIE_CFG_VERBOSE            1
#endif

/* Master enable. 0 compiles the module out entirely (no register access at
 * all), which is the safe setting until the base address is confirmed against
 * hardware - an AXI read to an unmapped address hangs the CPU. */
#ifndef ENABLE_PCIE_CONFIG
#define ENABLE_PCIE_CONFIG          1
#endif

/*---------------------------------------------------------------------------
 * Decoded command, produced by the parser and consumed by validate/apply.
 *---------------------------------------------------------------------------*/
typedef struct {
    u32 raw_ring;       /* ring_register as read                             */
    u32 raw_cfg;        /* RFDC_configuration as read                        */
    u32 target;         /* PCIE_TARGET_*                                     */
    u32 tile;           /* 0..3, or PCIE_TILE_ALL                            */
    u32 last_tile;      /* ring[27]                                          */
    u32 channel;        /* PCIE_CHAN_*                                       */
    u32 event;          /* PCIE_EVT_*                                        */
    int negative;       /* payload sign bit (events 1 and 2)                 */
    u32 magnitude;      /* payload without the sign bit                      */
} PcieCfgCmd;

/*---------------------------------------------------------------------------
 * Running counters. Exposed for the UART menu / debugging; the register map
 * provides no way to report these to the host.
 *---------------------------------------------------------------------------*/
typedef struct {
    u32 commands_seen;
    u32 commands_applied;
    u32 commands_rejected;
    PcieCfgError last_error;
    u32 last_event;
} PcieCfgStats;

/*---------------------------------------------------------------------------
 * API
 *---------------------------------------------------------------------------*/

/* Bind the manager to the RFDC instance and clear any stale NEW_CMD left in
 * the ring register by a previous run. Returns XST_SUCCESS / XST_FAILURE. */
int PcieCfg_Init(XRFdc *RFdcInstPtr);

/* Non-blocking. Reads ring_register; if NEW_CMD is set, decodes, validates,
 * applies and acknowledges exactly one command, then returns.
 * Returns 1 if a command was processed (successfully or not), 0 if idle. */
int PcieCfg_Poll(void);

/* Snapshot of the counters above. */
void PcieCfg_GetStats(PcieCfgStats *out);

/* Human-readable text for an error code (never NULL). */
const char *PcieCfg_ErrorString(PcieCfgError e);

/*---------------------------------------------------------------------------
 * Runtime base address control.
 *
 * PCIE_CTRL_BASEADDR is only the compile-time default. These let the operator
 * retarget the manager from the UART menu while hunting for the right aperture,
 * without a rebuild-and-reflash cycle for every guess.
 *---------------------------------------------------------------------------*/
UINTPTR PcieCfg_GetBase(void);
void    PcieCfg_SetBase(UINTPTR base);

/* Print base, both raw registers, decoded ring fields and the counters.
 * Safe to call at any time; performs two reads. */
void PcieCfg_Diagnostics(void);

#endif /* PCIE_CFG_H_ */

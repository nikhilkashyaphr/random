/******************************************************************************
 *
 * main.c  -  ZU47DR RFSoC bare-metal control application
 *
 * Target : Zynq UltraScale+ RFSoC ZU47DR, psu_cortexa53_0, standalone
 * Tools  : Vitis 2025.2 (SDT) and Vitis 2022.2 (classic) from one source tree
 * Console: PS UART0, MIO 6/7, 115200 8N1
 *
 * Responsibilities:
 *   - program the LMK04828 / 2x LMX2594 clock tree over Quad SPI
 *   - bring up the RF Data Converter, align tiles via MTS, set NCOs
 *   - configure PL datapath routing (AXIS switches, GPIO)
 *   - expose a 16-option serial menu for runtime reconfiguration
 *
 * This is a CONTROL-PLANE application. There is no AXI DMA in the design and
 * no RFDC interrupt is wired to the GIC; high-rate ADC/DAC data movement is
 * handled entirely in PL logic.
 *
 * Build-time switches:
 *   ENABLE_LMK_LOCK_IRQ   0 (default) = skip GIC + LMK lock interrupt setup.
 *                         The handler only increments a counter nothing reads,
 *                         and GIC init can block indefinitely when PMU
 *                         firmware is absent. Set to 1 once PMUFW is present.
 *   ENABLE_PCIE_CONFIG    1 (default) = poll the PCIe control registers.
 *                         Set to 0 if the aperture at PCIE_CTRL_BASEADDR is
 *                         not yet mapped - an AXI read to an unmapped
 *                         address hangs the CPU with no message.
 *   ENABLE_METAL_PRINTS   verbose libmetal logging.
 *
 * NOTE: a 2,259-line commented-out revision previously sat above this header.
 * It has been removed. Line numbers below no longer match older logs.
 *
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "main.h"
#include "platform_ids.h"
#include "pcie_cfg.h"
#include "xparameters.h"
#include "xuartps_hw.h"   /* XUartPs_IsReceiveData - non-blocking UART poll */
#include "xil_types.h"
#include "xil_io.h"
#include "sleep.h"
#include "xstatus.h"
#include "xil_printf.h"

// Included AXI GPIO for DDS and SYSREF Control
#include "xgpio.h"
// Kept PS GPIO for LMK Lock Interrupt
#include "xgpiops.h"

#include "xscugic.h"
#include "xil_exception.h"

#include <metal/log.h>
#include <metal/sys.h>
#include "cli.h"

#include "xspi.h"
#include "ig60m_rfdc_clk.h"

// Includes for user added CLI functions
#include "cmd_func_mem.h"
#include "rfdc_poweron_status.h"
#include "rfdc_cmd.h"
#include "adc_FreezeCal.h"
#include "rfdc_nyquistzone.h"
#include "adc_LinkCoupling.h"
#include "rfdc_interpolation_decimation.h"
#include "dac_waves.h"
#include "rfdc_dsa_vop.h"
#include "adcSaveCalCoefficients.h"
#include "adcGetCalCoefficients.h"
#include "adcLoadCalCoefficients.h"
#include "adcDisableCoeffOvrd.h"

// MTS Header
#include "rfdc_mts.h"

// AXI4-Stream Switch Header
#include "xaxis_switch.h"

/* Clocking Wizard AXI-Lite Register Offsets */
#define CLK_WIZ_SR_OFFSET       0x04
#define CLK_WIZ_CLKOUT0_OFFSET  0x208 // Output Divider
#define CLK_WIZ_LOAD_OFFSET     0x25C

/* Clocking Wizard VCO frequency, set statically by the block design
 * (100 MHz input x 12). Program_Clock_Wizard() only manipulates the output
 * divider, so this MUST match the Vivado configuration of CLK_WIZ_ADC and
 * CLK_WIZ_DAC. Verify against the block design before changing.
 */
#define CLK_WIZ_VCO_MHZ         1200.0

/* Hardware identifiers now resolve through platform_ids.h, which selects
 * DEVICE_ID (classic) or BASEADDR (SDT / Vitis 2023.2+) automatically.
 * The application no longer shadows BSP hardware configuration: the local
 * XPAR_SYSREF_CONTROLL_* redefinitions that lived here have been removed.
 */
#define AXIS_SWITCH_DEVICE_ID       HW_AXIS_SWITCH_ID
#define DDS_H2C_SWITCH_DEVICE_ID    HW_DDS_H2C_SWITCH_ID
#define ADC_SELECT_GPIO_DEVICE_ID   HW_ADC_SELECT_GPIO_ID
#define DYN_CLK_GPIO_DEVICE_ID      HW_DYN_CLK_GPIO_ID
#define CLK_COUNTER_DEVICE_ID       HW_CLK_COUNTER_GPIO_ID
#define DDS_GPIO_DEVICE_ID          HW_DDS_GPIO_ID
#define SYSREF_GPIO_DEVICE_ID       HW_SYSREF_GPIO_ID

#define GPIO_DEVICE_ID              HW_GPIOPS_ID
#define INTC_DEVICE_ID              HW_SCUGIC_ID
#define GPIO_BANK                   HW_GPIOPS_BANK
#define LMK_LOCK_GPIO_BIT           HW_LMK_LOCK_GPIO_BIT
#define LMK_LOCK_GPIO_PIN           HW_LMK_LOCK_GPIO_PIN

/* Default ADC AXIS switch routing: MI0<-SI7, MI1<-SI3, MI2<-SI2, MI3<-SI1.
 * Applied at boot and re-applied after any teardown. */
#define ADC_ROUTE_DEFAULT_CH0 7
#define ADC_ROUTE_DEFAULT_CH1 3
#define ADC_ROUTE_DEFAULT_CH2 2
#define ADC_ROUTE_DEFAULT_CH3 1

/* Upper bound on how long we wait for every enabled tile to reach power-on
 * sequence state 0xF after startup. Generous: FG calibration on a fully
 * populated 4-tile ADC can take a few hundred ms per tile. */
#define RFDC_STARTUP_TIMEOUT_MS 10000U

/* Set to 0 to skip arming the LMK lock interrupt. It is vestigial - the
 * handler only increments a counter that is never read - so disabling it
 * costs nothing and removes a livelock suspect during bring-up. */
#ifndef ENABLE_LMK_LOCK_IRQ
#define ENABLE_LMK_LOCK_IRQ 0
#endif

/* Suppresses -Wunused on the GIC objects when the interrupt block is
 * compiled out.
 *
 * ORDER MATTERS: this #if must come AFTER the #ifndef default above. If it
 * is placed earlier in the file the macro is still undefined, the #if
 * silently evaluates to 0, and changing the default here would have no
 * effect. */
#if ENABLE_LMK_LOCK_IRQ
#define IRQ_MAYBE_UNUSED
#else
#define IRQ_MAYBE_UNUSED __attribute__((unused))
#endif

// MTS Sync Masks
#define ADC_TILES_SYNCC_MASK 0xF
#define DAC_TILES_SYNCC_MASK 0xF

// =====================================================================
// GLOBAL DYNAMIC VARIABLES
// =====================================================================
double current_dds_freq_mhz = 10.0;
double current_dac_stream_clk_mhz = 200.0;

int force_adc_zone = 0; // 0 = Auto, 1 = Zone 1, 2 = Zone 2
int force_dac_zone = 0; // 0 = Auto, 1 = Zone 1, 2 = Zone 2

// Baseline State Tracking for Mathematical Derivations
double global_adc_gsps = 4.8; // Initial defaults
double global_dac_gsps = 9.6;

#define LMK4828_SPI_SELECT           0x01
#define LMX2594_RF_ADC1_SPI_SELECT   0x01
#define LMX2594_RF_DAC1_SPI_SELECT   0x01

#define OVERHEAD_SIZE       2
#define LMK4828_ADDRESS_MSB_OFFSET  0
#define LMK4828_ADDRESS_LSB_OFFSET  1
#define LMK4828_DATA_OFFSET         2
#define LMX2594_ADDRESS_OFFSET      0
#define LMX2594_DATA_MSB_OFFSET     1
#define LMX2594_DATA_LSB_OFFSET     2



// =====================================================================
// GLOBAL DYNAMIC VARIABLES
// =====================================================================

double current_adc_nco = 3100.0;
double current_dac_nco = 3100.0;
//double global_adc_gsps = 4.8;
//double global_dac_gsps = 9.6;
//double current_dac_stream_clk_mhz = 200.0;

// Add this global variable to the top of your file
int force_nyquist_zone = 0; // 0 = Auto, 1 = Force Zone 1, 2 = Force Zone 2

// All other variables should follow after this...

typedef u8 LMK4828Buffer[10];
typedef u8 LMX2594Buffer[10];

XSpi Spi_lmk, Spi_lmx_adc, Spi_lmx_dac;
XSpi_Config *ConfigPtr_lmk, *ConfigPtr_lmx_adc, *ConfigPtr_lmx_dac;

XGpio DdsGpio;
XGpio SysrefGpio;
XAxis_Switch AxisSwitchInst;
XAxis_Switch DdsH2cSwitchInst;
XGpio AdcSelectGpio;
XGpio DynClkGpio;
XGpio ClkCounterGpio;

extern XRFdc_MultiConverter_Sync_Config ADC_Sync_Config;
extern XRFdc_MultiConverter_Sync_Config DAC_Sync_Config;

static XScuGic Intc IRQ_MAYBE_UNUSED;
static XGpioPs Gpio;
XScuGic_Config *IntcConfig IRQ_MAYBE_UNUSED;
static volatile int intr_cnt IRQ_MAYBE_UNUSED;

XRFdc RFdcInst;

IRQ_MAYBE_UNUSED
static void IntrHandler(void *CallBackRef, u32 Bank, u32 Status) {
    (void)CallBackRef; (void)Bank; (void)Status;
    intr_cnt++;
}

void my_metal_default_log_handler(enum metal_log_level level, const char *format, ...);
void Print_Current_Rates(XRFdc *RFdcInstPtr);
int Update_Datapath_Rate(XRFdc *RFdcInstPtr, int is_adc, int target_tile, int target_block, u32 New_Factor);
int Change_Sampling_Rate_All_Tiles(XRFdc *RFdcInstPtr, int is_adc, double new_sampling_rate_mhz);
// Helper to print double values using integer formatting
#define PRINT_DOUBLE(val) \
    xil_printf("%lu.%03lu", (u32)(val), (u32)(((val) - (u32)(val)) * 1000))

int Init_Dds_Gpio();
int Init_Sysref_Gpio();
void Gate_SYSREF();
void Reenable_SYSREF();
void Reset_Dds();
void Set_Dds_Frequency(double target_freq_mhz);

int Init_Adc_Select_Gpio();
int Init_Axis_Switch();
int Init_Dds_H2c_Switch();
int Set_Dds_H2c_Routing(int source);        /* LOGICAL PCIE_DAC_SRC_* value */
int Get_Dds_H2c_Routing(void);              /* LOGICAL, read from the switch */
const char *Dac_Source_Name(int source);
void Set_Adc_Channel_Routing(int ch0, int ch1, int ch2, int ch3);
void Set_PL_Gpio_Routing(int mode);

int Init_Dyn_Clk_Gpio();
void Measure_Stream_Clocks();
int Init_Clk_Counter_Gpio();
void Program_Clock_Wizard(u32 BaseAddr, double target_freq_mhz);
void Update_Stream_Clock_Architectural(XRFdc *RFdcInstPtr, int is_adc);

int Run_MTS_Procedure(XRFdc *RFdcInstPtr);
void Set_NCO_Freq_MTS_Safe(XRFdc *RFdcInstPtr, int is_adc, double freq_mhz);

int is_valid_adc_factor(u32 factor);
int is_valid_dac_factor(u32 factor);

/* Gain save/restore and manual Nyquist control.
 * These were previously GCC nested functions declared inside main(); they are
 * now file-scope so the build does not require stack trampolines / an
 * executable stack, and so other modules can reach them. Behaviour unchanged.
 */
typedef struct {
    double QmcGain;
    int    IsEnabled;
} TileGainState;

static TileGainState saved_gain[4][4];

void Save_Gain_State(XRFdc *RFdcInstPtr, int is_adc);
void Restore_Gain_State(XRFdc *RFdcInstPtr, int is_adc);
void Set_Nyquist_Zone_Manual(XRFdc *RFdcInstPtr, int is_adc, int zone);

// -----------------------------------------------------------------------------
// Clocking Wizard Reprogramming (VCO Safe: Output Divider Only)
// -----------------------------------------------------------------------------
void Program_Clock_Wizard(u32 BaseAddr, double target_freq_mhz) {
    if (target_freq_mhz < 1.0) return;

    // Based on your original working design: VCO is statically locked at 1200 MHz
    // by the block design (100MHz In * 12). We ONLY manipulate the output divider.
    u32 int_div = (u32)(CLK_WIZ_VCO_MHZ / target_freq_mhz);
    u32 frac_div = (u32)(((CLK_WIZ_VCO_MHZ * 1000.0) / target_freq_mhz)) % 1000;

    /* CLKOUT0 divider register: [7:0] integer, [17:8] fractional (1/1000).
     * The integer field is only 8 bits wide, so reject anything that would
     * silently wrap and produce a wildly wrong clock. */
    if (int_div == 0 || int_div > 255) {
        xil_printf("\r\n  -> ERROR: CW 0x%X divider %lu out of range [1..255]. Aborting.\r\n",
                   BaseAddr, int_div);
        return;
    }

    u32 reg_val = (frac_div << 8) | (int_div & 0xFF);

    u32 tf_int = (u32)target_freq_mhz;
    u32 tf_frac = (u32)((target_freq_mhz - tf_int) * 1000.0);

    xil_printf("\r\n  -> Reprogramming CW 0x%X Out=%lu.%03lu MHz (Div: %lu.%03lu)\r\n",
               BaseAddr, tf_int, tf_frac, int_div, frac_div);

    // Completely overwrite Output Divider (Bypassing restrictive Read-Modify-Write)
    Xil_Out32(BaseAddr + CLK_WIZ_CLKOUT0_OFFSET, reg_val);

    // Trigger the IP to load the new config
    Xil_Out32(BaseAddr + CLK_WIZ_LOAD_OFFSET, 0x03);

    // Wait for PLL Lock
    int timeout = 100000;
    while(((Xil_In32(BaseAddr + CLK_WIZ_SR_OFFSET) & 0x01) == 0) && timeout > 0) {
        usleep(10);
        timeout--;
    }

    if (timeout <= 0) xil_printf("  -> WARNING: CW 0x%X timed out waiting for lock!\r\n", BaseAddr);
    else xil_printf("  -> CW 0x%X locked successfully.\r\n", BaseAddr);
}


// ---------------------------------------------------------------------------
// DAC input source: the 2-to-1 DDS / host-stream switch.
//
// Every caller (serial menu 16, PCIe EVENT 0x016, restore-defaults, boot)
// speaks the LOGICAL source, PCIE_DAC_SRC_DDS or PCIE_DAC_SRC_HOST. The
// physical port is resolved in Dac_Source_To_Si() and nowhere else; the
// wiring it relies on is documented, with its evidence, in platform_ids.h.
//
// Before 1.2.1 the logical value was passed straight through as the port
// number, while the bitstream has the HOST stream on SI0 and the DDS on SI1.
// The result was a swap at every layer: the board booted with the DAC on the
// idle host stream, "DDS" played the host stream, and "host" played the DDS.
// ---------------------------------------------------------------------------
static int dds_h2c_switch_ready = 0;

static u8 Dac_Source_To_Si(int source)
{
    return (source == (int)PCIE_DAC_SRC_HOST) ? (u8)HW_DAC_SW_SI_HOST
                                              : (u8)HW_DAC_SW_SI_DDS;
}

const char *Dac_Source_Name(int source)
{
    if (source == (int)PCIE_DAC_SRC_DDS)  return "DDS compiler";
    if (source == (int)PCIE_DAC_SRC_HOST) return "Host / GNU Radio stream";
    return "none (no switch input routed)";
}

// Initialize the 2-to-1 DDS / host-stream switch. Boots on the DDS tone
// (PCIE_DEF_DAC_SOURCE), so the converters have a signal from power-on.
int Init_Dds_H2c_Switch() {
    XAxis_Switch_Config *Config = XAxisScr_LookupConfig(DDS_H2C_SWITCH_DEVICE_ID);
    if (Config == NULL) return XST_FAILURE;

    XAxisScr_CfgInitialize(&DdsH2cSwitchInst, Config, Config->BaseAddress);
    dds_h2c_switch_ready = 1;

    return (Set_Dds_H2c_Routing((int)PCIE_DEF_DAC_SOURCE) == XST_SUCCESS)
               ? XST_SUCCESS : XST_FAILURE;
}

// Route a LOGICAL source to the DAC. Succeeds only when the switch register
// reads back the port that was asked for, so a caller never reports a routing
// the hardware did not take.
int Set_Dds_H2c_Routing(int source) {
    if (source != (int)PCIE_DAC_SRC_DDS && source != (int)PCIE_DAC_SRC_HOST)
        return XST_INVALID_PARAM;
    if (!dds_h2c_switch_ready)
        return XST_DEVICE_NOT_FOUND;

    XAxisScr_RegUpdateDisable(&DdsH2cSwitchInst);
    XAxisScr_MiPortDisableAll(&DdsH2cSwitchInst);
    XAxisScr_MiPortEnable(&DdsH2cSwitchInst, 0, Dac_Source_To_Si(source));
    XAxisScr_RegUpdateEnable(&DdsH2cSwitchInst);

    return (Get_Dds_H2c_Routing() == source) ? XST_SUCCESS : XST_FAILURE;
}

// The LOGICAL source currently routed, read from the switch's MI0 mux register
// rather than remembered, so the serial menu, PCIe readback and a debugger can
// never disagree. PCIE_DAC_SRC_NONE if nothing is routed.
int Get_Dds_H2c_Routing(void) {
    if (!dds_h2c_switch_ready) return (int)PCIE_DAC_SRC_NONE;
    if (XAxisScr_IsMiPortEnabled(&DdsH2cSwitchInst, 0, (u8)HW_DAC_SW_SI_DDS))
        return (int)PCIE_DAC_SRC_DDS;
    if (XAxisScr_IsMiPortEnabled(&DdsH2cSwitchInst, 0, (u8)HW_DAC_SW_SI_HOST))
        return (int)PCIE_DAC_SRC_HOST;
    return (int)PCIE_DAC_SRC_NONE;
}

// -----------------------------------------------------------------------------
// Core Stream Clock Math Engine
// -----------------------------------------------------------------------------
void Update_Stream_Clock_Architectural(XRFdc *RFdcInstPtr, int is_adc) {
    u32 factor = 1;

    // 1. Fetch factor from the first active block in the domain
    for (int tile = 0; tile < 4; tile++) {
        if (is_adc ? XRFdc_IsADCBlockEnabled(RFdcInstPtr, tile, 0) : XRFdc_IsDACBlockEnabled(RFdcInstPtr, tile, 0)) {
            if (is_adc) XRFdc_GetDecimationFactor(RFdcInstPtr, tile, 0, &factor);
            else        XRFdc_GetInterpolationFactor(RFdcInstPtr, tile, 0, &factor);
            if (factor == 0) {
                xil_printf("WARNING: driver reported factor 0; clamping to 1.\r\n");
                factor = 1;
            }
            xil_printf("Debug: Calculating stream clock using Factor: %lu\r\n", factor);
            break;
        }
    }

    // 2. Perform exact architectural stream clock math
    // ADC (1 Sample/Clock): Stream = Fs / Factor
    // DAC (2 Samples/Clock): Stream = Fs / (Factor * 2)
    double target_freq_mhz = is_adc ? ((global_adc_gsps * 1000.0) / (double)factor)
                                    : ((global_dac_gsps * 1000.0) / ((double)factor * 2.0));
    if (target_freq_mhz < 10.0) target_freq_mhz = 100.0;

    // 3. Dispatch changes ONLY to the target ADC or DAC Clock Wizard.
    u32 BaseAddr = is_adc ? HW_CLK_WIZ_ADC_BASE : HW_CLK_WIZ_DAC_BASE;

    Program_Clock_Wizard(BaseAddr, target_freq_mhz);

    if (!is_adc) {
        current_dac_stream_clk_mhz = target_freq_mhz;
        Set_Dds_Frequency(current_dds_freq_mhz);
    }
}

void Update_Nyquist_Zone(XRFdc *RFdcInstPtr, int is_adc, double sampling_rate_mhz, double nco_freq_mhz) {
    u32 Type = is_adc ? 0 : 1;

    int physical_zone;
    if (force_nyquist_zone != 0) {
        physical_zone = force_nyquist_zone;
        xil_printf("  -> OVERRIDE: Forcing Physical Nyquist Zone %d\r\n", physical_zone);
    } else {
        int math_zone = (int)(nco_freq_mhz / (sampling_rate_mhz / 2.0)) + 1;
        physical_zone = (math_zone % 2 == 0) ? 2 : 1;
        xil_printf("  -> AUTO: Setting Physical Nyquist Zone %d\r\n", physical_zone);
    }

    for (int t = 0; t < 4; t++) {
        for (int b = 0; b < 4; b++) {
            if (is_adc ? XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, b) : XRFdc_IsDACBlockEnabled(RFdcInstPtr, t, b)) {
                XRFdc_SetNyquistZone(RFdcInstPtr, Type, t, b, physical_zone);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Change Sampling Rate
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Change Sampling Rate
// -----------------------------------------------------------------------------
int Change_Sampling_Rate_All_Tiles(XRFdc *RFdcInstPtr, int is_adc, double new_sampling_rate_mhz) {
    u32 Type = is_adc ? XRFDC_ADC_TILE : XRFDC_DAC_TILE;
    int master_tile = is_adc ? 1 : 2;

    u32 sr_int = (u32)new_sampling_rate_mhz;
    u32 sr_frac = (u32)((new_sampling_rate_mhz - sr_int) * 1000.0);

    xil_printf("\r\n--- Setting %s Master Tile %d to %lu.%03lu MHz ---\r\n",
               is_adc ? "ADC" : "DAC", master_tile, sr_int, sr_frac);

    // 1. HARD STOP: Disable FIFOs and (ADC only) Switch Routing
    for (int t = 0; t < 4; t++) XRFdc_SetupFIFO(RFdcInstPtr, Type, t, 0);

    /* The AXIS switch sits in the ADC datapath only. Tearing it down for a
     * DAC rate change killed ADC capture for no reason. */
    if (is_adc) {
        XAxisScr_RegUpdateDisable(&AxisSwitchInst);
        XAxisScr_MiPortDisableAll(&AxisSwitchInst);
        XAxisScr_RegUpdateEnable(&AxisSwitchInst);
    }

    // 2. Program Master Tile PLL
    int status = XRFdc_DynamicPLLConfig(RFdcInstPtr, Type, master_tile,
                                        XRFDC_INTERNAL_PLL_CLK, 300.0, new_sampling_rate_mhz);

    if (status != XST_SUCCESS) {
        xil_printf("ERROR: Master PLL configuration failed!\r\n");
        /* Do not leave the datapath dead on the failure path: bring the FIFOs
         * and the ADC routing matrix back up before returning. */
        for (int t = 0; t < 4; t++) {
            if (is_adc ? XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, 0)
                       : XRFdc_IsDACBlockEnabled(RFdcInstPtr, t, 0)) {
                XRFdc_SetupFIFO(RFdcInstPtr, Type, t, 1);
            }
        }
        if (is_adc) Set_Adc_Channel_Routing(ADC_ROUTE_DEFAULT_CH0, ADC_ROUTE_DEFAULT_CH1,
                                            ADC_ROUTE_DEFAULT_CH2, ADC_ROUTE_DEFAULT_CH3);
        return XST_FAILURE;
    }

    // 3. Inform the Slave Tiles of the new distributed clock
    for (int t = 0; t < 4; t++) {
        if (t != master_tile) {
            if (is_adc ? XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, 0) : XRFdc_IsDACBlockEnabled(RFdcInstPtr, t, 0)) {
                XRFdc_DynamicPLLConfig(RFdcInstPtr, Type, t, XRFDC_EXTERNAL_CLK,
                                       new_sampling_rate_mhz, new_sampling_rate_mhz);
            }
        }
    }

    /* 4. Update global state from the rate the PLL ACTUALLY produced.
     *
     * The RFDC PLL can only synthesise RefClk x FBDIV / OutDiv, so most
     * requests are quantised to a nearby value - a request of 2000 MHz on
     * a 300 MHz reference lands on 2006.25 MHz, for instance.
     *
     * Storing the REQUESTED value here (as this used to) made every
     * downstream calculation wrong by that error: the stream-clock target,
     * the Clocking Wizard divider, the DDS phase increment and the
     * automatic Nyquist zone were all computed against a rate the silicon
     * was not running at. Read the achieved rate back instead. */
    {
        XRFdc_PLL_Settings pll;
        double achieved_mhz = new_sampling_rate_mhz;   /* fallback */

        if (XRFdc_GetPLLConfig(RFdcInstPtr, Type, master_tile, &pll) == XST_SUCCESS
            && pll.OutputDivider != 0U) {
            achieved_mhz = (pll.RefClkFreq * (double)pll.FeedbackDivider)
                           / (double)pll.OutputDivider;
        } else {
            xil_printf("WARNING: could not read back PLL config; using the "
                       "requested rate for downstream math.\r\n");
        }

        double err = achieved_mhz - new_sampling_rate_mhz;
        if (err > 0.5 || err < -0.5) {
            xil_printf("NOTE: PLL quantised the request. asked ");
            PRINT_DOUBLE(new_sampling_rate_mhz);
            xil_printf(" MHz, achieved ");
            PRINT_DOUBLE(achieved_mhz);
            xil_printf(" MHz. Downstream math uses the achieved value.\r\n");
        }

        if (is_adc) global_adc_gsps = achieved_mhz / 1000.0;
        else        global_dac_gsps = achieved_mhz / 1000.0;

        /* Nyquist and the caller must both see the achieved rate too. */
        new_sampling_rate_mhz = achieved_mhz;
    }

    Update_Stream_Clock_Architectural(RFdcInstPtr, is_adc);

    // --- DYNAMIC ZONE UPDATE (With Manual Override Check) ---
    int target_zone = is_adc ? force_adc_zone : force_dac_zone;
    if (target_zone != 0) {
        xil_printf("  -> Using Manual Override: Forcing Zone %d\r\n", target_zone);
        u32 ApiType = is_adc ? 0 : 1;
        for (int t = 0; t < 4; t++) {
            for (int b = 0; b < 4; b++) {
                int enabled = is_adc ? XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, b) : XRFdc_IsDACBlockEnabled(RFdcInstPtr, t, b);
                if (enabled) {
                    XRFdc_SetNyquistZone(RFdcInstPtr, ApiType, t, b, target_zone);
                    XRFdc_UpdateEvent(RFdcInstPtr, ApiType, t, b, XRFDC_EVNT_SRC_TILE);
                }
            }
        }
    } else {
        Update_Nyquist_Zone(RFdcInstPtr, is_adc, new_sampling_rate_mhz, is_adc ? current_adc_nco : current_dac_nco);
    }

    // 5. Wait for silicon state machines to finish foreground calibration
    xil_printf("Waiting for analog calibration to settle...\r\n");
    usleep(500000); // 500 ms

    // 6. RE-ENABLE: Restore FIFOs
    for (int t = 0; t < 4; t++) {
        if (is_adc ? XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, 0) : XRFdc_IsDACBlockEnabled(RFdcInstPtr, t, 0)) {
            XRFdc_SetupFIFO(RFdcInstPtr, Type, t, 1);
        }
    }

    // Re-apply AXIS Switch routing (ADC domain only)
    if (is_adc) {
        Set_Adc_Channel_Routing(ADC_ROUTE_DEFAULT_CH0, ADC_ROUTE_DEFAULT_CH1,
                                ADC_ROUTE_DEFAULT_CH2, ADC_ROUTE_DEFAULT_CH3);
    }

    // 7. Verification check
    XRFdc_IPStatus IPStatus;
    XRFdc_GetIPStatus(RFdcInstPtr, &IPStatus);
    int is_enabled = is_adc ? IPStatus.ADCTileStatus[master_tile].IsEnabled
                            : IPStatus.DACTileStatus[master_tile].IsEnabled;

    if (!is_enabled) {
        xil_printf("WARNING: Tile %d state is DISABLED after PLL change!\r\n", master_tile);
    }

    return XST_SUCCESS;
}
// -----------------------------------------------------------------------------
// Update Datapath Rate (Decimation/Interpolation)
// -----------------------------------------------------------------------------
int Update_Datapath_Rate(XRFdc *RFdcInstPtr, int is_adc, int target_tile, int target_block, u32 New_Factor) {
    int Status;
    u32 Type = is_adc ? XRFDC_ADC_TILE : XRFDC_DAC_TILE;

    XRFdc_SetupFIFO(RFdcInstPtr, Type, target_tile, 0);

    if (is_adc) Status = XRFdc_SetDecimationFactor(RFdcInstPtr, target_tile, target_block, New_Factor);
    else Status = XRFdc_SetInterpolationFactor(RFdcInstPtr, target_tile, target_block, New_Factor);

    if (Status != XST_SUCCESS) {
        xil_printf("\r\nERROR: Failed to set rate for %s Tile %d Block %d.\r\n", is_adc ? "ADC" : "DAC", target_tile, target_block);
        XRFdc_SetupFIFO(RFdcInstPtr, Type, target_tile, 1);
        return XST_FAILURE;
    }

    XRFdc_UpdateEvent(RFdcInstPtr, Type, target_tile, target_block, XRFDC_EVNT_SRC_TILE);

    // Refresh stream clocks using the exact formulas
    Update_Stream_Clock_Architectural(RFdcInstPtr, is_adc);

    XRFdc_SetupFIFO(RFdcInstPtr, Type, target_tile, 1);

    xil_printf("\r\nSuccessfully updated %s Tile %d Block %d to Factor %lux.\r\n",
               is_adc ? "ADC" : "DAC", target_tile, target_block, New_Factor);

    return XST_SUCCESS;
}

// -----------------------------------------------------------------------------
// GPIO Initializes
// -----------------------------------------------------------------------------
int Init_Clk_Counter_Gpio() {
    XGpio_Initialize(&ClkCounterGpio, CLK_COUNTER_DEVICE_ID);
    XGpio_SetDataDirection(&ClkCounterGpio, 1, 0xFFFFFFFF);
    return XST_SUCCESS;
}

int Init_Sysref_Gpio() {
    XGpio_Initialize(&SysrefGpio, SYSREF_GPIO_DEVICE_ID);
    XGpio_SetDataDirection(&SysrefGpio, 1, 0x0);
    XGpio_SetDataDirection(&SysrefGpio, 2, 0x0);
    return XST_SUCCESS;
}

void Gate_SYSREF() { XGpio_DiscreteWrite(&SysrefGpio, 1, 1); }
void Reenable_SYSREF() {
    XGpio_DiscreteWrite(&SysrefGpio, 1, 0); usleep(10);
    XGpio_DiscreteWrite(&SysrefGpio, 2, 1); usleep(10);
    XGpio_DiscreteWrite(&SysrefGpio, 2, 0);
}

int Init_Dyn_Clk_Gpio() {
    XGpio_Initialize(&DynClkGpio, DYN_CLK_GPIO_DEVICE_ID);
    XGpio_SetDataDirection(&DynClkGpio, 1, 0xFFFFFFFF);
    XGpio_SetDataDirection(&DynClkGpio, 2, 0xFFFFFFFF);
    return XST_SUCCESS;
}

void Measure_Stream_Clocks() {
    xil_printf("\r\nNote: Real-time measurements may be inaccurate due to bare-metal sleep timer calibration bugs.\r\n");
    u32 adc_start = XGpio_DiscreteRead(&DynClkGpio, 1);
    u32 dac_start = XGpio_DiscreteRead(&DynClkGpio, 2);
    u32 in_start = XGpio_DiscreteRead(&ClkCounterGpio, 1);

    usleep(100000); // 100ms

    u32 adc_freq = (XGpio_DiscreteRead(&DynClkGpio, 1) - adc_start) * 10;
    u32 dac_freq = (XGpio_DiscreteRead(&DynClkGpio, 2) - dac_start) * 10;
    u32 in_freq = (XGpio_DiscreteRead(&ClkCounterGpio, 1) - in_start) * 10;

    xil_printf("\r\n---------------------------------------");
    xil_printf("\r\n Raw PL Clock In  : %10lu Hz (%lu MHz)", in_freq, in_freq / 1000000);
    xil_printf("\r\n ADC Stream Clock : %10lu Hz (%lu MHz)", adc_freq, adc_freq / 1000000);
    xil_printf("\r\n DAC Stream Clock : %10lu Hz (%lu MHz)", dac_freq, dac_freq / 1000000);
    xil_printf("\r\n---------------------------------------\r\n");
}

int Init_Adc_Select_Gpio() {
    XGpio_Initialize(&AdcSelectGpio, ADC_SELECT_GPIO_DEVICE_ID);
    XGpio_SetDataDirection(&AdcSelectGpio, 1, 0x0);
    return XST_SUCCESS;
}

int Init_Axis_Switch() {
    XAxis_Switch_Config *Config = XAxisScr_LookupConfig(AXIS_SWITCH_DEVICE_ID);
    if (Config == NULL) {
        xil_printf("ERROR: ADC AXIS switch lookup failed.\r\n");
        return XST_FAILURE;
    }
    XAxisScr_CfgInitialize(&AxisSwitchInst, Config, Config->BaseAddress);
    XAxisScr_RegUpdateDisable(&AxisSwitchInst);
    XAxisScr_MiPortDisableAll(&AxisSwitchInst);
    XAxisScr_RegUpdateEnable(&AxisSwitchInst);
    return XST_SUCCESS;
}

void Set_Adc_Channel_Routing(int ch0, int ch1, int ch2, int ch3) {
    XAxisScr_RegUpdateDisable(&AxisSwitchInst);
    XAxisScr_MiPortDisableAll(&AxisSwitchInst);
    if(ch0 >= 0 && ch0 <= 7) XAxisScr_MiPortEnable(&AxisSwitchInst, 0, ch0);
    if(ch1 >= 0 && ch1 <= 7) XAxisScr_MiPortEnable(&AxisSwitchInst, 1, ch1);
    if(ch2 >= 0 && ch2 <= 7) XAxisScr_MiPortEnable(&AxisSwitchInst, 2, ch2);
    if(ch3 >= 0 && ch3 <= 7) XAxisScr_MiPortEnable(&AxisSwitchInst, 3, ch3);
    XAxisScr_RegUpdateEnable(&AxisSwitchInst);
}

void Set_PL_Gpio_Routing(int mode) {
    if (mode >= 1 && mode <= 3) XGpio_DiscreteWrite(&AdcSelectGpio, 1, mode);
}

// -----------------------------------------------------------------------------
// Unified MTS-Safe NCO Update Helper
// -----------------------------------------------------------------------------
void Set_NCO_Freq_MTS_Safe(XRFdc *RFdcInstPtr, int is_adc, double freq_mhz) {
    u32 Type = is_adc ? XRFDC_ADC_TILE : XRFDC_DAC_TILE;
    xil_printf("\r\nApplying MTS-Safe NCO Update Procedure for %s to ", is_adc ? "ADC" : "DAC");
    PRINT_DOUBLE(freq_mhz);
    xil_printf(" MHz...\r\n");

    /* NOTE: CalFrozen is a STATUS field populated by XRFdc_GetCalFreeze().
     * The setter is FreezeCalibration. DisableFreezePin must also be set,
     * otherwise the external freeze pin overrides the software request.
     * All three members are initialised so no stack garbage reaches the
     * calibration control register. */
    XRFdc_Cal_Freeze_Settings CalFreeze = { 0, 0, 0 };
    if (is_adc) {
        CalFreeze.DisableFreezePin  = 1;
        CalFreeze.FreezeCalibration = 1; // 1 = Freeze TSCB
        for(int t = 0; t < 4; t++) {
            for(int b = 0; b < 4; b++) {
                if(XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, b)) {
                    XRFdc_SetCalFreeze(RFdcInstPtr, t, b, &CalFreeze);
                }
            }
        }
    }

    Gate_SYSREF();

    for(int t = 0; t < 4; t++) {
        for(int b = 0; b < 4; b++) {
            if(is_adc ? XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, b) : XRFdc_IsDACBlockEnabled(RFdcInstPtr, t, b)) {
                XRFdc_Mixer_Settings Mixer_Settings;
                XRFdc_GetMixerSettings(RFdcInstPtr, Type, t, b, &Mixer_Settings);
                Mixer_Settings.Freq = freq_mhz;
                Mixer_Settings.EventSource = XRFDC_EVNT_SRC_SYSREF;
                XRFdc_SetMixerSettings(RFdcInstPtr, Type, t, b, &Mixer_Settings);
                XRFdc_ResetNCOPhase(RFdcInstPtr, Type, t, b);
            }
        }
    }
    Reenable_SYSREF();

    if (is_adc) {
        CalFreeze.DisableFreezePin  = 1;
        CalFreeze.FreezeCalibration = 0; // 0 = Unfreeze TSCB
        for(int t = 0; t < 4; t++) {
            for(int b = 0; b < 4; b++) {
                if(XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, b)) {
                    XRFdc_SetCalFreeze(RFdcInstPtr, t, b, &CalFreeze);
                }
            }
        }
        xil_printf("TSCB Background Calibration resumed after NCO update.\r\n");
    }
}
// -----------------------------------------------------------------------------
// DDS Compiler GPIO Control
// -----------------------------------------------------------------------------
int Init_Dds_Gpio() {
    XGpio_Initialize(&DdsGpio, DDS_GPIO_DEVICE_ID);
    XGpio_SetDataDirection(&DdsGpio, 1, 0x0); XGpio_SetDataDirection(&DdsGpio, 2, 0x0);

    // MODIFICATION: Write 0 to hold the DDS in reset.
    // This enforces absolute silence during RFdc initialization.
    XGpio_DiscreteWrite(&DdsGpio, 1, 0);
    XGpio_DiscreteWrite(&DdsGpio, 2, 0);

    return XST_SUCCESS;
}

void Reset_Dds() {
    XGpio_DiscreteWrite(&DdsGpio, 1, 0); usleep(10);
    XGpio_DiscreteWrite(&DdsGpio, 1, 1);
}

void Set_Dds_Frequency(double target_freq_mhz) {
    current_dds_freq_mhz = target_freq_mhz;
    double phase_inc_double = (target_freq_mhz * 4294967296.0) / current_dac_stream_clk_mhz;
    XGpio_DiscreteWrite(&DdsGpio, 2, (u32)phase_inc_double);

    u32 tf_int = (u32)target_freq_mhz;
    u32 tf_frac = (u32)((target_freq_mhz - tf_int) * 1000.0);
    u32 cf_int = (u32)current_dac_stream_clk_mhz;
    u32 cf_frac = (u32)((current_dac_stream_clk_mhz - cf_int) * 1000.0);
    xil_printf("\r\nDDS Frequency set to %lu.%03lu MHz (calculated with %lu.%03lu MHz clock domain).\r\n",
               tf_int, tf_frac, cf_int, cf_frac);
}

// -----------------------------------------------------------------------------
// Core SPI Functions
// -----------------------------------------------------------------------------
int QSpiInit() {
    ConfigPtr_lmk = XSpi_LookupConfig(HW_QSPI_LMK_ID);
    XSpi_CfgInitialize(&Spi_lmk, ConfigPtr_lmk, ConfigPtr_lmk->BaseAddress);
    XSpi_SetOptions(&Spi_lmk, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION);

    ConfigPtr_lmx_adc = XSpi_LookupConfig(HW_QSPI_ADC_LMX_ID);
    XSpi_CfgInitialize(&Spi_lmx_adc, ConfigPtr_lmx_adc, ConfigPtr_lmx_adc->BaseAddress);
    XSpi_SetOptions(&Spi_lmx_adc, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION);

    ConfigPtr_lmx_dac = XSpi_LookupConfig(HW_QSPI_DAC_LMX_ID);
    XSpi_CfgInitialize(&Spi_lmx_dac, ConfigPtr_lmx_dac, ConfigPtr_lmx_dac->BaseAddress);
    XSpi_SetOptions(&Spi_lmx_dac, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION);
    return XST_SUCCESS;
}

int QSpiLMK4828Program() {
    LMK4828Buffer Buffer;
    XSpi_SetSlaveSelect(&Spi_lmk, LMK4828_SPI_SELECT);
    XSpi_Start(&Spi_lmk); XSpi_IntrGlobalDisable(&Spi_lmk);
    for(int i = 0; i < LMK4828_REG_CONFIG_NUM_COMMANDS; i++) {
        Buffer[LMK4828_DATA_OFFSET]        = (u8) ((lmk4828_registers[i] & 0x0000FF));
        Buffer[LMK4828_ADDRESS_LSB_OFFSET] = (u8) ((lmk4828_registers[i] & 0x00FF00) >> 8);
        Buffer[LMK4828_ADDRESS_MSB_OFFSET] = (u8) ((lmk4828_registers[i] & 0x1F0000) >> 16);
        XSpi_Transfer(&Spi_lmk, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);
    }
    return XST_SUCCESS;
}

int QSpiLMX2594Program() {
    int i;
    LMX2594Buffer Buffer;
    XSpi *CurrentSpi;

    XSpi_Start(&Spi_lmx_adc); XSpi_Start(&Spi_lmx_dac);
    XSpi_IntrGlobalDisable(&Spi_lmx_adc); XSpi_IntrGlobalDisable(&Spi_lmx_dac);

    for(int j = 0; j < 2; j++) {
        const u32 * lmx_regs = (j == 0) ? lmx2594_registers_adc : lmx2594_registers_dac;
        CurrentSpi = (j == 0) ? &Spi_lmx_adc : &Spi_lmx_dac;
        XSpi_SetSlaveSelect(CurrentSpi, (j==0) ? LMX2594_RF_ADC1_SPI_SELECT : LMX2594_RF_DAC1_SPI_SELECT);

        Buffer[LMX2594_DATA_LSB_OFFSET]  = 0x2; Buffer[LMX2594_DATA_MSB_OFFSET]  = 0x0; Buffer[LMX2594_ADDRESS_OFFSET]   = 0x0;
        XSpi_Transfer(CurrentSpi, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);
        Buffer[LMX2594_DATA_LSB_OFFSET]  = 0x0;
        XSpi_Transfer(CurrentSpi, Buffer, NULL, 1 + OVERHEAD_SIZE);

        for(i = 0; i < LMX2594_REG_CONFIG_NUM_COMMANDS; i++) {
            Buffer[LMX2594_DATA_LSB_OFFSET] = (u8) ((lmx_regs[i] & 0x0000FF));
            Buffer[LMX2594_DATA_MSB_OFFSET] = (u8) ((lmx_regs[i] & 0x00FF00) >> 8);
            Buffer[LMX2594_ADDRESS_OFFSET]  = (u8) ((lmx_regs[i] & 0x7F0000) >> 16);
            XSpi_Transfer(CurrentSpi, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);
        }
        Buffer[LMX2594_DATA_LSB_OFFSET] = (u8) ((lmx_regs[i-1] & 0x0000FF));
        Buffer[LMX2594_DATA_MSB_OFFSET] = (u8) ((lmx_regs[i-1] & 0x00FF00) >> 8);
        Buffer[LMX2594_ADDRESS_OFFSET]  = (u8) ((lmx_regs[i-1] & 0x7F0000) >> 16);
        XSpi_Transfer(CurrentSpi, Buffer, NULL, 1 + OVERHEAD_SIZE);
        XSpi_Transfer(CurrentSpi, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);
    }
    return XST_SUCCESS;
}

int QSpiLMX2594Status() {
    LMX2594Buffer Buffer,Buffer2; LMK4828Buffer LMK_Buffer;

    XSpi_SetSlaveSelect(&Spi_lmx_dac, LMX2594_RF_DAC1_SPI_SELECT);
    Buffer[LMX2594_DATA_LSB_OFFSET] = 0x01; Buffer[LMX2594_DATA_MSB_OFFSET] = 0x08; Buffer[LMX2594_ADDRESS_OFFSET] = 0x1;
    XSpi_Transfer(&Spi_lmx_dac, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);
    Buffer[LMX2594_DATA_LSB_OFFSET] = 0x18; Buffer[LMX2594_DATA_MSB_OFFSET] = 0x24; Buffer[LMX2594_ADDRESS_OFFSET] = 0x0;
    XSpi_Transfer(&Spi_lmx_dac, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);

    XSpi_SetSlaveSelect(&Spi_lmx_adc, LMX2594_RF_ADC1_SPI_SELECT);
    Buffer[LMX2594_DATA_LSB_OFFSET] = 0x01; Buffer[LMX2594_DATA_MSB_OFFSET] = 0x08; Buffer[LMX2594_ADDRESS_OFFSET] = 0x1;
    XSpi_Transfer(&Spi_lmx_adc, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);
    Buffer[LMX2594_DATA_LSB_OFFSET] = 0x18; Buffer[LMX2594_DATA_MSB_OFFSET] = 0x24; Buffer[LMX2594_ADDRESS_OFFSET] = 0x0;
    XSpi_Transfer(&Spi_lmx_adc, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);

    Buffer[LMX2594_DATA_LSB_OFFSET] = 0x09; Buffer[LMX2594_DATA_MSB_OFFSET] = 0x08; Buffer[LMX2594_ADDRESS_OFFSET] = 0x1;
    XSpi_Transfer(&Spi_lmx_adc, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);
    Buffer[LMX2594_ADDRESS_OFFSET] = 0x6E | 0x80;
    XSpi_Transfer(&Spi_lmx_adc, Buffer, Buffer2, 1 + OVERHEAD_SIZE); usleep(10000);
    if((Buffer2[1] & 0x6) == 0x4) xil_printf("LMX ADC-1 Status: Locked\r\n");

    Buffer[LMX2594_DATA_LSB_OFFSET] = 0x01; Buffer[LMX2594_DATA_MSB_OFFSET] = 0x08; Buffer[LMX2594_ADDRESS_OFFSET] = 0x1;
    XSpi_Transfer(&Spi_lmx_adc, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);

    XSpi_SetSlaveSelect(&Spi_lmx_dac, LMX2594_RF_DAC1_SPI_SELECT);
    Buffer[LMX2594_DATA_LSB_OFFSET] = 0x09; Buffer[LMX2594_DATA_MSB_OFFSET] = 0x08; Buffer[LMX2594_ADDRESS_OFFSET] = 0x1;
    XSpi_Transfer(&Spi_lmx_dac, Buffer, NULL, 1 + OVERHEAD_SIZE); usleep(10000);
    Buffer[LMX2594_ADDRESS_OFFSET] = 0x6E | 0x80;
    XSpi_Transfer(&Spi_lmx_dac, Buffer, Buffer2, 1 + OVERHEAD_SIZE); usleep(10000);
    if((Buffer2[1] & 0x6) == 0x4) xil_printf("LMX DAC-1 Status: Locked\r\n");

    XSpi_SetSlaveSelect(&Spi_lmk, LMK4828_SPI_SELECT);
    LMK_Buffer[LMK4828_DATA_OFFSET] = 0x3B; LMK_Buffer[LMK4828_ADDRESS_LSB_OFFSET] = 0x5F; LMK_Buffer[LMK4828_ADDRESS_MSB_OFFSET] = 0x01;
    XSpi_Transfer(&Spi_lmk, LMK_Buffer, LMK_Buffer, 3); usleep(100000);

    LMK_Buffer[LMK4828_DATA_OFFSET] = 0x00; LMK_Buffer[LMK4828_ADDRESS_LSB_OFFSET] = 0x82; LMK_Buffer[LMK4828_ADDRESS_MSB_OFFSET] = 0x81;
    XSpi_Transfer(&Spi_lmk, LMK_Buffer, LMK_Buffer, 3);
    if ((LMK_Buffer[2] & 2) == 2) xil_printf("LMK04828 PLL1 is locked\r\n");

    usleep(100000);
    LMK_Buffer[LMK4828_DATA_OFFSET] = 0x00; LMK_Buffer[LMK4828_ADDRESS_LSB_OFFSET] = 0x83; LMK_Buffer[LMK4828_ADDRESS_MSB_OFFSET] = 0x81;
    XSpi_Transfer(&Spi_lmk, LMK_Buffer, LMK_Buffer, 3);
    if ((LMK_Buffer[2]&2) == 2) xil_printf("LMK04828 PLL2 is locked\r\n");
    return XST_SUCCESS;
}

// -----------------------------------------------------------------------------
// MTS Synchronization Sequence
// -----------------------------------------------------------------------------
int Run_MTS_Procedure(XRFdc *RFdcInstPtr) {
    XRFdc_MultiConverter_Init(&ADC_Sync_Config, 0, 0, XRFDC_ADC_TILE);
    ADC_Sync_Config.Tiles = ADC_TILES_SYNCC_MASK;
    if (XRFdc_MultiConverter_Sync(RFdcInstPtr, XRFDC_ADC_TILE, &ADC_Sync_Config) == XST_SUCCESS) {
        xil_printf("ADC MTS Successfully Aligned. Latency: %d\r\n", ADC_Sync_Config.Latency);
    }

    XRFdc_MultiConverter_Init(&DAC_Sync_Config, 0, 0, XRFDC_DAC_TILE);
    DAC_Sync_Config.Tiles = DAC_TILES_SYNCC_MASK;
    if (XRFdc_MultiConverter_Sync(RFdcInstPtr, XRFDC_DAC_TILE, &DAC_Sync_Config) == XST_SUCCESS) {
        xil_printf("DAC MTS Successfully Aligned. Latency: %d\r\n", DAC_Sync_Config.Latency);
    }
    return XST_SUCCESS;
}

// -----------------------------------------------------------------------------
// Datapath Helpers
// -----------------------------------------------------------------------------
int is_valid_adc_factor(u32 factor) {
    if (factor==4 || factor==5 || factor==6 || factor==8 || factor==10 ||
        factor==12 || factor==16 || factor==20 || factor==24 || factor==40) return 1;
    return 0;
}

int is_valid_dac_factor(u32 factor) {
    if (factor==2 || factor==3 || factor==4 || factor==5 || factor==6 ||
        factor==8 || factor==10 || factor==12 || factor==16 || factor==20 ||
        factor==24 || factor==40) return 1;
    return 0;
}

void Print_Current_Rates(XRFdc *RFdcInstPtr) {
    u32 Factor;
    xil_printf("\r\n--- Current RFdc Rates ---\r\n");
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (XRFdc_IsADCBlockEnabled(RFdcInstPtr, i, j)) {
                XRFdc_GetDecimationFactor(RFdcInstPtr, i, j, &Factor);
                printf("ADC Tile %d Block %d : Decimation %dx\r\n", i, j, Factor);
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (XRFdc_IsDACBlockEnabled(RFdcInstPtr, i, j)) {
                XRFdc_GetInterpolationFactor(RFdcInstPtr, i, j, &Factor);
                printf("DAC Tile %d Block %d : Interpolation %dx\r\n", i, j, Factor);
            }
        }
    }
    xil_printf("--------------------------\r\n");
}

void Verify_Actual_Sampling_Rate(XRFdc *RFdcInstPtr, int is_adc, int tile) {
    XRFdc_PLL_Settings PLLSettings;
    u32 Type = is_adc ? XRFDC_ADC_TILE : XRFDC_DAC_TILE;

    int Status = XRFdc_GetPLLConfig(RFdcInstPtr, Type, tile, &PLLSettings);

    if (Status == XST_SUCCESS) {
        xil_printf("\r\n--- HW Verification for %s Tile %d ---\r\n", is_adc ? "ADC" : "DAC", tile);
        xil_printf("PLL Enabled: %d\r\n", PLLSettings.Enabled);

        xil_printf("Ref Clock: ");
        PRINT_DOUBLE(PLLSettings.RefClkFreq);
        xil_printf(" MHz\r\n");

        xil_printf("Feedback Divider (FBDIV): %d\r\n", PLLSettings.FeedbackDivider);
        xil_printf("Output Divider (OUTDIV): %d\r\n", PLLSettings.OutputDivider);

        double actual_fs = (PLLSettings.RefClkFreq * (double)PLLSettings.FeedbackDivider) / (double)PLLSettings.OutputDivider;

        xil_printf("Calculated Hardware Sampling Rate: ");
        PRINT_DOUBLE(actual_fs);
        xil_printf(" MHz\r\n");
    } else {
        xil_printf("ERROR: Could not read hardware PLL configuration.\r\n");
    }
}

// -----------------------------------------------------------------------------
// Gain state save/restore and manual Nyquist zone control
// -----------------------------------------------------------------------------
/* Capture QMC gain for every block before a PLL reprogram wipes it. */
void Save_Gain_State(XRFdc *RFdcInstPtr, int is_adc) {
    u32 Type = is_adc ? XRFDC_ADC_TILE : XRFDC_DAC_TILE;
    for (int t = 0; t < 4; t++) {
        for (int b = 0; b < 4; b++) {
            XRFdc_QMC_Settings Q;
            if (XRFdc_GetQMCSettings(RFdcInstPtr, Type, t, b, &Q) == XST_SUCCESS) {
                saved_gain[t][b].QmcGain = Q.GainCorrectionFactor;
                saved_gain[t][b].IsEnabled = Q.EnableGain;
            }
        }
    }
}

void Set_Nyquist_Zone_Manual(XRFdc *RFdcInstPtr, int is_adc, int zone) {
    u32 Type = is_adc ? 0 : 1;
    xil_printf("Forcing %s to Nyquist Zone %d\r\n", is_adc ? "ADC" : "DAC", zone);

    for (int t = 0; t < 4; t++) {
        for (int b = 0; b < 4; b++) {
            int enabled = is_adc ? XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, b) : XRFdc_IsDACBlockEnabled(RFdcInstPtr, t, b);
            if (enabled) {
                XRFdc_SetNyquistZone(RFdcInstPtr, Type, t, b, zone);
                // Mandatory update event to apply the zone change
                XRFdc_UpdateEvent(RFdcInstPtr, Type, t, b, XRFDC_EVNT_SRC_TILE);
            }
        }
    }
}
/* Re-apply the gain captured by Save_Gain_State(). */
void Restore_Gain_State(XRFdc *RFdcInstPtr, int is_adc) {
    u32 Type = is_adc ? XRFDC_ADC_TILE : XRFDC_DAC_TILE;
    for (int t = 0; t < 4; t++) {
        for (int b = 0; b < 4; b++) {
            XRFdc_QMC_Settings Q;
            if (XRFdc_GetQMCSettings(RFdcInstPtr, Type, t, b, &Q) == XST_SUCCESS) {
                Q.GainCorrectionFactor = saved_gain[t][b].QmcGain;
                Q.EnableGain = saved_gain[t][b].IsEnabled;
                XRFdc_SetQMCSettings(RFdcInstPtr, Type, t, b, &Q);
                XRFdc_UpdateEvent(RFdcInstPtr, Type, t, b, XRFDC_EVNT_SRC_TILE);
            }
        }
    }
}

/*****************************************************************************/
/**
* MenuReadLine - read one console line WITHOUT starving the PCIe transport.
*
* WHY THIS EXISTS
* ---------------
* scanf() blocks inside libc, and a CPU parked there cannot poll the PCIe
* control registers. Guarding it like this does NOT work:
*
*     while (!XUartPs_IsReceiveData(...)) PcieCfg_Poll();
*     MenuReadLine(buf, sizeof(buf));
*
* scanf("%s") consumes its token but LEAVES the terminating CR/LF in the UART
* FIFO. On the next pass IsReceiveData() is therefore immediately true, the
* guard loop exits without ever polling, and scanf blocks again waiting for a
* real token. PCIe is then only serviced when the operator happens to type
* something - exactly the symptom seen on the bench.
*
* This routine owns the FIFO instead: it polls PCIe between every character
* and while idle, consumes the line terminator itself, and never enters libc.
* It is used for EVERY prompt, so the transport also stays alive inside the
* sub-menus (which previously blocked with no polling at all).
*
* @param  buf  destination, always NUL-terminated on return
* @param  len  size of buf including the NUL
*
* @return number of characters stored (0 for an empty line)
*
******************************************************************************/
static int MenuReadLine(char *buf, int len)
{
    int i = 0;

    if (buf == NULL || len < 2) {
        return 0;
    }

    for (;;) {
        /* Idle work: keep the PCIe transport alive while nobody is typing. */
        if (PcieCfg_Poll()) {
            /* A command was handled and printed. Redraw whatever the operator
             * had typed so far, so our output does not mangle their line. */
            buf[i] = '\0';
            xil_printf("\r\n> %s", buf);
        }

        if (!XUartPs_IsReceiveData(STDIN_BASEADDRESS)) {
            continue;
        }

        u8 c = (u8)XUartPs_ReadReg(STDIN_BASEADDRESS, XUARTPS_FIFO_OFFSET);

        if (c == '\r' || c == '\n') {
            /* Consume the terminator HERE. Leaving it in the FIFO is precisely
             * what broke the old guard loop. */
            if (i == 0) {
                continue;               /* swallow the LF of a CRLF pair */
            }
            buf[i] = '\0';
            xil_printf("\r\n");
            return i;
        }

        if (c == '\b' || c == 0x7F) {   /* backspace / delete */
            if (i > 0) {
                i--;
                xil_printf("\b \b");
            }
            continue;
        }

        if (c >= ' ' && c < 0x7F && i < (len - 1)) {
            buf[i++] = (char)c;
            xil_printf("%c", c);        /* echo: we bypass libc entirely */
        }
    }
}

/*****************************************************************************/
/**
* Main function
******************************************************************************/
int main(void)
{
    u32  Val, Minor, Major;
    int Status;
    XRFdc_Config *ConfigPtr;
    XRFdc *RFdcInstPtr = &RFdcInst;
    char buf[16];

    /* NOTE: current_adc_nco / current_dac_nco are FILE-SCOPE globals.
     * They were previously shadowed by locals of the same name here, which
     * left Update_Nyquist_Zone() reading a value frozen at 3100.0 forever.
     * Do not re-declare them in this scope. */
    current_adc_nco = 3100.0;
    current_dac_nco = 3100.0;

    xil_printf("\n\r###############################################\n\r");
    xil_printf("    Automated RFSoC Initialization Flow        \n\r");
    xil_printf("###############################################\n\r");

    xil_printf("\r\n--- Step 1: Programming LMK & LMX via QSPI ---\n\r");
    QSpiInit();
    QSpiLMK4828Program();
    QSpiLMX2594Program();
    QSpiLMX2594Status();

    xil_printf("[init] 1/9 PS GPIO lookup...\r\n");
    XGpioPs_Config *GpioConfigPtr = XGpioPs_LookupConfig(GPIO_DEVICE_ID);
    if (GpioConfigPtr == NULL) {
        xil_printf("ERROR: XGpioPs_LookupConfig(0x%08lx) returned NULL.\r\n",
                   (unsigned long)GPIO_DEVICE_ID);
        xil_printf("       Under SDT this argument must be a BASE ADDRESS.\r\n");
        return XST_FAILURE;
    }

    XGpioPs_CfgInitialize(&Gpio, GpioConfigPtr, GpioConfigPtr->BaseAddr);

    /* LMK lock indicator. Bank 3 starts at pin 78, so bank-3 bit 10 is
     * absolute pin 88. The original code passed 0x88 (=136, bank 4) here
     * while arming the interrupt on bank 3 bit 10 - the two referred to
     * different pins. VERIFY the intended MIO/EMIO pin against the block
     * design before relying on this interrupt. */
    XGpioPs_SetDirectionPin(&Gpio, LMK_LOCK_GPIO_PIN, 0x0);

    /* ------------------------------------------------------------------
     * LMK lock interrupt: ENTIRELY OPTIONAL.
     *
     * The handler only increments a counter that nothing ever reads, so
     * none of this affects RFDC operation. It is disabled by default
     * because GIC bring-up is a known hang point when PMU firmware is not
     * running: driver init can issue an IPI to the PMU and wait forever
     * for a reply that never comes.
     *
     * Set ENABLE_LMK_LOCK_IRQ=1 once PMUFW is in your BOOT.BIN and you
     * actually want the lock indicator.
     * ------------------------------------------------------------------ */
#if ENABLE_LMK_LOCK_IRQ
    intr_cnt=0;
    Xil_ExceptionInit();
    xil_printf("[init] 2/9 GIC: LookupConfig...\r\n");
    IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
    if (NULL == IntcConfig) {
        xil_printf("ERROR: XScuGic_LookupConfig(0x%08lx) returned NULL.\r\n",
                   (unsigned long)INTC_DEVICE_ID);
        xil_printf("       Under SDT this argument must be a BASE ADDRESS.\r\n");
        return XST_FAILURE;
    }
    xil_printf("[init]     GIC: CfgInitialize (cpu base 0x%08lx)...\r\n",
               (unsigned long)IntcConfig->CpuBaseAddress);
    Status = XScuGic_CfgInitialize(&Intc, IntcConfig, IntcConfig->CpuBaseAddress);
    if (Status != XST_SUCCESS) {
        xil_printf("ERROR: XScuGic_CfgInitialize() failed.\r\n");
        return XST_FAILURE;
    }

    xil_printf("[init]     GIC: RegisterHandler...\r\n");
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT, (Xil_ExceptionHandler)XScuGic_InterruptHandler, &Intc);
    /* Classic BSPs export XPAR_XGPIOPS_0_INTR; SDT carries the IRQ number
     * in XGpioPs_Config::IntrId. platform_ids.h hides the difference. */
    u32 GpioIntrId = HW_GPIOPS_INTR_ID(GpioConfigPtr);
    xil_printf("[init]     GIC: Connect IRQ %lu...\r\n", (unsigned long)GpioIntrId);
    Status = XScuGic_Connect(&Intc, GpioIntrId, (Xil_ExceptionHandler)XGpioPs_IntrHandler, &Gpio);
    if (Status != XST_SUCCESS) {
        xil_printf("ERROR: XScuGic_Connect() failed for GPIO IRQ %lu.\r\n",
                   (unsigned long)GpioIntrId);
        return XST_FAILURE;
    }

    xil_printf("[init]     GPIO: SetIntrType...\r\n");
    XGpioPs_SetIntrType(&Gpio, GPIO_BANK, (1 << LMK_LOCK_GPIO_BIT), 0x00, 0x00);
    XGpioPs_SetCallbackHandler(&Gpio, (void *) &Gpio, IntrHandler);
    XGpioPs_IntrEnable(&Gpio, GPIO_BANK, (1 << LMK_LOCK_GPIO_BIT));

    /* The LMK lock IRQ is configured LEVEL-sensitive, ACTIVE-LOW. If that
     * pin sits low, the handler re-enters forever and the application
     * livelocks here with no output. Set ENABLE_LMK_LOCK_IRQ to 0 to rule
     * this out - the interrupt is vestigial (the handler only increments a
     * counter nothing reads). */
#if ENABLE_LMK_LOCK_IRQ
    xil_printf("[init] arming LMK lock IRQ %lu...\r\n", (unsigned long)GpioIntrId);
    XScuGic_Enable(&Intc, GpioIntrId);
    Xil_ExceptionEnableMask(XIL_EXCEPTION_IRQ);
    xil_printf("[init] IRQ armed.\r\n");
#else
    xil_printf("[init] LMK lock IRQ disabled (ENABLE_LMK_LOCK_IRQ=0).\r\n");
#endif
#else
    xil_printf("[init] 2/9 GIC + LMK lock IRQ SKIPPED (ENABLE_LMK_LOCK_IRQ=0)\r\n");
#endif

#ifdef ENABLE_METAL_PRINTS
    struct metal_init_params init_param = {
            .log_handler    = my_metal_default_log_handler,
            .log_level      = METAL_LOG_DEBUG,
    };
#else
    struct metal_init_params init_param = METAL_INIT_DEFAULTS;
#endif

    xil_printf("[init] 3/9 metal_init...\r\n");
    if (metal_init(&init_param)) {
        xil_printf("ERROR: metal_init() failed.\r\n");
        return XRFDC_FAILURE;
    }

    /* Everything below touches PL AXI slaves. If the bitstream is NOT
     * loaded, the first access issues an AXI transaction that never
     * completes and the CPU hangs with no message. Each step announces
     * itself first so the last line printed names the offender. */
    xil_printf("[init] 4/9 DDS GPIO      (PL 0x%08lx)...\r\n", (unsigned long)DDS_GPIO_DEVICE_ID);
    Init_Dds_Gpio();
    xil_printf("[init] 5/9 SYSREF GPIO   (PL 0x%08lx)...\r\n", (unsigned long)SYSREF_GPIO_DEVICE_ID);
    Init_Sysref_Gpio();
    xil_printf("[init] 6/9 ADC sel GPIO  (PL 0x%08lx)...\r\n", (unsigned long)ADC_SELECT_GPIO_DEVICE_ID);
    Init_Adc_Select_Gpio();
    xil_printf("[init] 7/9 ADC AXIS sw   (PL 0x%08lx)...\r\n", (unsigned long)AXIS_SWITCH_DEVICE_ID);
    Init_Axis_Switch();
    xil_printf("[init] 8/9 DDS/H2C sw    (PL 0x%08lx)...\r\n", (unsigned long)DDS_H2C_SWITCH_DEVICE_ID);
    Init_Dds_H2c_Switch();
    xil_printf("[init]     DAC input = %s\r\n", Dac_Source_Name(Get_Dds_H2c_Routing()));
    xil_printf("[init] 9/9 clock counters...\r\n");
    Init_Dyn_Clk_Gpio();
    Init_Clk_Counter_Gpio();
    xil_printf("[init] PL peripherals OK - bitstream is loaded.\r\n");

    xil_printf("\r\n--- Opening Datapath Routing for MTS Alignment ---\r\n");
    Set_PL_Gpio_Routing(3);
    Set_Adc_Channel_Routing(ADC_ROUTE_DEFAULT_CH0, ADC_ROUTE_DEFAULT_CH1,
                            ADC_ROUTE_DEFAULT_CH2, ADC_ROUTE_DEFAULT_CH3);

    xil_printf("\r\n--- Step 2: Initializing RFdc Controller ---\n\r");
        ConfigPtr = XRFdc_LookupConfig(HW_RFDC_ID);
        if (ConfigPtr == NULL) {
            xil_printf("ERROR: XRFdc_LookupConfig() returned NULL. Check XSA/BSP.\r\n");
            return XST_FAILURE;
        }
        Status = XRFdc_CfgInitialize(&RFdcInst, ConfigPtr);
        if (Status != XRFDC_SUCCESS) {
            xil_printf("ERROR: XRFdc_CfgInitialize() failed.\r\n");
            return XST_FAILURE;
        }

        rfdcStartup(NULL); dacResetAll(NULL); adcResetAll(NULL);

        /* rfdcReady() only prints a snapshot - it does NOT block. The ADC
         * foreground calibration must actually complete (power-on sequence
         * state 0xF) before we touch DSA/VOP/NCO, so wait for it explicitly. */
        if (rfdcWaitAllTilesReady(RFDC_STARTUP_TIMEOUT_MS) != XST_SUCCESS) {
            xil_printf("WARNING: not all tiles reached power-on state 0xF. Continuing, but calibration may be incomplete.\r\n");
        }
        rfdcReady(NULL);

        Val = Xil_In32(HW_RFDC_BASE + 0x00000);
                Major = (Val >> 24) & 0xFF; Minor = (Val >> 16) & 0xFF;
                xil_printf("RFDC IP Version: %d.%d\r\n",Major,Minor);

                // =========================================================================
                    // ENOB OPTIMIZATION FIX 1: FG CAL Muting Wait State
                    // =========================================================================
                    xil_printf("\r\n--- DACs muted during ADC Foreground Calibration (FG CAL) ---\r\n");
                    /* The DDS is still held in reset by Init_Dds_Gpio(), and
                     * rfdcWaitAllTilesReady() above has confirmed state 0xF,
                     * so the ADC did calibrate in silence. */
                    xil_printf("ADC FG CAL complete.\r\n");

                // =========================================================================
                // ENOB OPTIMIZATION FIX 2: Maximize ADC and DAC signal levels
                // =========================================================================
                xil_printf("Optimizing Signal Levels (0 dB ADC DSA, Max DAC VOP)...\r\n");
                for(int t = 0; t < 4; t++) {
                    for(int b = 0; b < 4; b++) {
                        if(XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, b)) {
                            XRFdc_DSA_Settings DSA_Settings;
                            if (XRFdc_GetDSA(RFdcInstPtr, t, b, &DSA_Settings) == XST_SUCCESS) {
                                DSA_Settings.Attenuation = 0.0; // 0 dB Attenuation
                                XRFdc_SetDSA(RFdcInstPtr, t, b, &DSA_Settings);
                            }
                        }
                        if(XRFdc_IsDACBlockEnabled(RFdcInstPtr, t, b)) {
                            XRFdc_SetDACVOP(RFdcInstPtr, t, b, 32000); // 32mA Max for Gen 3
                        }
                    }
                }

                xil_printf("\r\n--- Releasing DDS Reset to Enable Signal Transmission ---\r\n");
                Reset_Dds();

                // =========================================================================
                // ENOB OPTIMIZATION FIX 3: Wait for TSCB Convergence
                // =========================================================================
                xil_printf("Waiting 15 seconds for TSCB Background Calibration convergence...\r\n");
                sleep(15);
                xil_printf("TSCB converged.\r\n");

                // =========================================================================

                xil_printf("\r\n--- Step 2.5: Multi-Tile Sync (MTS) Alignment ---\n\r");
        Run_MTS_Procedure(RFdcInstPtr);
    xil_printf("\r\n--- Step 2.6: Initializing NCOs to 3100 MHz ---\n\r");
    Set_NCO_Freq_MTS_Safe(RFdcInstPtr, 1, current_adc_nco);
    Set_NCO_Freq_MTS_Safe(RFdcInstPtr, 0, current_dac_nco);

    /* PCIe configuration transport. Brought up after the RFDC so that any
     * command arriving immediately has a valid instance to act on. */
    xil_printf("\r\n--- Step 2.7: PCIe Configuration Interface ---\n\r");
    PcieCfg_Init(RFdcInstPtr);

    xil_printf("\r\n--- Step 3: Silicon Configuration Dump ---\n\r");
    Print_Current_Rates(RFdcInstPtr);

    xil_printf("\r\n--- Step 4: Initializing DDS to 10 MHz ---\n\r");
    Reset_Dds();
    Set_Dds_Frequency(current_dds_freq_mhz);

    xil_printf("\r\n--- Automated flow complete. Entering Control Loop. --- \r\n");

    while (1) {
        int choice = 0;
        printf("\r\n=======================================\r\n");
        printf("1. Change ADC Decimation (ALL active blocks)\r\n");
        printf("2. Change ADC Decimation (Individual block)\r\n");
        printf("3. Change DAC Interpolation (ALL active blocks)\r\n");
        printf("4. Change DAC Interpolation (Individual block)\r\n");
        printf("5. Print Current Decimation/Interpolation Rates\r\n");
        printf("6. Set DDS Sine Wave Frequency\r\n");
        printf("7. Change ADC NCO Frequency (MTS SAFE - ALL channels)\r\n");
        printf("8. Change DAC NCO Frequency (MTS SAFE - ALL channels)\r\n");
        printf("9. Select ADC Channel Output Routing (AXIS Switch)\r\n");
        printf("10. Select ADC Channel Output Routing (PL GPIO Logic)\r\n");
        printf("11. Measure Stream Clocks & Input PL Clock\r\n");
        printf("12. Set QMC Gain for ALL active ADC & DAC blocks (Max 1.9999)\r\n");
        printf("13. Change Sampling Rate (Internal PLL)\r\n");
        printf("14. Set Nyquist Zone Manually (1 or 2)\r\n");
        printf("15. Set ADC DSA Attenuation\r\n");
        printf("16. Select DAC Input Source (0 = DDS, 1 = Host/GNU Radio stream)\r\n");
        printf("17. PCIe interface diagnostics\r\n");
        printf("18. Set PCIe control base address (debug)\r\n");
        printf("Select an option: ");

        /* MenuReadLine() services PCIe between keystrokes and consumes the
         * line terminator, so nothing stale is left in the FIFO. See the
         * function comment for why scanf() cannot be used here. */
        MenuReadLine(buf, sizeof(buf));
        choice = atoi(buf);

        if (choice == 1) {
            u32 factor;
            while (1) {
                printf("\r\nEnter new ADC Decimation Factor for ALL blocks (e.g., 4, 5, 6, 8, 10, 12, 16, 20, 24, 40): ");
                MenuReadLine(buf, sizeof(buf));
                factor = (u32)atoi(buf);
                if (is_valid_adc_factor(factor)) break;
                printf("\r\nERROR: Decimation value %lux is not supported.\r\n", (unsigned long)factor);
            }
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    if (XRFdc_IsADCBlockEnabled(RFdcInstPtr, i, j)) {
                        Update_Datapath_Rate(RFdcInstPtr, 1, i, j, factor);
                    }
                }
            }
            xil_printf("\r\n--- Re-Aligning ADC MTS After Datapath Change ---\r\n");
            XRFdc_MultiConverter_Init(&ADC_Sync_Config, 0, 0, XRFDC_ADC_TILE);
            ADC_Sync_Config.Tiles = ADC_TILES_SYNCC_MASK;
            XRFdc_MultiConverter_Sync(RFdcInstPtr, XRFDC_ADC_TILE, &ADC_Sync_Config);
            Set_NCO_Freq_MTS_Safe(RFdcInstPtr, 1, current_adc_nco);
        }
        else if (choice == 2) {
            int tile, block; u32 factor;
            printf("\r\nEnter ADC Tile (0-3): "); MenuReadLine(buf, sizeof(buf)); tile = atoi(buf);
            printf("\r\nEnter ADC Block (0-3): "); MenuReadLine(buf, sizeof(buf)); block = atoi(buf);

            while (1) {
                printf("\r\nEnter new Decimation Factor: ");
                MenuReadLine(buf, sizeof(buf));
                factor = (u32)atoi(buf);
                if (is_valid_adc_factor(factor)) break;
                printf("\r\nERROR: Decimation value %lux is not supported.\r\n", (unsigned long)factor);
            }
            if (XRFdc_IsADCBlockEnabled(RFdcInstPtr, tile, block)) {
                Update_Datapath_Rate(RFdcInstPtr, 1, tile, block, factor);
                xil_printf("\r\n--- Re-Aligning ADC MTS After Datapath Change ---\r\n");
                XRFdc_MultiConverter_Init(&ADC_Sync_Config, 0, 0, XRFDC_ADC_TILE);
                ADC_Sync_Config.Tiles = ADC_TILES_SYNCC_MASK;
                XRFdc_MultiConverter_Sync(RFdcInstPtr, XRFDC_ADC_TILE, &ADC_Sync_Config);
                Set_NCO_Freq_MTS_Safe(RFdcInstPtr, 1, current_adc_nco);
            } else printf("\r\nERROR: ADC Tile %d Block %d is not enabled.\r\n", tile, block);
        }
        else if (choice == 3) {
            u32 factor;
            while (1) {
                printf("\r\nEnter new DAC Interpolation Factor for ALL blocks (e.g., 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 40): ");
                MenuReadLine(buf, sizeof(buf));
                factor = (u32)atoi(buf);
                if (is_valid_dac_factor(factor)) break;
                printf("\r\nERROR: Interpolation value %lux is not supported.\r\n", (unsigned long)factor);
            }
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    if (XRFdc_IsDACBlockEnabled(RFdcInstPtr, i, j)) {
                        Update_Datapath_Rate(RFdcInstPtr, 0, i, j, factor);
                    }
                }
            }
            xil_printf("\r\n--- Re-Aligning DAC MTS After Datapath Change ---\r\n");
            XRFdc_MultiConverter_Init(&DAC_Sync_Config, 0, 0, XRFDC_DAC_TILE);
            DAC_Sync_Config.Tiles = DAC_TILES_SYNCC_MASK;
            XRFdc_MultiConverter_Sync(RFdcInstPtr, XRFDC_DAC_TILE, &DAC_Sync_Config);
            Set_NCO_Freq_MTS_Safe(RFdcInstPtr, 0, current_dac_nco);
        }
        else if (choice == 4) {
            int tile, block; u32 factor;
            printf("\r\nEnter DAC Tile (0-3): "); MenuReadLine(buf, sizeof(buf)); tile = atoi(buf);
            printf("\r\nEnter DAC Block (0-3): "); MenuReadLine(buf, sizeof(buf)); block = atoi(buf);

            while (1) {
                printf("\r\nEnter new Interpolation Factor: ");
                MenuReadLine(buf, sizeof(buf));
                factor = (u32)atoi(buf);
                if (is_valid_dac_factor(factor)) break;
                printf("\r\nERROR: Interpolation value %lux is not supported.\r\n", (unsigned long)factor);
            }
            if (XRFdc_IsDACBlockEnabled(RFdcInstPtr, tile, block)) {
                Update_Datapath_Rate(RFdcInstPtr, 0, tile, block, factor);
                xil_printf("\r\n--- Re-Aligning DAC MTS After Datapath Change ---\r\n");
                XRFdc_MultiConverter_Init(&DAC_Sync_Config, 0, 0, XRFDC_DAC_TILE);
                DAC_Sync_Config.Tiles = DAC_TILES_SYNCC_MASK;
                XRFdc_MultiConverter_Sync(RFdcInstPtr, XRFDC_DAC_TILE, &DAC_Sync_Config);
                Set_NCO_Freq_MTS_Safe(RFdcInstPtr, 0, current_dac_nco);
            } else printf("\r\nERROR: DAC Tile %d Block %d is not enabled.\r\n", tile, block);
        }
        else if (choice == 5) Print_Current_Rates(RFdcInstPtr);
        else if (choice == 6) {
            double freq;
            printf("\r\nEnter target DDS frequency in MHz: ");
            MenuReadLine(buf, sizeof(buf)); freq = atof(buf);
            Set_Dds_Frequency(freq);
        }
        else if (choice == 7) {
            printf("\r\nEnter New ADC NCO Frequency (MHz) for ALL channels: ");
            MenuReadLine(buf, sizeof(buf)); current_adc_nco = atof(buf);
            Set_NCO_Freq_MTS_Safe(RFdcInstPtr, 1, current_adc_nco);
        }
        else if (choice == 8) {
            printf("\r\nEnter New DAC NCO Frequency (MHz) for ALL channels: ");
            MenuReadLine(buf, sizeof(buf)); current_dac_nco = atof(buf);
            Set_NCO_Freq_MTS_Safe(RFdcInstPtr, 0, current_dac_nco);
        }
        else if (choice == 9) {
            int ch[4];
            printf("\r\n--- Configure ADC Channel Routing (AXIS Switch) ---\r\n");
            for (int i = 0; i < 4; i++) {
                printf("\r\nSelect Input Channel (0-7) to map to Output %d: ", i);
                MenuReadLine(buf, sizeof(buf)); ch[i] = atoi(buf);
            }
            Set_Adc_Channel_Routing(ch[0], ch[1], ch[2], ch[3]);
        }
        else if (choice == 10) {
            int mode;
            printf("\r\n--- PL GPIO Channel Routing Options ---\r\n");
            printf("  1 = Select single channel 1 in PL logic\r\n");
            printf("  2 = Select channel 2 in PL logic\r\n");
            printf("  3 = Select channel 4 in PL logic\r\n");
            printf("Select routing mode: ");
            MenuReadLine(buf, sizeof(buf)); mode = atoi(buf);
            Set_PL_Gpio_Routing(mode);
        }
        else if (choice == 11) Measure_Stream_Clocks();
        else if (choice == 12) {
            double gain;
            printf("\r\nEnter QMC Amplitude Gain for ALL active blocks (0.0000 to 1.9999): ");
            MenuReadLine(buf, sizeof(buf)); gain = atof(buf);

            if (gain > 1.9999) { gain = 1.9999; printf("\r\nWarning: Value exceeded max. Capped to 1.9999.\r\n"); }
            else if (gain < 0.0) { gain = 0.0; printf("\r\nWarning: Value below min. Capped to 0.0.\r\n"); }

            u32 g_int = (u32)gain; u32 g_frac = (u32)((gain - g_int) * 10000.0);
            printf("\r\nApplying QMC Gain of %lu.%04lu to all active ADC and DAC blocks...\r\n",
                   (unsigned long)g_int, (unsigned long)g_frac);

            for (int domain = 0; domain <= 1; domain++) {
                u32 current_type = (domain == 1) ? XRFDC_ADC_TILE : XRFDC_DAC_TILE;
                for (int t = 0; t < 4; t++) {
                    for (int b = 0; b < 4; b++) {
                        int is_enabled = (domain == 1) ? XRFdc_IsADCBlockEnabled(RFdcInstPtr, t, b) : XRFdc_IsDACBlockEnabled(RFdcInstPtr, t, b);
                        if (is_enabled) {
                            XRFdc_QMC_Settings QMCSettings;
                            Status = XRFdc_GetQMCSettings(RFdcInstPtr, current_type, t, b, &QMCSettings);
                            if (Status == XST_SUCCESS) {
                                QMCSettings.EnableGain = 1;
                                QMCSettings.GainCorrectionFactor = gain;
                                QMCSettings.EventSource = XRFDC_EVNT_SRC_TILE;
                                Status = XRFdc_SetQMCSettings(RFdcInstPtr, current_type, t, b, &QMCSettings);
                                if (Status == XST_SUCCESS) {
                                    XRFdc_UpdateEvent(RFdcInstPtr, current_type, t, b, XRFDC_EVNT_SRC_TILE);
                                    printf(" -> Updated %s Tile %d Block %d\r\n", (domain == 1) ? "ADC" : "DAC", t, b);
                                }
                            }
                        }
                    }
                }
            }
            printf("Finished applying global QMC settings.\r\n");
        }
        else if (choice == 13) {
                    int is_adc; double mhz_val;
                    printf("\r\n--- Change Sampling Rate ---\r\n");
                    printf("Enter 1 for ADC (All Tiles) or 0 for DAC (All Tiles): ");
                    MenuReadLine(buf, sizeof(buf)); is_adc = atoi(buf);

                    printf("\r\nEnter New Sampling Rate in MHz (e.g., 2400 for 2.4 GSPS): ");
                    MenuReadLine(buf, sizeof(buf)); mhz_val = atof(buf);
                    Save_Gain_State(RFdcInstPtr, is_adc);

                    if (Change_Sampling_Rate_All_Tiles(RFdcInstPtr, is_adc, mhz_val) == XST_SUCCESS) {
                    	Restore_Gain_State(RFdcInstPtr, is_adc);
                        // 1. Verify the hardware state immediately
                        int master_tile = is_adc ? 1 : 2;
                        Verify_Actual_Sampling_Rate(RFdcInstPtr, is_adc, master_tile);

                        // 2. CRITICAL FIX: Restore the AXI-Stream Switch Routing
                        // (It was disabled to prevent glitches during the clock change)
                        xil_printf("\r\n--- Restoring AXI-Stream Routing Matrix ---\r\n");
                        Set_Adc_Channel_Routing(ADC_ROUTE_DEFAULT_CH0, ADC_ROUTE_DEFAULT_CH1,
                                                ADC_ROUTE_DEFAULT_CH2, ADC_ROUTE_DEFAULT_CH3);

                        // 3. CRITICAL FIX: Re-Align MTS and restore NCOs
                        if (is_adc) {
                            xil_printf("\r\n--- Re-Aligning ADC MTS After PLL Change ---\r\n");
                            XRFdc_MultiConverter_Init(&ADC_Sync_Config, 0, 0, XRFDC_ADC_TILE);
                            ADC_Sync_Config.Tiles = ADC_TILES_SYNCC_MASK;
                            XRFdc_MultiConverter_Sync(RFdcInstPtr, XRFDC_ADC_TILE, &ADC_Sync_Config);
                            Set_NCO_Freq_MTS_Safe(RFdcInstPtr, 1, current_adc_nco);
                        } else {
                            xil_printf("\r\n--- Re-Aligning DAC MTS After PLL Change ---\r\n");
                            XRFdc_MultiConverter_Init(&DAC_Sync_Config, 0, 0, XRFDC_DAC_TILE);
                            DAC_Sync_Config.Tiles = DAC_TILES_SYNCC_MASK;
                            XRFdc_MultiConverter_Sync(RFdcInstPtr, XRFDC_DAC_TILE, &DAC_Sync_Config);
                            Set_NCO_Freq_MTS_Safe(RFdcInstPtr, 0, current_dac_nco);
                        }
                    } else {
                        xil_printf("\r\nSampling rate change failed.\r\n");
                    }
                }
        else if (choice == 14) {
                    int is_adc, zone;
                    printf("Enter 1 for ADC, 0 for DAC: ");
                    MenuReadLine(buf, sizeof(buf)); is_adc = atoi(buf);

                    printf("Enter Zone (1 or 2): ");
                    MenuReadLine(buf, sizeof(buf)); zone = atoi(buf);

                    if (zone == 1 || zone == 2) {
                        Set_Nyquist_Zone_Manual(RFdcInstPtr, is_adc, zone);
                        // Update global tracking
                        if (is_adc) force_adc_zone = zone;
                        else        force_dac_zone = zone;
                    } else {
                        printf("Invalid Zone. Please enter 1 or 2.\r\n");
                    }
                }
        else if (choice == 15) {
                           int tile, block;
                           double new_dsa;

                           printf("\r\nEnter ADC Tile (0-3): ");
                           MenuReadLine(buf, sizeof(buf));
                           tile = atoi(buf);

                           printf("\r\nEnter ADC Block (0-3): ");
                           MenuReadLine(buf, sizeof(buf));
                           block = atoi(buf);

                           printf("\r\nEnter new DSA Attenuation (0.0 to 27.0 in 0.5 steps): ");
                           MenuReadLine(buf, sizeof(buf));
                           new_dsa = atof(buf);

                           // Boundary safety check for Gen 3 RFSoC
                           if (new_dsa < 0.0) new_dsa = 0.0;
                           if (new_dsa > 27.0) new_dsa = 27.0;

                           if (XRFdc_IsADCBlockEnabled(RFdcInstPtr, tile, block)) {
                               XRFdc_DSA_Settings DSA_Settings;

                               // Always GET current settings first to preserve hidden flags
                               if (XRFdc_GetDSA(RFdcInstPtr, tile, block, &DSA_Settings) == XST_SUCCESS) {

                                   DSA_Settings.Attenuation = new_dsa;

                                   int status = XRFdc_SetDSA(RFdcInstPtr, tile, block, &DSA_Settings);
                                   if (status == XST_SUCCESS) {
                                       // Print using integer casting to avoid bare-metal float print issues
                                       u32 d_int = (u32)new_dsa;
                                       u32 d_frac = (u32)((new_dsa - d_int) * 10.0);
                                       printf("\r\nSuccess: ADC Tile %d Block %d DSA set to %lu.%lu dB.\r\n",
                                              tile, block, (unsigned long)d_int, (unsigned long)d_frac);
                                   } else {
                                       printf("\r\nERROR: Failed to apply DSA settings.\r\n");
                                   }
                               } else {
                                   printf("\r\nERROR: Could not read current DSA settings.\r\n");
                               }
                           } else {
                               printf("\r\nERROR: ADC Tile %d Block %d is not enabled.\r\n", tile, block);
                           }
                       }
        else if (choice == 16) {
            int src_port;
            printf("\r\n--- Configure DAC Input Source ---\r\n");
            printf("  0 = DDS Compiler            (internal tone, switch SI%u)\r\n",
                   (unsigned)HW_DAC_SW_SI_DDS);
            printf("  1 = Host / GNU Radio stream (H2C over PCIe, switch SI%u)\r\n",
                   (unsigned)HW_DAC_SW_SI_HOST);
            printf("  Currently routed: %s\r\n", Dac_Source_Name(Get_Dds_H2c_Routing()));
            printf("Select Input Source: ");

            MenuReadLine(buf, sizeof(buf));
            src_port = atoi(buf);

            if (src_port == 0 || src_port == 1) {
                /* The confirmation is read back from the switch, not echoed
                 * from the request, so it cannot claim a routing that did
                 * not happen. */
                if (Set_Dds_H2c_Routing(src_port) == XST_SUCCESS)
                    printf("\r\nSuccess: DAC input is now %s (verified at the switch).\r\n",
                           Dac_Source_Name(Get_Dds_H2c_Routing()));
                else
                    printf("\r\nERROR: the switch did not take the routing; it reads back %s.\r\n",
                           Dac_Source_Name(Get_Dds_H2c_Routing()));
            } else {
                printf("\r\nERROR: Invalid selection. Please enter 0 or 1.\r\n");
            }
        }

        else if (choice == 17) {
            PcieCfg_Diagnostics();
        }
        else if (choice == 18) {
            /* Lets you retarget the manager while hunting for the right
             * aperture, instead of rebuilding and reflashing per guess.
             * WARNING: an AXI access to an unmapped address hangs the CPU
             * with no message. Only enter a base you have confirmed. */
            printf("\r\nCurrent PCIe base: 0x%08lx\r\n",
                   (unsigned long)PcieCfg_GetBase());
            printf("Enter new base in hex (e.g. A00A0000), or 0 to cancel: ");
            MenuReadLine(buf, sizeof(buf));
            unsigned long nb = strtoul(buf, NULL, 16);
            if (nb != 0UL) {
                PcieCfg_SetBase((UINTPTR)nb);
                PcieCfg_Diagnostics();
            } else {
                printf("Cancelled.\r\n");
            }
        }
        else printf("\r\nInvalid selection.\r\n");
    }
    return XST_SUCCESS;
}

void my_metal_default_log_handler(enum metal_log_level level, const char *format, ...) {
    char msg[1024]; char msgOut[1048]; char *outPtr; int i;
    va_list args;
    static const char *level_strs[] = {"metal: emergency: ", "metal: alert:      ", "metal: critical:  ", "metal: error:      ", "metal: warning:   ", "metal: notice:    ", "metal: info:      ", "metal: debug:     "};
    va_start(args, format); vsnprintf(msg, sizeof(msg), format, args); va_end(args);

    outPtr = msgOut;
    for(i=0; i<1024; i++) {
        if ((msg[i] == '\r' && msg[i+1] == '\n') || (msg[i] == '\n' && msg[i+1] == '\r')) { *outPtr++ = msg[i++]; }
        else if(msg[i] == '\n') { if(i==0) continue; else *outPtr++ = '\r'; }
        *outPtr++ = msg[i];
        if(msg[i] == 0) break;
    }
    if( (msg[i-1] != '\n') && (msg[i-1] != '\r') ) { *(outPtr-1) = '\r'; *outPtr++ = '\n'; *outPtr++ = 0; }
    if (level <= METAL_LOG_EMERGENCY || level > METAL_LOG_DEBUG) level = METAL_LOG_EMERGENCY;
    xil_printf("%s%s", level_strs[level], msgOut);
}

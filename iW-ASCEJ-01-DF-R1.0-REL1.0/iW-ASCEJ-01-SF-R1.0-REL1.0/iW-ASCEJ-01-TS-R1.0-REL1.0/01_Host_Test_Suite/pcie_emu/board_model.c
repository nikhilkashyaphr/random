/*
 * The board, minus the silicon: everything pcie_cfg.c calls in main.c and the
 * RFDC driver, modelled closely enough for the PCIe protocol to be exercised
 * end to end. The DDS/H2C switch is modelled at REGISTER level with the
 * bitstream's real wiring (S00 <- host H2C stream, S01 <- DDS).
 *
 * Built twice:
 *   current  - the routing functions are main.c's own text (extracted)
 *   LEGACY   - main.c 1.2.0's routing, verbatim: the port number is the value
 */
#include "xil_types.h"
#include "xstatus.h"
#include "xrfdc.h"
#include "pcie_regs.h"

XRFdc  RFdcInst;
double current_adc_nco = 3100.0, current_dac_nco = 3100.0;
double global_adc_gsps = 4.8,  global_dac_gsps = 9.6;
double current_dds_freq_mhz = 10.0, current_dac_stream_clk_mhz = 200.0;
int    force_adc_zone = 0, force_dac_zone = 0;
static u32 g_dec = 24, g_itp = 24;

int  Update_Datapath_Rate(XRFdc *p, int is_adc, int t, int b, u32 f)
{ (void)p; (void)t; (void)b;
  if (is_adc) g_dec = f; else { g_itp = f; current_dac_stream_clk_mhz = global_dac_gsps * 1000.0 / (f * 2.0); }
  return XST_SUCCESS; }
void Set_NCO_Freq_MTS_Safe(XRFdc *p, int is_adc, double f) { (void)p; if (is_adc) current_adc_nco = f; else current_dac_nco = f; }
void Set_Dds_Frequency(double f) { current_dds_freq_mhz = f; }
int  Change_Sampling_Rate_All_Tiles(XRFdc *p, int is_adc, double mhz)
{ (void)p; if (is_adc) global_adc_gsps = mhz / 1000.0; else global_dac_gsps = mhz / 1000.0; return XST_SUCCESS; }
void Set_Nyquist_Zone_Manual(XRFdc *p, int is_adc, int z) { (void)p; if (is_adc) force_adc_zone = z; else force_dac_zone = z; }
void Set_Adc_Channel_Routing(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
void Set_PL_Gpio_Routing(int m) { (void)m; }
int  Run_MTS_Procedure(XRFdc *p) { (void)p; return XST_SUCCESS; }
void Save_Gain_State(XRFdc *p, int a) { (void)p; (void)a; }
void Restore_Gain_State(XRFdc *p, int a) { (void)p; (void)a; }

/* ---- RFDC driver ---- */
u32 XRFdc_IsADCBlockEnabled(XRFdc *p, u32 t, u32 b) { (void)p; return t < 4 && b < 2; }
u32 XRFdc_IsDACBlockEnabled(XRFdc *p, u32 t, u32 b) { (void)p; return t < 4 && b < 2; }
u32 XRFdc_GetMixerSettings(XRFdc *p, u32 ty, u32 t, u32 b, XRFdc_Mixer_Settings *m)
{ (void)p; (void)t; (void)b; m->Freq = ty == XRFDC_ADC_TILE ? current_adc_nco : current_dac_nco; m->PhaseOffset = 0; return XRFDC_SUCCESS; }
u32 XRFdc_SetMixerSettings(XRFdc *p, u32 ty, u32 t, u32 b, XRFdc_Mixer_Settings *m)
{ (void)p; (void)t; (void)b; if (ty == XRFDC_ADC_TILE) current_adc_nco = m->Freq; else current_dac_nco = m->Freq; return XRFDC_SUCCESS; }
u32 XRFdc_UpdateEvent(XRFdc *p, u32 ty, u32 t, u32 b, u32 ev) { (void)p; (void)ty; (void)t; (void)b; (void)ev; return XRFDC_SUCCESS; }
static float g_dsa = 0.0f; static double g_qmc = 1.0;
u32 XRFdc_GetDSA(XRFdc *p, u32 t, u32 b, XRFdc_DSA_Settings *d) { (void)p; (void)t; (void)b; d->Attenuation = g_dsa; return XRFDC_SUCCESS; }
u32 XRFdc_SetDSA(XRFdc *p, u32 t, u32 b, XRFdc_DSA_Settings *d) { (void)p; (void)t; (void)b; g_dsa = d->Attenuation; return XRFDC_SUCCESS; }
u32 XRFdc_GetQMCSettings(XRFdc *p, u32 ty, u32 t, u32 b, XRFdc_QMC_Settings *q) { (void)p; (void)ty; (void)t; (void)b; q->GainCorrectionFactor = g_qmc; q->EnableGain = 1; return XRFDC_SUCCESS; }
u32 XRFdc_SetQMCSettings(XRFdc *p, u32 ty, u32 t, u32 b, XRFdc_QMC_Settings *q) { (void)p; (void)ty; (void)t; (void)b; g_qmc = q->GainCorrectionFactor; return XRFDC_SUCCESS; }
u32 XRFdc_SetCalFreeze(XRFdc *p, u32 t, u32 b, XRFdc_Cal_Freeze_Settings *c) { (void)p; (void)t; (void)b; (void)c; return XRFDC_SUCCESS; }
u32 XRFdc_SetDACVOP(XRFdc *p, u32 t, u32 b, u32 uA) { (void)p; (void)t; (void)b; (void)uA; return XRFDC_SUCCESS; }
u32 XRFdc_SetupFIFO(XRFdc *p, u32 ty, int t, u8 en) { (void)p; (void)ty; (void)t; (void)en; return XRFDC_SUCCESS; }
u32 XRFdc_GetDecimationFactor(XRFdc *p, u32 t, u32 b, u32 *f) { (void)p; (void)t; (void)b; *f = g_dec; return XRFDC_SUCCESS; }
u32 XRFdc_GetInterpolationFactor(XRFdc *p, u32 t, u32 b, u32 *f) { (void)p; (void)t; (void)b; *f = g_itp; return XRFDC_SUCCESS; }

/* ---- DDS_H2C_switch, register level (PG085 MI_MUX[0]) ---- */
static u32 mi_mux0 = 0x80000000u;
typedef struct { struct { int MaxNumSI, MaxNumMI; } Config; } XAxis_Switch;
static XAxis_Switch DdsH2cSwitchInst = { { 2, 1 } };
static void XAxisScr_RegUpdateDisable(XAxis_Switch *p) { (void)p; }
static void XAxisScr_RegUpdateEnable(XAxis_Switch *p)  { (void)p; }
static void XAxisScr_MiPortDisableAll(XAxis_Switch *p) { (void)p; mi_mux0 = 0x80000000u; }
static void XAxisScr_MiPortEnable(XAxis_Switch *p, u8 mi, u8 si) { (void)p; if (mi == 0 && si < 2) mi_mux0 = si; }

/* What the DAC plays, by the netlist: 0 = host (H2C), 1 = DDS, -1 = nothing. */
int emu_dac_plays(void) { return (mi_mux0 >> 31) ? -1 : (int)mi_mux0; }

#ifndef LEGACY
#include "platform_ids.h"
static s32 XAxisScr_IsMiPortEnabled(XAxis_Switch *p, u8 mi, u8 si)
{ (void)p; return mi == 0 && !(mi_mux0 >> 31) && (mi_mux0 & 0x7FFFFFFFu) == si; }
int Set_Dds_H2c_Routing(int source);
int Get_Dds_H2c_Routing(void);
const char *Dac_Source_Name(int source);
#include "routing_under_test.inc"
void emu_board_boot(void)     /* what Init_Dds_H2c_Switch() does */
{ dds_h2c_switch_ready = 1; Set_Dds_H2c_Routing((int)PCIE_DEF_DAC_SOURCE); }
#else
/* main.c 1.2.0, verbatim */
void Set_Dds_H2c_Routing(int si_port) {
    XAxisScr_RegUpdateDisable(&DdsH2cSwitchInst);
    XAxisScr_MiPortDisableAll(&DdsH2cSwitchInst);
    if(si_port >= 0 && si_port <= 1) {
        XAxisScr_MiPortEnable(&DdsH2cSwitchInst, 0, si_port);
    }
    XAxisScr_RegUpdateEnable(&DdsH2cSwitchInst);
}
void emu_board_boot(void)     /* 1.2.0 Init_Dds_H2c_Switch(): "SI 0 (DDS)" */
{ XAxisScr_MiPortDisableAll(&DdsH2cSwitchInst); XAxisScr_MiPortEnable(&DdsH2cSwitchInst, 0, 0); }
#endif

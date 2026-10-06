/* Minimal RFDC driver surface used by pcie_cfg.c, backed by a model in
 * board_model.c. Signatures follow xrfdc.h of the 2025.2 BSP. */
#ifndef XRFDC_H
#define XRFDC_H
#include "xil_types.h"
#define XRFDC_SUCCESS 0U
#define XRFDC_FAILURE 1U
#define XRFDC_ADC_TILE 0U
#define XRFDC_DAC_TILE 1U
#define XRFDC_EVENT_MIXER 0x1U
#define XRFDC_EVNT_SRC_TILE 0x2U
typedef struct { int dummy; } XRFdc;
typedef struct { double Freq; double PhaseOffset; u32 EventSource; u32 CoarseMixFreq;
                 u32 MixerMode; u8 FineMixerScale; u8 MixerType; } XRFdc_Mixer_Settings;
typedef struct { u32 DisableRTS; float Attenuation; } XRFdc_DSA_Settings;
typedef struct { u32 EnablePhase; u32 EnableGain; double GainCorrectionFactor;
                 double PhaseCorrectionFactor; s32 OffsetCorrectionFactor;
                 u32 EventSource; } XRFdc_QMC_Settings;
typedef struct { u32 CalFrozen; u32 DisableFreezePin; u32 FreezeCalibration; } XRFdc_Cal_Freeze_Settings;
u32 XRFdc_IsADCBlockEnabled(XRFdc *p, u32 t, u32 b);
u32 XRFdc_IsDACBlockEnabled(XRFdc *p, u32 t, u32 b);
u32 XRFdc_GetMixerSettings(XRFdc *p, u32 ty, u32 t, u32 b, XRFdc_Mixer_Settings *m);
u32 XRFdc_SetMixerSettings(XRFdc *p, u32 ty, u32 t, u32 b, XRFdc_Mixer_Settings *m);
u32 XRFdc_UpdateEvent(XRFdc *p, u32 ty, u32 t, u32 b, u32 ev);
u32 XRFdc_GetDSA(XRFdc *p, u32 t, u32 b, XRFdc_DSA_Settings *d);
u32 XRFdc_SetDSA(XRFdc *p, u32 t, u32 b, XRFdc_DSA_Settings *d);
u32 XRFdc_GetQMCSettings(XRFdc *p, u32 ty, u32 t, u32 b, XRFdc_QMC_Settings *q);
u32 XRFdc_SetQMCSettings(XRFdc *p, u32 ty, u32 t, u32 b, XRFdc_QMC_Settings *q);
u32 XRFdc_SetCalFreeze(XRFdc *p, u32 t, u32 b, XRFdc_Cal_Freeze_Settings *c);
u32 XRFdc_SetDACVOP(XRFdc *p, u32 t, u32 b, u32 uA);
u32 XRFdc_SetupFIFO(XRFdc *p, u32 ty, int t, u8 en);
u32 XRFdc_GetDecimationFactor(XRFdc *p, u32 t, u32 b, u32 *f);
u32 XRFdc_GetInterpolationFactor(XRFdc *p, u32 t, u32 b, u32 *f);
#endif

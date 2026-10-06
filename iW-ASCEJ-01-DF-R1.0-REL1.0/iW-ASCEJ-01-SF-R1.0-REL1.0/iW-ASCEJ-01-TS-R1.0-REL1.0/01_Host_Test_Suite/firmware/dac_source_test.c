/*
 * Host-side test of the firmware's DAC input source routing.
 *
 * The routing functions are compiled FROM main.c's own text (extracted by
 * run_tests.sh), against a register-level model of the AXI4-Stream switch
 * wired exactly as the bitstream's hardware handoff says:
 *
 *     S00_AXIS <- H2C_data_in (host)      S01_AXIS <- dds_compiler_2 (DDS)
 *
 * so the test checks behaviour at the switch, not the code's own opinion.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
#define XST_SUCCESS 0
#define XST_FAILURE 1
#define XST_DEVICE_NOT_FOUND 2
#define XST_INVALID_PARAM 15
#define TRUE 1
#define FALSE 0
#include "pcie_regs.h"

/* Hardware facts, copied from platform_ids.h by run_tests.sh. */
#include "platform_wiring.h"

/* ---- register model of axis_switch v1.1 (PG085) ---- */
static u32 mi_mux0 = 0x80000000u;          /* disabled at reset             */
static int reg_updates = 0;
typedef struct { struct { int MaxNumSI, MaxNumMI; } Config; } XAxis_Switch;
static XAxis_Switch DdsH2cSwitchInst = { { 2, 1 } };
static void XAxisScr_RegUpdateDisable(XAxis_Switch *p) { (void)p; }
static void XAxisScr_RegUpdateEnable(XAxis_Switch *p)  { (void)p; reg_updates++; }
static void XAxisScr_MiPortDisableAll(XAxis_Switch *p) { (void)p; mi_mux0 = 0x80000000u; }
static void XAxisScr_MiPortEnable(XAxis_Switch *p, u8 mi, u8 si)
{ (void)p; if (mi == 0 && si < 2) mi_mux0 = si; }
static s32 XAxisScr_IsMiPortEnabled(XAxis_Switch *p, u8 mi, u8 si)
{ (void)p; return mi == 0 && !(mi_mux0 >> 31) && (mi_mux0 & 0x7FFFFFFFu) == si; }

/* What the DAC actually receives, per the hwh netlist. */
static const char *dac_sees(void)
{
    if (mi_mux0 >> 31) return "nothing";
    return mi_mux0 == 0 ? "host" : mi_mux0 == 1 ? "dds" : "?";
}

int Set_Dds_H2c_Routing(int source);
int Get_Dds_H2c_Routing(void);
const char *Dac_Source_Name(int source);
#include "routing_under_test.inc"          /* the functions, from main.c */

static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("  PASS  " __VA_ARGS__); \
                           else { printf("  FAIL  " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(void)
{
    printf("DAC input source routing (firmware main.c)\n");

    CHECK(Set_Dds_H2c_Routing(PCIE_DAC_SRC_DDS) == XST_DEVICE_NOT_FOUND,
          "refuses to touch the switch before it is initialised");
    CHECK(strcmp(dac_sees(), "nothing") == 0, "  ...and leaves it untouched");

    dds_h2c_switch_ready = 1;              /* what Init_Dds_H2c_Switch does */
    CHECK(Set_Dds_H2c_Routing((int)PCIE_DEF_DAC_SOURCE) == XST_SUCCESS,
          "boot default routes successfully");
    CHECK(strcmp(dac_sees(), "dds") == 0, "boot default puts the DDS on the DAC (was: host)");

    CHECK(Set_Dds_H2c_Routing(PCIE_DAC_SRC_HOST) == XST_SUCCESS, "select host");
    CHECK(strcmp(dac_sees(), "host") == 0, "host selection puts the H2C stream on the DAC");
    CHECK(Get_Dds_H2c_Routing() == (int)PCIE_DAC_SRC_HOST, "readback reports host");

    CHECK(Set_Dds_H2c_Routing(PCIE_DAC_SRC_DDS) == XST_SUCCESS, "select DDS");
    CHECK(strcmp(dac_sees(), "dds") == 0, "DDS selection puts the DDS on the DAC");
    CHECK(Get_Dds_H2c_Routing() == (int)PCIE_DAC_SRC_DDS, "readback reports DDS");

    CHECK(Set_Dds_H2c_Routing(2)  == XST_INVALID_PARAM, "rejects source 2");
    CHECK(Set_Dds_H2c_Routing(-1) == XST_INVALID_PARAM, "rejects source -1");
    CHECK(strcmp(dac_sees(), "dds") == 0, "an invalid request leaves the routing alone");

    mi_mux0 = 0x80000000u;
    CHECK(Get_Dds_H2c_Routing() == (int)PCIE_DAC_SRC_NONE, "disabled switch reads NONE");
    CHECK(strcmp(Dac_Source_Name(PCIE_DAC_SRC_DDS), "DDS compiler") == 0, "name: DDS");
    CHECK(strcmp(Dac_Source_Name(PCIE_DAC_SRC_HOST), "Host / GNU Radio stream") == 0, "name: host");

    /* The switch is changed atomically: one committed update per request. */
    reg_updates = 0;
    Set_Dds_H2c_Routing(PCIE_DAC_SRC_HOST);
    CHECK(reg_updates == 1, "exactly one register commit per change");

    printf("\nRESULT: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

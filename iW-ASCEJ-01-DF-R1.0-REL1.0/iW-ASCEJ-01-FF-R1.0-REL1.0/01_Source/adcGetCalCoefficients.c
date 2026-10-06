/*
 * adcFreezeCal.c
 *
 *  Created on: Aug 10, 2019
 *      Author: jlantz
 */



/***************************** Include Files *********************************/
#include "xil_io.h"
#include "xil_types.h"
#include <stdio.h>
#include "cli.h"
#include "xparameters.h"
#include "main.h"
#include "xrfdc.h"
#include "adcGetCalCoefficients.h"


/************************** Constant Definitions *****************************/

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Function Prototypes ******************************/
void displayCoeff(XRFdc_Calibration_Coefficients *CoeffPtr);

/************************** Variable Definitions *****************************/

/************************** Function Definitions ******************************/


/*****************************************************************************/
/**
*
* cli_adcGetCalCoefficients_init Add functions from this file to CLI
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void cli_adcGetCalCoefficients_init(void)
{
	static CMDSTRUCT cliCmds[] = {
		//000000000011111111112222    000000000011111111112222222222333333333
		//012345678901234567890123    012345678901234567890123456789012345678
		{"adcGetCalCoeff"     , "- <tile> <adc> Get Cal Coeff for ch"     , 2, *adcGetCalCoeff},
	};

	cli_addCmds(cliCmds, sizeof(cliCmds)/sizeof(cliCmds[0]));
}


/*****************************************************************************/
/**
*
* adcGetCalCoeff TBD
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void adcGetCalCoeff (u32 *cmdVals)
{
	u32 Tile_Id;
	u32 Block_Id;
	XRFdc_IPStatus ipStatus;
	XRFdc* RFdcInstPtr = &RFdcInst;
	XRFdc_Calibration_Coefficients calCoefficients;

	Tile_Id = cmdVals[0];
	Block_Id = cmdVals[1];

    XRFdc_GetIPStatus(RFdcInstPtr, &ipStatus);
	xil_printf("\n\r###############################################\n\r");
	xil_printf("=== ADC Cal Coefficients Report ===\n\r");

	if(XRFdc_IsADCBlockEnabled(RFdcInstPtr, Tile_Id, Block_Id) == 1) {

		XRFdc_GetCalCoefficients(RFdcInstPtr, Tile_Id, Block_Id,XRFDC_CAL_BLOCK_OCB1, &calCoefficients);
		xil_printf("   ADC Tile%d ch%d OCB1 Cal Coefficients: \r\n", Tile_Id, Block_Id);
		displayCoeff(&calCoefficients);

		XRFdc_GetCalCoefficients(RFdcInstPtr, Tile_Id, Block_Id,XRFDC_CAL_BLOCK_OCB2, &calCoefficients);
		xil_printf("   ADC Tile%d ch%d OCB2 Cal Coefficients: \r\n", Tile_Id, Block_Id);
		displayCoeff(&calCoefficients);

		XRFdc_GetCalCoefficients(RFdcInstPtr, Tile_Id, Block_Id,XRFDC_CAL_BLOCK_GCB, &calCoefficients);
		xil_printf("   ADC Tile%d ch%d GCB Cal Coefficients: \r\n", Tile_Id, Block_Id);
		displayCoeff(&calCoefficients);

		XRFdc_GetCalCoefficients(RFdcInstPtr, Tile_Id, Block_Id,XRFDC_CAL_BLOCK_TSCB, &calCoefficients);
		xil_printf("   ADC Tile%d ch%d TSCB Cal Coefficients: \r\n", Tile_Id, Block_Id);
		displayCoeff(&calCoefficients);

	} else {
			(xil_printf("Tile%d Ch%d is not available.\n\r",Tile_Id,Block_Id ));
	}

	xil_printf("###############################################\r\n\n");

	return;
}


/*****************************************************************************/
/**
*
* displayCoeff TBD
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void displayCoeff(XRFdc_Calibration_Coefficients *CoeffPtr)
{
	xil_printf("      Coeff0: %d\r\n", CoeffPtr->Coeff0);
	xil_printf("      Coeff1: %d\r\n", CoeffPtr->Coeff1);
	xil_printf("      Coeff2: %d\r\n", CoeffPtr->Coeff2);
	xil_printf("      Coeff3: %d\r\n", CoeffPtr->Coeff3);
	xil_printf("      Coeff4: %d\r\n", CoeffPtr->Coeff4);
	xil_printf("      Coeff5: %d\r\n", CoeffPtr->Coeff5);
	xil_printf("      Coeff6: %d\r\n", CoeffPtr->Coeff6);
	xil_printf("      Coeff7: %d\r\n", CoeffPtr->Coeff7);

	return;
}




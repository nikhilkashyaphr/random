/*
 * main.h
 *
 *  Created on: Sep 17, 2017
 *      Author: johnmcd
 */

#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

/***************************** Include Files ********************************/
#include "xparameters.h"
#include "platform_ids.h"
#include "xil_types.h"
#include "xrfdc.h"

/******************** Constant Definitions **********************************/

// Necessary to use this define when using jtagterminal but not SDK jtaguart console
//#define STRIP_CHAR_CR


// RFDC defines
/* Resolved via platform_ids.h so this header builds under both the
 * classic DEVICE_ID flow and the SDT BASEADDR flow (Vitis 2023.2+). */
#define RFDC_DEVICE_ID  HW_RFDC_ID
#define RFDC_BASE       HW_RFDC_BASE

// Number of Tiles and Blocks in device
#define NUM_TILES 4
#define NUM_BLOCKS 4
/**************************** Type Definitions *******************************/


/***************** Macros (Inline Functions) Definitions *********************/


/************************** Function Prototypes *****************************/


/************************** Variable Definitions ****************************/

extern XRFdc RFdcInst;      /* RFdc driver instance */


#endif /* SRC_MAIN_H_ */

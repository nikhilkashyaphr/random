/*
 * lscript.ld FRAGMENT -- REFERENCE ONLY, NOT APPLIED.
 *
 * Required ONLY if adcSaveCalCoefficients.c / adcLoadCalCoefficients.c are
 * activated (they are dead code in the current build -- main() never calls
 * cli_init(), so no CLI command is reachable).
 *
 * Size derivation (corrected -- the original in-source comment said 3 coeff
 * types, which under-sizes the region by 2304 bytes and lets storedCoeffSets
 * overrun it):
 *
 *   _COEFF_SIZE = 4          sets      (NUM_SETS)
 *               * 4          tiles     (NUM_TILES)
 *               * 4          blocks    (NUM_BLOCKS)
 *               * 4          coeff types  <-- OCB1, OCB2, TSCB, GCB
 *               * (4 + 8*4)  bytes  ('written' flag + 8 x u32)
 *               = 9216 bytes (0x2400)
 *
 * Verify against:  sizeof(storedCoeffSets) == 9216
 */

_COEFF_SIZE = 4 * (4 * (4 * (4 * (4 + (8 * 4)))));

/* In MEMORY { } -- carve the region off the TOP of the app's DDR block: */
/*
   psu_ddr_0_MEM_0     : ORIGIN = 0x0,
                         LENGTH = 0x00100000 - _COEFF_SIZE
   psu_ddr_0_MEM_COEFF : ORIGIN = 0x00100000 - _COEFF_SIZE,
                         LENGTH = _COEFF_SIZE
*/

/* In SECTIONS { } -- NOLOAD so the coefficients survive a warm restart: */
/*
   .coeff_mem (NOLOAD) : {
      _CoeffMemBase = .;
      KEEP(*(.coeffSection));
      _CoeffMemEnd = .;
   } > psu_ddr_0_MEM_COEFF
*/

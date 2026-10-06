#!/usr/bin/env bash
# Print main.c's DAC-input routing code, verbatim: from the ready flag to the
# end of Get_Dds_H2c_Routing(), without Init_Dds_H2c_Switch() (which needs the
# real driver). Used by the host-side tests so they exercise the shipped text.
set -euo pipefail
tr -d '\r' < "$1" | awk '/^static int dds_h2c_switch_ready/{on=1}
     on && /^int Init_Dds_H2c_Switch/{skip=1}
     on && skip && /^}/{skip=0; next}
     on && !skip{print}
     on && /^int Get_Dds_H2c_Routing/{get=1}
     get && /^}/{exit}'

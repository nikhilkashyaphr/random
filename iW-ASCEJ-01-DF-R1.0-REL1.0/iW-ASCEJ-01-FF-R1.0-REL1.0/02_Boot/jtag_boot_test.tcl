#-------------------------------------------------------------------------------
# jtag_boot_test.tcl
#
# Proves whether your BOOT ARTIFACTS are good, with the boot device taken out of
# the picture entirely. Loads PMUFW -> FSBL -> bitstream -> app over JTAG, in the
# same order BOOT.BIN would.
#
# PREREQUISITE: set the board's mode pins/DIP switches to *** JTAG *** (0x0) and
# power-cycle. In JTAG boot mode the DAP is enabled unconditionally, so the
# "Could not find ARM device" error cannot occur. If JTAG still fails in JTAG
# boot mode, the problem is the cable/board/power, not your image.
#
# Usage:
#     xsdb jtag_boot_test.tcl
#
# Edit the four paths below if yours differ.
#-------------------------------------------------------------------------------

set WS    /home/iwave/Downloads/Baremetal_COde/Udpate_S1
set PMUFW $WS/platform_ZU47/export/platform_ZU47/sw/qemu/pmufw.elf
set FSBL  $WS/platform_ZU47/export/platform_ZU47/sw/boot/fsbl.elf
set BIT   $WS/platform_ZU47/export/platform_ZU47/hw/sdt/H2C_pcie_control.bit
set APP   $WS/app_component/build/app_component.elf

foreach f [list $PMUFW $FSBL $BIT $APP] {
    if {![file exists $f]} { puts "MISSING: $f" ; exit 1 }
}

puts "\n=== connect ==="
connect
puts [targets]

# --- 1. PMU firmware ----------------------------------------------------------
# 0xFFCA0038 = CSU.PMU_GLOBAL access control. Writing 0x1FF opens PMU RAM to
# the debugger so the PMU firmware can be downloaded.
puts "\n=== PMU firmware ==="
targets -set -filter {name =~ "*PSU*"}
mwr 0xFFCA0038 0x1FF
targets -set -filter {name =~ "*MicroBlaze PMU*"}
dow $PMUFW
con
puts "  PMUFW running"
after 500

# --- 2. FSBL ------------------------------------------------------------------
# Run it to completion by breaking on XFsbl_Exit. The FSBL does PS init and DDR
# bring-up; we then load the remaining partitions ourselves.
puts "\n=== FSBL ==="
targets -set -filter {name =~ "*Cortex-A53 #0*"}
rst -processor
dow $FSBL
bpadd -addr &XFsbl_Exit
con -block -timeout 60
puts "  FSBL reached XFsbl_Exit (PS init + DDR done)"
bpremove -all

# --- 3. Bitstream -------------------------------------------------------------
puts "\n=== bitstream ==="
targets -set -filter {name =~ "*PL*"}
fpga -file $BIT
puts "  PL configured"

# --- 4. Application -----------------------------------------------------------
puts "\n=== application ==="
targets -set -filter {name =~ "*Cortex-A53 #0*"}
dow $APP
con
puts "  application running - watch UART0 (MIO 6/7) at 115200 8N1"
puts "  expect the banner within ~2 s, and the menu after ~25 s"

puts {
=== how to read the result ===
  Everything above ran and you SEE UART output
      -> artifacts are good. The fault is the BOOT DEVICE or how BOOT.BIN was
         written to it (SD slot, or QSPI -flash_type). Not the ELFs.

  Everything ran but you see NO UART output
      -> the artifacts run but the console path is wrong. Check which
         /dev/ttyUSB* maps to PS UART0, and the baud.

  FSBL step times out at XFsbl_Exit
      -> the FSBL itself is failing. Rebuild it, and enable FSBL_DEBUG_INFO
         so it tells you why.
}

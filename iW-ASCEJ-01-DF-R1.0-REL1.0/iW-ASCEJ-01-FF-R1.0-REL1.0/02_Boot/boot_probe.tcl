#-------------------------------------------------------------------------------
# boot_probe.tcl - find out WHERE a silent boot is stopping, using JTAG.
#
# Run with the board POWERED and set to your intended boot mode (SD or QSPI),
# a few seconds after power-on. Do NOT reset the board from the debugger first:
# that destroys the evidence.
#
#     xsdb boot_probe.tcl
#
# Reads only. Changes nothing.
#-------------------------------------------------------------------------------

proc hexs {v} { return [format 0x%08X $v] }

puts "\n=== connecting ==="
connect

# --- 1. What boot mode did the BootROM actually latch? ------------------------
# CRL_APB.BOOT_MODE_USER, bits [3:0] = boot_mode_reg (sampled from mode pins).
targets -set -filter {name =~ "PSU"}
set bm [mrd -value 0xFF5E0200]
set mode [expr {$bm & 0xF}]

array set modes {
    0 "JTAG"        1 "QSPI-24"     2 "QSPI-32"    3 "SD0"
    4 "NAND"        5 "SD1"         6 "eMMC"       7 "USB"
    8 "PJTAG-0"     9 "PJTAG-1"    14 "SD1-LS"
}
set name "UNKNOWN"
if {[info exists modes($mode)]} { set name $modes($mode) }

puts "\n=== boot mode ==="
puts "  BOOT_MODE_USER = [hexs $bm]   ->  mode \[3:0\] = $mode  ($name)"
if {$mode == 0} {
    puts "  >> Mode pins say JTAG. The BootROM will NOT load BOOT.BIN at all."
    puts "  >> This alone explains total silence. Fix the mode pins/DIP switches."
}

# --- 2. Did the A53 ever leave the reset vector? ------------------------------
puts "\n=== A53 #0 state ==="
if {[catch {
    targets -set -filter {name =~ "Cortex-A53 #0"}
    set st [state]
    puts "  state: $st"
    if {[catch {set pc [print -force $pc]} e]} {
        catch {set pc [rrd pc]}
    }
    catch {puts "  pc   : $pc"}
} err]} {
    puts "  could not read A53 #0: $err"
    puts "  >> If the core is held in reset, the BootROM never handed off."
}

# --- 3. Is the PL configured? -------------------------------------------------
# PCAP status: DONE bit indicates the bitstream loaded.
puts "\n=== PL configuration ==="
targets -set -filter {name =~ "PSU"}
if {[catch {
    set pcap [mrd -value 0xFFCA3010]
    puts "  PCAP_STATUS = [hexs $pcap]"
    puts "  PL_DONE (bit 3) = [expr {($pcap >> 3) & 1}]   (1 = bitstream loaded)"
} e]} { puts "  read failed: $e" }

# --- 4. Reference: what a healthy sequence looks like -------------------------
puts "\n=== interpreting ==="
puts {
  mode=0 (JTAG)          -> BootROM never reads flash/SD. Fix mode pins.
  mode=SD but silence    -> wrong SD SLOT (this design has BOTH SD0 and SD1),
                            or card not FAT32 / BOOT.BIN not in root.
  mode=QSPI but silence  -> flashed with the wrong -flash_type. This design is
                            SINGLE x4 (QSPI on MIO 0-5), so use qspi-x4-single.
  A53 running, PL_DONE=1 -> boot worked; the problem is the SERIAL PATH, not
                            the image. Check port and baud (UART0, MIO 6/7).
  A53 held in reset      -> BootROM rejected the image before handoff.
}
puts "=== done ===\n"

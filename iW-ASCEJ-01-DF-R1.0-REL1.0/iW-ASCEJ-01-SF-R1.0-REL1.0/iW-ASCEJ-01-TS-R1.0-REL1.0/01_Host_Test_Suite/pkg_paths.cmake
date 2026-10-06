# Package layout (iW-ASCEJ-01 REL1.0). The test suite lives in
#   <DF>/iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-TS-R1.0-REL1.0/01_Host_Test_Suite
# and builds the code under test straight from the SC and FF folders, so the
# tests always exercise the delivered sources. Override with -DPKG_FW=... etc.
get_filename_component(PKG_TS_ROOT "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(PKG_SF "${PKG_TS_ROOT}/../.." ABSOLUTE)
get_filename_component(PKG_DF "${PKG_SF}/.." ABSOLUTE)
if(NOT PKG_GUI)
  set(PKG_GUI "${PKG_SF}/iW-ASCEJ-01-SC-R1.0-REL1.0/01_sdr_workstation_GUI")
endif()
if(NOT PKG_FW)
  set(PKG_FW "${PKG_DF}/iW-ASCEJ-01-FF-R1.0-REL1.0/01_Source")
endif()
foreach(d PKG_GUI PKG_FW)
  if(NOT EXISTS "${${d}}")
    message(FATAL_ERROR "${d} not found: ${${d}} (package layout changed? pass -D${d}=...)")
  endif()
endforeach()

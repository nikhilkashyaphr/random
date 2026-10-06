# End-to-end PCIe control test: GUI RfdcControl <-> emulated BAR <-> firmware
# pcie_cfg.c, current (emu_test) and 1.2.0 (emu_test_legacy).
#   cmake -S pcie_emu -B build-emu && cmake --build build-emu && ctest --test-dir build-emu
cmake_minimum_required(VERSION 3.16)
project(pcie_emu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt5 REQUIRED COMPONENTS Core)
find_package(Threads REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/../pkg_paths.cmake)
set(FW   ${PKG_FW})
set(GUI  ${PKG_GUI}/src/core)
set(HERE ${CMAKE_CURRENT_SOURCE_DIR})

# main.c's routing functions, verbatim
execute_process(COMMAND ${PKG_TS_ROOT}/firmware/extract_routing.sh ${FW}/main.c
                OUTPUT_FILE ${CMAKE_CURRENT_BINARY_DIR}/routing_under_test.inc
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "could not extract the routing code from main.c")
endif()

add_library(fw_current STATIC ${FW}/pcie_cfg.c ${HERE}/board_model.c)
target_include_directories(fw_current PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${FW} ${HERE}/stubs)

add_library(fw_legacy STATIC ${HERE}/legacy_1.2.0/pcie_cfg.c ${HERE}/board_model.c)
target_include_directories(fw_legacy PRIVATE ${HERE}/legacy_1.2.0 ${FW} ${HERE}/stubs)
target_compile_definitions(fw_legacy PRIVATE LEGACY)

foreach(v current legacy)
    if(v STREQUAL current)
        set(exe emu_test)
    else()
        set(exe emu_test_legacy)
    endif()
    add_executable(${exe} ${HERE}/emu_test.cpp ${GUI}/RfdcControl.cpp ${GUI}/RfdcControl.h)
    target_include_directories(${exe} PRIVATE ${GUI})
    target_link_libraries(${exe} fw_${v} Qt5::Core Threads::Threads)
    if(v STREQUAL legacy)
        target_compile_definitions(${exe} PRIVATE LEGACY)
    endif()
endforeach()

enable_testing()
add_test(NAME pcie_e2e_fw_1_2_1 COMMAND emu_test)
add_test(NAME pcie_e2e_fw_1_2_0 COMMAND emu_test_legacy)

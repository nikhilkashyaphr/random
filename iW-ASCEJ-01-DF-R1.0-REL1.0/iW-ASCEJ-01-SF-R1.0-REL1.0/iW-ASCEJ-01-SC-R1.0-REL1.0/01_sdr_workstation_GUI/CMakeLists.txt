cmake_minimum_required(VERSION 3.16)
project(sdr_workstation VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Qt6 preferred, Qt5 accepted so the project builds on older hosts too.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(PROJECT_SOURCES
    src/main.cpp

    src/core/Types.h
    src/core/Fft.h
    src/core/SampleCodec.h
    src/core/Sources.h
    src/core/Sources.cpp
    src/core/DspEngine.h
    src/core/DspEngine.cpp
    src/core/Recorder.h
    src/core/Recorder.cpp
    src/core/SystemProbe.h
    src/core/SystemProbe.cpp
    src/core/DriverManager.h
    src/core/BackendLauncher.h
    src/core/BackendLauncher.cpp
    src/ui/BackendModeDialog.h
    src/core/RecordingGuard.h
    src/core/RecordingGuard.cpp
    src/core/ResourceMonitor.h
    src/core/ResourceMonitor.cpp
    src/core/H2cControl.h
    src/core/H2cControl.cpp
    src/core/RfdcControl.h
    src/core/RfdcControl.cpp
    src/core/IqGenerator.h
    src/core/IqGenerator.cpp
    src/core/Waveform.h
    src/core/CaptureBudget.h
    src/core/pcie_regs.h
    src/core/HardwareDb.h
    src/core/HardwareDb.cpp
    src/core/ConfigValidator.h
    src/core/ConfigValidator.cpp
    src/core/RoceShmSource.h
    src/core/RoceShmSource.cpp
    src/core/GpuFft.h
    src/core/GpuFft.cpp
    src/core/GpuIqPipeline.h
    src/core/GpuIqPipeline.cpp
    src/core/GpuMathRef.h
    src/core/PcapExtract.h
    src/core/PcapExtract.cpp
    src/core/GpuNetProbe.h
    src/core/GpuNetProbe.cpp
    src/core/DriverManager.cpp

    src/ui/Theme.h
    src/ui/ColorMap.h
    src/ui/PlotBase.h
    src/ui/PlotBase.cpp
    src/ui/Plots.h
    src/ui/Plots.cpp
    src/ui/ResourceBar.h
    src/ui/ResourceBar.cpp
    src/ui/ControlWindow3D.h
    src/ui/ControlWindow3D.cpp
    src/ui/TitleBar.h
    src/ui/ChannelDialog.h
    src/ui/Panels.h
    src/ui/Panels.cpp
    src/ui/ConfigDialog.h
    src/ui/ConfigDialog.cpp
    src/ui/PcieDiscoveryDialog.h
    src/ui/PcieDiscoveryDialog.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp

    resources/resources.qrc
)

add_executable(sdr_workstation ${PROJECT_SOURCES})
add_dependencies(sdr_workstation backend_helpers)

if(SDR_HAVE_CUDA)
    target_compile_definitions(sdr_workstation PRIVATE SDR_ENABLE_CUDA)
    target_link_libraries(sdr_workstation PRIVATE CUDA::cudart CUDA::cufft)
    set_target_properties(sdr_workstation PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
endif()
target_link_libraries(sdr_workstation PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)

if(MSVC)
    target_compile_options(sdr_workstation PRIVATE /W4)
    set_target_properties(sdr_workstation PROPERTIES WIN32_EXECUTABLE TRUE)
else()
    target_compile_options(sdr_workstation PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Threads are used directly by the acquisition and DSP stages.
# ---------------------------------------------------------------------------
# Optional CUDA: the GPUDirect RoCEv2 path.
#
# OFF by default so the application builds on a machine with no toolkit —
# which is most development machines. With it OFF, GpuIqPipeline still
# compiles and reports "built without CUDA" rather than disappearing, so the
# UI can explain the absence instead of silently offering a dead option.
# ---------------------------------------------------------------------------
option(SDR_ENABLE_CUDA "Build the GPUDirect (CUDA IPC + cuFFT) ingest path" OFF)

if(SDR_ENABLE_CUDA)
    find_package(CUDAToolkit)
    if(CUDAToolkit_FOUND)
        enable_language(CUDA)
        list(APPEND PROJECT_SOURCES src/core/GpuKernels.cu)
        set(SDR_HAVE_CUDA TRUE)
        message(STATUS "CUDA enabled: ${CUDAToolkit_VERSION} — GPUDirect path built")
    else()
        # Asked for, not available: say so loudly. Silently building a
        # CPU-only binary that claims GPU support is worse than failing here.
        message(WARNING
            "SDR_ENABLE_CUDA=ON but no CUDA toolkit was found. "
            "Building WITHOUT the GPUDirect path.")
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(sdr_workstation PRIVATE Threads::Threads)

install(TARGETS sdr_workstation RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Backend helper applications.
#
# `cmake --build build` builds EVERYTHING: the Qt workstation, the C2H/H2C
# helpers, and the UDP receiver. Previously each had to be built by hand in
# its own directory, which meant the GUI could offer a transport whose
# binary did not exist.
#
# Each is driven through its own Makefile rather than being re-expressed as
# CMake targets: they are vendor-supplied and their build flags (-O3
# -march=native -flto for the UDP receiver) are part of their contract, so
# reproducing them here would risk building something subtly different from
# what was validated.
# ---------------------------------------------------------------------------

find_program(MAKE_EXECUTABLE NAMES make gmake REQUIRED)

# UDP receiver: backend/udp/iwfg_c2h
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backend/udp/Makefile")
    add_custom_target(udp_receiver ALL
        COMMAND ${MAKE_EXECUTABLE} -j4
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/backend/udp"
        COMMENT "Building UDP receiver (backend/udp/iwfg_c2h)"
        VERBATIM)
    add_dependencies(sdr_workstation udp_receiver)
endif()

# C2H / H2C helpers: backend/bin/{c2h_stream,iwfg_h2c}
#
# These are built HERE, on the machine that will run them, and backend/bin must
# never be shipped prebuilt. It was, once, and the consequence was subtle: make
# saw the packaged binaries as newer than the sources, skipped the rebuild, and
# the host then loaded a binary compiled against a different glibc —
#     libc.so.6: version `GLIBC_2.38' not found (required by iwfg_h2c)
# The packaging script now excludes backend/bin for exactly this reason.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backend/Makefile")
    # -B: rebuild unconditionally. Two small C files cost about a second, and
    # it makes the helpers immune to timestamp problems entirely — a packaged
    # or restored binary with a newer mtime than its source can no longer
    # suppress the rebuild, which is the failure this is guarding against.
    add_custom_target(backend_helpers ALL
        COMMAND ${MAKE_EXECUTABLE} -B -j4
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/backend"
        COMMENT "Building C2H/H2C helpers for THIS host (backend/bin)"
        VERBATIM)
    add_dependencies(sdr_workstation backend_helpers)
endif()

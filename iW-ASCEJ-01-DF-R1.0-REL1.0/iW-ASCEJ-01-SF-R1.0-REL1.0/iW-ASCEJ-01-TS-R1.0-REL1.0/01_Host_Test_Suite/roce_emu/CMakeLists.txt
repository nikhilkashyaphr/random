# Host-side tests for the RoCEv2 ingest path (TS-11).
#
# No NIC, no RDMA stack, no GPU and no board. The shipping RoceShmSource is
# built straight from the SC folder and driven against synthetic rings that are
# byte-compatible with both producers, so the path that was previously only
# checkable on a cabled bench is now a regression gate.
#
#   cmake -S roce_emu -B build && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(sdr_roce_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt5 REQUIRED COMPONENTS Core)
find_package(Threads REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/../pkg_paths.cmake)
set(GUI ${PKG_GUI}/src)
# ../gui supplies spectrum.h, the same tiny analyser the other GUI suites use,
# so the tone check here measures frequency exactly as they do.
include_directories(${GUI}/core ${CMAKE_CURRENT_SOURCE_DIR}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../gui)

# The ring reader under test, plus Sources.cpp for the ISignalSource base
# (admit/publish/accountBytes live there).
set(ROCE_SRC
    ${GUI}/core/RoceShmSource.cpp ${GUI}/core/RoceShmSource.h
    ${GUI}/core/Sources.cpp       ${GUI}/core/Sources.h
    ${GUI}/core/IqGenerator.cpp   ${GUI}/core/IqGenerator.h)

add_executable(roce_ring_test roce_ring_test.cpp ${ROCE_SRC})
target_link_libraries(roce_ring_test Qt5::Core Threads::Threads)

add_executable(roce_tap_test roce_tap_test.cpp ${ROCE_SRC})
target_link_libraries(roce_tap_test Qt5::Core Threads::Threads)

# The GPU spectrum maths is header-only (GpuMathRef.h) plus GpuIqPipeline's
# non-CUDA branch, so it needs no device and no toolkit to be checked.
add_executable(gpu_math_test gpu_math_test.cpp
    ${GUI}/core/GpuIqPipeline.cpp ${GUI}/core/GpuIqPipeline.h
    ${GUI}/core/GpuMathRef.h)
target_link_libraries(gpu_math_test Qt5::Core Threads::Threads)

foreach(t roce_ring_test roce_tap_test gpu_math_test)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
endforeach()

enable_testing()
add_test(NAME roce_host_ring  COMMAND roce_ring_test)
add_test(NAME roce_tap_ring   COMMAND roce_tap_test)
add_test(NAME roce_gpu_math   COMMAND gpu_math_test)

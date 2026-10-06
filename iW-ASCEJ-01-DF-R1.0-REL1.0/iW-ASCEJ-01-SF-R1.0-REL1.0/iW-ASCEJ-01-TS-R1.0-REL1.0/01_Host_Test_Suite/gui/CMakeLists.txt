# Host-side tests for the GUI's transmit path and simulation.
#   cmake -S gui -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.16)
project(sdr_workstation_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt5 REQUIRED COMPONENTS Core Widgets)
find_package(Threads REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/../pkg_paths.cmake)
set(GUI ${PKG_GUI}/src)
include_directories(${GUI}/core ${GUI}/ui ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(tx_path_test tx_path_test.cpp
    ${GUI}/core/IqGenerator.cpp ${GUI}/core/IqGenerator.h)
target_link_libraries(tx_path_test Qt5::Core Threads::Threads)

add_executable(sim_loopback_test sim_loopback_test.cpp
    ${GUI}/core/Sources.cpp ${GUI}/core/Sources.h
    ${GUI}/core/IqGenerator.cpp ${GUI}/core/IqGenerator.h)
target_link_libraries(sim_loopback_test Qt5::Core)

add_executable(control_window_test control_window_test.cpp
    ${GUI}/ui/ControlWindow3D.cpp ${GUI}/ui/ControlWindow3D.h
    ${GUI}/core/H2cControl.cpp ${GUI}/core/H2cControl.h
    ${GUI}/core/RfdcControl.cpp ${GUI}/core/RfdcControl.h
    ${GUI}/core/IqGenerator.cpp ${GUI}/core/IqGenerator.h
    ${GUI}/core/BackendLauncher.cpp ${GUI}/core/BackendLauncher.h)
target_link_libraries(control_window_test Qt5::Widgets Threads::Threads)

# The whole application (every GUI source except main.cpp) for the end-to-end
# simulation test.
set(CMAKE_AUTORCC ON)
file(GLOB APP_SOURCES ${GUI}/core/*.cpp ${GUI}/core/*.h ${GUI}/ui/*.cpp ${GUI}/ui/*.h)
add_executable(app_loopback_test app_loopback_test.cpp ${APP_SOURCES}
    ${GUI}/../resources/resources.qrc)
target_include_directories(app_loopback_test PRIVATE ${GUI} ${GUI}/ui ${GUI}/core)
target_link_libraries(app_loopback_test Qt5::Widgets Threads::Threads)

foreach(t tx_path_test sim_loopback_test control_window_test)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
endforeach()

enable_testing()
add_test(NAME tx_path        COMMAND tx_path_test)
add_test(NAME sim_loopback   COMMAND sim_loopback_test)
add_test(NAME control_window COMMAND control_window_test)
add_test(NAME app_loopback   COMMAND app_loopback_test)
set_tests_properties(control_window app_loopback PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

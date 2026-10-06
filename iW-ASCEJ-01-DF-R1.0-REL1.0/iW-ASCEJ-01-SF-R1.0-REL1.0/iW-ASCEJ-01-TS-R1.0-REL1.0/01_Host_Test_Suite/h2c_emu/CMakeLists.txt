# The real backend helpers (iwfg_h2c, c2h_stream) driven by the GUI's real
# IqGenerator, BackendLauncher and DmaSource, with the card replaced by an
# LD_PRELOAD fake (fake_iwfg.so) that records what the DAC would play and
# loops it back into the C2H stream.
#   cmake -S h2c_emu -B build-h2c && cmake --build build-h2c && ctest --test-dir build-h2c
cmake_minimum_required(VERSION 3.16)
project(h2c_emu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt5 REQUIRED COMPONENTS Core Widgets)
find_package(Threads REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/../pkg_paths.cmake)
set(GUI  ${PKG_GUI}/src/core)
set(H2C  ${PKG_GUI}/backend/H2C)
set(C2H  ${PKG_GUI}/backend/C2H)

# The helpers exactly as backend/Makefile builds them
add_executable(iwfg_h2c ${H2C}/iwfg_h2c.c)
target_include_directories(iwfg_h2c PRIVATE ${H2C})
target_compile_options(iwfg_h2c PRIVATE -O2 -Wall -Wextra)
target_link_libraries(iwfg_h2c Threads::Threads)

add_executable(c2h_stream ${C2H}/fifo_pipe.c)
target_include_directories(c2h_stream PRIVATE ${C2H})
target_compile_options(c2h_stream PRIVATE -O2 -Wall -Wextra)

add_library(fake_iwfg SHARED fake_iwfg.c)
target_include_directories(fake_iwfg PRIVATE ${H2C})
target_link_libraries(fake_iwfg dl)

add_executable(h2c_emu_test h2c_emu_test.cpp
    ${GUI}/IqGenerator.cpp ${GUI}/IqGenerator.h
    ${GUI}/BackendLauncher.cpp ${GUI}/BackendLauncher.h)
target_include_directories(h2c_emu_test PRIVATE ${GUI} ${CMAKE_CURRENT_SOURCE_DIR}/../gui)
target_link_libraries(h2c_emu_test Qt5::Core Threads::Threads)
target_compile_options(h2c_emu_test PRIVATE -Wall -Wextra)

add_executable(loopback_emu_test loopback_emu_test.cpp
    ${GUI}/IqGenerator.cpp ${GUI}/IqGenerator.h
    ${GUI}/BackendLauncher.cpp ${GUI}/BackendLauncher.h
    ${GUI}/Sources.cpp ${GUI}/Sources.h)
target_include_directories(loopback_emu_test PRIVATE ${GUI} ${CMAKE_CURRENT_SOURCE_DIR}/../gui)
target_link_libraries(loopback_emu_test Qt5::Core Threads::Threads)
target_compile_options(loopback_emu_test PRIVATE -Wall -Wextra)

# The whole application (every GUI source except main.cpp), live session.
set(CMAKE_AUTORCC ON)
set(APPSRC ${PKG_GUI}/src)
file(GLOB APP_SOURCES ${APPSRC}/core/*.cpp ${APPSRC}/core/*.h ${APPSRC}/ui/*.cpp ${APPSRC}/ui/*.h)
add_executable(app_live_emu_test app_live_emu_test.cpp ${APP_SOURCES}
    ${PKG_GUI}/resources/resources.qrc)
target_include_directories(app_live_emu_test PRIVATE ${APPSRC} ${APPSRC}/ui ${APPSRC}/core)
target_link_libraries(app_live_emu_test Qt5::Widgets Threads::Threads)

enable_testing()
add_test(NAME h2c_real_helper COMMAND h2c_emu_test)
set_tests_properties(h2c_real_helper PROPERTIES
    ENVIRONMENT "H2C_BIN=$<TARGET_FILE:iwfg_h2c>;FAKE_IWFG_SO=$<TARGET_FILE:fake_iwfg>"
    TIMEOUT 180)
add_test(NAME loopback_chain COMMAND loopback_emu_test)
set_tests_properties(loopback_chain PROPERTIES
    ENVIRONMENT "H2C_BIN=$<TARGET_FILE:iwfg_h2c>;C2H_BIN=$<TARGET_FILE:c2h_stream>;FAKE_IWFG_SO=$<TARGET_FILE:fake_iwfg>"
    TIMEOUT 300)
add_test(NAME app_live_session COMMAND app_live_emu_test)
set_tests_properties(app_live_session PROPERTIES
    ENVIRONMENT "H2C_BIN=$<TARGET_FILE:iwfg_h2c>;C2H_BIN=$<TARGET_FILE:c2h_stream>;FAKE_IWFG_SO=$<TARGET_FILE:fake_iwfg>;QT_QPA_PLATFORM=offscreen"
    TIMEOUT 240)

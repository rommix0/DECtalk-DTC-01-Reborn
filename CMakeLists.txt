# The DTC-01 v1.8 speech library and the programs that use it (REFERENCE.md s17.12).
#
#   DECtalk.dll / libtts_us.so   the speech library (dapi's names and exports; src/api, src/speech, src/kernel)
#   say                          speaks its arguments or standard input (src/apps/say)
#   speak                        the speaking text editor: Win32 on Windows, GTK 3 on Linux (src/apps/speak)
#   test_lib                     the library's check (decomp/test/test_lib.c); DTC01_TESTS
#
# Nothing here needs the ROMs. The build goes in build/ at the top of the project, 64-bit by default
# (CMakePresets.json):
#   Windows, from an x64 developer prompt (vcvars64.bat; VS 2022's Build Tools bring CMake and Ninja):
#     cmake --preset x64            then   cmake --build --preset x64
#   Windows, 32-bit, from an x86 developer prompt (vcvars32.bat), into build/x86:
#     cmake --preset x86            then   cmake --build --preset x86
#   Linux (gcc; speak needs GTK 3):
#     cmake --preset linux          then   cmake --build --preset linux
# Without the presets: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release, then cmake --build build.
# A 32-bit compiler is refused unless DTC01_32BIT is on (the x86 preset sets it).
# The programs and the library land in build/bin (on Linux the library in build/lib, found through the rpath).
# The emulator-based ROM checks (decomp/build_*_test.bat, check_frames.py, check_lib.py) are separate and unchanged.
cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Build in build/, not in the source tree: cmake --preset x64 (or linux), or cmake -S . -B build. "
                      "Then delete the CMakeCache.txt and CMakeFiles/ this run left here.")
endif()
project(DTC01 VERSION 1.8 LANGUAGES C)

option(DTC01_TESTS "Build test_lib, the library's check" ON)
option(DTC01_SPEAK "Build speak (on Linux: needs GTK 3)" ON)
option(DTC01_32BIT "Allow a 32-bit build (the default is 64-bit)" OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT DTC01_32BIT)
  message(FATAL_ERROR "This is a 32-bit compiler, and the build is 64-bit by default. Use an x64 developer prompt "
                      "(vcvars64.bat) or a 64-bit gcc; for a 32-bit build, use the x86 preset or -DDTC01_32BIT=ON.")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# build/bin and build/lib for every configuration (also with the Visual Studio generator)
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/lib)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/lib)
endforeach()
if(NOT WIN32)
  set(CMAKE_BUILD_RPATH "\$ORIGIN/../lib")
  set(CMAKE_INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
find_package(Threads REQUIRED)
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
  set(APP_WARNINGS /W4)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  set(APP_WARNINGS -Wall -Wextra)
endif()

# ---- the library ----
set(SPEECH
  ${SRC}/speech/engine.c
  ${SRC}/speech/tx_scan.c ${SRC}/speech/tx_num.c ${SRC}/speech/tx_word.c ${SRC}/speech/tx_dict.c ${SRC}/speech/tx_lts.c
  ${SRC}/speech/tx_phon.c ${SRC}/speech/tx_clause.c ${SRC}/speech/tx_rom.c ${SRC}/speech/tx_rom_lts.c ${SRC}/speech/tx_rom_dict.c
  ${SRC}/speech/ph_frame.c ${SRC}/speech/ph_timing.c ${SRC}/speech/ph_alloph.c ${SRC}/speech/ph_clause.c ${SRC}/speech/ph_command.c
  ${SRC}/speech/ph_task.c ${SRC}/speech/ph_rom.c
  ${SRC}/speech/dsp_synth.c ${SRC}/speech/dsp_rom.c ${SRC}/speech/dsp_link.c
  ${SRC}/kernel/kernel.c ${SRC}/kernel/console.c)
add_library(dectalk SHARED
  ${SRC}/api/ttsapi.c ${SRC}/api/tts_audio.c ${SRC}/api/tts_dict.c ${SRC}/api/tts_stubs.c ${SPEECH})
target_include_directories(dectalk PUBLIC $<BUILD_INTERFACE:${SRC}/api>
                                          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
                                  PRIVATE ${SRC}/speech ${SRC}/kernel)
set_target_properties(dectalk PROPERTIES PUBLIC_HEADER ${SRC}/api/ttsapi.h)
if(WIN32)
  # dapi's name, and its exports and ordinals from the .def file
  set_target_properties(dectalk PROPERTIES OUTPUT_NAME DECtalk)
  target_sources(dectalk PRIVATE ${SRC}/api/dectalk.def)
  target_link_libraries(dectalk PRIVATE winmm user32)
else()
  # dapi's Linux name; only the TextToSpeech functions are exported (ALSA is loaded at run time, with dlopen)
  set_target_properties(dectalk PROPERTIES OUTPUT_NAME tts_us C_VISIBILITY_PRESET hidden)
  target_compile_definitions(dectalk PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
  target_link_libraries(dectalk PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---- say ----
add_executable(say ${SRC}/apps/say/say.c)
target_compile_options(say PRIVATE ${APP_WARNINGS})
target_link_libraries(say PRIVATE dectalk Threads::Threads)

# ---- speak: one core, a front end per system ----
if(DTC01_SPEAK)
  set(SPEAK_CORE ${SRC}/apps/speak/speak_core.c ${SRC}/apps/speak/speak_pics.c)
  if(WIN32)
    add_executable(speak WIN32 ${SRC}/apps/speak/speak_win.c ${SRC}/apps/speak/speak_win.rc ${SPEAK_CORE})
    target_link_libraries(speak PRIVATE dectalk comctl32 comdlg32 shell32 advapi32 gdi32 user32)
  else()
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(GTK3 IMPORTED_TARGET gtk+-3.0)
    endif()
    if(GTK3_FOUND)
      add_executable(speak ${SRC}/apps/speak/speak_gtk.c ${SPEAK_CORE})
      target_link_libraries(speak PRIVATE dectalk PkgConfig::GTK3)
    else()
      message(STATUS "GTK 3 not found (on Debian/Ubuntu: libgtk-3-dev): speak is not built")
    endif()
  endif()
  if(TARGET speak)
    target_compile_options(speak PRIVATE ${APP_WARNINGS})
    install(TARGETS speak RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()

# ---- dtc01term: the unit's host side (src/host) on the library, over console/stdio/TCP/COM lines ----
add_executable(dtc01term
  ${SRC}/term/dtc01term.c ${SRC}/term/term_speech.c ${SRC}/term/term_dev.c ${SRC}/term/term_line.c
  ${SRC}/host/hs_parse.c ${SRC}/host/hs_command.c ${SRC}/host/hs_reply.c ${SRC}/host/hs_settings.c
  ${SRC}/host/hs_rom.c ${SRC}/host/hs_phone.c ${SRC}/host/hs_setup.c ${SRC}/host/hs_help.c ${SRC}/host/hs_tasks.c
  ${SRC}/kernel/kernel.c ${SRC}/kernel/console.c ${SRC}/kernel/stream.c)
target_include_directories(dtc01term PRIVATE ${SRC}/term ${SRC}/host ${SRC}/kernel)
target_compile_options(dtc01term PRIVATE ${APP_WARNINGS})
target_link_libraries(dtc01term PRIVATE dectalk Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32 winmm>)
if(NOT WIN32)
  target_compile_definitions(dtc01term PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()
install(TARGETS dtc01term RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- the checks (decomp/test, which a checkout without decomp/ does not have) ----
if(DTC01_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/decomp/test/test_lib.c)
  add_executable(test_lib decomp/test/test_lib.c)
  set_target_properties(test_lib PROPERTIES C_STANDARD 11)     # timespec_get
  target_link_libraries(test_lib PRIVATE dectalk)
  if(NOT WIN32)
    target_compile_definitions(test_lib PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  # the kernel's host-side calls and dtc01term's devices
  add_executable(test_kernel decomp/test/test_kernel.c ${SRC}/kernel/kernel.c ${SRC}/term/term_dev.c
                 ${SRC}/term/term_line.c)
  target_include_directories(test_kernel PRIVATE ${SRC}/kernel ${SRC}/term)
  target_compile_options(test_kernel PRIVATE ${APP_WARNINGS})
  target_link_libraries(test_kernel PRIVATE Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>)
  # dtc01term's lines (TCP and the escape key)
  add_executable(test_line decomp/test/test_line.c ${SRC}/term/term_line.c)
  target_include_directories(test_line PRIVATE ${SRC}/term)
  target_compile_options(test_line PRIVATE ${APP_WARNINGS})
  target_link_libraries(test_line PRIVATE Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>)
endif()

install(TARGETS dectalk say
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

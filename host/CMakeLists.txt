cmake_minimum_required(VERSION 3.20)
project(r3_host
    VERSION 0.2.0
    LANGUAGES C CXX
    DESCRIPTION "Host of the recompiled Resistance 3 (PS3): the runtime and the port, without any game code"
)

# ---------------------------------------------------------------------------
# What this builds
# ---------------------------------------------------------------------------
# One executable, r3_host: the ps3recomp runtime, the toolkit's PPU boot scaffold and this port's sources
# (main.cpp, the HLE overrides), with every dependency linked in statically. It contains nothing derived from the
# game. The lifted PPU and SPU code is a separate loadable module that the user's machine builds from their own copy
# of the game (launcher/); the host loads it at start (main.cpp, load_game_module) and the module resolves its
# runtime calls against the symbols this executable exports.
#
# Because the module is compiled against this build's headers, the build also writes an SDK next to the executable
# (sdk/, see the install rules) and an ABI string that both sides check.
#

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED PS3RECOMP_DIR)
    message(FATAL_ERROR "Pass -DPS3RECOMP_DIR=<toolkit checkout>")
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")

# The host links no LGPL code: the ATRAC3plus decoder is swapped for a stub (the game imports no cellAtrac).
set(PS3RECOMP_ATRAC_LGPL OFF CACHE BOOL "" FORCE)

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)
# Each lifted SPU image takes two entries of the workload registry (spu_workload.c, default 256). The module lifts
# about 220 images (every Insomniac buffer module among them); past the cap the last ones -- the SPURS task ELFs --
# were dropped and the boot hung on an event flag that task sets. Each Insomniac module also takes one overlay source
# (spu_channels.c, default 128, spu-runtime/0009); past that cap a module stays in the interpreter. A frame of the
# Haven fight binds about 1700 distinct textures; the draw engine's cache holds 1024 by default (gcm-rsx/0023), and
# past it every frame re-created ~700 of them. The fight also records ~7800 draws a frame besides clears and copies;
# past the Metal backend's default cap of 8192 records it submits the frame so far and waits for it (gcm-rsx/0025;
# it used to drop the rest), a stall mid-frame that 16384 avoids.
target_compile_definitions(ps3recomp_runtime PRIVATE SPU_WORKLOAD_MAX=1024 SPU_OVL_SRC_MAX=512
                           RSX_DRAW_ENGINE_TEXTURE_CACHE=2048 ENG_MAX_RECORDS=16384)

# ---------------------------------------------------------------------------
# Generated, game-independent files
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")

# The lifter's header preamble without the per-game function table.
set(HOST_HEADER "${GEN_DIR}/ppu_recomp.h")
add_custom_command(
    OUTPUT "${HOST_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" -I "${REPO_ROOT}/scripts/make_host_header.py"
            "${PS3RECOMP_DIR}" "${HOST_HEADER}"
    DEPENDS "${REPO_ROOT}/scripts/make_host_header.py" "${PS3RECOMP_DIR}/tools/ppu_lifter.py"
    COMMENT "Generating the host's ppu_recomp.h")

# The firmware import NID -> HLE handler table (every module).
# The generator scans the handlers the libs define, so a toolkit change to those files regenerates it too.
file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/libs/*.c")
set(HLE_NID_TABLE "${GEN_DIR}/ppu_hle_nids.cpp")
add_custom_command(
    OUTPUT "${HLE_NID_TABLE}"
    COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/gen_hle_nids.py" --out "${HLE_NID_TABLE}" --all
    DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py" "${PS3RECOMP_DIR}/tools/nid_database.py" ${HLE_NID_SOURCES}
    COMMENT "Generating the HLE NID -> handler table")

# ABI string: a hash over everything a module is compiled against. A module built for another host refuses to load.
file(GLOB_RECURSE ABI_INPUTS CONFIGURE_DEPENDS
    "${PS3RECOMP_DIR}/include/*.h"
    "${PS3RECOMP_DIR}/runtime/ppu/*.h"
    "${PS3RECOMP_DIR}/runtime/spu/*.h"
    "${PS3RECOMP_DIR}/runtime/memory/*.h"
    "${PS3RECOMP_DIR}/runtime/platform/*.h"
    "${PS3RECOMP_DIR}/tools/ppu_lifter.py"
    "${PS3RECOMP_DIR}/tools/spu_lifter.py"
    "${PS3RECOMP_DIR}/tools/spu_regcache.py")   # rewrites the SPU lifter's output (spu-runtime/0015)
list(FILTER ABI_INPUTS EXCLUDE REGEX "/tests/")
list(SORT ABI_INPUTS)
set(ABI_TEXT "")
foreach(f IN LISTS ABI_INPUTS)
    file(SHA256 "${f}" h)
    string(APPEND ABI_TEXT "${h}\n")
endforeach()
string(SHA256 R3_HOST_ABI "${ABI_TEXT}")
string(SUBSTRING "${R3_HOST_ABI}" 0 16 R3_HOST_ABI)
file(WRITE "${CMAKE_BINARY_DIR}/ABI" "${R3_HOST_ABI}\n")
message(STATUS "Host ABI:        ${R3_HOST_ABI}")

# ---------------------------------------------------------------------------
# The executable
# ---------------------------------------------------------------------------
# The boot scaffold is compiled here: it includes ppu_recomp.h, which in the host is the game-independent one.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    r3_hle.cpp
    r3_input.cpp
    r3_controls.cpp
    r3_glyphs.cpp
    spurs_lfqueue.cpp
    crypto.cpp
    unself.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${HOST_HEADER}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${GEN_DIR}"                               # the host's ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/prx"
    "${PS3RECOMP_DIR}/runtime/platform"
    "${PS3RECOMP_DIR}/libs/video"
    "${PS3RECOMP_DIR}/libs/input"              # pad_keymap.h, cellPad.h
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(${PROJECT_NAME} PRIVATE R3_HOST_ABI="${R3_HOST_ABI}")
target_link_libraries(${PROJECT_NAME} PRIVATE ps3recomp_runtime)
if(UNIX)
    target_link_libraries(${PROJECT_NAME} PRIVATE m)
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# The module is not linked against the host at build time; it binds to the host's symbols when it is loaded. So the
# host has to export every runtime symbol, including those no host object refers to: keep the whole runtime archive
# and export all of it.
if(APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE
        "LINKER:-force_load,$<TARGET_FILE:ps3recomp_runtime>" "LINKER:-export_dynamic")
elseif(UNIX)
    target_link_options(${PROJECT_NAME} PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:ps3recomp_runtime>,--no-whole-archive" "LINKER:--export-dynamic")
endif()
# TODO: Windows with MSVC: the same needs a .def file or __declspec(dllexport) on the runtime.

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
endif()

# The keyboard/mouse glue on its own (see the files): not part of the default build.
add_executable(test_r3_input EXCLUDE_FROM_ALL tests/test_r3_input.cpp r3_input.cpp)
add_executable(test_r3_controls EXCLUDE_FROM_ALL tests/test_r3_controls.cpp r3_controls.cpp)
add_executable(test_r3_glyphs EXCLUDE_FROM_ALL tests/test_r3_glyphs.cpp r3_glyphs.cpp r3_controls.cpp)
add_executable(test_r3_unself EXCLUDE_FROM_ALL tests/test_r3_unself.cpp unself.cpp crypto.cpp)
if(APPLE)
    target_link_libraries(test_r3_glyphs PRIVATE "-framework ApplicationServices")
endif()
foreach(t test_r3_input test_r3_controls test_r3_glyphs test_r3_unself)
    target_include_directories(${t} PRIVATE "${PS3RECOMP_DIR}/include" "${PS3RECOMP_DIR}/libs/input")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# SDK for the module build
# ---------------------------------------------------------------------------
# What the launcher needs to lift and compile a game against this host: the lifter tools, the headers the lifted code
# includes, and the one SPU runtime source that has to be compiled with the lift (spu_dispatch_mt.c, musttail).
set(SDK_DIR "${CMAKE_BINARY_DIR}/sdk")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SDK_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SDK_DIR}/runtime"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/include" "${SDK_DIR}/include"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/tools" "${SDK_DIR}/tools"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/runtime/ppu" "${SDK_DIR}/runtime/ppu"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/runtime/spu" "${SDK_DIR}/runtime/spu"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/runtime/memory" "${SDK_DIR}/runtime/memory"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/runtime/platform" "${SDK_DIR}/runtime/platform"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/ABI" "${SDK_DIR}/ABI"
    COMMENT "Writing the module SDK")

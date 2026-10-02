# PHETAMINE — build file.
#
# Three properties of this build are load-bearing and are enforced here, not in a
# wiki page:
#
#   1. **No base relocations.** The shellcode stub maps the module at its
#      preferred base (docs/DECISIONS.md ADR-1), so the image must not need
#      fix-ups. `/FIXED /DYNAMICBASE:NO`, plus a post-build check that reads the
#      actual DataDirectory[BASERELOC] — a claim about the linker is worthless
#      without reading the linked output.
#   2. **Nothing in the client's code is patched.** No `/hotpatch`, no
#      `/INCREMENTAL`, no trampoline libraries. The only writes the module makes
#      at runtime are its own memory, the fps cap value, and the identity field of
#      threads it owns (ADR-4).
#   3. **The stub is freestanding.** It is a DLL with no CRT, no imports, no
#      relocations and all data merged into `.text`, because the blob generator
#      takes `.text` and nothing else (tools/gen/shellcode_blob.mjs).
#
# Debug vs Release is the caller's choice; both are supported for the module. The
# UI is separate and Debug-only (ui/PHETAMINEUI/README.md).
cmake_minimum_required(VERSION 3.24)
project(PHETAMINE VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "PHETAMINE is a Windows x64 in-process module; it cannot be built on ${CMAKE_SYSTEM_NAME}. "
        "The repository's docs, tooling and static checks run anywhere — the module itself needs "
        "MSVC or clang-cl on Windows (docs/STATUS.md §4).")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "PHETAMINE is x64-only: the VM layout, calling convention and shellcode are all x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ------------------------------------------------------------------ the module
set(PHETAMINE_SOURCES
    src/dllmain.cpp

    src/common/log.cpp
    src/common/seh.cpp

    src/memory/pe.cpp
    src/memory/offsets.cpp
    src/memory/scanner.cpp
    src/memory/instance_walker.cpp

    src/lua/state.cpp
    src/lua/env.cpp
    src/lua/identity.cpp

    src/scheduler/scheduler.cpp
    src/scheduler/rendezvous.cpp

    src/executor/executor.cpp

    src/ipc/protocol.cpp
    src/ipc/pipe_server.cpp

    src/native_api/registry.cpp
    src/native_api/env.cpp
    src/native_api/identity.cpp
    src/native_api/closures.cpp
    src/native_api/metatable.cpp
    src/native_api/instances.cpp
    src/native_api/net.cpp
    src/native_api/fs.cpp
    src/native_api/misc.cpp

    src/net/client.cpp

    src/core/core.cpp

    src/inject/loader.cpp
)

add_library(PHETAMINE SHARED ${PHETAMINE_SOURCES})
set_target_properties(PHETAMINE PROPERTIES
    OUTPUT_NAME "PHETAMINE"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded"          # /MT: our heap is NOT the client's
)
target_include_directories(PHETAMINE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(PHETAMINE PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    PHETAMINE_VERSION="${PROJECT_VERSION}"
    PHETAMINE_ROBLOX_VERSION="version-02c37bc51a384b8f"
)

if(MSVC)
    # /EHa, not /EHsc: seh.h wraps raw `__try` around plugin-friendly POD calls,
    # and the compiler refuses that under /EHsc (C2712) in any function that needs
    # unwinding. /EHa is the documented workaround and is what the whole tree is
    # compiled with — mixing them per-file would change ABI assumptions.
    target_compile_options(PHETAMINE PRIVATE
        /W4 /permissive- /EHa /MP /Zc:__cplusplus /utf-8
        /wd4100        # unreferenced formal parameter (stub/API shims)
        /wd4200        # zero-sized array in a POD struct
        /wd4201        # anonymous structs (PE unions, documented)
        /wd4324        # padded structure due to alignment (the graph records)
    )
    target_link_options(PHETAMINE PRIVATE
        /DYNAMICBASE:NO
        /FIXED
        /INCREMENTAL:NO
        /OPT:REF
        /OPT:ICF
        /MANIFEST:NO
    )
else()
    target_compile_options(PHETAMINE PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
    target_link_options(PHETAMINE PRIVATE -Wl,--no-insert-timestamp)
endif()

target_link_libraries(PHETAMINE PRIVATE winhttp bcrypt)

# ------------------------------------------------------------------- the stub
# Built as a DLL so the blob generator can read AddressOfEntryPoint; it is never
# loaded by the OS. /MERGE:.rdata=.text is what makes the string constants part of
# the blob: the generator copies `.text` and only `.text`.
add_library(PHETAMINEStub SHARED src/inject/stub/stub.c)
set_target_properties(PHETAMINEStub PROPERTIES
    OUTPUT_NAME "PHETAMINEStub"
    PREFIX ""
    SUFFIX ".dll"
)
if(MSVC)
    target_compile_options(PHETAMINEStub PRIVATE
        /O2 /GS- /Gs9999999 /Zl /W4 /wd4100
    )
    target_link_options(PHETAMINEStub PRIVATE
        /NODEFAULTLIB
        /NOENTRY
        /ENTRY:StubMain
        /SUBSYSTEM:WINDOWS
        /MERGE:.rdata=.text
        /MERGE:.pdata=.text
        /DYNAMICBASE:NO
        /FIXED
        /INCREMENTAL:NO
        /MANIFEST:NO
        /ALIGN:16
    )
endif()

# --------------------------------------------------------------- generated bits
find_program(PHETAMINE_NODE NAMES node node.exe)
if(NOT PHETAMINE_NODE)
    message(WARNING
        "node was not found: src/memory/offsets_table.inc and the loader blob cannot be regenerated. "
        "Install Node 22+ and re-run cmake, or generate the files on a machine that has it.")
endif()

# The offsets table (src/memory/offsets_table.inc) is generated from
# offsets_embedded.json. Regenerated when the JSON is newer than the .inc.
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/src/memory/offsets_table.inc"
    COMMAND "${PHETAMINE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/offsets/gen_offsets_header.mjs"
            --out "${CMAKE_CURRENT_SOURCE_DIR}/src/memory/offsets_table.inc"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/memory/offsets_embedded.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/offsets/gen_offsets_header.mjs"
    COMMENT "Generating src/memory/offsets_table.inc from offsets_embedded.json"
    VERBATIM)
add_custom_target(PHETAMINEOffsets DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/memory/offsets_table.inc")
add_dependencies(PHETAMINE PHETAMINEOffsets)

# The loader blob. Generating it needs the *linked* stub, so it is a POST_BUILD
# step of PHETAMINEStub rather than a configure-time thing.
if(PHETAMINE_NODE)
    add_custom_command(TARGET PHETAMINEStub POST_BUILD
        COMMAND "${PHETAMINE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/shellcode_blob.mjs"
                --in "$<TARGET_FILE:PHETAMINEStub>"
                --out "${CMAKE_BINARY_DIR}/PHETAMINE.stub.bin"
        COMMENT "Turning the linked stub into the raw loader blob"
        VERBATIM)
endif()

# -------------------------------------------------------------- post-build check
# Reads the linked module and refuses to call it done if it has relocations or is
# missing its entry export. This is the check that ADR-1's promise rests on.
if(PHETAMINE_NODE)
    add_custom_command(TARGET PHETAMINE POST_BUILD
        COMMAND "${PHETAMINE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/check_module.mjs"
                --module "$<TARGET_FILE:PHETAMINE>"
                --expect-version "version-02c37bc51a384b8f"
        COMMENT "Validating the module image (x64, no relocations, PhetamineEntry exported)"
        VERBATIM)
endif()

# ------------------------------------------------------------------- the UI
# Not part of the CMake build on purpose: the UI is .NET, and it is Debug-only.
# See ui/PHETAMINEUI/README.md.
add_custom_target(PHETAMINEUI-reminder
    COMMAND ${CMAKE_COMMAND} -E echo
            "The UI is a separate .NET project: cd ui/PHETAMINEUI && dotnet build -c Debug")

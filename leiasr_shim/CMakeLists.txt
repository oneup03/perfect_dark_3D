# LeiaSR shim DLL (leiasr_shim.dll) — MSVC-built bridge between the
# MinGW64 PD port executable and the LeiaSR Simulated Reality OpenGL weaver.
# The port executable loads this DLL at runtime via LoadLibrary; absence is
# graceful (LeiaSR mode falls back to plain SbS).
#
# This is NOT wired into the main CMakeLists.txt: the port is GCC/MinGW64 and
# this is MSVC. Build it once after the SDK submodule is updated:
#
#   cd leiasr_shim
#   cmake -B build -A x64
#   cmake --build build --config Release
#
# The output leiasr_shim.dll lands in build/Release/. Copy it next to
# pd.x86_64.exe so the runtime loader can pick it up via LoadLibrary.
#
# The SR SDK is vendored as a submodule at port/external/LeiaSR64; that's the
# default LEIASR_SDKROOT below. Override with -DLEIASR_SDKROOT=... if pointing
# at a system-wide install instead.

cmake_minimum_required(VERSION 3.21)
project(leiasr_shim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED LEIASR_SDKROOT)
    set(LEIASR_SDKROOT "${CMAKE_CURRENT_SOURCE_DIR}/../port/external/LeiaSR64"
        CACHE PATH "LeiaSR SDK root")
endif()
message(STATUS "leiasr_shim: LEIASR_SDKROOT = ${LEIASR_SDKROOT}")
if(NOT EXISTS "${LEIASR_SDKROOT}/lib/cmake")
    message(FATAL_ERROR
        "LEIASR_SDKROOT does not contain lib/cmake; expected the SDK layout "
        "from port/external/LeiaSR64/. Did you forget `git submodule update "
        "--init port/external/LeiaSR64`?")
endif()

list(APPEND CMAKE_PREFIX_PATH "${LEIASR_SDKROOT}/lib/cmake")
find_package(simulatedreality REQUIRED)
find_package(srOpenGL REQUIRED)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(BUILD_PLATFORM 32)
endif()

add_library(leiasr_shim SHARED leiasr_shim.cpp)

target_link_libraries(leiasr_shim PRIVATE
    simulatedreality
    srOpenGL${BUILD_PLATFORM}::srOpenGL
)

set_target_properties(leiasr_shim PROPERTIES
    OUTPUT_NAME "leiasr_shim"
    PREFIX ""
)

if(MSVC)
    # /EHa lets the C++ try/catch(...) in srk_init catch the SEH that
    # delay-load throws when a SR runtime DLL is missing. Without it,
    # missing-DLL SEHs propagate up and abort the process.
    target_compile_options(leiasr_shim PRIVATE /W3 /wd4251 /wd4275 /EHa)

    # Delay-load every SR runtime DLL so leiasr_shim.dll itself loads even on
    # machines without the SR runtime installed. The DLLs are resolved at
    # first use (inside srk_init / srk_weave), where our try/catch can fail
    # gracefully and the engine drops back to plain SbS. delayimp.lib supplies
    # the runtime stub the linker injects for /DELAYLOAD.
    target_link_libraries(leiasr_shim PRIVATE delayimp)
    # We only directly import from these two DLLs in this build; LNK4199
    # complains about /DELAYLOAD entries we don't actually import from, so
    # keep the list pruned to the actual imports. (If the SDK changes and
    # the shim grows new imports, expand this list to match.)
    target_link_options(leiasr_shim PRIVATE
        /DELAYLOAD:SimulatedRealityCore.dll
        /DELAYLOAD:SimulatedRealityOpenGL.dll
    )
endif()

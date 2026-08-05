# LeiaSR shim DLL (leiasr_shim.dll) — MSVC-built bridge between the
# MinGW64 PD port executable and the LeiaSR Simulated Reality OpenGL weaver.
# The port executable loads this DLL at runtime via LoadLibrary; absence is
# graceful (LeiaSR mode falls back to plain SbS).
#
# This is NOT wired into the main CMakeLists.txt: the port is GCC/MinGW64 and
# this is MSVC. Build it by hand after touching leiasr_shim.cpp:
#
#   cmake -S leiasr_shim -B leiasr_shim/build -A x64
#   cmake --build leiasr_shim/build --config Release
#
# The output leiasr_shim.dll lands in build/Release/. scripts/build.ps1 stages
# it next to pd.x86_64.exe; CI does the same from a separate windows-latest job.
#
# The SR SDK comes from the bo3b/SR-lib submodule at libs/SR-lib (branch
# api_expansion), whose own CMakeLists locates the nested per-architecture SDK
# tree, exposes SRLib::SDK / SRLib::SR, and owns the /DELAYLOAD list. Do not
# hand-roll SDK paths or delay-load entries here — that list is the part
# integrations get wrong, and centralising it means an SDK that grows a new DLL
# dependency fixes every consumer at once.

cmake_minimum_required(VERSION 3.21)
project(leiasr_shim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to the submodule; overridable for a checkout that keeps SR-lib
# elsewhere. Note this is the SR-lib repo root, NOT the SDK root — the SDK
# lives at SR-SDK-<version>/<arch>/ underneath and SR-lib's CMakeLists globs
# for it, so a version bump needs no change here.
if(NOT DEFINED SRLIB_DIR)
    set(SRLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libs/SR-lib"
        CACHE PATH "bo3b/SR-lib checkout (api_expansion branch)")
endif()

if(NOT EXISTS "${SRLIB_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "SR-lib not found at ${SRLIB_DIR}. Run:\n"
        "  git submodule update --init libs/SR-lib\n"
        "(bo3b/SR-lib is private — you need read access on your GitHub account.)")
endif()

add_subdirectory("${SRLIB_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/SR-lib")

if(NOT SRLIB_FOUND)
    message(FATAL_ERROR
        "SR-lib is present at ${SRLIB_DIR} but reported SRLIB_FOUND=FALSE — the "
        "nested SR-SDK-*/ tree is missing or this is a non-Windows/non-x64 "
        "configure. The shim cannot be built without it; the engine still "
        "builds and falls back to SbS when the DLL is absent.")
endif()

add_library(leiasr_shim SHARED leiasr_shim.cpp)

# SRLib::SR rather than SRLib::SDK: we want SR.cpp's SRInterfaceOGL wrapper,
# which owns the SRContext lifecycle (create -> CreateGLWeaver -> initialize,
# in that order — getting it wrong silently produces an un-woven image), the
# per-backend LoadLibraryW probe, and the create()/deleteSRContext() pairing.
target_link_libraries(leiasr_shim PRIVATE SRLib::SR)

set_target_properties(leiasr_shim PROPERTIES
    OUTPUT_NAME "leiasr_shim"
    PREFIX ""
)

if(MSVC)
    # /EHa lets the C++ try/catch(...) in this TU catch the SEH that the
    # delay-load helper raises when an SR runtime DLL is missing. The blast
    # radius is acceptable precisely because this whole TU exists for SR
    # integration — there is no non-SR code in scope. It layers with SR-lib's
    # LoadLibraryW preflight: the preflight fails fast on the common
    # no-runtime path, /EHa catches anything that slips past (e.g. a partial
    # install where one sibling DLL is missing).
    target_compile_options(leiasr_shim PRIVATE /W3 /wd4251 /wd4275 /EHa)

    # Delay-load every SR-touched DLL so leiasr_shim.dll itself loads on
    # machines that have never had the SR Platform installed. The DLL list
    # lives in libs/SR-lib/CMakeLists.txt; this also pulls in delayimp and
    # silences LNK4199 for the entries we don't happen to import.
    srlib_apply_delayload(leiasr_shim)
endif()

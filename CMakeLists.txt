cmake_minimum_required(VERSION 3.25)

project(ReZeroGui
    VERSION 0.1.0
    DESCRIPTION "A lightweight immediate mode GUI library for embedders"
    LANGUAGES CXX
)
include(FetchContent)

# ----------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------

message(STATUS "Build the ReZeroGui examples ${REZEROGUI_BUILD_EXAMPLES}")

# ----------------------------------------------------------------------
# Compiler flags
# ----------------------------------------------------------------------

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(REZEROGUI_NO_EXCEPTIONS -fno-exceptions -fno-unwind-tables)
    set(REZEROGUI_NO_RTTI -fno-rtti)
    set(REZEROGUI_WARNING_OPTIONS -Wall -Wextra)
elseif(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(REZEROGUI_NO_EXCEPTIONS /EHs- /EHc- /D_HAS_EXCEPTIONS=0)
    set(REZEROGUI_NO_RTTI /GR-)
    set(REZEROGUI_WARNING_OPTIONS /W4 /utf-8)
endif()

# ----------------------------------------------------------------------
# Library
# ----------------------------------------------------------------------

add_library(ReZeroGui INTERFACE)
add_library(ReZero::ReZeroGui ALIAS ReZeroGui)

target_compile_features(ReZeroGui INTERFACE cxx_std_23)

target_include_directories(ReZeroGui INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_options(ReZeroGui INTERFACE
    ${REZEROGUI_NO_EXCEPTIONS}
    ${REZEROGUI_NO_RTTI}
    ${REZEROGUI_WARNING_OPTIONS}
)

# ----------------------------------------------------------------------
# Fetch Truetype
# ----------------------------------------------------------------------

FetchContent_Declare(
    ReZeroTruetype
    GIT_REPOSITORY https://github.com/ReZeroShay/ReZeroTruetype.git
    GIT_TAG main
)
FetchContent_MakeAvailable(ReZeroTruetype)

target_link_libraries(ReZeroGui INTERFACE ReZero::ReZeroTrueType)


if(REZEROGUI_BUILD_EXAMPLES)
    # The Direct3D example needs the Windows SDK.
    if(WIN32)
        add_subdirectory(examples/Dx9Example)
        add_subdirectory(examples/Dx11Example)
        add_subdirectory(examples/Dx12Example)
    endif()
endif()

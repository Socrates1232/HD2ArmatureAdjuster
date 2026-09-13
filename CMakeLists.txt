cmake_minimum_required(VERSION 3.24)

project(HD2PaletteProbe LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HD2PaletteProbe requires a 64-bit Windows build.")
endif()

set(RESHADE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade/include"
    CACHE PATH "Directory containing reshade.hpp")

if(NOT EXISTS "${RESHADE_INCLUDE_DIR}/reshade.hpp")
    message(FATAL_ERROR "ReShade 6.5.1 headers were not found. See README.md for setup.")
endif()

add_library(HD2PaletteProbe SHARED src/addon.cpp)
target_sources(HD2PaletteProbe PRIVATE src/matrix_scan.hpp)
target_compile_features(HD2PaletteProbe PRIVATE cxx_std_17)
target_include_directories(HD2PaletteProbe PRIVATE
	"${RESHADE_INCLUDE_DIR}")
target_compile_definitions(HD2PaletteProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

set_target_properties(HD2PaletteProbe PROPERTIES
    PREFIX ""
    SUFFIX ".addon64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    target_compile_options(HD2PaletteProbe PRIVATE /W4 /permissive- /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(addon_load_test tests/addon_load_test.cpp)
    target_compile_features(addon_load_test PRIVATE cxx_std_17)
    target_include_directories(addon_load_test PRIVATE "${RESHADE_INCLUDE_DIR}")
    target_compile_definitions(addon_load_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(addon_load_test PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME addon_loads_and_registers
        COMMAND addon_load_test "$<TARGET_FILE:HD2PaletteProbe>")
endif()

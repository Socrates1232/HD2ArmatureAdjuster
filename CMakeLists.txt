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
target_sources(HD2PaletteProbe PRIVATE src/ib_layout.hpp src/instance_lifecycle.hpp
    src/profile_scan.hpp src/runtime_profile.hpp)
target_compile_features(HD2PaletteProbe PRIVATE cxx_std_17)
target_include_directories(HD2PaletteProbe PRIVATE
	"${RESHADE_INCLUDE_DIR}")
target_compile_definitions(HD2PaletteProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(HD2PaletteProbe PRIVATE Psapi)

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
	find_package(Python3 COMPONENTS Interpreter REQUIRED)

    add_executable(addon_load_test tests/addon_load_test.cpp)
    target_compile_features(addon_load_test PRIVATE cxx_std_17)
    target_include_directories(addon_load_test PRIVATE "${RESHADE_INCLUDE_DIR}")
    target_compile_definitions(addon_load_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(addon_load_test PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME addon_loads_and_registers
        COMMAND addon_load_test "$<TARGET_FILE:HD2PaletteProbe>")

    add_executable(ib_layout_test tests/ib_layout_test.cpp src/ib_layout.hpp)
    target_compile_features(ib_layout_test PRIVATE cxx_std_17)
    target_include_directories(ib_layout_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(ib_layout_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME inverse_bind_layouts COMMAND ib_layout_test)

    add_executable(profile_scan_test tests/profile_scan_test.cpp src/profile_scan.hpp)
    target_compile_features(profile_scan_test PRIVATE cxx_std_17)
    target_include_directories(profile_scan_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(profile_scan_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME converted_profile_scan COMMAND profile_scan_test)

    add_executable(runtime_profile_test tests/runtime_profile_test.cpp src/runtime_profile.hpp)
    target_compile_features(runtime_profile_test PRIVATE cxx_std_17)
    target_include_directories(runtime_profile_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(runtime_profile_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME runtime_profile_format COMMAND runtime_profile_test)

    add_executable(instance_lifecycle_test tests/instance_lifecycle_test.cpp src/instance_lifecycle.hpp)
    target_include_directories(instance_lifecycle_test PRIVATE src)
    target_compile_features(instance_lifecycle_test PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(instance_lifecycle_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME instance_lifecycle_registry COMMAND instance_lifecycle_test)

    add_test(NAME runtime_profile_pipeline
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/profile_pipeline_test.py")
    add_test(NAME shoulder_branch_pipeline
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/branch_target_pipeline_test.py")
    add_test(NAME resource_monitor_summary
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_monitor_summary_test.py")
endif()

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
    src/profile_scan.hpp src/runtime_profile.hpp src/retarget/custom_runtime.hpp
    src/retarget/custom_intent.hpp
    src/retarget/live_palette.hpp src/retarget/publisher.hpp src/retarget/runtime_adapter.hpp
    src/retarget/rig_package.hpp src/retarget/json.hpp src/retarget/sha256.hpp
    src/retarget/plan_encoder.hpp src/retarget/evaluator.hpp src/retarget/math_affine.hpp)
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

    add_executable(ib_replacement_generator_test tests/ib_replacement_generator_test.cpp
        src/ib_replacement_generator.hpp src/ib_layout.hpp)
    target_compile_features(ib_replacement_generator_test PRIVATE cxx_std_17)
    target_include_directories(ib_replacement_generator_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(ib_replacement_generator_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME inverse_bind_replacement_generator COMMAND ib_replacement_generator_test)

    add_executable(retarget_core_test tests/retarget_core_test.cpp
        src/retarget/math_affine.hpp src/retarget/evaluator.hpp src/retarget/plan_encoder.hpp
        src/ib_layout.hpp)
    target_compile_features(retarget_core_test PRIVATE cxx_std_17)
    target_include_directories(retarget_core_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(retarget_core_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME custom_armature_retarget_core COMMAND retarget_core_test)

    add_executable(rig_package_test tests/rig_package_test.cpp
        src/retarget/rig_package.hpp src/retarget/runtime_adapter.hpp src/retarget/sha256.hpp
        src/retarget/json.hpp src/retarget/evaluator.hpp
        src/retarget/math_affine.hpp src/ib_layout.hpp)
    target_compile_features(rig_package_test PRIVATE cxx_std_17)
    target_include_directories(rig_package_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(rig_package_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME custom_armature_rig_package COMMAND rig_package_test)

    add_executable(live_palette_test tests/live_palette_test.cpp
        src/retarget/live_palette.hpp src/retarget/custom_runtime.hpp src/retarget/publisher.hpp
        src/retarget/runtime_adapter.hpp src/retarget/rig_package.hpp src/retarget/json.hpp
        src/retarget/plan_encoder.hpp src/retarget/evaluator.hpp src/retarget/math_affine.hpp
        src/ib_layout.hpp)
    target_compile_features(live_palette_test PRIVATE cxx_std_17)
    target_include_directories(live_palette_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(live_palette_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME custom_armature_live_palette COMMAND live_palette_test)

    add_executable(custom_intent_test tests/custom_intent_test.cpp
        src/retarget/custom_intent.hpp)
    target_compile_features(custom_intent_test PRIVATE cxx_std_17)
    target_include_directories(custom_intent_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(custom_intent_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME custom_armature_intent_mailbox COMMAND custom_intent_test)

    add_executable(publisher_test tests/publisher_test.cpp src/retarget/publisher.hpp)
    target_compile_features(publisher_test PRIVATE cxx_std_17)
    target_include_directories(publisher_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(publisher_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME custom_armature_publisher COMMAND publisher_test)

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
    add_test(NAME semantic_rig_sidecar_pipeline
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/rig_sidecar_pipeline_test.py")
    add_test(NAME pose_branch_plan_pipeline
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pose_branch_plan_test.py")
    add_test(NAME resource_monitor_summary
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_monitor_summary_test.py")
    add_test(NAME custom_armature_pipeline
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/custom_armature_pipeline_test.py")
    add_test(NAME nonblocking_present_path
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/present_path_test.py")
endif()

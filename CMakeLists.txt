cmake_minimum_required(VERSION 3.24)
project(CS2DLSS LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "CS2-DLSS requires Windows x64 and MSVC.")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported build configurations" FORCE)

find_package(imgui CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SimpleIni CONFIG REQUIRED)

set(CS2_DLSS_FXC "" CACHE FILEPATH "Optional path to Windows SDK fxc.exe")
if(NOT CS2_DLSS_FXC)
    get_filename_component(windows_sdk_root
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
    file(GLOB sdk_fxc "${windows_sdk_root}/bin/*/x64/fxc.exe")
    list(SORT sdk_fxc COMPARE NATURAL ORDER DESCENDING)
    find_program(CS2_DLSS_FXC_EXECUTABLE NAMES fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64")
    if(CS2_DLSS_FXC_EXECUTABLE)
        set(CS2_DLSS_FXC "${CS2_DLSS_FXC_EXECUTABLE}")
    elseif(sdk_fxc)
        list(GET sdk_fxc 0 CS2_DLSS_FXC)
    else()
        message(FATAL_ERROR "Install Windows SDK or set CS2_DLSS_FXC to fxc.exe.")
    endif()
endif()
set(shader_header "${CMAKE_CURRENT_BINARY_DIR}/generated/motion_vectors.generated.h")
add_custom_command(OUTPUT "${shader_header}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${CS2_DLSS_FXC}" /nologo /T cs_5_0 /E main /O3
        /Fh "${shader_header}" /Vn motion_vectors_shader
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion_vectors.hlsl"
    DEPENDS shaders/motion_vectors.hlsl
    VERBATIM COMMENT "Compiling motion vectors (cs_5_0)")

set(fallback_headers)
foreach(stage IN ITEMS vs ps)
    if(stage STREQUAL "vs")
        set(entry vertex_main)
    else()
        set(entry pixel_main)
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/generated/upscale_fallback_${stage}.generated.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${CS2_DLSS_FXC}" /nologo /T "${stage}_5_0" /E "${entry}" /O3
            /Fh "${header}" /Vn "upscale_fallback_${stage}_shader"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/upscale_fallback.hlsl"
        DEPENDS shaders/upscale_fallback.hlsl
        VERBATIM COMMENT "Compiling upscale fallback (${stage}_5_0)")
    list(APPEND fallback_headers "${header}")
endforeach()

set(rcas_header "${CMAKE_CURRENT_BINARY_DIR}/generated/rcas.generated.h")
add_custom_command(OUTPUT "${rcas_header}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${CS2_DLSS_FXC}" /nologo /T cs_5_0 /E main /O3
        /Fh "${rcas_header}" /Vn rcas_shader
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/rcas.hlsl"
    DEPENDS shaders/rcas.hlsl
    VERBATIM COMMENT "Compiling RCAS (cs_5_0)")

add_library(cs2_dlss SHARED
    src/dllmain.cpp src/d3d11_proxy.cpp src/d3d11_proxy.def src/runtime.cpp src/settings.cpp src/logging.cpp
    src/hooks.cpp src/game.cpp src/input.cpp src/ngx.cpp
    src/dlss.cpp src/rcas.cpp src/gpu_timer.cpp src/upscale_fallback.cpp src/overlay.cpp src/overlay_panel.cpp "${shader_header}" "${rcas_header}" ${fallback_headers})
target_compile_features(cs2_dlss PRIVATE cxx_std_23)
target_precompile_headers(cs2_dlss PRIVATE src/pch.hpp)
target_include_directories(cs2_dlss PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(cs2_dlss PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(cs2_dlss PRIVATE /W4 /permissive- /EHsc /utf-8 /Zi
    "$<$<CONFIG:Release>:/O2;/GL;/Gw>")
target_link_options(cs2_dlss PRIVATE /DEBUG:FULL
    "$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>")
target_link_libraries(cs2_dlss PRIVATE imgui::imgui minhook::minhook spdlog::spdlog
    SimpleIni::SimpleIni dxgi user32 shell32 ole32 version)
set_target_properties(cs2_dlss PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/out/$<LOWER_CASE:$<CONFIG>>"
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/out/$<LOWER_CASE:$<CONFIG>>"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/out/$<LOWER_CASE:$<CONFIG>>"
    PDB_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/out/$<LOWER_CASE:$<CONFIG>>")
install(TARGETS cs2_dlss RUNTIME DESTINATION .)

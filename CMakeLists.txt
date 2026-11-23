cmake_minimum_required(VERSION 3.25)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
project(thprac_c7 LANGUAGES C CXX RC)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure with the MSVC Win32 (x86) toolchain.")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(vendor "${PROJECT_SOURCE_DIR}/thprac/src/3rdParties")
set(prac "${PROJECT_SOURCE_DIR}/thprac/src/thprac")

set(THPRAC_DX8_SDK "$ENV{THPRAC_DX8_SDK}" CACHE PATH "DirectX 8 SDK directory")
set(legacy_headers "${CMAKE_BINARY_DIR}/legacy-headers")
foreach(header IN ITEMS d3d8.h d3d8caps.h d3d8types.h dsound.h)
    if(NOT EXISTS "${THPRAC_DX8_SDK}/include/${header}")
        message(FATAL_ERROR "Set THPRAC_DX8_SDK to a local DirectX 8 SDK directory containing include/${header}.")
    endif()
    configure_file("${THPRAC_DX8_SDK}/include/${header}" "${legacy_headers}/legacy-dx8/${header}" COPYONLY)
endforeach()

function(thprac_native target)
    target_compile_definitions(${target} PRIVATE WIN32 UNICODE _UNICODE WINVER=0x0601 _WIN32_WINNT=0x0601
        _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8;/arch:SSE2;/Gy;/GS;/sdl>
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc;/permissive>)
    target_link_options(${target} PRIVATE /INCREMENTAL:NO /OPT:REF /OPT:ICF /SAFESEH:NO)
endfunction()

include(sources.cmake)
add_library(thprac_common STATIC ${THPRAC_C7_SOURCES} "${prac}/thprac_dx8_image.cpp")
thprac_native(thprac_common)
target_link_libraries(thprac_common PUBLIC
    kernel32 ntdll user32 freetype gdi32 gdiplus dxguid d3d9 xinput winmm dinput8 ws2_32
    wininet Shlwapi ole32 comdlg32 shell32 advapi32 delayimp dsound)
target_link_directories(thprac_common PUBLIC
    "${vendor}/FreeType/win32" "${vendor}/minizip/lib")
target_include_directories(thprac_common PUBLIC "${PROJECT_SOURCE_DIR}/thprac" "${legacy_headers}"
    "${vendor}/rapidjson/include" "${vendor}/MetroHash" "${vendor}/distorm/include"
    "${vendor}/ImGui" "${vendor}/FreeType/include" "${vendor}/MinHook/include")
target_compile_definitions(thprac_common PUBLIC HAVE_SSE2 DISTORM_LIGHT _WINDOWS MS_TOOLSET_VERSION=${MSVC_TOOLSET_VERSION})
target_compile_options(thprac_common PUBLIC $<$<COMPILE_LANGUAGE:C,CXX>:/fp:fast>)
target_link_options(thprac_common INTERFACE /IGNORE:4098 /IGNORE:4204 /DELAYLOAD:wininet.dll)

add_executable(iat_patch IATPatch/main.cpp)
thprac_native(iat_patch)
set_target_properties(iat_patch PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")


add_executable(thprac WIN32 "${prac}/main.cpp" "${prac}/blobs/th19_fast_msvc.obj" thprac/thprac.rc)
thprac_native(thprac)
set(shared_icon_dir "${CMAKE_BINARY_DIR}/shared-assets")
configure_file("${PROJECT_SOURCE_DIR}/extensions/launcher-layout/thprac/icon-yinyang-gradient.ico"
    "${shared_icon_dir}/icon-yinyang-gradient.ico" COPYONLY)
target_include_directories(thprac PRIVATE "${shared_icon_dir}")
set_property(SOURCE thprac/thprac.rc APPEND PROPERTY OBJECT_DEPENDS "${shared_icon_dir}/icon-yinyang-gradient.ico")
target_link_libraries(thprac PRIVATE thprac_common)
target_link_options(thprac PRIVATE /FORCE:MULTIPLE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
add_custom_target(thprac_distribution ALL
    COMMAND $<TARGET_FILE:iat_patch> "$<TARGET_FILE:thprac>" "$<TARGET_FILE_DIR:thprac>/thprac_compatible.exe"
    COMMAND "${CMAKE_COMMAND}" "-DLINKER=${CMAKE_LINKER}"
        "-DBINARY=$<TARGET_FILE_DIR:thprac>/thprac_compatible.exe" -P "${PROJECT_SOURCE_DIR}/cmake/check_compatible_imports.cmake"
    DEPENDS thprac iat_patch VERBATIM)
target_include_directories(thprac_common PRIVATE "${PROJECT_SOURCE_DIR}/sdk")

include(CTest)
add_subdirectory(extensions)

if(BUILD_TESTING)
    add_executable(hud_contract test/modular/hud_contract.cpp)
    thprac_native(hud_contract)
    target_include_directories(hud_contract PRIVATE "${PROJECT_SOURCE_DIR}/sdk")
    set_target_properties(hud_contract PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-bin")
    add_dependencies(hud_contract status_hud)
    add_test(NAME status_hud_contract COMMAND hud_contract
        "$<TARGET_FILE_DIR:thprac>/plugins/game/$<TARGET_FILE_NAME:status_hud>")
    foreach(variant IN ITEMS thprac thprac_compatible)
        add_test(NAME "launcher_${variant}_contract" COMMAND "${CMAKE_COMMAND}"
            "-DLAUNCHER=$<TARGET_FILE_DIR:thprac>/${variant}.exe"
            "-DWORK=${CMAKE_BINARY_DIR}/test-bin/no-extensions-${variant}"
            -P "${PROJECT_SOURCE_DIR}/test/modular/launcher_contract.cmake")
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(feature_contract test/modular/feature_contract.cpp)
    thprac_native(feature_contract)
    target_include_directories(feature_contract PRIVATE sdk extensions/capture extensions/route extensions/repairs
        extensions/launcher-layout/thprac/src/thprac)
    set_target_properties(feature_contract PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-bin")
    add_dependencies(feature_contract capture route f11_settings game_repairs)
    add_test(NAME feature_contract COMMAND feature_contract
        "$<TARGET_FILE_DIR:thprac>/plugins/game/$<TARGET_FILE_NAME:capture>"
        "$<TARGET_FILE_DIR:thprac>/plugins/game/$<TARGET_FILE_NAME:route>"
        "$<TARGET_FILE_DIR:thprac>/plugins/game/$<TARGET_FILE_NAME:f11_settings>"
        "$<TARGET_FILE_DIR:thprac>/plugins/game/$<TARGET_FILE_NAME:game_repairs>")
endif()

if(BUILD_TESTING)
    add_executable(inventory_contract test/modular/inventory_contract.cpp
        "${prac}/thprac_extension_inventory.cpp")
    thprac_native(inventory_contract)
    target_include_directories(inventory_contract PRIVATE "${prac}" "${vendor}/rapidjson/include")
    set_target_properties(inventory_contract PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-bin")
    add_test(NAME inventory_contract COMMAND inventory_contract "${CMAKE_BINARY_DIR}/test-bin/inventory")
endif()

if(BUILD_TESTING)
    add_executable(stage_tracker_contract test/modular/stage_tracker_contract.cpp)
    thprac_native(stage_tracker_contract)
    target_include_directories(stage_tracker_contract PRIVATE "${prac}" "${PROJECT_SOURCE_DIR}/sdk")
    set_target_properties(stage_tracker_contract PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-bin")
    add_test(NAME stage_tracker_contract COMMAND stage_tracker_contract)
endif()

if(BUILD_TESTING)
    add_executable(game_bgm_contract test/modular/game_bgm_contract.cpp)
    thprac_native(game_bgm_contract)
    target_include_directories(game_bgm_contract PRIVATE sdk)
    target_link_libraries(game_bgm_contract PRIVATE winmm)
    set_target_properties(game_bgm_contract PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-bin/bgm-fixture")
    add_dependencies(game_bgm_contract game_bgm)
    add_test(NAME game_bgm_contract COMMAND game_bgm_contract
        "$<TARGET_FILE_DIR:thprac>/plugins/game/$<TARGET_FILE_NAME:game_bgm>")
endif()

if(BUILD_TESTING)
    add_executable(quick_practice_contract test/modular/quick_practice_contract.cpp)
    thprac_native(quick_practice_contract)
    target_include_directories(quick_practice_contract PRIVATE extensions/quick-practice)
    target_link_libraries(quick_practice_contract PRIVATE thprac_layout_support)
    set_target_properties(quick_practice_contract PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-bin")
    add_dependencies(quick_practice_contract quick_practice game_adapter th15_practice)
    add_test(NAME quick_practice_contract COMMAND quick_practice_contract
        "$<TARGET_FILE_DIR:thprac>/plugins/shared/$<TARGET_FILE_NAME:quick_practice>"
        "$<TARGET_FILE_DIR:thprac>/plugins/game/$<TARGET_FILE_NAME:game_adapter>"
        "$<TARGET_FILE_DIR:thprac>/plugins/game/$<TARGET_FILE_NAME:th15_practice>")
endif()

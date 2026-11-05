include(sources.cmake)
set(vendor "${CMAKE_CURRENT_SOURCE_DIR}/thprac/src/3rdParties")
set(prac "${CMAKE_CURRENT_SOURCE_DIR}/thprac/src/thprac")

add_library(thprac_layout_support STATIC ${THPRAC_LAYOUT_SOURCES} feature_runtime.cpp ../capture/adapter.cpp
    "${PROJECT_SOURCE_DIR}/thprac/src/thprac/thprac_extension_manager.cpp"
    "${PROJECT_SOURCE_DIR}/thprac/src/thprac/thprac_extension_inventory.cpp")
thprac_native(thprac_layout_support)
target_include_directories(thprac_layout_support PRIVATE "${PROJECT_SOURCE_DIR}/thprac/src/thprac")
target_compile_definitions(thprac_layout_support PUBLIC HAVE_SSE2 DISTORM_LIGHT _WINDOWS MS_TOOLSET_VERSION=${MSVC_TOOLSET_VERSION})
target_compile_options(thprac_layout_support PUBLIC $<$<COMPILE_LANGUAGE:C,CXX>:/fp:fast>)
target_include_directories(thprac_layout_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/thprac" "${prac}" "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/sdk" "${CMAKE_CURRENT_SOURCE_DIR}/../capture" "${CMAKE_CURRENT_SOURCE_DIR}/../route" "${CMAKE_CURRENT_SOURCE_DIR}/../repairs"
    "${vendor}/rapidjson/include" "${vendor}/MetroHash" "${vendor}/distorm/include"
    "${vendor}/ImGui" "${vendor}/FreeType/include" "${vendor}/MinHook/include")
target_link_directories(thprac_layout_support PUBLIC
    "${vendor}/FreeType/win32" "${vendor}/d3d8/lib" "${vendor}/d3d9/lib" "${vendor}/minizip/lib")
target_link_libraries(thprac_layout_support PUBLIC kernel32 ntdll user32 freetype gdi32 gdiplus dxguid d3d9
    xinput winmm dinput8 ws2_32 Shlwapi ole32 comdlg32 shell32 advapi32 delayimp dsound)

thprac_c7_extension(launcher_layout launcher entry.cpp thprac/thprac.rc)
target_include_directories(launcher_layout PRIVATE "${shared_icon_dir}")
set_property(SOURCE thprac/thprac.rc APPEND PROPERTY OBJECT_DEPENDS "${shared_icon_dir}/icon-yinyang-gradient.ico")
target_link_options(launcher_layout PRIVATE /IGNORE:4098 /IGNORE:4204)
target_link_libraries(launcher_layout PRIVATE thprac_layout_support)


include(adapter_sources.cmake)
thprac_c7_extension(game_adapter game ../game-adapter/entry.cpp ${THPRAC_ADAPTER_SOURCES} "${prac}/blobs/th19_fast_msvc.obj")
target_link_libraries(game_adapter PRIVATE thprac_layout_support)
target_link_options(game_adapter PRIVATE /FORCE:MULTIPLE /IGNORE:4098 /IGNORE:4204)

thprac_c7_extension(th15_practice game ../th15-practice/entry.cpp "${prac}/thprac_th15.cpp")
target_link_libraries(th15_practice PRIVATE thprac_layout_support)
target_link_options(th15_practice PRIVATE /IGNORE:4098 /IGNORE:4204)

function(thprac_c7_extension target context)
    set(description "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/descriptions/${target}.json")
    if(NOT EXISTS "${description}")
        message(FATAL_ERROR "Plugin ${target} requires a description at ${description}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${description}")
    file(SIZE "${description}" metadata_size)
    if(metadata_size GREATER 8192)
        message(FATAL_ERROR "Plugin ${target} description exceeds 8 KiB")
    endif()
    file(READ "${description}" metadata)
    foreach(locale IN ITEMS zh en ja)
        string(JSON field_type ERROR_VARIABLE invalid TYPE "${metadata}" "${locale}")
        if(invalid OR NOT field_type STREQUAL "STRING")
            message(FATAL_ERROR "Plugin ${target} description requires a string for '${locale}'")
        endif()
        string(JSON field GET "${metadata}" "${locale}")
        string(STRIP "${field}" field)
        if(field STREQUAL "")
            message(FATAL_ERROR "Plugin ${target} description for '${locale}' is empty")
        endif()
    endforeach()

    cmake_parse_arguments(PLUGIN "DISABLED_BY_DEFAULT" "" "" ${ARGN})
    add_library(${target} MODULE ${PLUGIN_UNPARSED_ARGUMENTS})
    set(shipped "$<TARGET_FILE_NAME:${target}>")
    if(PLUGIN_DISABLED_BY_DEFAULT)
        string(APPEND shipped ".disabled")
    endif()
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${description}")
    if(target STREQUAL "game_adapter")
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/game-adapter/adapter.def")
    else()
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/extension.def")
    endif()
    thprac_native(${target})
    target_link_options(${target} PRIVATE /NOIMPLIB /NOEXP)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/sdk")
    set_target_properties(${target} PROPERTIES SUFFIX ".thp.dll"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extension-binaries/$<CONFIG>")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:thprac>/plugins/${context}"
        COMMAND $<TARGET_FILE:iat_patch> "$<TARGET_FILE:${target}>"
            "$<TARGET_FILE_DIR:thprac>/plugins/${context}/${shipped}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${description}"
            "$<TARGET_FILE_DIR:thprac>/plugins/${context}/$<TARGET_FILE_NAME:${target}>.json"
        COMMAND "${CMAKE_COMMAND}" "-DLINKER=${CMAKE_LINKER}"
            "-DBINARY=$<TARGET_FILE_DIR:thprac>/plugins/${context}/${shipped}"
            -P "${PROJECT_SOURCE_DIR}/cmake/check_compatible_imports.cmake"
        VERBATIM)
    add_dependencies(${target} iat_patch)
endfunction()

add_subdirectory(launcher-layout)
thprac_c7_extension(status_hud game status-hud/status_hud.cpp)

set(adapter_sources "${CMAKE_CURRENT_SOURCE_DIR}/launcher-layout/thprac/src/thprac")
thprac_c7_extension(capture game capture/entry.cpp "${adapter_sources}/thprac_boss_capture.cpp")
target_include_directories(capture PRIVATE "${adapter_sources}")
target_link_libraries(capture PRIVATE shell32)
thprac_c7_extension(route game route/entry.cpp)
target_include_directories(route PRIVATE "${adapter_sources}")
thprac_c7_extension(game_volume shared game-volume/entry.cpp game-volume/volume.rc)
target_link_libraries(game_volume PRIVATE comctl32 shell32)
thprac_c7_extension(f11_settings game settings/entry.cpp)
thprac_c7_extension(game_repairs game repairs/entry.cpp)


thprac_c7_extension(sdk_example game DISABLED_BY_DEFAULT sdk-example/entry.cpp)

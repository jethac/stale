add_executable(stale_desktop WIN32 main.cpp)
target_compile_features(stale_desktop PRIVATE cxx_std_20)
target_link_libraries(stale_desktop PRIVATE stale_windows_core Slint::Slint shell32 user32 advapi32)
target_include_directories(stale_desktop PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_definitions(stale_desktop PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(stale_desktop PRIVATE /utf-8)
    set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS /W4)
endif()
set_target_properties(stale_desktop PROPERTIES
    OUTPUT_NAME Stale
    SLINT_EMBED_RESOURCES embed-files)
configure_file(font.slint.in "${CMAKE_CURRENT_BINARY_DIR}/font.slint" @ONLY)
slint_target_sources(stale_desktop app.slint
    LIBRARY_PATHS "material=${STALE_SLINT_SOURCE_DIR}/ui-libraries/material/src/material.slint"
        "material-font=${CMAKE_CURRENT_BINARY_DIR}/font.slint")
if(MSVC)
    get_target_property(stale_desktop_sources stale_desktop SOURCES)
    foreach(stale_source IN LISTS stale_desktop_sources)
        if(stale_source MATCHES "slint_generated_.*\\.cpp$")
            set_source_files_properties("${stale_source}" PROPERTIES COMPILE_OPTIONS /W0)
        endif()
    endforeach()
endif()
add_custom_command(TARGET stale_desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:stale_desktop> $<TARGET_FILE_DIR:stale_desktop>
    COMMAND_EXPAND_LISTS)
install(TARGETS stale_desktop RUNTIME DESTINATION .)

if(BUILD_TESTING)
    add_executable(stale_agent_transport_tests ../tests/agent/transport_tests.cpp)
    target_compile_features(stale_agent_transport_tests PRIVATE cxx_std_20)
    target_include_directories(stale_agent_transport_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(stale_agent_transport_tests PRIVATE Slint::Slint advapi32)
    add_test(NAME stale_agent_transport COMMAND stale_agent_transport_tests)
    set_tests_properties(stale_agent_transport PROPERTIES TIMEOUT 30)
    add_executable(stale_recommendation_tests ../tests/agent/recommendation_tests.cpp)
    target_compile_features(stale_recommendation_tests PRIVATE cxx_std_17)
    target_include_directories(stale_recommendation_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME stale_recommendations COMMAND stale_recommendation_tests)
endif()



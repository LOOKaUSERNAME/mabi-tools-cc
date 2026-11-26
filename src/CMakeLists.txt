find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets Network)
set(CC_APP_VERSION "2026.11.26" CACHE STRING "Application calendar version")
if(NOT CC_APP_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+([.][0-9]+)?$")
    message(FATAL_ERROR "Expected a numeric three- or four-part application version")
endif()
set(CC_RESOURCE_VERSION "${CC_APP_VERSION}")
if(CC_RESOURCE_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
    string(APPEND CC_RESOURCE_VERSION ".0")
endif()
string(REPLACE "." "," CC_RESOURCE_VERSION "${CC_RESOURCE_VERSION}")
set(CC_ICON_FILE "${PROJECT_SOURCE_DIR}/assets/Assets/channel-checker.ico")
enable_language(RC)
configure_file(app/app.rc.in app.rc @ONLY)
set(cc_assets "${PROJECT_SOURCE_DIR}/assets")
set(cc_catalog "${cc_assets}/erinn-na.csv")

set(cc_window_sources
    profile/profile_paths.cpp
    desktop/channel_card.cpp
    profile/channel_settings.cpp
    desktop/dialogs.cpp
    profile/history_replay.cpp
    profile/history_retention.cpp
    desktop/scan_config.cpp
    desktop/main_window.cpp
    profile/history_store.cpp
    desktop/replay_model.cpp
    desktop/window_layout.cpp
    desktop/window_scan.cpp
    desktop/window_services.cpp
    desktop/window_settings.cpp
    profile/wpf_migration.cpp
    services/service_http.cpp
    services/notice_cache.cpp
    services/service_flow.cpp
    services/service_policy.cpp
    services/update_download.cpp
)

function(cc_configure_window target)
    target_include_directories(${target} PRIVATE app desktop profile services measurement/scan)
    target_compile_definitions(${target} PRIVATE
        CC_PROFILE_HISTORY CC_WINDOW_SERVICES NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /fp:strict)
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network dwmapi)

    # The packaging runner and Qt plugin staging both use this directory.
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    qt_add_resources(${target} ${target}_assets PREFIX / BASE "${cc_assets}"
        FILES "${cc_assets}/Assets/mabitools.png" "${cc_assets}/Assets/channel-checker.ico"
              "${cc_assets}/help-content.json" "${cc_catalog}")
endfunction()

add_executable(cc_app app/main.cpp "${CMAKE_CURRENT_BINARY_DIR}/app.rc" ${cc_window_sources})
cc_configure_window(cc_app)
target_compile_definitions(cc_app PRIVATE CC_HTTPS_SERVICES CC_BUNDLED_COMPARISON)
target_compile_definitions(cc_app PRIVATE CC_APP_VERSION="${CC_APP_VERSION}")
if(DEFINED CC_UPDATE_FIXTURE_PORT OR DEFINED CC_ENABLE_NATIVE_UPDATES)
    message(FATAL_ERROR "The application only supports its fixed native HTTPS feed; use separate fixture test executables.")
endif()
target_link_libraries(cc_app PRIVATE cc_scan_production)

if(BUILD_TESTING)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Test)
    add_executable(cc_window_tests ../tests/desktop/service_window_fixtures.cpp ${cc_window_sources})
    cc_configure_window(cc_window_tests)
    target_include_directories(cc_window_tests PRIVATE ../tests/services)
    target_link_libraries(cc_window_tests PRIVATE cc_endpoints Qt6::Test)
    add_executable(cc_update_tests ../tests/services/update_fixtures.cpp services/service_flow.cpp services/service_policy.cpp
        services/service_http.cpp services/update_download.cpp profile/profile_paths.cpp)
    target_include_directories(cc_update_tests PRIVATE desktop profile services measurement/analysis measurement/scan)
    target_compile_definitions(cc_update_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_compile_options(cc_update_tests PRIVATE /W4 /WX /utf-8)
    target_link_libraries(cc_update_tests PRIVATE Qt6::Core Qt6::Network)
    set_target_properties(cc_update_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_executable(cc_https_tests ../tests/services/https_checks.cpp services/service_flow.cpp services/service_policy.cpp
        services/service_http.cpp services/update_download.cpp profile/profile_paths.cpp)
    target_include_directories(cc_https_tests PRIVATE desktop profile services measurement/analysis measurement/scan)
    target_compile_definitions(cc_https_tests PRIVATE CC_HTTPS_SERVICES NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_compile_options(cc_https_tests PRIVATE /W4 /WX /utf-8)
    target_link_libraries(cc_https_tests PRIVATE Qt6::Core Qt6::Network)
    set_target_properties(cc_https_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# Record the exact checkout used for reports and packaging.
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE CC_SOURCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
target_compile_definitions(cc_app PRIVATE CC_SOURCE_COMMIT="${CC_SOURCE_COMMIT}")
set_target_properties(cc_app PROPERTIES OUTPUT_NAME "MabiTools.ChannelChecker" WIN32_EXECUTABLE TRUE)
if(BUILD_TESTING)
 target_compile_definitions(cc_window_tests PRIVATE CC_SOURCE_COMMIT="${CC_SOURCE_COMMIT}")
 target_compile_definitions(cc_https_tests PRIVATE CC_SOURCE_COMMIT="${CC_SOURCE_COMMIT}" CC_APP_VERSION="${CC_APP_VERSION}")
endif()

# The numerical core is independent of Windows and Qt. Keep strict floating
# point in both compiler branches so recommendation comparisons stay stable.
add_library(cc_domain STATIC analysis/statistics.cpp analysis/history.cpp)
target_include_directories(cc_domain PUBLIC analysis)
target_compile_features(cc_domain PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(cc_domain PRIVATE /W4 /WX /fp:strict)
else()
    target_compile_options(cc_domain PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
endif()

if(BUILD_TESTING)
    add_executable(cc_native_tests ../../tests/analysis/core_tests.cpp)
    target_link_libraries(cc_native_tests PRIVATE cc_domain)
    add_test(NAME cc_native_tests COMMAND cc_native_tests)
endif()

# Keep the portable domain available on other platforms. The live transport and
# address parser require Windows; consumers do not need Qt to use this target.
if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(cc_catalog "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/erinn-na.csv")
    # The production scanner and consumers share this generated catalogue. The Qt
    # resource embeds the same CSV; production policy has no runtime override.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cc_catalog}")
    file(STRINGS "${cc_catalog}" cc_rows REGEX "^[0-9]")
    list(LENGTH cc_rows cc_catalog_size)
    if(NOT cc_catalog_size EQUAL 16)
        message(FATAL_ERROR "Expected the 16-channel Erinn catalogue.")
    endif()
    set(cc_catalog_rows "")
    foreach(row IN LISTS cc_rows)
        if(NOT row MATCHES "^([0-9]+),([0-9]+[.][0-9]+[.][0-9]+[.][0-9]+),([0-9]+)$")
            message(FATAL_ERROR "Expected channel,IPv4,port entries.")
        endif()
        string(APPEND cc_catalog_rows "    {${CMAKE_MATCH_1}, \"${CMAKE_MATCH_2}\", ${CMAKE_MATCH_3}},\n")
    endforeach()
    configure_file(scan/comparison_catalog.hpp.in generated/comparison_catalog.hpp @ONLY)

    add_library(cc_scan_production STATIC
        scan/coordinator.cpp scan/win_probe.cpp scan/report.cpp scan/endpoints.cpp)
    target_include_directories(cc_scan_production PUBLIC
        scan "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(cc_scan_production PUBLIC cc_domain PRIVATE ws2_32)
    target_compile_features(cc_scan_production PUBLIC cxx_std_20)
    target_compile_definitions(cc_scan_production PRIVATE
        CC_BUNDLED_COMPARISON WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_compile_options(cc_scan_production PRIVATE /W4 /WX /utf-8 /fp:strict)
endif()

if(NOT CC_BUILD_QT)
    return()
endif()

# Fixtures use their own local test servers. Keep these targets separate from
# cc_scan_production: linking both would mix incompatible endpoint policies.
add_library(cc_scan STATIC scan/coordinator.cpp scan/win_probe.cpp scan/report.cpp)
target_include_directories(cc_scan PUBLIC scan)
target_link_libraries(cc_scan PUBLIC cc_domain PRIVATE ws2_32)
target_compile_definitions(cc_scan PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(cc_scan PRIVATE /W4 /WX /utf-8 /fp:strict)

add_library(cc_endpoints STATIC scan/endpoints.cpp)
target_include_directories(cc_endpoints PUBLIC scan)
target_link_libraries(cc_endpoints PUBLIC cc_scan PRIVATE ws2_32)
target_compile_definitions(cc_endpoints PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(cc_endpoints PRIVATE /W4 /WX /utf-8)

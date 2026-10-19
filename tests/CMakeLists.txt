find_package(GTest REQUIRED)

file(GLOB CORE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
add_executable(core_tests ${CORE_TEST_SOURCES})
target_link_libraries(core_tests PRIVATE cardinal_core GTest::gtest_main GTest::gmock)
target_compile_definitions(core_tests PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(GoogleTest)
gtest_discover_tests(core_tests)

# Latency numbers on a 2000-card board; run by hand (see docs/performance.md).
add_executable(core_bench EXCLUDE_FROM_ALL bench/core_bench.cpp)
target_link_libraries(core_bench PRIVATE cardinal_core)
target_include_directories(core_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

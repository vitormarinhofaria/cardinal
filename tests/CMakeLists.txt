find_package(GTest REQUIRED)

file(GLOB CORE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
add_executable(core_tests ${CORE_TEST_SOURCES})
target_link_libraries(core_tests PRIVATE cardinal_core GTest::gtest_main GTest::gmock)

include(GoogleTest)
gtest_discover_tests(core_tests)

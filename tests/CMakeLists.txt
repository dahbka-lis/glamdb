add_test(NAME glamdb.smoke COMMAND glamdb)
set_tests_properties(glamdb.smoke PROPERTIES LABELS "smoke" TIMEOUT 10)

find_package(GTest 1.17 CONFIG REQUIRED)
add_executable(glamdb_unit unit/api_test.cpp)
target_link_libraries(glamdb_unit PRIVATE glamdb::core GTest::gtest_main glamdb_build_options)
target_include_directories(glamdb_unit PRIVATE "${PROJECT_SOURCE_DIR}")
include(GoogleTest)
gtest_discover_tests(glamdb_unit TEST_PREFIX glamdb.unit. PROPERTIES LABELS unit TIMEOUT 30)

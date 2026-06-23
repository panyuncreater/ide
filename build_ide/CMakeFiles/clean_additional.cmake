# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Debug")
  file(REMOVE_RECURSE
  "CMakeFiles\\minilang_core_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\minilang_core_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\minilang_ide_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\minilang_ide_autogen.dir\\ParseCache.txt"
  "_deps\\googletest-build\\googlemock\\CMakeFiles\\gmock_autogen.dir\\AutogenUsed.txt"
  "_deps\\googletest-build\\googlemock\\CMakeFiles\\gmock_autogen.dir\\ParseCache.txt"
  "_deps\\googletest-build\\googlemock\\CMakeFiles\\gmock_main_autogen.dir\\AutogenUsed.txt"
  "_deps\\googletest-build\\googlemock\\CMakeFiles\\gmock_main_autogen.dir\\ParseCache.txt"
  "_deps\\googletest-build\\googlemock\\gmock_autogen"
  "_deps\\googletest-build\\googlemock\\gmock_main_autogen"
  "_deps\\googletest-build\\googletest\\CMakeFiles\\gtest_autogen.dir\\AutogenUsed.txt"
  "_deps\\googletest-build\\googletest\\CMakeFiles\\gtest_autogen.dir\\ParseCache.txt"
  "_deps\\googletest-build\\googletest\\CMakeFiles\\gtest_main_autogen.dir\\AutogenUsed.txt"
  "_deps\\googletest-build\\googletest\\CMakeFiles\\gtest_main_autogen.dir\\ParseCache.txt"
  "_deps\\googletest-build\\googletest\\gtest_autogen"
  "_deps\\googletest-build\\googletest\\gtest_main_autogen"
  "minilang_core_autogen"
  "minilang_ide_autogen"
  "tests\\CMakeFiles\\minilang_tests_autogen.dir\\AutogenUsed.txt"
  "tests\\CMakeFiles\\minilang_tests_autogen.dir\\ParseCache.txt"
  "tests\\minilang_tests_autogen"
  )
endif()

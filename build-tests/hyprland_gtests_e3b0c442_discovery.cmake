include("/usr/share/cmake/Modules/GoogleTestAddTests.cmake")
gtest_discover_tests_impl(
  TEST_EXECUTABLE [==[/home/zoan/Projects/Hyprland/build-tests/hyprland_gtests]==]
  TEST_EXECUTOR [==[]==]
  TEST_WORKING_DIR [==[/home/zoan/Projects/Hyprland/build-tests]==]
  TEST_EXTRA_ARGS [==[]==]
  TEST_PROPERTIES [==[]==]
  TEST_PREFIX [==[]==]
  TEST_SUFFIX [==[]==]
  TEST_FILTER [==[]==]
  NO_PRETTY_TYPES [==[FALSE]==]
  NO_PRETTY_VALUES [==[FALSE]==]
  TEST_LIST [==[hyprland_gtests_TESTS]==]
  CTEST_FILE [==[/home/zoan/Projects/Hyprland/build-tests/hyprland_gtests_e3b0c442_tests.cmake]==]
  TEST_DISCOVERY_TIMEOUT [==[5]==]
  TEST_DISCOVERY_EXTRA_ARGS [==[]==]
  TEST_XML_OUTPUT_DIR [==[]==]
  TEST_JSON_OUTPUT_DIR [==[/home/zoan/Projects/Hyprland/build-tests]==]
)

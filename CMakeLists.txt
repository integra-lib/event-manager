cmake_minimum_required(VERSION 3.21)

project(
  hwlib_event_manager
  VERSION 0.2.0
  LANGUAGES CXX)

add_library(hwlib_event_manager INTERFACE)
add_library(Hwlib::event_manager ALIAS hwlib_event_manager)

target_include_directories(hwlib_event_manager
                           INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hwlib_event_manager INTERFACE cxx_std_20)

# Dependants read the version from this property rather than from a variable: a
# variable set here is invisible in a sibling directory, and a stale one left in
# the parent scope would be read instead — a version check that silently passes.
set_target_properties(hwlib_event_manager PROPERTIES HWLIB_VERSION
                                                     ${PROJECT_VERSION})

if(PROJECT_IS_TOP_LEVEL)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2)
  FetchContent_MakeAvailable(googletest)

  include(GoogleTest)
  enable_testing()

  add_executable(event-manager-test tests/event_manager_test.cpp)
  target_link_libraries(event-manager-test PRIVATE Hwlib::event_manager
                                                   GTest::gtest_main)
  gtest_discover_tests(event-manager-test)
endif()

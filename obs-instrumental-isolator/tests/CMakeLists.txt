# Unit tests for the DSP core (all platforms) and an end-to-end test that
# loads the real plugin into libobs (Linux, where libobs is a system library).

include(FetchContent)

find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
  )
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(Catch2)
  # Third-party code: never fail the build on its warnings.
  foreach(_catch_target IN ITEMS Catch2 Catch2WithMain)
    if(TARGET ${_catch_target})
      set_target_properties(${_catch_target} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
      target_compile_options(${_catch_target} PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
    endif()
  endforeach()
  # The plugin template defines UNICODE/_UNICODE globally on Windows, which
  # makes Catch2 emit wmain() instead of main(). Keep a plain console main().
  if(TARGET Catch2WithMain)
    target_compile_definitions(Catch2WithMain PRIVATE DO_NOT_USE_WMAIN)
  endif()
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
include(Catch)

add_executable(
  isolator-tests
  test_crossover.cpp
  test_params.cpp
  test_predictor.cpp
  test_analysis.cpp
  test_engine.cpp
  test_wav.cpp
  test_helpers.hpp
)
target_link_libraries(isolator-tests PRIVATE isolator-dsp isolator-wav Catch2::Catch2WithMain)
target_include_directories(isolator-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(APPLE)
  # Plain command-line test binary; no bundle or code signing needed.
  set_target_properties(isolator-tests PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
endif()

catch_discover_tests(isolator-tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS "unit")

# ---------------------------------------------------------------------------
# End-to-end test: load the compiled plugin into a real libobs instance, push
# audio through an OBS source with the filter attached and verify the result.
if(TARGET ${CMAKE_PROJECT_NAME} AND (OS_LINUX OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  add_executable(isolator-obs-integration obs_integration_test.cpp)
  target_link_libraries(isolator-obs-integration PRIVATE OBS::libobs isolator-dsp)
  target_compile_definitions(
    isolator-obs-integration
    PRIVATE
      ISOLATOR_MODULE_PATH="$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
      ISOLATOR_DATA_PATH="${CMAKE_SOURCE_DIR}/data"
  )
  find_program(XVFB_RUN_EXECUTABLE xvfb-run)
  if(XVFB_RUN_EXECUTABLE)
    # libobs needs an X display even for audio-only work: use a virtual one.
    add_test(
      NAME obs_integration
      COMMAND "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:isolator-obs-integration>
    )
  else()
    add_test(NAME obs_integration COMMAND isolator-obs-integration)
  endif()
  set_tests_properties(obs_integration PROPERTIES LABELS "integration" TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

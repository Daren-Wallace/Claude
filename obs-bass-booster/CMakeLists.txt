cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES C CXX)

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(BASS_BOOSTER_DSP_ONLY "Build only the DSP library, tests and tools (libobs not required)" OFF)
option(BASS_BOOSTER_ENABLE_TESTS "Build the unit and integration tests" OFF)
option(BASS_BOOSTER_BUILD_TOOLS "Build the offline bass-booster-render tool" OFF)

if(BASS_BOOSTER_DSP_ONLY)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
  endif()
  if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
  else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  endif()
else()
  include(compilerconfig)
  include(defaults)
  include(helpers)
endif()

# --------------------------------------------------------------------------
# DSP core: pure C++17, no libobs dependency. Shared by the plugin, the
# tests and the offline render tool.
add_library(bass-booster-dsp STATIC)
target_sources(
  bass-booster-dsp
  PRIVATE src/dsp/bass_engine.cpp src/dsp/params.cpp
  PUBLIC
    src/dsp/bass_engine.hpp
    src/dsp/biquad.hpp
    src/dsp/generators.hpp
    src/dsp/param_fields.hpp
    src/dsp/params.hpp
    src/dsp/utils.hpp
)
target_include_directories(bass-booster-dsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(bass-booster-dsp PUBLIC cxx_std_17)
set_target_properties(bass-booster-dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --------------------------------------------------------------------------
# The OBS plugin module.
if(NOT BASS_BOOSTER_DSP_ONLY)
  add_library(${CMAKE_PROJECT_NAME} MODULE)

  find_package(libobs REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs bass-booster-dsp)

  if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  endif()

  if(ENABLE_QT)
    find_package(Qt6 COMPONENTS Widgets Core)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
    target_compile_options(
      ${CMAKE_PROJECT_NAME}
      PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
    )
    set_target_properties(
      ${CMAKE_PROJECT_NAME}
      PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
    )
  endif()

  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c src/bass-filter.cpp src/bass-filter.h)

  set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
endif()

# --------------------------------------------------------------------------
if(BASS_BOOSTER_BUILD_TOOLS OR BASS_BOOSTER_ENABLE_TESTS)
  add_subdirectory(tools)
endif()

if(BASS_BOOSTER_ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# WAV I/O library (shared with the tests) and the offline render tool.

add_library(bass-booster-wav STATIC wav.cpp wav.hpp)
target_include_directories(bass-booster-wav PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(bass-booster-wav PUBLIC cxx_std_17)

add_executable(bass-booster-render render.cpp)
target_link_libraries(bass-booster-render PRIVATE bass-booster-dsp bass-booster-wav)
if(APPLE)
  set_target_properties(bass-booster-render PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
endif()

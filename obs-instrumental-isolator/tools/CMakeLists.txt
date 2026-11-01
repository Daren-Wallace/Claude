# WAV I/O library (shared with the tests) and the offline render tool.

add_library(isolator-wav STATIC wav.cpp wav.hpp testmix.hpp)
target_include_directories(isolator-wav PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(isolator-wav PUBLIC cxx_std_17)

add_executable(isolator-render render.cpp)
target_link_libraries(isolator-render PRIVATE isolator-dsp isolator-wav)
if(APPLE)
  set_target_properties(isolator-render PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
endif()

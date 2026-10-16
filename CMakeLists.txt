cmake_minimum_required(VERSION 3.21)
project(Glint VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

option(GLINT_BUILD_TESTS "Build Glint core tests" ON)

add_library(glint_core STATIC src/core.cpp)
target_include_directories(glint_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(glint_core PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(glint_core PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_compile_definitions(glint_core PRIVATE _CRT_SECURE_NO_WARNINGS=1)
  set_property(TARGET glint_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(glint_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  add_executable(glint WIN32
    src/main.cpp
    src/win_app.cpp
    src/win_common.cpp
    src/native_host.cpp
    src/registration.cpp
    src/win_resources.rc
  )
  target_link_libraries(glint PRIVATE glint_core user32 shell32 comctl32 advapi32 ole32)
  target_include_directories(glint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(glint PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(glint PRIVATE /W4 /permissive- /EHsc /utf-8 /DUNICODE /D_UNICODE)
    set_property(TARGET glint PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  set_target_properties(glint PROPERTIES OUTPUT_NAME "Glint")
endif()

if(GLINT_BUILD_TESTS)
  add_executable(glint_core_tests tests/test_core.cpp)
  target_link_libraries(glint_core_tests PRIVATE glint_core)
  target_compile_features(glint_core_tests PRIVATE cxx_std_20)
  if(MSVC)
    set_property(TARGET glint_core_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_test(NAME glint_core COMMAND glint_core_tests)
endif()

install(TARGETS glint RUNTIME DESTINATION . OPTIONAL)

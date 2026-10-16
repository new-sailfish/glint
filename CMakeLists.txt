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
    src/setup_window.cpp
    src/extension_bundle.cpp
    src/win_resources.rc
  )
  target_link_libraries(glint PRIVATE glint_core user32 shell32 comctl32 advapi32 ole32)
  target_include_directories(glint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(glint PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(glint PRIVATE /W4 /permissive- /EHsc /utf-8 /DUNICODE /D_UNICODE)
    target_link_options(glint PRIVATE /MANIFEST:NO)
    set_property(TARGET glint PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  set_target_properties(glint PROPERTIES OUTPUT_NAME "Glint")
endif()

if(GLINT_BUILD_TESTS)
  add_executable(glint_core_tests tests/test_core.cpp)
  target_link_libraries(glint_core_tests PRIVATE glint_core)
  target_compile_features(glint_core_tests PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(glint_core_tests PRIVATE /utf-8 /UNDEBUG)
    set_property(TARGET glint_core_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_test(NAME glint_core COMMAND glint_core_tests)

  if(WIN32)
    add_executable(glint_registration_tests tests/test_registration.cpp src/registration.cpp src/win_common.cpp)
    target_include_directories(glint_registration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(glint_registration_tests PRIVATE glint_core advapi32)
    target_compile_features(glint_registration_tests PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(glint_registration_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
      set_property(TARGET glint_registration_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME glint_registration COMMAND glint_registration_tests)

    add_executable(glint_extension_bundle_tests
      tests/test_extension_bundle.cpp
      src/extension_bundle.cpp
      src/extension_bundle_resources.rc
    )
    target_include_directories(glint_extension_bundle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(glint_extension_bundle_tests PRIVATE shell32 ole32)
    target_compile_features(glint_extension_bundle_tests PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(glint_extension_bundle_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
      set_property(TARGET glint_extension_bundle_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME glint_extension_bundle COMMAND glint_extension_bundle_tests)
  endif()
endif()

if(WIN32)
  install(TARGETS glint RUNTIME DESTINATION .)
endif()

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/VersionNumber.cmake")

function(glint_add_build_version target)
  set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
  # A custom target runs on every requested desktop build, including otherwise
  # unchanged builds. A dependency, not ALL or a configure step, keeps test-only
  # targets and configuration from consuming a version.
  add_custom_target(${target}_build_version
    COMMAND "${CMAKE_COMMAND}"
      "-DGLINT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DGLINT_OUTPUT_DIR=${generated_dir}"
      -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GenerateBuildVersion.cmake"
    BYPRODUCTS
      "${generated_dir}/glint_version.h"
      "${generated_dir}/glint_version.rc"
      "${generated_dir}/app.manifest"
    COMMENT "Reserve and generate the Glint build version"
    VERBATIM
  )
  add_dependencies(${target} ${target}_build_version)
  set_source_files_properties("${generated_dir}/glint_version.rc" PROPERTIES GENERATED TRUE)
  target_sources(${target} PRIVATE "${generated_dir}/glint_version.rc")
  target_include_directories(${target} PRIVATE "${generated_dir}")
endfunction()

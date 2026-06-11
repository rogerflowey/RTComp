# Clang attribute plugin building the [[rt::...]] namespace.
# Loaded via:  clang -fplugin=path/to/libRTAttrPlugin.so

cmake_minimum_required(VERSION 3.13.4)

find_package(Clang REQUIRED CONFIG)

add_library(RTAttrPlugin SHARED
  RTAttrPlugin.cpp
)

target_include_directories(RTAttrPlugin PRIVATE
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS}
)

# On macOS, dlopen wants `.so` to match the existing test convention.
if(APPLE)
  set_target_properties(RTAttrPlugin PROPERTIES SUFFIX ".so")
endif()

# Place output next to libRTEffect.so so the lit test substitution
# `%plugin_dir` finds both libraries.
set_target_properties(RTAttrPlugin PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Plugins must not statically link Clang libs; they run inside the
# clang process which already provides them.
if(APPLE)
  set_target_properties(RTAttrPlugin PROPERTIES
    LINK_FLAGS "-undefined dynamic_lookup")
endif()

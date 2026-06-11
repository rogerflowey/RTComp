cmake_minimum_required(VERSION 3.13.4)

project(RTEffect)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM CMake dir: ${LLVM_DIR}")

# Check if add_llvm_pass_plugin is available
if(COMMAND add_llvm_pass_plugin)
  message(STATUS "Using add_llvm_pass_plugin")

  add_llvm_pass_plugin(RTEffect
    lib/EffectSummary.cpp
    lib/ExternalFuncTable.cpp
    lib/RTEffectInferPass.cpp
    lib/RTConstraintCheckPass.cpp
    lib/RTSanPlacementPass.cpp
    lib/RTEffectPlugin.cpp
  )

  target_include_directories(RTEffect PUBLIC
    ${CMAKE_SOURCE_DIR}/include
  )
else()
  message(STATUS "add_llvm_pass_plugin not available, using add_library")

  include_directories(${LLVM_INCLUDE_DIRS})
  add_definitions(${LLVM_DEFINITIONS})

  add_library(RTEffect SHARED
    lib/EffectSummary.cpp
    lib/ExternalFuncTable.cpp
    lib/RTEffectInferPass.cpp
    lib/RTConstraintCheckPass.cpp
    lib/RTSanPlacementPass.cpp
    lib/RTEffectPlugin.cpp
  )

  target_include_directories(RTEffect PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  target_link_libraries(RTEffect LLVM)
endif()

# The lit test harness references the plugin as `libRTEffect.so` on every
# platform. Override the dylib suffix on Apple to keep tests portable.
if(APPLE)
  set_target_properties(RTEffect PROPERTIES SUFFIX ".so")
endif()

# Optional: build the [[rt::...]] Clang attribute plugin when the Clang
# CMake package is discoverable.
option(RTEFFECT_BUILD_CLANG_PLUGIN "Build the [[rt::]] Clang attribute plugin" ON)
if(RTEFFECT_BUILD_CLANG_PLUGIN)
  add_subdirectory(clang_plugin)
endif()

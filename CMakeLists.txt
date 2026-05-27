cmake_minimum_required(VERSION 3.13.4)

project(RTEffect)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

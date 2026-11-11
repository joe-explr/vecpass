# CMakeLists.txt — builds libVecPass.so, an out-of-tree LLVM pass plugin.
#
# Usage:
#   source scripts/env.sh
#   cmake -S . -B build -DLLVM_DIR=$VECPASS_LLVM_PREFIX/lib/cmake/llvm
#   cmake --build build -j
#   opt -load-pass-plugin=./build/libVecPass.so -passes=vecpass-print ...
cmake_minimum_required(VERSION 3.20)
project(VecPass LANGUAGES CXX)

# find_package(LLVM) reads LLVMConfig.cmake from the LLVM install. It gives us
# include dirs, compile definitions, and LLVM_ENABLE_RTTI etc. so that our
# plugin is compiled the same way LLVM itself was.
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
if(LLVM_VERSION_MAJOR LESS 17)
  message(FATAL_ERROR "vecpass needs LLVM >= 17 (found ${LLVM_PACKAGE_VERSION})")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

# LLVM is built without RTTI; a plugin that derives from LLVM classes must
# match, or we get undefined `typeinfo for llvm::...` symbols at load time.
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# MODULE = a shared object meant for dlopen(), never linked against.
# We do NOT link libLLVM: `opt` already has it loaded, and the plugin's
# undefined LLVM symbols resolve against the copy inside opt at dlopen time.
# Linking a second copy would give two sets of global registries.
add_library(VecPass MODULE
  src/Plugin.cpp
  src/LoopPrinter.cpp
  src/VecPass.cpp
)
set_target_properties(VecPass PROPERTIES PREFIX "lib")

cmake_minimum_required(VERSION 3.14)

project(llc2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})


file(GLOB_RECURSE SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	target_link_libraries(${PROJECT_NAME} PRIVATE
	${LLVM_LIBRARY_DIRS}/liblldb.dylib
	)
endif()

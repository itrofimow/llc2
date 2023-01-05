cmake_minimum_required(VERSION 3.14)

project(llc2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#find_package(lldb-12-dev REQUIRED)

find_library(LIBLLDB12 NAMES liblldb-12)

file(GLOB_RECURSE SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
  /usr/lib/llvm-12/include
)
target_link_libraries(${PROJECT_NAME} PRIVATE
  /home/itrofimow/work/llvm-project/build_lldb/lib/liblldb.so
  )

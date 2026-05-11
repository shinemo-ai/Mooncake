cmake_minimum_required(VERSION 3.20)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Add source files for base.
list(
  APPEND
  SRC
  ${CMAKE_CURRENT_LIST_DIR}/status.cpp
  ${CMAKE_CURRENT_LIST_DIR}/net_utils.cpp)

# # Build all the source files of base dir into a static lib('base')
add_library(base STATIC ${SRC})
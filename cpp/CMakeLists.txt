cmake_minimum_required(VERSION 3.10)
project(spirit_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(spirit spirit.cpp)
target_compile_options(spirit PRIVATE -Wall -Wextra)

# `make test` runs the differential test against the Python original one dir up.
find_program(BASH_EXE bash)
if(BASH_EXE)
  add_custom_target(check
    COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh
    DEPENDS spirit
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Differential test vs ../spirit.py")
endif()

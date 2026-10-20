cmake_minimum_required(VERSION 3.16)
project(de-imagedisk VERSION 1.20 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(IMD_WERROR "Treat compiler warnings as errors" OFF)
option(IMD_STATIC "Link the tools statically (Linux release builds)" OFF)

function(imd_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra)
  if(IMD_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

add_library(imdcore STATIC
  src/core/cli.cpp
  src/core/imd_image.cpp
  src/core/td0_reader.cpp)
target_include_directories(imdcore PUBLIC src/core)
target_compile_definitions(imdcore PUBLIC IMD_VERSION="${PROJECT_VERSION}")
imd_warnings(imdcore)

foreach(tool imdu bin2imd td02imd)
  add_executable(${tool} src/${tool}/main.cpp)
  target_link_libraries(${tool} PRIVATE imdcore)
  imd_warnings(${tool})
  if(IMD_STATIC)
    target_link_options(${tool} PRIVATE -static)
  endif()
endforeach()

install(TARGETS imdu bin2imd td02imd RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

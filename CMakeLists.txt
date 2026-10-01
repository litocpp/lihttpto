cmake_minimum_required(VERSION 3.28)
project(lihttpto VERSION 0.1.0 LANGUAGES CXX)

if(NOT TARGET rstd::rstd OR NOT TARGET rstd.parse)
  message(FATAL_ERROR "lihttpto requires the rstd::rstd and rstd.parse targets")
endif()

file(GLOB_RECURSE LIHTTPTO_MODULES CONFIGURE_DEPENDS src/*.cppm)
file(GLOB_RECURSE LIHTTPTO_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(lihttpto STATIC ${LIHTTPTO_SOURCES})
add_library(lihttpto::lihttpto ALIAS lihttpto)
target_compile_features(lihttpto PUBLIC cxx_std_20)
target_sources(lihttpto PUBLIC FILE_SET modules TYPE CXX_MODULES FILES ${LIHTTPTO_MODULES})
target_link_libraries(lihttpto PUBLIC rstd::rstd rstd.parse)

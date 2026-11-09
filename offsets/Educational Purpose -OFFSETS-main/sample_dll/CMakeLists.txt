cmake_minimum_required(VERSION 3.20)

project(sample_dll LANGUAGES CXX)

add_library(sample_dll SHARED
    src/sample_dll.cpp
)

target_compile_features(sample_dll PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(sample_dll PRIVATE /W4 /permissive-)
endif()

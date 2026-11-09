cmake_minimum_required(VERSION 3.20)
project(injectable_dll LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build as a shared library (DLL on Windows)
add_library(payload SHARED payload.cpp)

# Link against user32 for MessageBoxA (used by the hook target)
target_link_libraries(payload PRIVATE user32 winmm)

# Strip the "lib" prefix — output is payload.dll
set_target_properties(payload PROPERTIES PREFIX "")


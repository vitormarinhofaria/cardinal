cmake_minimum_required(VERSION 3.22)
project(cardinal VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

enable_testing()
add_subdirectory(core)
add_subdirectory(ui-gtk)
add_subdirectory(tests)

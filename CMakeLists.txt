cmake_minimum_required(VERSION 3.24)
project(TitanZFS VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings and optimization flags
if (MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-variable -Wno-unused-but-set-variable)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wno-packed-not-aligned)
    endif()
    if (WIN32)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    endif()
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Standalone TitanZFS core library
add_library(titanzfs_core INTERFACE)
target_include_directories(titanzfs_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Executables
add_executable(titanzfs_tests tests/test_titanzfs.cpp)
target_link_libraries(titanzfs_tests PRIVATE titanzfs_core)

add_executable(zpool src/tools/zpool_cli.cpp)
target_link_libraries(zpool PRIVATE titanzfs_core)

add_executable(zfs src/tools/zfs_cli.cpp)
target_link_libraries(zfs PRIVATE titanzfs_core)

add_executable(titanzfs_svc src/service/titanzfs_svc.cpp)
target_link_libraries(titanzfs_svc PRIVATE titanzfs_core)

if (WIN32)
    add_executable(titanzfs_mount src/mount/winfsp_mount.cpp)
    target_link_libraries(titanzfs_mount PRIVATE titanzfs_core)
endif()

# CTest registration
enable_testing()
add_test(NAME TitanZfsCoreTests COMMAND titanzfs_tests)

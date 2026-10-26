cmake_minimum_required(VERSION 3.24)
project(TitanZFS VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings and optimization flags
if (WIN32)
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

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

    add_executable(titanzfs_admin WIN32 src/admin/titanzfs_admin.cpp)
    target_link_libraries(titanzfs_admin PRIVATE titanzfs_core user32 gdi32 comctl32 dwmapi uxtheme ole32 shell32)

    add_executable(titanzfs_packager src/tools/titanzfs_packager.cpp)
    target_link_libraries(titanzfs_packager PRIVATE titanzfs_core)

    add_executable(titanzfs_setup WIN32 src/installer/titanzfs_setup.cpp)
    target_link_libraries(titanzfs_setup PRIVATE titanzfs_core user32 gdi32 gdiplus comctl32 dwmapi uxtheme ole32 oleaut32 shell32 advapi32)
    add_dependencies(titanzfs_setup titanzfs_admin titanzfs_svc titanzfs_mount zpool zfs titanzfs_packager)

    set(WINFSP_MSI "${CMAKE_CURRENT_SOURCE_DIR}/redist/winfsp-2.1.25156.msi")
    set(PACKAGER_EXTRA_ARGS "")
    if (EXISTS "${WINFSP_MSI}")
        set(PACKAGER_EXTRA_ARGS "${WINFSP_MSI}")
    endif()

    add_custom_command(
        TARGET titanzfs_setup POST_BUILD
        COMMAND titanzfs_packager $<TARGET_FILE:titanzfs_setup>
                $<TARGET_FILE:titanzfs_admin>
                $<TARGET_FILE:titanzfs_svc>
                $<TARGET_FILE:titanzfs_mount>
                $<TARGET_FILE:zpool>
                $<TARGET_FILE:zfs>
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/titanzfs_hero.jpg
                ${PACKAGER_EXTRA_ARGS}
        COMMENT "Embedding all binaries, WinFsp, and assets into standalone titanzfs_setup.exe..."
    )
endif()

# ============================================================================
# CPack Packaging Configuration
# ============================================================================
set(CPACK_PACKAGE_NAME "TitanZFS")
set(CPACK_PACKAGE_VENDOR "MicaNT-Kernel")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native ZFS Storage Engine and Filesystem for Windows NT and MicaNT")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

install(TARGETS titanzfs_tests zpool zfs titanzfs_svc RUNTIME DESTINATION bin)
if (WIN32)
    install(TARGETS titanzfs_mount titanzfs_admin titanzfs_setup RUNTIME DESTINATION bin)
    install(DIRECTORY assets/ DESTINATION assets)
    install(DIRECTORY redist/ DESTINATION redist OPTIONAL)
endif()

set(CPACK_GENERATOR "ZIP")
include(CPack)

# CTest registration
enable_testing()
add_test(NAME TitanZfsCoreTests COMMAND titanzfs_tests)

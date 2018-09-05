# Required cmake version
cmake_minimum_required(VERSION 2.8.5)
cmake_policy(SET CMP0048 NEW)

# Specify search path for CMake modules to be loaded by include() 
# and find_package()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules")

# Add defaults for cmake
# Those need to be set before the project() call.
include(DefineCMakeDefaults)
include(DefineCompilerFlags)

project(socket_wrapper VERSION 1.1.9 LANGUAGES C)

# global needed variables
set(APPLICATION_NAME ${PROJECT_NAME})

# SOVERSION scheme: CURRENT.AGE.REVISION
#   If there was an incompatible interface change:
#     Increment CURRENT. Set AGE and REVISION to 0
#   If there was a compatible interface change:
#     Increment AGE. Set REVISION to 0
#   If the source code was changed, but there were no interface changes:
#     Increment REVISION.
set(LIBRARY_VERSION "0.1.9")
set(LIBRARY_SOVERSION "0")

# add definitions
include(DefinePlatformDefaults)
include(DefineInstallationPaths)
include(DefineOptions.cmake)
include(CPackConfig.cmake)
include(CompilerChecks.cmake)

# disallow in-source build
include(MacroEnsureOutOfSourceBuild)
macro_ensure_out_of_source_build("${PROJECT_NAME} requires an out of source build. Please create a separate build directory and run 'cmake /path/to/${PROJECT_NAME} [options]' there.")

# Find out if we have threading available
set(CMAKE_THREAD_PREFER_PTHREADS ON)
find_package(Threads)

# config.h checks
include(ConfigureChecks.cmake)
configure_file(config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config.h)

# check subdirectories
add_subdirectory(src)

if (UNIT_TESTING)
    find_package(cmocka 0.4.1 REQUIRED)

    include(AddCMockaTest)
    add_subdirectory(tests)
endif (UNIT_TESTING)

# pkg-config file
get_filename_component(SOCKET_WRAPPER_LIB ${SOCKET_WRAPPER_LOCATION} NAME)

configure_file(socket_wrapper.pc.cmake ${CMAKE_CURRENT_BINARY_DIR}/socket_wrapper.pc @ONLY)
install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/socket_wrapper.pc
  DESTINATION
    ${LIB_INSTALL_DIR}/pkgconfig
  COMPONENT
    pkgconfig
)

# cmake config files
configure_file(socket_wrapper-config-version.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/socket_wrapper-config-version.cmake @ONLY)
configure_file(socket_wrapper-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/socket_wrapper-config.cmake @ONLY)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/socket_wrapper-config-version.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/socket_wrapper-config.cmake
    DESTINATION
        ${CMAKE_INSTALL_DIR}/socket_wrapper
    COMPONENT
        devel
)

add_subdirectory(doc)

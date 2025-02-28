cmake_minimum_required(VERSION 3.20)
project(externis)

option(EXTERNIS_BUILD_TEST "Build the compiler plugin test" ON)

if(NOT GCC_PLUGIN_INCLUDE)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=plugin
        OUTPUT_VARIABLE GCC_PLUGIN_INCLUDE
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

# IS_ABSOLUTE isn't working here?
if(NOT GCC_PLUGIN_INCLUDE MATCHES "^/.*")
    message(FATAL_ERROR "GCC plugin path is not absolute? ${GCC_PLUGIN_INCLUDE}")
endif()

message(STATUS "GCC plugin headers found in " ${GCC_PLUGIN_INCLUDE})

if(NOT EXISTS ${GCC_PLUGIN_INCLUDE}/include/gcc-plugin.h)
    message(FATAL_ERROR "gcc-plugin.h not found under ${GCC_PLUGIN_INCLUDE}/include")
endif()

# If there's an gcc prefix set, calculate the install location relative to that sysroot so that
# DESTDIR and CMAKE_PREFIX_PATH could be used to relocate the artifact being built
if(EXTERNIS_GCC_PREFIX)
    cmake_path(
        RELATIVE_PATH GCC_PLUGIN_INCLUDE BASE_DIRECTORY ${EXTERNIS_GCC_PREFIX} OUTPUT_VARIABLE
        RELATIVE_PLUGIN_INCLUDE
    )

    # If the plugin include is not under the sysroot, what's even happening? Just fall back on
    # default behavior
    if(RELATIVE_PLUGIN_INCLUDE MATCHES "[.][.]/.*")
        message(
            WARNING
                "EXTERNIS_GCC_PREFIX set to ${EXTERNIS_GCC_PREFIX}, but GCC_PLUGIN_INCLUDE is not a child of that directory ${GCC_PLUGIN_INCLUDE}"
        )
    else()
        set(GCC_PLUGIN_INSTALL_PATH "${RELATIVE_PLUGIN_INCLUDE}")
        message(
            STATUS "Calculated install directory relative to sysroot: ${GCC_PLUGIN_INSTALL_PATH}"
        )
    endif()
endif()

add_library(externis SHARED externis.cc tracking.cc output.cc)
target_include_directories(externis PRIVATE ${GCC_PLUGIN_INCLUDE}/include)

# Optional, useful for debugging.
find_package(fmt)
if(fmt_FOUND)
    target_link_libraries(externis PRIVATE fmt::fmt)
endif()

set_target_properties(externis PROPERTIES CXX_STANDARD 20)
set_target_properties(externis PROPERTIES COMPILE_FLAGS "-fno-rtti -g -Wall")
set_target_properties(externis PROPERTIES PREFIX "" OUTPUT_NAME "externis")
set(EXTERNIS_PLUGIN_PATH ${CMAKE_BINARY_DIR}/externis.so)
install(TARGETS externis DESTINATION ${GCC_PLUGIN_INSTALL_PATH})

if(EXTERNIS_BUILD_TEST)
    add_subdirectory(test)
endif()

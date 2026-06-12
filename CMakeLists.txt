cmake_minimum_required(VERSION 3.15)

if( NOT CMAKE_EXPERIMENTAL_CXX_IMPORT_STD )
    if( ${CMAKE_VERSION} VERSION_GREATER_EQUAL 4.3 )
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 451f2fe2-a8a2-47c3-bc32-94786d8fc91b)
    elseif( ${CMAKE_VERSION} VERSION_GREATER_EQUAL 3.30 )
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    endif ()
endif ()

set(PROJECT_NAME "cppFileLoader")
set(PROJECT_UPPERCASE_NAME "CPPFILELOADER")
set(LIBRARY_TARGET "FileLoader")
set(LIBRARY_ALIAS "${PROJECT_NAME}::${LIBRARY_TARGET}")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(IS_TOP_LEVEL ON)
else()
	set(IS_TOP_LEVEL OFF)
endif()

# this needs to use a custom top level variable instead of PROJECT_IS_TOP_LEVEL since that variable is only set after the project() call, but we need to know whether to append the "tests" feature to the vcpkg manifest before that.
option(${PROJECT_UPPERCASE_NAME}_BUILD_TESTING "Build tests for the file loader" ${IS_TOP_LEVEL})

# this needs to be before the project() call since vcpkg manifest is parsed at the project() call.
if(${PROJECT_UPPERCASE_NAME}_BUILD_TESTING)
    list(APPEND VCPKG_MANIFEST_FEATURES tests)
endif()

project(${PROJECT_NAME} VERSION 1.0.0 LANGUAGES CXX)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(CLANG_TIDY_ENABLED "Enable Clang-Tidy static analysis" ON)
option(ENABLE_LOGGING "Enable header-only logging interface" ON)

# Module-related options (default OFF for backward compatibility).
option(${PROJECT_UPPERCASE_NAME}_USE_STD_MODULE
    "Use import std; instead of #include in the module files" OFF)
option(${PROJECT_UPPERCASE_NAME}_BUILD_AS_MODULE
    "Build with C++20 module files that can be imported instead of #included" OFF)

set(LOGIFACE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/cppLoggingInterface" CACHE PATH "Optional path to the cppLoggingInterface repo")

if(NOT TARGET logiface::logiface)
	if(EXISTS "${LOGIFACE_PATH}/CMakeLists.txt")
		add_subdirectory("${LOGIFACE_PATH}" cppLoggingInterface)
	else()
		message(FATAL_ERROR "Missing logging interface at ${LOGIFACE_PATH}; provide the repo there or set LOGIFACE_PATH to another directory.")
	endif()
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# disable Clang-Tidy if not using Clang or GCC
if(CLANG_TIDY_ENABLED AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
	set(CLANG_TIDY_ENABLED OFF)
endif()
if(CLANG_TIDY_ENABLED)
	include(ConfigureClangTidy)
endif()

add_subdirectory(src)

if(${PROJECT_UPPERCASE_NAME}_BUILD_TESTING)
	add_subdirectory(tests)
endif()
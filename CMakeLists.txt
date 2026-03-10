cmake_minimum_required(VERSION 3.15)

set(PROJECT_NAME "cppFileLoader")
set(PROJECT_UPPERCASE_NAME "CPPFILELOADER")
set(LIBRARY_TARGET "FileLoader")
set(LIBRARY_ALIAS "${PROJECT_NAME}::${LIBRARY_TARGET}")

option(${PROJECT_UPPERCASE_NAME}_BUILD_TESTING "Build tests for the file loader" ${PROJECT_IS_TOP_LEVEL})

if(${PROJECT_UPPERCASE_NAME}_BUILD_TESTING)
    list(APPEND VCPKG_MANIFEST_FEATURES tests)
endif()

project(${PROJECT_NAME} VERSION 1.0.0 LANGUAGES CXX)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(CLANG_TIDY_ENABLED "Enable Clang-Tidy static analysis" ON)
option(ENABLE_LOGGING "Enable header-only logging interface" ON)

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
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

set(DEFAULT_LOGGING_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/cppLoggingInterface")
set(LOGIFACE_PATH "" CACHE PATH "Optional path to the cppLoggingInterface repo")

if(LOGIFACE_PATH)
	set(LOGGING_INTERFACE_DIR "${LOGIFACE_PATH}")
else()
	set(LOGGING_INTERFACE_DIR "${DEFAULT_LOGGING_INTERFACE_DIR}")
endif()

if(EXISTS "${LOGGING_INTERFACE_DIR}/CMakeLists.txt")
	add_subdirectory("${LOGGING_INTERFACE_DIR}" cppLoggingInterface)
else()
	message(FATAL_ERROR "Missing logging interface at ${LOGGING_INTERFACE_DIR}; provide the repo there or set LOGIFACE_PATH to another directory.")
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
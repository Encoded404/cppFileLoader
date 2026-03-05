cmake_minimum_required(VERSION 3.15)

set(PROJECT_NAME "ModernCMakeLibraryTemplate")
set(LIBRARY_TARGET "modern_cmake_library")

project(${PROJECT_NAME} VERSION 1.0.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

set(mclt_export_name ModernCMakeLibraryTemplateTargets)
set(mclt_config_install_dir "${CMAKE_INSTALL_LIBDIR}/cmake/ModernCMakeLibraryTemplate")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# disable Clang-Tidy if not using Clang or GCC
if(CLANG_TIDY_ENABLED AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
	set(CLANG_TIDY_ENABLED OFF)
endif()
if(CLANG_TIDY_ENABLED)
	include(ConfigureClangTidy)
endif()

add_subdirectory(src)

if(BUILD_TESTING)
	add_subdirectory(tests)
endif()

configure_package_config_file(
	cmake/ModernCMakeLibraryTemplateConfig.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}/ModernCMakeLibraryTemplateConfig.cmake"
	INSTALL_DESTINATION "${mclt_config_install_dir}"
)

write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/ModernCMakeLibraryTemplateConfigVersion.cmake"
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion
)

install(
	FILES
		"${CMAKE_CURRENT_BINARY_DIR}/ModernCMakeLibraryTemplateConfig.cmake"
		"${CMAKE_CURRENT_BINARY_DIR}/ModernCMakeLibraryTemplateConfigVersion.cmake"
	DESTINATION "${mclt_config_install_dir}"
)

install(
	EXPORT ${mclt_export_name}
	NAMESPACE ModernCMakeLibrary::
	DESTINATION "${mclt_config_install_dir}"
)
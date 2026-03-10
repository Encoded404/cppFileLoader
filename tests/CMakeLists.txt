set(TEST_TARGET_NAME "file_formats_tests")

# Find Python for test file generation
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Find dependencies
find_package(GTest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
include(GoogleTest)

# ============================================================================
# Generate test data files using Python script
# ============================================================================
set(TEST_DATA_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/test_data")
file(MAKE_DIRECTORY "${TEST_DATA_OUTPUT_DIR}")

add_custom_command(
    OUTPUT 
        "${TEST_DATA_OUTPUT_DIR}/test_data.kv"
        "${TEST_DATA_OUTPUT_DIR}/test_data.bin"
        "${TEST_DATA_OUTPUT_DIR}/test_data.tlv"
        "${TEST_DATA_OUTPUT_DIR}/expected_results.json"
    COMMAND 
        "${Python3_EXECUTABLE}" 
        "${CMAKE_CURRENT_SOURCE_DIR}/generate_test_files.py"
        "${TEST_DATA_OUTPUT_DIR}"
        --seed 42
    DEPENDS 
        "${CMAKE_CURRENT_SOURCE_DIR}/generate_test_files.py"
    COMMENT "Generating test files with expected results..."
    VERBATIM
)

add_custom_target(
    generate_test_data ALL
    DEPENDS 
        "${TEST_DATA_OUTPUT_DIR}/test_data.kv"
        "${TEST_DATA_OUTPUT_DIR}/test_data.bin"
        "${TEST_DATA_OUTPUT_DIR}/test_data.tlv"
        "${TEST_DATA_OUTPUT_DIR}/expected_results.json"
)

# ============================================================================
# File formats tests
# ============================================================================
add_executable(${TEST_TARGET_NAME} file_formats_tests.cpp)

# Ensure test data is generated before compiling tests
add_dependencies(${TEST_TARGET_NAME} generate_test_data)

# Link libraries
target_link_libraries(${TEST_TARGET_NAME} PRIVATE
    ${LIBRARY_TARGET}
    GTest::gtest
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)

# Add include directories
target_include_directories(${TEST_TARGET_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)

# Set compiler options for the test executable
target_compile_options(${TEST_TARGET_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
	target_compile_options(${TEST_TARGET_NAME} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Let CTest discover individual GoogleTest tests in the binary
gtest_discover_tests(${TEST_TARGET_NAME}
    PROPERTIES
        ENVIRONMENT "TEST_LOG_DIR=${CMAKE_BINARY_DIR}/Testing/logs"
        ENVIRONMENT "CMAKE_BINARY_DIR=${CMAKE_BINARY_DIR}"
)

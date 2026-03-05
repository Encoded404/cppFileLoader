# Add the test executable
add_executable(example_tests example_tests.cpp)

# Find GTest installed via vcpkg and link it along with the main library
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
target_link_libraries(example_tests PRIVATE
    ${LIBRARY_TARGET}
    GTest::gtest
    GTest::gtest_main
)

# Set compiler options for the test executable
target_compile_options(example_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
	target_compile_options(example_tests PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Let CTest discover individual GoogleTest tests in the binary
gtest_discover_tests(example_tests
    PROPERTIES
        ENVIRONMENT "TEST_LOG_DIR=${CMAKE_BINARY_DIR}/Testing/logs"
)

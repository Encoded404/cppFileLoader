find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)

if(NOT CLANG_TIDY_EXECUTABLE)
    message(WARNING "Clang-Tidy not configured: 'clang-tidy' executable not found.")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy")
    # Use the project's .clang-tidy configuration; do not override checks here.
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};-p=${CMAKE_BINARY_DIR}")
    message(STATUS "Clang-Tidy enabled (using .clang-tidy at ${CMAKE_CURRENT_SOURCE_DIR}).")
else()
    message(WARNING "Clang-Tidy disabled: '.clang-tidy' not found in ${CMAKE_CURRENT_SOURCE_DIR}.")
endif()
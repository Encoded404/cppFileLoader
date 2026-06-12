# Main library target
add_library(${LIBRARY_TARGET} STATIC)
add_library(${LIBRARY_ALIAS} ALIAS ${LIBRARY_TARGET})

target_link_libraries(${LIBRARY_TARGET} PUBLIC logiface)

target_compile_definitions(${LIBRARY_TARGET}
    PUBLIC
        $<$<BOOL:${ENABLE_LOGGING}>:LOGIFACE_ENABLE_LOGGING=1>
        $<$<NOT:$<BOOL:${ENABLE_LOGGING}>>:LOGIFACE_ENABLE_LOGGING=0>
)

target_compile_options(${LIBRARY_TARGET} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
        $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${LIBRARY_TARGET} PRIVATE
                "$<$<CONFIG:Debug>:-g>"
                "$<$<CONFIG:Debug>:-O0>"
                "$<$<CONFIG:RelWithDebInfo>:-g>"
                "$<$<CONFIG:RelWithDebInfo>:-O2>"
                "$<$<CONFIG:Release>:-O3>"
        )
endif()

target_include_directories(${LIBRARY_TARGET}
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

# Module build path (CPPFILELOADER_BUILD_AS_MODULE=ON)
# When enabled, the library exposes C++20 module files that consumers
# can `import` instead of `#include`.  The header files remain
# available for backwards compatibility.
if(${PROJECT_UPPERCASE_NAME}_BUILD_AS_MODULE)

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_MODULE_STD ON)

    target_compile_features(${LIBRARY_TARGET} PUBLIC cxx_std_23)
    set_target_properties(${LIBRARY_TARGET} PROPERTIES CXX_MODULE_STD ON)

    target_compile_definitions(${LIBRARY_TARGET}
        PUBLIC
            $<$<BOOL:${CPPFILELOADER_USE_STD_MODULE}>:CPPFILELOADER_USE_STD_MODULE=1>
    )

    target_sources(${LIBRARY_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/Types.cppm
            ${CMAKE_CURRENT_SOURCE_DIR}/IncrementalBuffer.cppm
            ${CMAKE_CURRENT_SOURCE_DIR}/FileLoader.cppm
    )

# Traditional header/source build path (default)
else()

    target_compile_features(${LIBRARY_TARGET} PUBLIC cxx_std_20)

    target_sources(${LIBRARY_TARGET} PRIVATE
        IncrementalBuffer.cpp
    )

endif()
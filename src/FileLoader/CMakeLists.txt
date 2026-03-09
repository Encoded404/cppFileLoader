# Main library target
add_library(${LIBRARY_TARGET} STATIC)
add_library(${LIBRARY_ALIAS} ALIAS ${LIBRARY_TARGET})

target_compile_features(${LIBRARY_TARGET} PUBLIC cxx_std_20)
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
                "$<$<CONFIG:Release>:-O3>"
        )
endif()

target_include_directories(${LIBRARY_TARGET}
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_sources(${LIBRARY_TARGET} PRIVATE
        IncrementalBuffer.cpp
)
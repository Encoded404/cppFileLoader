# Main library target
add_library(${LIBRARY_TARGET} STATIC)
add_library(ModernCMakeLibrary::modern_cmake_library ALIAS ${LIBRARY_TARGET})

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
        $<INSTALL_INTERFACE:include/modern_cmake_library>
)

target_sources(${LIBRARY_TARGET} PRIVATE
        lib_example.cpp
        lib_privateCode.cpp
)

install(
    TARGETS ${LIBRARY_TARGET} logiface
    EXPORT ${mclt_export_name}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION include/modern_cmake_library
)

install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/include/
    DESTINATION include/modern_cmake_library
    FILES_MATCHING PATTERN "*.hpp"
)

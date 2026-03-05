#ifndef MODERN_CMAKE_LIBRARY_LIB1_EXAMPLE_HPP
#define MODERN_CMAKE_LIBRARY_LIB1_EXAMPLE_HPP

// public header file, installed or simply included in the application, used by library users

namespace modern_cmake_library
{
    class example
    {
    private:
        /* data */
        /* private functions */
    public:
        /* public functions */
        int exampleFunction(int value);
        int anotherExampleFunction(int value);
    };
    

} // namespace modern_cmake_library

#endif // MODERN_CMAKE_LIBRARY_LIB1_EXAMPLE_HPP
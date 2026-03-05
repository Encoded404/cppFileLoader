#include <modern_cmake_library/lib1/lib_example.hpp>

#include "lib_privateCode.hpp"

#include <iostream>

#include <logging/logging.hpp>

namespace modern_cmake_library {
    int example::exampleFunction(int value)
    {
        LOGIFACE_LOG(debug, "exampleFunction invoked");
        return value * 2;
    }
    int example::anotherExampleFunction(int value)
    {
        LOGIFACE_LOG(debug, "anotherExampleFunction invoked");
        int result = modern_cmake_library::PrivateCode::doSomethingPrivate(value);
        return result + 10;
    }
} // namespace modern_cmake_library
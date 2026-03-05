# modern-cmake-library-template

Modern CMake starter geared toward libraries: the library is the primary artifact, an optional CLI demonstrates usage, and GoogleTest is wired in via vcpkg. Clang-Tidy can be enabled when available.

## Layout

```
modern-cmake-library-template/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
│   └── ConfigureClangTidy.cmake
├── include/modern_cmake_library/         # Public headers
│   ├── lib1/lib_example.hpp
│   └── logging/
│       ├── logging.hpp                   # Optional logging interface
│       └── ConsoleLogger.hpp             # example logger implimentation   
├── src/
│   ├── CMakeLists.txt
│   ├── app/                              # Optional example CLI (BUILD_CLI)
│   │   ├── CMakeLists.txt
│   │   └── main.cpp
│   └── lib1/
│       ├── CMakeLists.txt
│       ├── lib_example.cpp
│       └── lib_privateCode.cpp/.hpp     # Internal-only code
├── tests/
│   ├── CMakeLists.txt
│   ├── example_tests.cpp
│   └── test_logging.hpp
│
├── vcpkg.json
└── vcpkg-configuration.json
```

## Prerequisites

- CMake ≥ 3.15
- A C++20-capable compiler
- vcpkg available and `VCPKG_ROOT` set (the default preset expects it).

## VCPKG installation
you can follow the guide [here](https://github.com/microsoft/vcpkg) for installation

## Clone

```bash
git clone <repository-url>
cd modern-cmake-library-template
```

## Configure

Use the provided Ninja multi-config preset:

```bash
cmake --preset default -S . -B build
```

Options:
- `-DBUILD_TESTING=OFF` to skip tests (default: ON via preset)
- `-DENABLE_LOGGING=OFF` to compile out logging macros (default: ON)
- `-DCLANG_TIDY_ENABLED=OFF` to skip clang-tidy configuration

If you prefer a manual invoke, or vcpkg lives elsewhere, specify the toolchain and cache toggles directly:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DBUILD_TESTING=ON
```

## Build

```bash
cmake --build build --config Debug   # or Release/RelWithDebInfo
```

## Tests

Tests are controlled by `BUILD_TESTING` (default ON in the preset). Disable with `-DBUILD_TESTING=OFF` if you only want the app:

```bash
ctest --test-dir build -C Debug
```

## Install & consume

Install to a prefix (example):

```bash
cmake --install build --config Debug --prefix /usr/local
```

Then consume via CMake in a downstream project:

```cmake
find_package(ModernCMakeLibraryTemplate CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ModernCMakeLibrary::modern_cmake_library)
```

Public headers install to `include/modern_cmake_library/...`; adjust names/namespace as you adopt this template for your project.

## Clang-Tidy

Set `-DCLANG_TIDY_ENABLED=ON` (default) to enable static analysis when `clang-tidy` is found and a `.clang-tidy` file is present at the project root. Set it to `OFF` to skip configuring Clang-Tidy even if available.

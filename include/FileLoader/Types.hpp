#ifndef FILE_LOADER_TYPES_HPP
#define FILE_LOADER_TYPES_HPP

#include <vector>
#include <span>
#include <cstddef>

namespace FileLoader
{
    using Byte = std::byte;
    using ByteBuffer = std::vector<Byte>;
    using ByteSpan = std::span<const Byte>;
}

#endif // FILE_LOADER_TYPES_HPP
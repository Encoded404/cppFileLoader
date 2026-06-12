module;

#ifdef CPPFILELOADER_USE_STD_MODULE
// GMF is intentionally empty — import std; in module purview
#else
#include <vector>
#include <span>
#include <cstddef>
#endif

export module FileLoader.Types;

#ifdef CPPFILELOADER_USE_STD_MODULE
import std;
#endif

export namespace FileLoader
{
    using Byte = std::byte;
    using ByteBuffer = std::vector<Byte>;
    using ByteSpan = std::span<const Byte>;
}

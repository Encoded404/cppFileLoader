#ifndef FILELOADER_TEST_FILE_FORMATS_HPP
#define FILELOADER_TEST_FILE_FORMATS_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace FileLoader::TestFormats {

// ============================================================================
// File Format 1: Plain Text Key-Value Format
// ============================================================================
// Simple human-readable format
// Format:
//   key1=value1
//   key2=value2
//   # comment lines start with #
//

struct KeyValuePair {
    std::string key;
    std::string value;
};

struct KeyValueData {
    std::vector<KeyValuePair> pairs;
};

// ============================================================================
// File Format 2: Binary Fixed-Record Format
// ============================================================================
// Header: [MAGIC:4 bytes] [VERSION:2 bytes] [RECORD_COUNT:4 bytes] [RESERVED:2 bytes]
// Then: RECORD_COUNT records, each:
//   [ID:4 bytes] [TIMESTAMP:8 bytes] [VALUE:4 bytes] [CHECKSUM:4 bytes]
//

#pragma pack(push, 1)
struct BinaryFormatHeader {
    uint32_t magic = 0xDEADBEEF;
    uint16_t version = 1;
    uint32_t record_count = 0;
    uint16_t reserved = 0;
};

struct BinaryFormatRecord {
    uint32_t id;
    uint64_t timestamp;
    uint32_t value;
    uint32_t checksum;
};
#pragma pack(pop)

struct BinaryFixedData {
    BinaryFormatHeader header;
    std::vector<BinaryFormatRecord> records;
};

// ============================================================================
// File Format 3: Binary TLV Format (Type-Length-Value)
// ============================================================================
// TLV entries format:
//   [TAG:1 byte] [LENGTH:2 bytes] [VALUE:LENGTH bytes]
//
// Tags:
//   0x01: Metadata (string, value is JSON)
//   0x02: DataRecord (16 bytes: 4-byte id, 8-byte timestamp, 4-byte value)
//   0xFF: End of file marker
//

enum class TLVTag : uint8_t {
    Metadata = 0x01,
    DataRecord = 0x02,
    EndOfFile = 0xFF
};

struct TLVEntry {
    TLVTag tag;
    std::vector<std::byte> value;
};

struct BinaryTLVData {
    std::string metadata;
    std::vector<std::pair<uint32_t, uint32_t>> records; // id, value
    std::vector<uint64_t> timestamps;
};

} // namespace FileLoader::TestFormats

#endif // FILELOADER_TEST_FILE_FORMATS_HPP

#ifndef FILELOADER_TEST_FILE_ASSEMBLERS_HPP
#define FILELOADER_TEST_FILE_ASSEMBLERS_HPP

#include "FileLoader/FileLoader.hpp"
#include "TestFileFormats.hpp"
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <endian.h>  // For be16toh

namespace FileLoader::TestFormatAssemblers {

// ============================================================================
// Plain Text Key-Value Assembler
// ============================================================================
class KeyValueAssembler : public IAssembler<TestFormats::KeyValueData, AssemblyMode::FullBuffer> {
public:
    std::future<std::shared_ptr<TestFormats::KeyValueData>> AssembleFromFullBuffer(
        std::shared_ptr<ByteBuffer> buffer) override 
    {
        auto prom = std::make_shared<std::promise<std::shared_ptr<TestFormats::KeyValueData>>>();
        
        try {
            auto result = std::make_shared<TestFormats::KeyValueData>();
            
            if (!buffer || buffer->empty()) {
                prom->set_value(result);
                return prom->get_future();
            }
            
            // Convert buffer to string
            const std::string content(buffer->begin(), buffer->end());
            std::istringstream iss(content);
            std::string line;
            
            while (std::getline(iss, line)) {
                // Skip empty lines and comments
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                
                // Find = separator
                const size_t eq_pos = line.find('=');
                if (eq_pos == std::string::npos) {
                    throw std::runtime_error("Invalid key=value format: " + line);
                }
                
                std::string key = line.substr(0, eq_pos);
                std::string value = line.substr(eq_pos + 1);
                
                // Trim whitespace
                key.erase(0, key.find_first_not_of(" \t\r\n"));
                key.erase(key.find_last_not_of(" \t\r\n") + 1);
                value.erase(0, value.find_first_not_of(" \t\r\n"));
                value.erase(value.find_last_not_of(" \t\r\n") + 1);
                
                result->pairs.emplace_back(key, value);
            }
            
            prom->set_value(result);
        } catch (...) {
            prom->set_exception(std::current_exception());
        }
        
        return prom->get_future();
    }
};

// ============================================================================
// Binary Fixed-Record Assembler
// ============================================================================
class BinaryFixedAssembler : public IAssembler<TestFormats::BinaryFixedData, AssemblyMode::FullBuffer> {
public:
    std::future<std::shared_ptr<TestFormats::BinaryFixedData>> AssembleFromFullBuffer(
        std::shared_ptr<ByteBuffer> buffer) override 
    {
        auto prom = std::make_shared<std::promise<std::shared_ptr<TestFormats::BinaryFixedData>>>();
        
        try {
            auto result = std::make_shared<TestFormats::BinaryFixedData>();
            
            if (!buffer || buffer->size() < sizeof(TestFormats::BinaryFormatHeader)) {
                throw std::runtime_error("Buffer too small for header");
            }
            
            // Parse header
            memcpy(&result->header, buffer->data(), sizeof(TestFormats::BinaryFormatHeader));
            
            if (result->header.magic != 0xDEADBEEF) {
                throw std::runtime_error("Invalid magic number");
            }
            
            if (result->header.version != 1) {
                throw std::runtime_error("Unsupported version");
            }
            
            // Parse records
            const size_t record_size = sizeof(TestFormats::BinaryFormatRecord);
            const size_t header_size = sizeof(TestFormats::BinaryFormatHeader);
            const size_t expected_size = header_size + (result->header.record_count * record_size);
            
            if (buffer->size() < expected_size) {
                throw std::runtime_error("Buffer size mismatch");
            }
            
            result->records.resize(result->header.record_count);
            memcpy(result->records.data(), 
                       buffer->data() + header_size, 
                       result->header.record_count * record_size);
            
            prom->set_value(result);
        } catch (...) {
            prom->set_exception(std::current_exception());
        }
        
        return prom->get_future();
    }
};

// ============================================================================
// Binary TLV Assembler
// ============================================================================
class BinaryTLVAssembler : public IAssembler<TestFormats::BinaryTLVData, AssemblyMode::FullBuffer> {
public:
    std::future<std::shared_ptr<TestFormats::BinaryTLVData>> AssembleFromFullBuffer(
        std::shared_ptr<ByteBuffer> buffer) override 
    {
        auto prom = std::make_shared<std::promise<std::shared_ptr<TestFormats::BinaryTLVData>>>();
        
        try {
            auto result = std::make_shared<TestFormats::BinaryTLVData>();
            
            // Handle empty buffer gracefully
            if (!buffer || buffer->empty()) {
                prom->set_value(result);
                return prom->get_future();
            }
            
            // If buffer is too small but non-empty, it's an error
            if (buffer->size() < 4) {
                throw std::runtime_error("Buffer too small for magic number");
            }
            
            // Read and validate magic number
            uint32_t magic = 0;
            memcpy(&magic, buffer->data(), 4);
            if (magic != 0xDEADBEEF) {
                throw std::runtime_error("Invalid TLV magic number");
            }
            
            size_t offset = 4;  // Start after magic number
            bool found_eof = false;
            
            while (offset < buffer->size() && !found_eof) {
                auto tag = static_cast<TestFormats::TLVTag>(buffer->at(offset));
                offset += 1;  // Move past tag byte
                
                // EOF marker is just a single byte with no length or value
                if (tag == TestFormats::TLVTag::EndOfFile) {
                    found_eof = true;
                    break;
                }
                
                // All other entries have a 2-byte length field (big-endian)
                if (offset + 2 > buffer->size()) {
                    throw std::runtime_error("Incomplete TLV length field");
                }
                
                uint16_t length_be = 0;
                memcpy(&length_be, buffer->data() + offset, 2);
                const uint16_t length = be16toh(length_be);  // Convert from big-endian
                offset += 2;
                
                if (offset + length > buffer->size()) {
                    throw std::runtime_error("Incomplete TLV value");
                }
                
                switch (tag) {
                    case TestFormats::TLVTag::Metadata: {
                        result->metadata = std::string(
                            reinterpret_cast<const char*>(buffer->data() + offset),
                            length
                        );
                        break;
                    }
                    case TestFormats::TLVTag::DataRecord: {
                        if (length != 16) {
                            throw std::runtime_error("DataRecord must be 16 bytes");
                        }
                        uint32_t id;
                        uint64_t timestamp;
                        uint32_t value;
                        memcpy(&id, buffer->data() + offset, 4);
                        memcpy(&timestamp, buffer->data() + offset + 4, 8);
                        memcpy(&value, buffer->data() + offset + 12, 4);
                        
                        result->records.emplace_back(id, value);
                        result->timestamps.push_back(timestamp);
                        break;
                    }
                    default:
                        throw std::runtime_error("Unknown TLV tag: " + std::to_string(static_cast<int>(tag)));
                }
                
                offset += length;
            }
            
            if (!found_eof) {
                throw std::runtime_error("Missing end-of-file marker");
            }
            
            prom->set_value(result);
        } catch (...) {
            prom->set_exception(std::current_exception());
        }
        
        return prom->get_future();
    }
};

} // namespace FileLoader::TestFormatAssemblers

#endif // FILELOADER_TEST_FILE_ASSEMBLERS_HPP

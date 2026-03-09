#include <gtest/gtest.h>

#include <logging/logging.hpp>

#include <FileLoader/IncrementalBuffer.hpp>

#include "test_logging.hpp"
#include "TestFileFormats.hpp"
#include "TestFileAssemblers.hpp"

#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>
#include <cstddef>

using FileLoader::ByteBuffer;
using json = nlohmann::json;
namespace Fs = std::filesystem;
namespace Formats = FileLoader::TestFormats;
namespace Assemblers = FileLoader::TestFormatAssemblers;

namespace {
// Install the per-test file logger once for the test binary.
[[maybe_unused]] const bool kLoggerInstalled = [] {
    TestLogging::InstallPerTestFileLogger();
    return true;
}();

// Helper to load a file into a ByteBuffer
ByteBuffer LoadFileToBuffer(const Fs::path& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open file: " + file_path.string());
    }
    
    ByteBuffer buffer;
    file.seekg(0, std::ios::end);
    const std::size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    buffer.resize(size);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    return buffer;
}

// Find the test data directory
Fs::path GetTestDataDir() {
    // The data should be in CMAKE_BINARY_DIR/tests/test_data
    const char* binary_dir = std::getenv("CMAKE_BINARY_DIR");
    if (!binary_dir) {
        throw std::runtime_error("CMAKE_BINARY_DIR environment variable not set");
    }
    
    Fs::path data_dir = Fs::path(binary_dir) / "tests" / "test_data";
    if (!Fs::exists(data_dir)) {
        throw std::runtime_error("Test data directory not found: " + data_dir.string());
    }
    return data_dir;
}

// Load expected results
json LoadExpectedResults(const Fs::path& test_data_dir) {
    const Fs::path results_file = test_data_dir / "expected_results.json";
    std::ifstream file(results_file);
    if (!file) {
        throw std::runtime_error("Cannot open expected results file: " + results_file.string());
    }
    
    json j;
    file >> j;
    return j;
}

}  // anonymous namespace

// ============================================================================
// Tests for Plain Text Key-Value Format
// ============================================================================
class KeyValueFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_data_dir_ = GetTestDataDir();
        expected_results_ = LoadExpectedResults(test_data_dir_);
        LOGIFACE_LOG(info, "KeyValue test data located");
    }
    
    Fs::path test_data_dir_;
    json expected_results_;
};

TEST_F(KeyValueFileTest, ParseValidFile) {
    const Fs::path kv_file = test_data_dir_ / "test_data.kv";
    ASSERT_TRUE(Fs::exists(kv_file)) << "Key-value test file not found";
    
    auto buffer = std::make_shared<ByteBuffer>(LoadFileToBuffer(kv_file));
    Assemblers::KeyValueAssembler assembler;
    auto result_future = assembler.AssembleFromFullBuffer(buffer);
    
    LOGIFACE_LOG(info, "Parsing key-value file");
    
    auto result = result_future.get();
    ASSERT_TRUE(result);
    
    const auto& expected_pairs = expected_results_["keyvalue"]["pairs"];
    ASSERT_EQ(result->pairs.size(), expected_pairs.size()) 
        << "Pair count mismatch";
    
    LOGIFACE_LOG(info, "Loaded key-value pairs");
    
    for (size_t i = 0; i < result->pairs.size(); ++i) {
        const auto& actual = result->pairs[i];
        const auto& expected = expected_pairs[i];
        
        EXPECT_EQ(actual.key, expected["key"].get<std::string>())
            << "Key mismatch at index " << i;
        EXPECT_EQ(actual.value, expected["value"].get<std::string>())
            << "Value mismatch at index " << i;
    }
}

TEST_F(KeyValueFileTest, HandleEmptyBuffer) {
    Assemblers::KeyValueAssembler assembler;
    auto empty_buffer = std::make_shared<ByteBuffer>();
    
    auto result = assembler.AssembleFromFullBuffer(empty_buffer).get();
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->pairs.empty());
    
    LOGIFACE_LOG(info, "Empty buffer handled correctly");
}

TEST_F(KeyValueFileTest, HandleNullBuffer) {
    Assemblers::KeyValueAssembler assembler;
    
    auto result = assembler.AssembleFromFullBuffer(nullptr).get();
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->pairs.empty());
    
    LOGIFACE_LOG(info, "Null buffer handled correctly");
}

// ============================================================================
// Tests for Binary Fixed-Record Format
// ============================================================================
class BinaryFixedFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_data_dir_ = GetTestDataDir();
        expected_results_ = LoadExpectedResults(test_data_dir_);
        LOGIFACE_LOG(info, "Binary Fixed test data located");
    }
    
    Fs::path test_data_dir_;
    json expected_results_;
};

TEST_F(BinaryFixedFileTest, ParseValidFile) {
    const Fs::path bin_file = test_data_dir_ / "test_data.bin";
    ASSERT_TRUE(Fs::exists(bin_file)) << "Binary test file not found";
    
    auto buffer = std::make_shared<ByteBuffer>(LoadFileToBuffer(bin_file));
    Assemblers::BinaryFixedAssembler assembler;
    auto result_future = assembler.AssembleFromFullBuffer(buffer);
    
    LOGIFACE_LOG(info, "Parsing binary fixed file");
    
    auto result = result_future.get();
    ASSERT_TRUE(result);
    
    // Verify header
    EXPECT_EQ(result->header.magic, 0xDEADBEEF) << "Magic number mismatch";
    EXPECT_EQ(result->header.version, 1) << "Version mismatch";
    
    const auto& expected_records = expected_results_["binary_fixed"]["records"];
    ASSERT_EQ(result->records.size(), expected_records.size())
        << "Record count mismatch";
    
    LOGIFACE_LOG(info, "Loaded records from binary file");
    
    for (size_t i = 0; i < result->records.size(); ++i) {
        const auto& actual = result->records[i];
        const auto& expected = expected_records[i];
        
        EXPECT_EQ(actual.id, expected["id"].get<uint32_t>())
            << "ID mismatch at record " << i;
        EXPECT_EQ(actual.timestamp, expected["timestamp"].get<uint64_t>())
            << "Timestamp mismatch at record " << i;
        EXPECT_EQ(actual.value, expected["value"].get<uint32_t>())
            << "Value mismatch at record " << i;
    }
}

TEST_F(BinaryFixedFileTest, RejectInvalidMagic) {
    Assemblers::BinaryFixedAssembler assembler;
    
    // Create buffer with invalid magic
    auto buffer = std::make_shared<ByteBuffer>(sizeof(Formats::BinaryFormatHeader));
    std::memset(buffer->data(), 0xFF, buffer->size());
    
    EXPECT_THROW(
        assembler.AssembleFromFullBuffer(buffer).get(),
        std::runtime_error
    ) << "Should reject invalid magic number";
    
    LOGIFACE_LOG(info, "Invalid magic number correctly rejected");
}

TEST_F(BinaryFixedFileTest, RejectTruncatedBuffer) {
    Assemblers::BinaryFixedAssembler assembler;
    
    // Create truncated buffer
    auto buffer = std::make_shared<ByteBuffer>(5);
    
    EXPECT_THROW(
        assembler.AssembleFromFullBuffer(buffer).get(),
        std::runtime_error
    ) << "Should reject truncated buffer";
    
    LOGIFACE_LOG(info, "Truncated buffer correctly rejected");
}

// ============================================================================
// Tests for Binary TLV Format
// ============================================================================
class BinaryTLVFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_data_dir_ = GetTestDataDir();
        expected_results_ = LoadExpectedResults(test_data_dir_);
        LOGIFACE_LOG(info, "Binary TLV test data located");
    }
    
    Fs::path test_data_dir_;
    json expected_results_;
};

TEST_F(BinaryTLVFileTest, ParseValidFile) {
    const Fs::path tlv_file = test_data_dir_ / "test_data.tlv";
    ASSERT_TRUE(Fs::exists(tlv_file)) << "TLV test file not found";
    
    auto buffer = std::make_shared<ByteBuffer>(LoadFileToBuffer(tlv_file));
    Assemblers::BinaryTLVAssembler assembler;
    auto result_future = assembler.AssembleFromFullBuffer(buffer);
    
    LOGIFACE_LOG(info, "Parsing binary TLV file");
    
    auto result = result_future.get();
    ASSERT_TRUE(result);
    
    // Verify metadata
    EXPECT_FALSE(result->metadata.empty()) << "Metadata should not be empty";
    
    const auto& expected_records = expected_results_["binary_tlv"]["records"];
    ASSERT_EQ(result->records.size(), expected_records.size())
        << "Record count mismatch";
    
    LOGIFACE_LOG(info, "Loaded records from TLV file");
    
    for (size_t i = 0; i < result->records.size(); ++i) {
        const auto& actual = result->records[i];
        const auto& expected = expected_records[i];
        
        EXPECT_EQ(actual.first, expected["id"].get<uint32_t>())
            << "ID mismatch at record " << i;
        EXPECT_EQ(actual.second, expected["value"].get<uint32_t>())
            << "Value mismatch at record " << i;
        
        EXPECT_EQ(result->timestamps[i], expected["timestamp"].get<uint64_t>())
            << "Timestamp mismatch at record " << i;
    }
}

TEST_F(BinaryTLVFileTest, RejectMissingEOF) {
    Assemblers::BinaryTLVAssembler assembler;
    
    // Create buffer without EOF marker
    auto buffer = std::make_shared<ByteBuffer>();
    
    // Add metadata TLV without EOF
    std::string metadata = "test";
    buffer->push_back(std::byte(0x01)); // TAG_METADATA
    buffer->push_back(std::byte(0x04));
    buffer->push_back(std::byte(0x00));
    for (char c : metadata) {
        buffer->push_back(std::byte(static_cast<unsigned char>(c)));
    }
    
    EXPECT_THROW(
        assembler.AssembleFromFullBuffer(buffer).get(),
        std::runtime_error
    ) << "Should reject missing EOF marker";
    
    LOGIFACE_LOG(info, "Missing EOF marker correctly rejected");
}

TEST_F(BinaryTLVFileTest, HandleEmptyFile) {
    Assemblers::BinaryTLVAssembler assembler;
    auto empty_buffer = std::make_shared<ByteBuffer>();
    
    auto result = assembler.AssembleFromFullBuffer(empty_buffer).get();
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->records.empty());
    EXPECT_TRUE(result->metadata.empty());
    
    LOGIFACE_LOG(info, "Empty TLV file handled correctly");
}

// ============================================================================
// Integration Tests - Compare All Formats
// ============================================================================
class FormatIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_data_dir_ = GetTestDataDir();
        expected_results_ = LoadExpectedResults(test_data_dir_);
    }
    
    Fs::path test_data_dir_;
    json expected_results_;
};

TEST_F(FormatIntegrationTest, AllFormatsLoadSuccessfully) {
    LOGIFACE_LOG(info, "Testing all formats load and parse correctly");
    
    // Test Key-Value
    {
        const Fs::path kv_file = test_data_dir_ / "test_data.kv";
        auto buffer = std::make_shared<ByteBuffer>(LoadFileToBuffer(kv_file));
        Assemblers::KeyValueAssembler assembler;
        auto result = assembler.AssembleFromFullBuffer(buffer).get();
        ASSERT_TRUE(result);
        EXPECT_GT(result->pairs.size(), 0);
        LOGIFACE_LOG(info, "✓ Key-Value format loaded");
    }
    
    // Test Binary Fixed
    {
        const Fs::path bin_file = test_data_dir_ / "test_data.bin";
        auto buffer = std::make_shared<ByteBuffer>(LoadFileToBuffer(bin_file));
        Assemblers::BinaryFixedAssembler assembler;
        auto result = assembler.AssembleFromFullBuffer(buffer).get();
        ASSERT_TRUE(result);
        EXPECT_GT(result->records.size(), 0);
        LOGIFACE_LOG(info, "✓ Binary Fixed format loaded");
    }
    
    // Test Binary TLV
    {
        const Fs::path tlv_file = test_data_dir_ / "test_data.tlv";
        auto buffer = std::make_shared<ByteBuffer>(LoadFileToBuffer(tlv_file));
        Assemblers::BinaryTLVAssembler assembler;
        auto result = assembler.AssembleFromFullBuffer(buffer).get();
        ASSERT_TRUE(result);
        EXPECT_GT(result->records.size(), 0);
        LOGIFACE_LOG(info, "✓ Binary TLV format loaded");
    }
    
    LOGIFACE_LOG(info, "✓ All formats loaded successfully!");
}

TEST_F(FormatIntegrationTest, ExpectedResultsFileExists) {
    const Fs::path results_file = test_data_dir_ / "expected_results.json";
    EXPECT_TRUE(Fs::exists(results_file)) << "Expected results file not found";
    
    LOGIFACE_LOG(info, "Expected results file found");
    
    // Verify it has the expected structure
    EXPECT_TRUE(expected_results_.contains("keyvalue"));
    EXPECT_TRUE(expected_results_.contains("binary_fixed"));
    EXPECT_TRUE(expected_results_.contains("binary_tlv"));
    
    LOGIFACE_LOG(info, "✓ Expected results file has correct structure");
}

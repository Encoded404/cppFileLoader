#include <cstdint>
#include <cstring>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>

int main() {
    std::ifstream file("/home/aronhoy/Documents/GitHub/cpp/cppFileLoader/build/tests/test_data/test_data.tlv", 
                       std::ios::binary);
    
    if (!file) {
        std::cerr << "Cannot open file\n";
        return 1;
    }
    
    // Read entire file
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    file.close();
    
    std::cout << "Total file size: " << data.size() << " bytes\n\n";
    
    // Parse magic
    uint32_t magic = 0;
    memcpy(&magic, data.data(), 4);
    std::cout << "Magic: 0x" << std::hex << magic << std::dec << "\n";
    
    size_t offset = 4;
    int entry_num = 0;
    
    while (offset < data.size()) {
        uint8_t tag = data[offset];
        std::cout << "\nEntry " << entry_num++ << " at offset " << offset << ":\n";
        std::cout << "  Tag: 0x" << std::hex << (int)tag << std::dec;
        
        if (tag == 0xFF) {
            std::cout << " (EOF)\n";
            offset += 1;
            break;
        }
        
        offset += 1;
        
        if (offset + 2 > data.size()) {
            std::cout << "  ERROR: Not enough bytes for length!\n";
            break;
        }
        
        uint16_t length = 0;
        memcpy(&length, data.data() + offset, 2);
        // Convert from big-endian if needed
        length = ((length & 0xFF) << 8) | ((length >> 8) & 0xFF);
        
        std::cout << " Length: " << length << "\n";
        offset += 2;
        
        if (offset + length > data.size()) {
            std::cout << "  ERROR: Not enough bytes for value! Have " 
                      << (data.size() - offset) << ", need " << length << "\n";
            break;
        }
        
        if (tag == 0x01) {
            std::cout << "  Metadata: " << std::string(reinterpret_cast<const char*>(data.data() + offset), length) << "\n";
        } else if (tag == 0x02) {
            std::cout << "  DataRecord (16 bytes):\n";
            uint32_t id;
            uint64_t timestamp;
            uint32_t value;
            memcpy(&id, data.data() + offset, 4);
            memcpy(&timestamp, data.data() + offset + 4, 8);
            memcpy(&value, data.data() + offset + 12, 4);
            std::cout << "    ID: " << id << " TS: " << timestamp << " Value: " << value << "\n";
        }
        
        offset += length;
    }
    
    std::cout << "\nFinal offset: " << offset << " / " << data.size() << "\n";
    return 0;
}

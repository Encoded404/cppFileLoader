#!/usr/bin/env python3
"""
Generate test files for the FileLoader test suite.
Creates three file formats with random data and expected results in JSON.
"""

import json
import struct
import random
import string
import sys
import time
from pathlib import Path
from datetime import datetime


def generate_random_keyvalue_pairs(num_entries=10):
    """Generate random key-value pairs for plain text format"""
    pairs = []
    for i in range(num_entries):
        pairs.append({
            'key': f'key_{i:03d}',
            'value': f'value_{random.randint(0, 10000)}'
        })
    return pairs


def generate_random_binary_records(num_entries=15):
    """Generate random records for binary fixed format"""
    records = []
    timestamp_base = int(time.time() * 1000)  # Current time in milliseconds
    for i in range(num_entries):
        records.append({
            'id': i + 1,
            'timestamp': timestamp_base - random.randint(0, 10000),
            'value': random.randint(0, 100000)
        })
    return records


def generate_random_tlv_records(num_entries=12):
    """Generate random records for TLV format"""
    records = []
    timestamp_base = int(time.time() * 1000)
    for i in range(num_entries):
        records.append({
            'id': random.randint(1000, 10000),
            'timestamp': timestamp_base - random.randint(0, 10000),
            'value': random.randint(0, 50000)
        })
    return records


def write_plain_text_keyvalue_file(filepath, pairs):
    """Write plain text key=value format file (.kv)"""
    with open(filepath, 'w') as f:
        for pair in pairs:
            f.write(f"{pair['key']}={pair['value']}\n")


def write_binary_record_format(filepath, records):
    """Write binary fixed record format file
    
    Header: [MAGIC:4 bytes] [VERSION:2 bytes] [RECORD_COUNT:4 bytes] [RESERVED:2 bytes]
    Then: RECORD_COUNT records, each:
        [ID:4 bytes] [TIMESTAMP:8 bytes] [VALUE:4 bytes] [CHECKSUM:4 bytes]
    """
    MAGIC = 0xDEADBEEF
    VERSION = 1
    
    with open(filepath, 'wb') as f:
        # Write header
        f.write(struct.pack('<I', MAGIC))           # magic (4 bytes, little-endian)
        f.write(struct.pack('<H', VERSION))         # version (2 bytes)
        f.write(struct.pack('<I', len(records)))    # record_count (4 bytes)
        f.write(struct.pack('<H', 0))               # reserved (2 bytes)
        
        # Write records
        for record in records:
            record_id = record['id']
            timestamp = record['timestamp']
            value = record['value']
            # Checksum: simple sum of id + value
            checksum = (record_id + value) & 0xFFFFFFFF
            
            f.write(struct.pack('<I', record_id))      # ID (4 bytes, little-endian)
            f.write(struct.pack('<Q', timestamp))      # TIMESTAMP (8 bytes, little-endian)
            f.write(struct.pack('<i', value))           # VALUE (4 bytes, signed int)
            f.write(struct.pack('<I', checksum))        # CHECKSUM (4 bytes)


def write_tlv_format(filepath, records, metadata=None):
    """Write TLV (Type-Length-Value) format file
    
    TLV entries format:
        [TAG:1 byte] [LENGTH:2 bytes] [VALUE:LENGTH bytes]
    
    Tags:
        0x01: Metadata (string, value is JSON)
        0x02: DataRecord (16 bytes: 4-byte id, 8-byte timestamp, 4-byte value)
        0xFF: End of file marker
    """
    MAGIC = 0xDEADBEEF
    TAG_METADATA = 0x01
    TAG_DATA_RECORD = 0x02
    TAG_EOF = 0xFF
    
    with open(filepath, 'wb') as f:
        # Write magic number
        f.write(struct.pack('<I', MAGIC))
        
        # Write metadata entry if provided
        if metadata:
            metadata_str = json.dumps(metadata)
            metadata_bytes = metadata_str.encode('utf-8')
            f.write(struct.pack('B', TAG_METADATA))
            f.write(struct.pack('>H', len(metadata_bytes)))  # Big-endian length
            f.write(metadata_bytes)
        
        # Write data records
        for record in records:
            record_id = record['id']
            timestamp = record['timestamp']
            value = record['value']
            
            # DataRecord: 16 bytes total (4-byte id, 8-byte timestamp, 4-byte value)
            record_data = struct.pack('<I', record_id)        # ID (4 bytes)
            record_data += struct.pack('<Q', timestamp)       # TIMESTAMP (8 bytes)
            record_data += struct.pack('<i', value)            # VALUE (4 bytes)
            
            f.write(struct.pack('B', TAG_DATA_RECORD))
            f.write(struct.pack('>H', len(record_data)))      # Big-endian length
            f.write(record_data)
        
        # Final end-of-file marker
        f.write(struct.pack('B', TAG_EOF))


def write_json_expected_results(filepath, kv_pairs, binary_records, tlv_records):
    """Write expected results JSON file in the expected format"""
    timestamp_base = int(time.time() * 1000)
    
    data = {
        "keyvalue": {
            "type": "KeyValue",
            "file": "test_data.kv",
            "pairs": kv_pairs
        },
        "binary_fixed": {
            "type": "BinaryFixed",
            "file": "test_data.bin",
            "records": binary_records
        },
        "binary_tlv": {
            "type": "BinaryTLV",
            "file": "test_data.tlv",
            "metadata": {
                "version": "1.0",
                "created": datetime.now().isoformat(),
                "record_count": len(tlv_records)
            },
            "records": tlv_records
        }
    }
    
    with open(filepath, 'w') as f:
        json.dump(data, f, indent=2)


def main():
    # Get output directory from command line or use current directory
    if len(sys.argv) > 1:
        output_dir = Path(sys.argv[1])
    else:
        output_dir = Path.cwd()
    
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Generate test data with fixed seed for reproducibility
    random.seed(42)
    
    kv_pairs = generate_random_keyvalue_pairs(10)
    binary_records = generate_random_binary_records(15)
    tlv_records = generate_random_tlv_records(12)
    
    print(f"Generating test files in {output_dir}")
    
    # Write files
    kv_file = output_dir / 'test_data.kv'
    binary_file = output_dir / 'test_data.bin'
    tlv_file = output_dir / 'test_data.tlv'
    results_file = output_dir / 'expected_results.json'
    
    write_plain_text_keyvalue_file(str(kv_file), kv_pairs)
    print(f"✓ Generated {kv_file}")
    
    write_binary_record_format(str(binary_file), binary_records)
    print(f"✓ Generated {binary_file}")
    
    tlv_metadata = {
        "version": "1.0",
        "created": datetime.now().isoformat(),
        "record_count": len(tlv_records)
    }
    write_tlv_format(str(tlv_file), tlv_records, tlv_metadata)
    print(f"✓ Generated {tlv_file}")
    
    write_json_expected_results(str(results_file), kv_pairs, binary_records, tlv_records)
    print(f"✓ Generated {results_file}")
    
    print(f"Generated {len(kv_pairs)} key-value pairs, {len(binary_records)} binary records, {len(tlv_records)} TLV records")


if __name__ == '__main__':
    main()
    main()

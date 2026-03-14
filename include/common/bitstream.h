#ifndef LIBSWF_COMMON_BITSTREAM_H
#define LIBSWF_COMMON_BITSTREAM_H

#include "types.h"
#include <vector>
#include <cstring>
#include <stdexcept>

namespace libswf {

// Bit stream reader for SWF parsing (little-endian bit packing)
class BitStream {
private:
    const uint8* data_;
    size_t size_;
    size_t bytePos_;
    uint8 bitPos_;  // Current bit position within byte (0-7)
    
public:
    BitStream(const uint8* data, size_t size)
        : data_(data), size_(size), bytePos_(0), bitPos_(0) {}
    
    void reset() {
        bytePos_ = 0;
        bitPos_ = 0;
    }
    
    [[nodiscard]] size_t bytePos() const noexcept { return bytePos_; }
    [[nodiscard]] size_t bitPos() const noexcept { return bitPos_; }
    [[nodiscard]] size_t remainingBytes() const noexcept { 
        return (bytePos_ < size_) ? (size_ - bytePos_) : 0; 
    }
    
    // Fast aligned byte read - use when bits are aligned
    [[nodiscard]] uint8 readByte() {
        if (bytePos_ >= size_) {
            throw std::out_of_range("BitStream read past end");
        }
        return data_[bytePos_++];
    }
    
    // Read multiple bytes at once (faster than individual reads)
    void readBytes(uint8* dest, size_t count) {
        if (bytePos_ + count > size_) {
            throw std::out_of_range("BitStream read past end");
        }
        std::memcpy(dest, data_ + bytePos_, count);
        bytePos_ += count;
    }
    
    // Read little-endian uint16
    [[nodiscard]] uint16 readLE16() {
        if (bytePos_ + 2 > size_) {
            throw std::out_of_range("BitStream read past end");
        }
        uint16 result = data_[bytePos_] | (data_[bytePos_ + 1] << 8);
        bytePos_ += 2;
        return result;
    }
    
    // Read little-endian uint32
    [[nodiscard]] uint32 readLE32() {
        if (bytePos_ + 4 > size_) {
            throw std::out_of_range("BitStream read past end");
        }
        uint32 result = data_[bytePos_] 
                      | (data_[bytePos_ + 1] << 8)
                      | (data_[bytePos_ + 2] << 16)
                      | (data_[bytePos_ + 3] << 24);
        bytePos_ += 4;
        return result;
    }
    
    // Read n bits as unsigned
    [[nodiscard]] uint32 readUB(int n) {
        if (n <= 0 || n > 32) {
            throw std::invalid_argument("Invalid bit count");
        }
        
        // Fast path: byte-aligned read of 8, 16, or 32 bits
        if (bitPos_ == 0) {
            if (n == 8) return readByte();
            if (n == 16) return readLE16();
            if (n == 32) return readLE32();
        }
        
        uint32 result = 0;
        int bitsRead = 0;
        
        while (bitsRead < n) {
            if (bytePos_ >= size_) {
                throw std::out_of_range("BitStream read past end");
            }
            
            int bitsAvailable = 8 - bitPos_;
            int bitsNeeded = n - bitsRead;
            int bitsToRead = (bitsAvailable < bitsNeeded) ? bitsAvailable : bitsNeeded;
            
            uint8 mask = static_cast<uint8>((1u << bitsToRead) - 1);
            uint8 value = (data_[bytePos_] >> (bitsAvailable - bitsToRead)) & mask;
            
            result = (result << bitsToRead) | value;
            bitsRead += bitsToRead;
            
            bitPos_ += static_cast<uint8>(bitsToRead);
            if (bitPos_ >= 8) {
                bitPos_ = 0;
                bytePos_++;
            }
        }
        
        return result;
    }
    
    // Read n bits as signed
    int32 readSB(int n) {
        uint32 value = readUB(n);
        
        // Sign extend
        if (value & (1 << (n - 1))) {
            value |= ~((1 << n) - 1);
        }
        
        return static_cast<int32>(value);
    }
    
    // Read n bits as signed fixed-point
    float32 readFB(int n) {
        int32 value = readSB(n);
        return static_cast<float32>(value) / 65536.0f;
    }
    
    // Align to byte boundary
    void alignByte() {
        if (bitPos_ != 0) {
            bitPos_ = 0;
            bytePos_++;
        }
    }
    
    // Check if more bits available
    bool hasMoreBits() const {
        return bytePos_ < size_ || (bytePos_ == size_ && bitPos_ == 0);
    }
};

// BitStreamWriter for writing bits to a byte stream
class BitStreamWriter {
public:
    BitStreamWriter() : buffer_(1, 0), bytePos_(0), bitPos_(0) {}
    
    // Write n bits (unsigned)
    void writeUB(uint32 value, int n) {
        if (n <= 0 || n > 32) {
            throw std::runtime_error("Invalid bit count");
        }
        for (int i = n - 1; i >= 0; --i) {
            uint8 bit = (value >> i) & 1;
            buffer_[bytePos_] |= (bit << (7 - bitPos_));
            
            bitPos_++;
            if (bitPos_ >= 8) {
                bitPos_ = 0;
                bytePos_++;
                buffer_.push_back(0);
            }
        }
    }
    
    // Write n bits (signed) - two's complement
    void writeSB(int32 value, int n) {
        if (value < 0) {
            // Convert to two's complement
            uint32 uvalue = (1u << n) + value;
            writeUB(uvalue, n);
        } else {
            writeUB(static_cast<uint32>(value), n);
        }
    }
    
    // Align to byte boundary
    void alignByte() {
        if (bitPos_ != 0) {
            bitPos_ = 0;
            bytePos_++;
            buffer_.push_back(0);
        }
    }
    
    // Get the written data
    std::vector<uint8> getData() const {
        if (bitPos_ == 0) {
            // Already aligned, return all bytes except the extra one added by constructor
            return std::vector<uint8>(buffer_.begin(), buffer_.begin() + bytePos_);
        }
        // Not aligned, return current buffer including partial byte
        return std::vector<uint8>(buffer_.begin(), buffer_.begin() + bytePos_ + 1);
    }
    
    // Get current byte count
    size_t size() const {
        return bytePos_ + (bitPos_ > 0 ? 1 : 0);
    }

private:
    std::vector<uint8> buffer_;
    size_t bytePos_;
    int bitPos_;
};

} // namespace libswf

#endif // LIBSWF_COMMON_BITSTREAM_H

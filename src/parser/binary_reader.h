#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace mde {

// --- Big-endian decode helpers -------------------------------------------
//
// ITCH 5.0 multi-byte integer fields are big-endian on the wire. This
// machine (x64) is little-endian, so every one of these performs a real
// byte-order swap by assembling the value byte-by-byte -- never a raw
// memcpy/reinterpret_cast, which on this host would silently produce the
// wrong value (and would be undefined behavior for misaligned reads
// besides).

inline uint16_t decode_be_u16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline uint32_t decode_be_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

// ITCH timestamps are 48-bit (6-byte) big-endian; widen to uint64_t.
inline uint64_t decode_be_u48(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 40) |
           (static_cast<uint64_t>(p[1]) << 32) |
           (static_cast<uint64_t>(p[2]) << 24) |
           (static_cast<uint64_t>(p[3]) << 16) |
           (static_cast<uint64_t>(p[4]) << 8) |
           static_cast<uint64_t>(p[5]);
}

inline uint64_t decode_be_u64(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) |
           static_cast<uint64_t>(p[7]);
}

// Thrown when a read (or an upfront frame-length check) would go past the
// end of the underlying buffer. Covers both a message frame that claims
// more bytes than the file actually contains, and a frame whose declared
// length is too small to hold the fields its message type requires.
class BufferUnderrunError : public std::runtime_error {
public:
    explicit BufferUnderrunError(const std::string& what) : std::runtime_error(what) {}
};

// Sequential, bounds-checked cursor over a byte buffer. The ITCH parser
// scopes one of these to exactly one message frame's declared length, so a
// malformed or truncated frame throws instead of reading out of bounds or
// spilling into the next frame's bytes.
class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size) : data_(data), size_(size), pos_(0) {}

    size_t position() const { return pos_; }
    size_t remaining() const { return size_ - pos_; }

    uint8_t read_u8() {
        require(1);
        return data_[pos_++];
    }

    uint16_t read_be_u16() {
        require(2);
        uint16_t value = decode_be_u16(data_ + pos_);
        pos_ += 2;
        return value;
    }

    uint32_t read_be_u32() {
        require(4);
        uint32_t value = decode_be_u32(data_ + pos_);
        pos_ += 4;
        return value;
    }

    uint64_t read_be_u48() {
        require(6);
        uint64_t value = decode_be_u48(data_ + pos_);
        pos_ += 6;
        return value;
    }

    uint64_t read_be_u64() {
        require(8);
        uint64_t value = decode_be_u64(data_ + pos_);
        pos_ += 8;
        return value;
    }

    // Copies `count` raw bytes verbatim (no byte-swap) -- used for
    // fixed-width text fields like the space-padded ticker symbol.
    void read_bytes(char* dest, size_t count) {
        require(count);
        std::memcpy(dest, data_ + pos_, count);
        pos_ += count;
    }

    void skip(size_t count) {
        require(count);
        pos_ += count;
    }

private:
    void require(size_t count) const {
        if (count > size_ - pos_) {
            throw BufferUnderrunError(
                "BinaryReader: requested " + std::to_string(count) + " bytes at offset " +
                std::to_string(pos_) + " but only " + std::to_string(size_ - pos_) + " remain");
        }
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_;
};

} // namespace mde

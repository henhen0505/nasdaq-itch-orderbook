#include "parser/binary_reader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using mde::BinaryReader;
using mde::BufferUnderrunError;

TEST(BinaryReaderTest, ReadU8KnownValue) {
    std::vector<uint8_t> data = {0x42};
    BinaryReader reader(data.data(), data.size());
    EXPECT_EQ(reader.read_u8(), 0x42);
}

TEST(BinaryReaderTest, ReadBeU16KnownValue) {
    // 0x1234 big-endian on the wire -> bytes [0x12, 0x34]
    std::vector<uint8_t> data = {0x12, 0x34};
    BinaryReader reader(data.data(), data.size());
    EXPECT_EQ(reader.read_be_u16(), 0x1234u);
}

TEST(BinaryReaderTest, ReadBeU32KnownValue) {
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    BinaryReader reader(data.data(), data.size());
    EXPECT_EQ(reader.read_be_u32(), 0xDEADBEEFu);
}

TEST(BinaryReaderTest, ReadBeU48KnownValue) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    BinaryReader reader(data.data(), data.size());
    EXPECT_EQ(reader.read_be_u48(), 0x010203040506ull);
}

TEST(BinaryReaderTest, ReadBeU64KnownValue) {
    std::vector<uint8_t> data = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    BinaryReader reader(data.data(), data.size());
    EXPECT_EQ(reader.read_be_u64(), 0x0123456789ABCDEFull);
}

// Explicit big-endian-vs-host-endian test. This machine is little-endian
// x64: a naive reinterpret_cast<const uint16_t*>(&bytes) read of
// {0x12, 0x34} would yield 0x3412, not 0x1234. Assert the decoded value is
// the big-endian interpretation, not what a raw host-native read would give.
TEST(BinaryReaderTest, BigEndianNotHostEndian) {
    std::vector<uint8_t> data = {0x12, 0x34};
    BinaryReader reader(data.data(), data.size());
    uint16_t value = reader.read_be_u16();
    EXPECT_EQ(value, 0x1234u);
    EXPECT_NE(value, 0x3412u);
}

TEST(BinaryReaderTest, SequentialReadsAdvancePositionAndExhaustBuffer) {
    std::vector<uint8_t> data = {0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD};
    BinaryReader reader(data.data(), data.size());
    EXPECT_EQ(reader.read_be_u16(), 0x0001u);
    EXPECT_EQ(reader.position(), 2u);
    EXPECT_EQ(reader.read_be_u32(), 0xAABBCCDDu);
    EXPECT_EQ(reader.remaining(), 0u);
}

TEST(BinaryReaderTest, ReadBytesCopiesRawWithoutByteSwap) {
    std::vector<uint8_t> data = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    BinaryReader reader(data.data(), data.size());
    char buf[8] = {};
    reader.read_bytes(buf, 8);
    EXPECT_EQ(std::string(buf, 8), "AAPL    ");
}

TEST(BinaryReaderTest, SkipAdvancesPosition) {
    std::vector<uint8_t> data = {0, 0, 0, 0, 0xFF};
    BinaryReader reader(data.data(), data.size());
    reader.skip(4);
    EXPECT_EQ(reader.read_u8(), 0xFF);
}

TEST(BinaryReaderTest, UnderrunOnU16Throws) {
    std::vector<uint8_t> data = {0x01};
    BinaryReader reader(data.data(), data.size());
    EXPECT_THROW(reader.read_be_u16(), BufferUnderrunError);
}

TEST(BinaryReaderTest, UnderrunOnU48Throws) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    BinaryReader reader(data.data(), data.size());
    EXPECT_THROW(reader.read_be_u48(), BufferUnderrunError);
}

TEST(BinaryReaderTest, UnderrunOnU64Throws) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    BinaryReader reader(data.data(), data.size());
    EXPECT_THROW(reader.read_be_u64(), BufferUnderrunError);
}

TEST(BinaryReaderTest, UnderrunOnReadBytesThrows) {
    std::vector<uint8_t> data = {'A', 'B'};
    BinaryReader reader(data.data(), data.size());
    char buf[8] = {};
    EXPECT_THROW(reader.read_bytes(buf, 8), BufferUnderrunError);
}

TEST(BinaryReaderTest, EmptyBufferReadThrowsNotCrashes) {
    std::vector<uint8_t> data;
    BinaryReader reader(data.data(), data.size());
    EXPECT_EQ(reader.remaining(), 0u);
    EXPECT_THROW(reader.read_u8(), BufferUnderrunError);
}

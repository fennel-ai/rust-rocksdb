//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "rocksdb/slice.h"

#include <gtest/gtest.h>

#include "port/port.h"
#include "port/stack_trace.h"
#include "rocksdb/data_structure.h"
#include "rocksdb/types.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"

namespace ROCKSDB_NAMESPACE {

TEST(SliceTest, StringView) {
  std::string s = "foo";
  std::string_view sv = s;
  ASSERT_EQ(Slice(s), Slice(sv));
  ASSERT_EQ(Slice(s), Slice(std::move(sv)));
}

std::string base64_decode(const std::string& encoded) {
    static const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string decoded;
    int val = 0, valb = -8;
    
    for (unsigned char c : encoded) {
        if (c == '=') break;
        
        size_t pos = base64_chars.find(c);
        if (pos == std::string::npos) continue;
        
        val = (val << 6) + pos;
        valb += 6;
        
        if (valb >= 0) {
            decoded.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    
    return decoded;
}

// Write a varint to the buffer for testing purposes
void write_varint(char*& p, uint32_t value) {
    while (value >= 0x80) { // While there are more than 7 bits
        *p++ = static_cast<char>((value & 0x7F) | 0x80); // Set the MSB to 1 to indicate more bytes
        value >>= 7; // Shift the value by 7 bits to process the next 7 bits
    }
    *p++ = static_cast<char>(value & 0x7F); // Last byte with MSB set to 0
}


TEST(SliceTest, MagicBytes) {
  // Basic test with magic bytes
  std::string test_key = "\ttest:kfam\x01\x02\x03\x04\x05\x06\x07\x08\x01c\0\0\0\0\0\0\0\0";
  Slice s(test_key);
  ASSERT_TRUE(s.has_magic_bytes(test_key.data(), test_key.size()));

  // Test with base64 encoded key containing magic bytes
  std::string base64_encoded = "DXRlc3Q6a2ZhbXRlc3QBAgMEBQYHCAFjAAAAAAAAAAJyljxiihpLPIOeS6vfEiep";
  std::string decoded_key = base64_decode(base64_encoded);
  Slice test_slice(decoded_key);
  ASSERT_TRUE(test_slice.has_magic_bytes(decoded_key.data(), decoded_key.size()));

  // Test extract_key_without_magic_bytes
  std::string buffer;
  buffer.resize(decoded_key.size());
  Slice extracted_slice = test_slice.extract_key_without_magic_bytes(buffer);
  // 8 bytes for magic bytes, 16 bytes for varint, 8 bytes for key length
  ASSERT_EQ(buffer.size(), decoded_key.size() - MAGIC_BYTES_LENGTH - 16 - 8);

  // Test with another base64 encoded key
  std::string base64_encoded2 = "DXRlc3Q6a2ZhbXRlc3QBAgMEBQYHCAFjAAAAAAAAAAA=";
  std::string decoded_key2 = base64_decode(base64_encoded2);
  Slice test_slice_2(decoded_key2);
  ASSERT_TRUE(test_slice_2.has_magic_bytes(decoded_key2.data(), decoded_key2.size()));

  std::string buffer_2;
  buffer_2.resize(decoded_key2.size());
  Slice extracted_slice_2 = test_slice_2.extract_key_without_magic_bytes(buffer_2);
  // 8 bytes for magic bytes, 8  bytes for ts as i64 
  ASSERT_EQ(buffer_2.size(), decoded_key2.size() - MAGIC_BYTES_LENGTH - 8);

  // Negative test - incorrect magic bytes
  std::string negative_test_key("\ttest:kfam\x01\x02\x04\x04\x05\x06\x07\x08\x01c\0\0\0\0\0\0\0\0", 28);
  Slice negative_test_slice(negative_test_key);
  ASSERT_FALSE(negative_test_slice.has_magic_bytes(negative_test_key.data(), negative_test_key.size()));

  // Test with different prefix
  std::string another_test_key("\rtest:kfamtest\x01\x02\x03\x04\x05\x06\x07\x08\x01b\0\0\0\0\0\0\0\x03", 32);
  Slice another_test_slice(another_test_key);
  ASSERT_TRUE(another_test_slice.has_magic_bytes(another_test_key.data(), another_test_key.size()));
}

TEST(SliceTest, VarintEncodingDecoding) {
  Slice s = Slice("");
  // Test varint encoding/decoding with random values
  for (int i = 0; i < 100; i++) {
    uint32_t value = rand();
    std::string buffer;
    buffer.resize(5); // Maximum 5 bytes for varint32
    char* ptr = &buffer[0];
    write_varint(ptr, value);
    const char* read_ptr = buffer.data();
    auto [read_value, bytes_read] = s.read_varint(read_ptr, buffer.data() + buffer.size());
    ASSERT_EQ(value, read_value);
  }

  // Test specific edge cases
  std::vector<uint32_t> test_cases = {
    0,                    // Minimum value
    127,                  // Max 1-byte value
    128,                  // Min 2-byte value
    16383,               // Max 2-byte value
    16384,               // Min 3-byte value
    2097151,             // Max 3-byte value
    2097152,             // Min 4-byte value
    268435455,           // Max 4-byte value
    268435456,           // Min 5-byte value
    std::numeric_limits<uint32_t>::max()  // Maximum value
  };

  for (uint32_t value : test_cases) {
    std::string buffer;
    buffer.resize(5);
    char* ptr = &buffer[0];
    write_varint(ptr, value);
    const char* read_ptr = buffer.data();
    auto [read_value, bytes_read] = s.read_varint(read_ptr, buffer.data() + buffer.size());
    ASSERT_EQ(value, read_value);
  }
}

// Use this to keep track of the cleanups that were actually performed
void Multiplier(void* arg1, void* arg2) {
  int* res = static_cast<int*>(arg1);
  int* num = static_cast<int*>(arg2);
  *res *= *num;
}

class PinnableSliceTest : public testing::Test {
 public:
  void AssertSameData(const std::string& expected, const PinnableSlice& slice) {
    std::string got;
    got.assign(slice.data(), slice.size());
    ASSERT_EQ(expected, got);
  }
};

// Test that the external buffer is moved instead of being copied.
TEST_F(PinnableSliceTest, MoveExternalBuffer) {
  Slice s("123");
  std::string buf;
  PinnableSlice v1(&buf);
  v1.PinSelf(s);

  PinnableSlice v2(std::move(v1));
  ASSERT_EQ(buf.data(), v2.data());
  ASSERT_EQ(&buf, v2.GetSelf());

  PinnableSlice v3;
  v3 = std::move(v2);
  ASSERT_EQ(buf.data(), v3.data());
  ASSERT_EQ(&buf, v3.GetSelf());
}

TEST_F(PinnableSliceTest, Move) {
  int n2 = 2;
  int res = 1;
  const std::string const_str1 = "123";
  const std::string const_str2 = "ABC";
  Slice slice1(const_str1);
  Slice slice2(const_str2);

  {
    // Test move constructor on a pinned slice.
    res = 1;
    PinnableSlice v1;
    v1.PinSlice(slice1, Multiplier, &res, &n2);
    PinnableSlice v2(std::move(v1));

    // Since v1's Cleanable has been moved to v2,
    // no cleanup should happen in Reset.
    v1.Reset();
    ASSERT_EQ(1, res);

    AssertSameData(const_str1, v2);
  }
  // v2 is cleaned up.
  ASSERT_EQ(2, res);

  {
    // Test move constructor on an unpinned slice.
    PinnableSlice v1;
    v1.PinSelf(slice1);
    PinnableSlice v2(std::move(v1));

    AssertSameData(const_str1, v2);
  }

  {
    // Test move assignment from a pinned slice to
    // another pinned slice.
    res = 1;
    PinnableSlice v1;
    v1.PinSlice(slice1, Multiplier, &res, &n2);
    PinnableSlice v2;
    v2.PinSlice(slice2, Multiplier, &res, &n2);
    v2 = std::move(v1);

    // v2's Cleanable will be Reset before moving
    // anything from v1.
    ASSERT_EQ(2, res);
    // Since v1's Cleanable has been moved to v2,
    // no cleanup should happen in Reset.
    v1.Reset();
    ASSERT_EQ(2, res);

    AssertSameData(const_str1, v2);
  }
  // The Cleanable moved from v1 to v2 will be Reset.
  ASSERT_EQ(4, res);

  {
    // Test move assignment from a pinned slice to
    // an unpinned slice.
    res = 1;
    PinnableSlice v1;
    v1.PinSlice(slice1, Multiplier, &res, &n2);
    PinnableSlice v2;
    v2.PinSelf(slice2);
    v2 = std::move(v1);

    // Since v1's Cleanable has been moved to v2,
    // no cleanup should happen in Reset.
    v1.Reset();
    ASSERT_EQ(1, res);

    AssertSameData(const_str1, v2);
  }
  // The Cleanable moved from v1 to v2 will be Reset.
  ASSERT_EQ(2, res);

  {
    // Test move assignment from an upinned slice to
    // another unpinned slice.
    PinnableSlice v1;
    v1.PinSelf(slice1);
    PinnableSlice v2;
    v2.PinSelf(slice2);
    v2 = std::move(v1);

    AssertSameData(const_str1, v2);
  }

  {
    // Test move assignment from an upinned slice to
    // a pinned slice.
    res = 1;
    PinnableSlice v1;
    v1.PinSelf(slice1);
    PinnableSlice v2;
    v2.PinSlice(slice2, Multiplier, &res, &n2);
    v2 = std::move(v1);

    // v2's Cleanable will be Reset before moving
    // anything from v1.
    ASSERT_EQ(2, res);

    AssertSameData(const_str1, v2);
  }
  // No Cleanable is moved from v1 to v2, so no more cleanup.
  ASSERT_EQ(2, res);
}

// ***************************************************************** //
// Unit test for SmallEnumSet
class SmallEnumSetTest : public testing::Test {
 public:
  SmallEnumSetTest() {}
  ~SmallEnumSetTest() {}
};

TEST_F(SmallEnumSetTest, SmallEnumSetTest1) {
  FileTypeSet fs;  // based on a legacy enum type
  ASSERT_TRUE(fs.empty());
  ASSERT_TRUE(fs.Add(FileType::kIdentityFile));
  ASSERT_FALSE(fs.empty());
  ASSERT_FALSE(fs.Add(FileType::kIdentityFile));
  ASSERT_TRUE(fs.Add(FileType::kInfoLogFile));
  ASSERT_TRUE(fs.Contains(FileType::kIdentityFile));
  ASSERT_FALSE(fs.Contains(FileType::kDBLockFile));
  ASSERT_FALSE(fs.empty());
  ASSERT_FALSE(fs.Remove(FileType::kDBLockFile));
  ASSERT_TRUE(fs.Remove(FileType::kIdentityFile));
  ASSERT_FALSE(fs.empty());
  ASSERT_TRUE(fs.Remove(FileType::kInfoLogFile));
  ASSERT_TRUE(fs.empty());
}

namespace {
enum class MyEnumClass { A, B, C };
}  // namespace

using MyEnumClassSet = SmallEnumSet<MyEnumClass, MyEnumClass::C>;

TEST_F(SmallEnumSetTest, SmallEnumSetTest2) {
  MyEnumClassSet s;  // based on an enum class type
  ASSERT_TRUE(s.Add(MyEnumClass::A));
  ASSERT_TRUE(s.Contains(MyEnumClass::A));
  ASSERT_FALSE(s.Contains(MyEnumClass::B));
  ASSERT_TRUE(s.With(MyEnumClass::B).Contains(MyEnumClass::B));
  ASSERT_TRUE(s.With(MyEnumClass::A).Contains(MyEnumClass::A));
  ASSERT_FALSE(s.Contains(MyEnumClass::B));
  ASSERT_FALSE(s.Without(MyEnumClass::A).Contains(MyEnumClass::A));
  ASSERT_FALSE(
      s.With(MyEnumClass::B).Without(MyEnumClass::B).Contains(MyEnumClass::B));
  ASSERT_TRUE(
      s.Without(MyEnumClass::B).With(MyEnumClass::B).Contains(MyEnumClass::B));
  ASSERT_TRUE(s.Contains(MyEnumClass::A));

  const MyEnumClassSet cs = s;
  ASSERT_TRUE(cs.Contains(MyEnumClass::A));
  ASSERT_EQ(cs, MyEnumClassSet{MyEnumClass::A});
  ASSERT_EQ(cs.Without(MyEnumClass::A), MyEnumClassSet{});
  ASSERT_EQ(cs, MyEnumClassSet::All().Without(MyEnumClass::B, MyEnumClass::C));
  ASSERT_EQ(cs.With(MyEnumClass::B, MyEnumClass::C), MyEnumClassSet::All());
  ASSERT_EQ(
      MyEnumClassSet::All(),
      MyEnumClassSet{}.With(MyEnumClass::A, MyEnumClass::B, MyEnumClass::C));
  ASSERT_NE(cs, MyEnumClassSet{MyEnumClass::B});
  ASSERT_NE(cs, MyEnumClassSet::All());

  int count = 0;
  for (MyEnumClass e : cs) {
    ASSERT_EQ(e, MyEnumClass::A);
    ++count;
  }
  ASSERT_EQ(count, 1);

  count = 0;
  for (MyEnumClass e : MyEnumClassSet::All().Without(MyEnumClass::B)) {
    ASSERT_NE(e, MyEnumClass::B);
    ++count;
  }
  ASSERT_EQ(count, 2);

  for (MyEnumClass e : MyEnumClassSet{}) {
    (void)e;
    assert(false);
  }
}

// ***************************************************************** //
// Unit test for Status
TEST(StatusTest, Update) {
  const Status ok = Status::OK();
  const Status inc = Status::Incomplete("blah");
  const Status notf = Status::NotFound("meow");

  Status s = ok;
  ASSERT_TRUE(s.UpdateIfOk(Status::Corruption("bad")).IsCorruption());
  ASSERT_TRUE(s.IsCorruption());

  s = ok;
  ASSERT_TRUE(s.UpdateIfOk(Status::OK()).ok());
  ASSERT_TRUE(s.UpdateIfOk(ok).ok());
  ASSERT_TRUE(s.ok());

  ASSERT_TRUE(s.UpdateIfOk(inc).IsIncomplete());
  ASSERT_TRUE(s.IsIncomplete());

  ASSERT_TRUE(s.UpdateIfOk(notf).IsIncomplete());
  ASSERT_TRUE(s.UpdateIfOk(ok).IsIncomplete());
  ASSERT_TRUE(s.IsIncomplete());

  // Keeps left-most non-OK status
  s = ok;
  ASSERT_TRUE(
      s.UpdateIfOk(Status()).UpdateIfOk(notf).UpdateIfOk(inc).IsNotFound());
  ASSERT_TRUE(s.IsNotFound());
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

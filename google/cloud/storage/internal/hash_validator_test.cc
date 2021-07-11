// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/storage/internal/hash_validator.h"
#include "google/cloud/storage/internal/hash_validator_impl.h"
#include "google/cloud/storage/internal/object_metadata_parser.h"
#include "google/cloud/storage/internal/object_requests.h"
#include "google/cloud/storage/object_metadata.h"
#include "google/cloud/status.h"
#include "absl/memory/memory.h"
#include <gmock/gmock.h>

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace internal {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

// These values were obtained using:
// echo -n '' > foo.txt && gsutil hash foo.txt
auto constexpr kEmptyStringCrc32cChecksum = "AAAAAA==";
auto constexpr kEmptyStringMD5Hash = "1B2M2Y8AsgTpgAmY7PhCfg==";

// /bin/echo -n 'The quick brown fox jumps over the lazy dog' > foo.txt
// gsutil hash foo.txt
auto constexpr kQuickFoxCrc32cChecksum = "ImIEBA==";
auto constexpr kQuickFoxMD5Hash = "nhB9nTcrtoJr2B01QqQZ1g==";

void UpdateValidator(HashValidator& validator, std::string const& buffer) {
  validator.Update(buffer.data(), buffer.size());
}

using HashValue = HashValidator::HashValues::value_type;

TEST(NullHashValidatorTest, Simple) {
  NullHashValidator validator;
  validator.ProcessHeader("x-goog-hash", "md5=<placeholder-for-test>");
  UpdateValidator(validator, "The quick");
  UpdateValidator(validator, " brown");
  UpdateValidator(validator, " fox jumps over the lazy dog");
  auto result = std::move(validator).Finish();
  EXPECT_TRUE(result.computed.empty());
  EXPECT_TRUE(result.received.empty());
}

TEST(MD5HashValidator, Empty) {
  MD5HashValidator validator;
  validator.ProcessHeader("x-goog-hash",
                          "md5=" + std::string{kEmptyStringMD5Hash});
  auto result = std::move(validator).Finish();
  EXPECT_EQ(result.computed, result.received);
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"md5", kEmptyStringMD5Hash}));
  EXPECT_FALSE(result.is_mismatch);
}

TEST(MD5HashValidator, Simple) {
  MD5HashValidator validator;
  UpdateValidator(validator, "The quick");
  UpdateValidator(validator, " brown");
  UpdateValidator(validator, " fox jumps over the lazy dog");
  validator.ProcessHeader("x-goog-hash", "md5=<invalid-value-for-test>");
  auto result = std::move(validator).Finish();
  EXPECT_THAT(result.received,
              ElementsAre(HashValue{"md5", "<invalid-value-for-test>"}));
  EXPECT_THAT(result.computed, ElementsAre(HashValue{"md5", kQuickFoxMD5Hash}));
  EXPECT_TRUE(result.is_mismatch);
}

TEST(MD5HashValidator, MultipleHashesMd5AtEnd) {
  MD5HashValidator validator;
  UpdateValidator(validator, "The quick");
  UpdateValidator(validator, " brown");
  UpdateValidator(validator, " fox jumps over the lazy dog");
  validator.ProcessHeader(
      "x-goog-hash", "crc32c=<should-be-ignored>,md5=<invalid-value-for-test>");
  auto result = std::move(validator).Finish();
  EXPECT_THAT(result.received,
              ElementsAre(HashValue{"md5", "<invalid-value-for-test>"}));
  EXPECT_THAT(result.computed, ElementsAre(HashValue{"md5", kQuickFoxMD5Hash}));
  EXPECT_TRUE(result.is_mismatch);
}

TEST(MD5HashValidator, MultipleHashes) {
  MD5HashValidator validator;
  UpdateValidator(validator, "The quick");
  UpdateValidator(validator, " brown");
  UpdateValidator(validator, " fox jumps over the lazy dog");
  validator.ProcessHeader(
      "x-goog-hash", "md5=<invalid-value-for-test>,crc32c=<should-be-ignored>");
  auto result = std::move(validator).Finish();
  EXPECT_THAT(result.received,
              ElementsAre(HashValue{"md5", "<invalid-value-for-test>"}));
  EXPECT_THAT(result.computed, ElementsAre(HashValue{"md5", kQuickFoxMD5Hash}));
  EXPECT_TRUE(result.is_mismatch);
}

TEST(Crc32cHashValidator, Empty) {
  Crc32cHashValidator validator;
  validator.ProcessHeader("x-goog-hash",
                          "crc32c=" + std::string{kEmptyStringCrc32cChecksum});
  validator.ProcessHeader("x-goog-hash", "md5=<invalid-should-be-ignored>");
  auto result = std::move(validator).Finish();
  EXPECT_EQ(result.computed, result.received);
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kEmptyStringCrc32cChecksum}));
  EXPECT_FALSE(result.is_mismatch);
}

TEST(Crc32cHashValidator, Simple) {
  Crc32cHashValidator validator;
  UpdateValidator(validator, "The quick");
  UpdateValidator(validator, " brown");
  UpdateValidator(validator, " fox jumps over the lazy dog");
  validator.ProcessHeader("x-goog-hash", "crc32c=<invalid-value-for-test>");
  auto result = std::move(validator).Finish();
  EXPECT_THAT(result.received,
              ElementsAre(HashValue{"crc32c", "<invalid-value-for-test>"}));
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
  EXPECT_TRUE(result.is_mismatch);
}

TEST(Crc32cHashValidator, MultipleHashesCrc32cAtEnd) {
  Crc32cHashValidator validator;
  UpdateValidator(validator, "The quick");
  UpdateValidator(validator, " brown");
  UpdateValidator(validator, " fox jumps over the lazy dog");
  validator.ProcessHeader("x-goog-hash",
                          "md5=<ignored>,crc32c=<invalid-value-for-test>");
  auto result = std::move(validator).Finish();
  EXPECT_THAT(result.received,
              ElementsAre(HashValue{"crc32c", "<invalid-value-for-test>"}));
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
  EXPECT_TRUE(result.is_mismatch);
}

TEST(Crc32cHashValidator, MultipleHashes) {
  Crc32cHashValidator validator;
  UpdateValidator(validator, "The quick");
  UpdateValidator(validator, " brown");
  UpdateValidator(validator, " fox jumps over the lazy dog");
  validator.ProcessHeader("x-goog-hash",
                          "crc32c=<invalid-value-for-test>,md5=<ignored>");
  auto result = std::move(validator).Finish();
  EXPECT_THAT(result.received,
              ElementsAre(HashValue{"crc32c", "<invalid-value-for-test>"}));
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
  EXPECT_TRUE(result.is_mismatch);
}

TEST(CompositeHashValidator, Empty) {
  CompositeValidator validator(absl::make_unique<Crc32cHashValidator>(),
                               absl::make_unique<MD5HashValidator>());
  validator.ProcessHeader("x-goog-hash",
                          "crc32c=" + std::string{kEmptyStringCrc32cChecksum});
  validator.ProcessHeader("x-goog-hash",
                          "md5=" + std::string{kEmptyStringMD5Hash});
  auto result = std::move(validator).Finish();
  EXPECT_EQ(result.computed, result.received);
  EXPECT_THAT(
      result.computed,
      UnorderedElementsAre(HashValue{"md5", kEmptyStringMD5Hash},
                           HashValue{"crc32c", kEmptyStringCrc32cChecksum}));
  EXPECT_FALSE(result.is_mismatch);
}

TEST(CompositeHashValidator, Simple) {
  CompositeValidator validator(absl::make_unique<Crc32cHashValidator>(),
                               absl::make_unique<MD5HashValidator>());
  UpdateValidator(validator, "The quick");
  UpdateValidator(validator, " brown");
  UpdateValidator(validator, " fox jumps over the lazy dog");
  validator.ProcessHeader("x-goog-hash", "crc32c=<invalid-crc32c-for-test>");
  validator.ProcessHeader("x-goog-hash", "md5=<invalid-md5-for-test>");
  auto result = std::move(validator).Finish();
  EXPECT_THAT(
      result.received,
      UnorderedElementsAre(HashValue{"md5", "<invalid-md5-for-test>"},
                           HashValue{"crc32c", "<invalid-crc32c-for-test>"}));
  EXPECT_THAT(
      result.computed,
      UnorderedElementsAre(HashValue{"md5", kQuickFoxMD5Hash},
                           HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
  EXPECT_TRUE(result.is_mismatch);
}

TEST(CompositeHashValidator, ProcessMetadata) {
  CompositeValidator validator(absl::make_unique<Crc32cHashValidator>(),
                               absl::make_unique<MD5HashValidator>());
  UpdateValidator(validator, "The quick");
  UpdateValidator(validator, " brown");
  UpdateValidator(validator, " fox jumps over the lazy dog");
  auto object_metadata = internal::ObjectMetadataParser::FromJson(
                             nlohmann::json{
                                 {"crc32c", kQuickFoxCrc32cChecksum},
                                 {"md5Hash", kQuickFoxMD5Hash},
                             })
                             .value();
  validator.ProcessMetadata(object_metadata);
  auto result = std::move(validator).Finish();
  EXPECT_THAT(
      result.computed,
      UnorderedElementsAre(HashValue{"md5", kQuickFoxMD5Hash},
                           HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
  EXPECT_THAT(
      result.received,
      UnorderedElementsAre(HashValue{"md5", kQuickFoxMD5Hash},
                           HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
  EXPECT_FALSE(result.is_mismatch);
}

TEST(CompositeHashValidator, Missing) {
  CompositeValidator validator(absl::make_unique<Crc32cHashValidator>(),
                               absl::make_unique<MD5HashValidator>());
  UpdateValidator(validator, "The quick");
  UpdateValidator(validator, " brown");
  UpdateValidator(validator, " fox jumps over the lazy dog");
  validator.ProcessHeader("x-goog-hash",
                          "crc32c=" + std::string{kQuickFoxCrc32cChecksum});
  auto result = std::move(validator).Finish();
  EXPECT_THAT(
      result.computed,
      UnorderedElementsAre(HashValue{"md5", kQuickFoxMD5Hash},
                           HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
  EXPECT_THAT(result.received,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
  EXPECT_FALSE(result.is_mismatch);
}

TEST(CreateHashValidator, ReadNull) {
  auto validator =
      CreateHashValidator(ReadObjectRangeRequest("test-bucket", "test-object")
                              .set_multiple_options(DisableCrc32cChecksum(true),
                                                    DisableMD5Hash(true)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_TRUE(result.computed.empty());
}

TEST(CreateHashValidator, ReadOnlyCrc32c) {
  auto validator =
      CreateHashValidator(ReadObjectRangeRequest("test-bucket", "test-object")
                              .set_multiple_options(DisableMD5Hash(true)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, ReadDisableCrc32cTrue) {
  auto validator = CreateHashValidator(
      ReadObjectRangeRequest("test-bucket", "test-object")
          .set_multiple_options(DisableCrc32cChecksum(true)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed, IsEmpty());
}

TEST(CreateHashValidator, ReadWithoutConstructors) {
  auto validator =
      CreateHashValidator(ReadObjectRangeRequest("test-bucket", "test-object"));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, ReadDisableCrc32cFalse) {
  auto validator = CreateHashValidator(
      ReadObjectRangeRequest("test-bucket", "test-object")
          .set_multiple_options(DisableCrc32cChecksum(false)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, ReadDisableMD5False) {
  auto validator =
      CreateHashValidator(ReadObjectRangeRequest("test-bucket", "test-object")
                              .set_multiple_options(DisableMD5Hash(false)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(
      result.computed,
      UnorderedElementsAre(HashValue{"md5", kQuickFoxMD5Hash},
                           HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, ReadDisableCrc32cDefaultConstructor) {
  auto validator =
      CreateHashValidator(ReadObjectRangeRequest("test-bucket", "test-object")
                              .set_multiple_options(DisableCrc32cChecksum()));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, ReadDisableMD5DefaultConstructor) {
  auto validator =
      CreateHashValidator(ReadObjectRangeRequest("test-bucket", "test-object")
                              .set_multiple_options(DisableMD5Hash()));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, WriteNull) {
  auto validator =
      CreateHashValidator(ResumableUploadRequest("test-bucket", "test-object")
                              .set_multiple_options(DisableCrc32cChecksum(true),
                                                    DisableMD5Hash(true)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed, IsEmpty());
}

TEST(CreateHashValidator, WriteOnlyCrc32c) {
  auto validator =
      CreateHashValidator(ResumableUploadRequest("test-bucket", "test-object")
                              .set_multiple_options(DisableMD5Hash(true)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, WriteDisableCrc32cTrue) {
  auto validator = CreateHashValidator(
      ResumableUploadRequest("test-bucket", "test-object")
          .set_multiple_options(DisableCrc32cChecksum(true)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed, IsEmpty());
}

TEST(CreateHashValidator, WriteWithoutConstrutors) {
  auto validator =
      CreateHashValidator(ResumableUploadRequest("test-bucket", "test-object"));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, WriteBothFalse) {
  auto validator = CreateHashValidator(
      ResumableUploadRequest("test-bucket", "test-object")
          .set_multiple_options(DisableCrc32cChecksum(false),
                                DisableMD5Hash(false)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(
      result.computed,
      UnorderedElementsAre(HashValue{"md5", kQuickFoxMD5Hash},
                           HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, WriteBothDefaultConstructor) {
  auto validator = CreateHashValidator(
      ResumableUploadRequest("test-bucket", "test-object")
          .set_multiple_options(DisableCrc32cChecksum(), DisableMD5Hash()));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, WriteDisableCrc32False) {
  auto validator = CreateHashValidator(
      ResumableUploadRequest("test-bucket", "test-object")
          .set_multiple_options(DisableCrc32cChecksum(false)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, WriteDisableMD5False) {
  auto validator =
      CreateHashValidator(ResumableUploadRequest("test-bucket", "test-object")
                              .set_multiple_options(DisableMD5Hash(false)));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum},
                          HashValue{"md5", kQuickFoxMD5Hash}));
}

TEST(CreateHashValidator, WriteDisableCrc32cDefaultConstructor) {
  auto validator =
      CreateHashValidator(ResumableUploadRequest("test-bucket", "test-object")
                              .set_multiple_options(DisableCrc32cChecksum()));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(CreateHashValidator, WriteDisableMD5DefaultConstructor) {
  auto validator =
      CreateHashValidator(ResumableUploadRequest("test-bucket", "test-object")
                              .set_multiple_options(DisableMD5Hash()));
  UpdateValidator(*validator, "The quick brown fox jumps over the lazy dog");
  auto result = std::move(*validator).Finish();
  EXPECT_THAT(result.computed,
              ElementsAre(HashValue{"crc32c", kQuickFoxCrc32cChecksum}));
}

TEST(FormatHashValidator, All) {
  struct Test {
    std::string expected;
    HashValidator::HashValues values;
  } cases[] = {
      {"", {}},
      {"hash", {{"key", "hash"}}},
      {"k1=h1, k2=h2", {{"k1", "h1"}, {"k2", "h2"}}},
  };

  for (auto const& test : cases) {
    auto const received = HashValidator::Result{/*.received=*/test.values,
                                                /*.computed=*/{}, false};
    EXPECT_EQ(test.expected, FormatReceivedHashes(received));
    EXPECT_THAT(FormatComputedHashes(received), IsEmpty());

    auto const computed = HashValidator::Result{
        /*.received=*/{}, /*.computed=*/test.values, false};
    EXPECT_EQ(test.expected, FormatComputedHashes(computed));
    EXPECT_THAT(FormatReceivedHashes(computed), IsEmpty());
  }
}

}  // namespace
}  // namespace internal
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google

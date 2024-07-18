// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/opentelemetry/configure_basic_tracing.h"
#include "google/cloud/storage/grpc_plugin.h"
#include "google/cloud/storage/testing/storage_integration_test.h"
#include "google/cloud/internal/absl_str_cat_quiet.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/internal/random.h"
#include "google/cloud/opentelemetry_options.h"
#include "google/cloud/testing_util/scoped_environment.h"
#include "google/cloud/testing_util/status_matchers.h"
#include "absl/strings/string_view.h"
#include <gmock/gmock.h>
#include <vector>

namespace google {
namespace cloud {
namespace storage {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN
namespace {

using ::google::cloud::internal::GetEnv;
using ::google::cloud::testing_util::ScopedEnvironment;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Not;

class ObjectRepro353688666IntegrationTest
    : public google::cloud::storage::testing::StorageIntegrationTest {};

TEST_F(ObjectRepro353688666IntegrationTest, ReadManyRanges) {
  auto const bucket_name =
      GetEnv("GOOGLE_CLOUD_CPP_STORAGE_TEST_BUCKET_NAME").value_or("");
  ASSERT_THAT(bucket_name, Not(IsEmpty()))
      << "GOOGLE_CLOUD_CPP_STORAGE_TEST_BUCKET_NAME is not set";
  auto const project_id = GetEnv("GOOGLE_CLOUD_PROJECT").value_or("");
  ASSERT_THAT(project_id, Not(IsEmpty())) << "GOOGLE_CLOUD_PROJECT is not set";

  auto configuration =
      google::cloud::otel::ConfigureBasicTracing(Project(project_id));

  auto client = storage_experimental::DefaultGrpcClient(
      Options{}
          .set<OpenTelemetryTracingOption>(true)
          .set<RetryPolicyOption>(LimitedErrorCountRetryPolicy(10).clone())
          .set<DownloadStallTimeoutOption>(std::chrono::seconds(5))
          .set<TransferStallTimeoutOption>(std::chrono::seconds(30))
          .set<IdempotencyPolicyOption>(AlwaysRetryIdempotencyPolicy().clone())
          .set<BackoffPolicyOption>(
              ExponentialBackoffPolicy(std::chrono::milliseconds(800),
                                       std::chrono::minutes(5), 2.0)
                  .clone()));
  auto const object_name = MakeRandomObjectName();
  auto constexpr kMaxRangeSize = 8 * 1024 * 1024;
  auto constexpr kSize = 8 * kMaxRangeSize;
  auto const data = MakeRandomData(kSize);
  auto const view = absl::string_view(data);

  auto insert =
      client.InsertObject(bucket_name, object_name, data, IfGenerationMatch(0));
  ASSERT_STATUS_OK(insert);
  ScheduleForDelete(*insert);

  auto generator = google::cloud::internal::MakeDefaultPRNG();
  auto start_generator =
      std::uniform_int_distribution<std::int64_t>(0, kSize - kMaxRangeSize);
  auto size_generator =
      std::uniform_int_distribution<std::int64_t>(0, kMaxRangeSize - 1);

  std::vector<std::thread> workers(128);
  for (int i = 0; i != 1024; ++i) {
    if (i != 0 && i % 100 == 0) std::cerr << "iteration=" << i << std::endl;

    std::generate(workers.begin(), workers.end(), [&]() {
      auto const begin = start_generator(generator);
      auto const size = size_generator(generator);
      return std::thread(
          [&](std::int64_t begin, std::int64_t size) {
            SCOPED_TRACE(absl::StrCat("Running iteration ", i, " size=", size,
                                      " begin=", begin));
            std::string received;
            std::vector<char> buffer(128 * 1024);
            auto is = client.ReadObject(bucket_name, object_name,
                                        ReadRange(begin, begin + size));
            while (!is.eof()) {
              is.read(buffer.data(), buffer.size());
              if (is.gcount() == 0) {
                ASSERT_TRUE(is.eof());
                continue;
              }
              received.append(buffer.begin(),
                              std::next(buffer.begin(), is.gcount()));
            }
            EXPECT_FALSE(is.bad());
            EXPECT_STATUS_OK(is.status());
            ASSERT_THAT(size, Eq(received.size()));
            EXPECT_THAT(view.substr(begin, size), Eq(received));
          },
          begin, size);
    });
    for (auto& t : workers) t.join();
  }
}

}  // namespace
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace storage
}  // namespace cloud
}  // namespace google

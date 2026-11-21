// Copyright 2026 Google LLC
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

#ifndef GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_PUBSUB_TEST_PROXY_PUBSUB_TEST_PROXY_H
#define GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_PUBSUB_TEST_PROXY_PUBSUB_TEST_PROXY_H

#include "google/cloud/pubsub/blocking_publisher.h"
#include "google/cloud/pubsub/publisher.h"
#include "google/cloud/pubsub/retry_policy.h"
#include "google/cloud/pubsub/subscription.h"
#include "google/cloud/pubsub/test_proxy/trace_exporter.h"
#include "google/cloud/pubsub/topic.h"
#include "google/cloud/options.h"
#include "google/cloud/status.h"
#include "google/cloud/status_or.h"
#include "google/cloud/version.h"
#include "pubsub/conformance/v1/proxy.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace google {
namespace cloud {
namespace pubsub {
namespace test_proxy {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN

namespace pb = ::pubsub::conformance::v1;

Status ValidateListenPort(int port);
StatusOr<Topic> ParseTopicName(std::string const& t, std::string const& p);
StatusOr<Subscription> ParseSubscriptionName(std::string const& s,
                                             std::string const& p);
std::int32_t ToGrpcErrorCode(StatusCode code);
pb::SettlementResultEvent::AckStatus ToAckStatus(Status const& status);
std::shared_ptr<RetryPolicy> MakeCustomRetryPolicy(
    std::vector<std::int32_t> retry_codes, std::int32_t max_attempts,
    std::int64_t total_timeout_ms, bool stop_on_ok);
Options BuildPublisherOptions(pb::CreatePublisherRequest const& request);
Options BuildSubscriberOptions(pb::StartSubscriberCommand const& command);
Options BuildUnaryPullOptions(pb::UnaryPullRequest const& request);

class PubSubTestProxy final : public pb::PubSubProxy::Service {
  using gs = grpc::Status;
  using SCtx = grpc::ServerContext*;
  template <class T>
  using Req = T const*;

 public:
  using ShutdownCallback = std::function<void(std::chrono::milliseconds)>;
  PubSubTestProxy(std::shared_ptr<TraceStore> trace_store,
                  ShutdownCallback shutdown_callback = {});
  ~PubSubTestProxy() override;

  gs GetCapabilities(SCtx, Req<pb::GetCapabilitiesRequest>,
                     pb::GetCapabilitiesResponse*) override;
  gs ResetTraces(SCtx, Req<pb::ResetTracesRequest>,
                 pb::ResetTracesResponse*) override;
  gs DrainTraces(SCtx, Req<pb::DrainTracesRequest>,
                 pb::DrainTracesResponse*) override;
  gs CreatePublisher(SCtx, Req<pb::CreatePublisherRequest>,
                     pb::CreatePublisherResponse*) override;
  gs Publish(SCtx, Req<pb::PublishRequest>, pb::PublishResponse*) override;
  gs AwaitPublish(SCtx, Req<pb::AwaitPublishRequest>,
                  pb::PublishResponse*) override;
  gs FlushPublisher(SCtx, Req<pb::FlushPublisherRequest>,
                    pb::FlushPublisherResponse*) override;
  gs StopPublisher(SCtx, Req<pb::StopPublisherRequest>,
                   pb::StopPublisherResponse*) override;
  gs ResumePublish(SCtx, Req<pb::ResumePublishRequest>,
                   pb::ResumePublishResponse*) override;
  gs ClosePublisher(SCtx, Req<pb::ClosePublisherRequest>,
                    pb::ClosePublisherResponse*) override;
  gs SubscriberSession(
      SCtx,
      grpc::ServerReaderWriter<pb::SubscriberEvent, pb::SubscriberCommand>*)
      override;
  gs UnaryPull(SCtx, Req<pb::UnaryPullRequest>,
               pb::UnaryPullResponse*) override;
  gs ShutdownProxy(SCtx, Req<pb::ShutdownProxyRequest>,
                   pb::ShutdownProxyResponse*) override;

  void InitiateShutdown();
  bool WaitForSyncWorkers(std::chrono::milliseconds timeout);

 private:
  struct PublisherHandle;
  struct SyncCallRegistry;
  using PubPtr = std::shared_ptr<PublisherHandle>;
  PubPtr GetPublisher(std::string const& id);

  std::shared_ptr<TraceStore> trace_store_;
  ShutdownCallback shutdown_callback_;
  std::atomic<bool> shutting_down_{false};
  std::shared_ptr<SyncCallRegistry> sync_calls_;

  std::mutex publishers_mu_;
  std::uint64_t next_publisher_id_{1};
  std::unordered_map<std::string, PubPtr> publishers_;
  std::unordered_set<PubPtr> closing_publishers_;

  std::mutex sessions_mu_;
  std::uint64_t next_session_id_{1};
  std::unordered_map<std::uint64_t, std::function<void()>> active_sessions_;
};

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace test_proxy
}  // namespace pubsub
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_PUBSUB_TEST_PROXY_PUBSUB_TEST_PROXY_H

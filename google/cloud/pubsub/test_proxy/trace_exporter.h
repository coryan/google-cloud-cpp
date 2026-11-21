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

#ifndef GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_PUBSUB_TEST_PROXY_TRACE_EXPORTER_H
#define GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_PUBSUB_TEST_PROXY_TRACE_EXPORTER_H

#include "google/cloud/version.h"
#include "pubsub/conformance/v1/proxy.pb.h"
#include <google/protobuf/timestamp.pb.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/nostd/unique_ptr.h>
#include <opentelemetry/sdk/trace/exporter.h>
#include <opentelemetry/sdk/trace/span_data.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/tracer_provider.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace google {
namespace cloud {
namespace pubsub {
namespace test_proxy {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN

class TraceStore {
 public:
  void AddSpan(::pubsub::conformance::v1::TraceSpan span);
  void SetTracerProvider(
      std::weak_ptr<opentelemetry::sdk::trace::TracerProvider> provider);
  std::weak_ptr<opentelemetry::sdk::trace::TracerProvider> tracer_provider();
  void ForceFlush();
  void Shutdown();
  void Clear();
  std::vector<::pubsub::conformance::v1::TraceSpan> Drain();

 private:
  std::mutex mu_;
  std::vector<::pubsub::conformance::v1::TraceSpan> spans_;
  std::mutex provider_mu_;
  std::weak_ptr<opentelemetry::sdk::trace::TracerProvider> provider_;
};

::pubsub::conformance::v1::TraceSpan ConvertSpanData(
    opentelemetry::sdk::trace::SpanData const& span_data);

class UnboundedProxySpanExporter final
    : public opentelemetry::sdk::trace::SpanExporter {
  using RecPtr = std::unique_ptr<opentelemetry::sdk::trace::Recordable>;
  using us = std::chrono::microseconds;

 public:
  explicit UnboundedProxySpanExporter(std::shared_ptr<TraceStore> store);
  ~UnboundedProxySpanExporter() override = default;
  RecPtr MakeRecordable() noexcept override;
  opentelemetry::sdk::common::ExportResult Export(
      opentelemetry::nostd::span<RecPtr> const& spans) noexcept override;
  bool ForceFlush(us = (us::max)()) noexcept override { return true; }
  bool Shutdown(us = (us::max)()) noexcept override { return true; }

 private:
  std::shared_ptr<TraceStore> store_;
};

std::shared_ptr<TraceStore> InstallProxyTracerProvider();
void ResetGlobalTracerProvider();

class ScopedProxyTracerProvider {
 public:
  ScopedProxyTracerProvider();
  ~ScopedProxyTracerProvider();
  ScopedProxyTracerProvider(ScopedProxyTracerProvider const&) = delete;
  ScopedProxyTracerProvider& operator=(ScopedProxyTracerProvider const&) =
      delete;
  std::shared_ptr<TraceStore> store() const { return store_; }

 private:
  opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>
      previous_;
  std::shared_ptr<TraceStore> store_;
};

class ScopedTraceParent {
 public:
  ScopedTraceParent(std::string const& traceparent,
                    std::string const& tracestate);
  ScopedTraceParent(ScopedTraceParent const&) = delete;
  ScopedTraceParent& operator=(ScopedTraceParent const&) = delete;

 private:
  opentelemetry::nostd::unique_ptr<opentelemetry::context::Token> token_;
};

void InjectW3CTraceContext(std::string* traceparent, std::string* tracestate);
void DurationToTimestamp(std::chrono::nanoseconds d,
                         google::protobuf::Timestamp* ts);

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace test_proxy
}  // namespace pubsub
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_PUBSUB_TEST_PROXY_TRACE_EXPORTER_H

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

#include "google/cloud/pubsub/test_proxy/trace_exporter.h"
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/nostd/variant.h>
#include <opentelemetry/sdk/common/attribute_utils.h>
#include <opentelemetry/sdk/trace/simple_processor.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/span_metadata.h>
#include <cctype>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace google {
namespace cloud {
namespace pubsub {
namespace test_proxy {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN

namespace pb = ::pubsub::conformance::v1;
namespace ot = opentelemetry;
namespace ot_tr = ot::trace;
namespace ot_sdk = ot::sdk::trace;
namespace ot_ctx = ot::context;
using OwnedAttr = ot::sdk::common::OwnedAttributeValue;
using ExportRes = ot::sdk::common::ExportResult;
using Recordable = ot_sdk::Recordable;
using ProvPtr = ot::nostd::shared_ptr<ot_tr::TracerProvider>;
using Lock = std::lock_guard<std::mutex>;
using ns = std::chrono::nanoseconds;
using sec = std::chrono::seconds;
using Str = std::string;
using sv = ot::nostd::string_view;
template <class T>
using SP = std::shared_ptr<T>;
template <class T>
using WP = std::weak_ptr<T>;
template <class T>
using UPtr = std::unique_ptr<T>;
template <class T>
using Vec = std::vector<T>;
using std::chrono::duration_cast;
template <class T, class... Ts>
inline constexpr bool kOneOf = (std::is_same_v<T, Ts> || ...);

namespace {
bool AsciiEq(sv a, sv b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](unsigned char x, unsigned char y) {
                      return std::tolower(x) == std::tolower(y);
                    });
}

class Carrier final : public ot_ctx::propagation::TextMapCarrier {
 public:
  Carrier(Str tp, Str ts) : tp_(std::move(tp)), ts_(std::move(ts)) {}
  sv Get(sv k) const noexcept override {
    return AsciiEq(k, "traceparent")  ? sv(tp_)
           : AsciiEq(k, "tracestate") ? sv(ts_)
                                      : sv{};
  }
  void Set(sv k, sv v) noexcept override {
    if (AsciiEq(k, "traceparent")) tp_.assign(v.data(), v.size());
    if (AsciiEq(k, "tracestate")) ts_.assign(v.data(), v.size());
  }
  bool Keys(ot::nostd::function_ref<bool(sv)> cb) const noexcept override {
    return (tp_.empty() || cb("traceparent")) &&
           (ts_.empty() || cb("tracestate"));
  }
  Str tp_, ts_;
};

pb::SpanKind ToProtoKind(ot_tr::SpanKind k) {
  using K = ot_tr::SpanKind;
  return k == K::kInternal   ? pb::SPAN_KIND_INTERNAL
         : k == K::kServer   ? pb::SPAN_KIND_SERVER
         : k == K::kClient   ? pb::SPAN_KIND_CLIENT
         : k == K::kProducer ? pb::SPAN_KIND_PRODUCER
         : k == K::kConsumer ? pb::SPAN_KIND_CONSUMER
                             : pb::SPAN_KIND_UNSPECIFIED;
}

template <std::size_t N, class Id>
Str ToHex(Id const& id) {
  char b[N];
  return id.ToLowerBase16(b), Str(b, N);
}

template <typename Out>
void PopulateAttrs(std::unordered_map<Str, OwnedAttr> const& attrs, Out* out) {
  using i32 = std::int32_t;
  using u32 = std::uint32_t;
  using i64 = std::int64_t;
  using u64 = std::uint64_t;
  for (auto const& kv : attrs) {
    pb::AttributeValue v;
    ot::nostd::visit(
        [&v](auto const& x) {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, bool>)
            v.set_bool_value(x);
          else if constexpr (kOneOf<T, i32, u32, i64, u64>)
            v.set_int64_value(static_cast<i64>(x));
          else if constexpr (std::is_same_v<T, double>)
            v.set_double_value(x);
          else if constexpr (std::is_same_v<T, Str>)
            v.set_string_value(x);
          else if constexpr (std::is_same_v<T, Vec<bool>>)
            for (bool e : x) v.mutable_bool_list()->add_values(e);
          else if constexpr (kOneOf<T, Vec<i32>, Vec<u32>, Vec<i64>, Vec<u64>,
                                    Vec<std::uint8_t>>)
            for (auto e : x)
              v.mutable_int64_list()->add_values(static_cast<i64>(e));
          else if constexpr (std::is_same_v<T, Vec<double>>)
            for (double e : x) v.mutable_double_list()->add_values(e);
          else if constexpr (std::is_same_v<T, Vec<Str>>)
            for (auto const& e : x) v.mutable_string_list()->add_values(e);
        },
        kv.second);
    (*out)[kv.first] = std::move(v);
  }
}
template <typename Out>
void PopulateSpanCtx(ot_tr::SpanContext const& ctx, Out* out) {
  if (ctx.trace_id().IsValid()) out->set_trace_id(ToHex<32>(ctx.trace_id()));
  if (ctx.span_id().IsValid()) out->set_span_id(ToHex<16>(ctx.span_id()));
  if (ctx.trace_state() && !ctx.trace_state()->Empty())
    out->set_trace_state(ctx.trace_state()->ToHeader());
  out->set_trace_flags(ctx.trace_flags().flags());
}
template <class F>
auto WithLock(std::mutex& m, F&& f) -> decltype(f()) {
  Lock lk(m);
  return f();
}
}  // namespace

void TraceStore::AddSpan(pb::TraceSpan s) {
  WithLock(mu_, [&] { spans_.push_back(std::move(s)); });
}
void TraceStore::SetTracerProvider(WP<ot_sdk::TracerProvider> p) {
  WithLock(provider_mu_, [&] { provider_ = std::move(p); });
}
WP<ot_sdk::TracerProvider> TraceStore::tracer_provider() {
  return WithLock(provider_mu_, [&] { return provider_; });
}
void TraceStore::ForceFlush() {
  if (auto p = WithLock(provider_mu_, [&] { return provider_.lock(); }))
    p->ForceFlush();
}
void TraceStore::Shutdown() {
  auto p = WithLock(provider_mu_,
                    [&] { return std::exchange(provider_, {}).lock(); });
  if (p) p->ForceFlush(), p->Shutdown();
}
void TraceStore::Clear() {
  ForceFlush(), WithLock(mu_, [&] { spans_.clear(); });
}
Vec<pb::TraceSpan> TraceStore::Drain() {
  return ForceFlush(), WithLock(mu_, [&] { return std::exchange(spans_, {}); });
}

pb::TraceSpan ConvertSpanData(ot_sdk::SpanData const& sd) {
  pb::TraceSpan out;
  PopulateSpanCtx(sd.GetSpanContext(), &out);
  if (auto const& p = sd.GetParentSpanId(); p.IsValid() && p != ot_tr::SpanId())
    out.set_parent_span_id(ToHex<16>(p));
  auto name = sd.GetName();
  using S = ot_tr::StatusCode;
  auto st = sd.GetStatus();
  out.set_name(name.data(), name.size()),
      out.set_kind(ToProtoKind(sd.GetSpanKind())),
      out.set_status_code(st == S::kOk      ? pb::SPAN_STATUS_CODE_OK
                          : st == S::kError ? pb::SPAN_STATUS_CODE_ERROR
                                            : pb::SPAN_STATUS_CODE_UNSET);
  auto s_ns = duration_cast<ns>(sd.GetStartTime().time_since_epoch());
  DurationToTimestamp(s_ns, out.mutable_start_time());
  DurationToTimestamp(s_ns + duration_cast<ns>(sd.GetDuration()),
                      out.mutable_end_time());
  out.set_status_message(sd.GetDescription());
  auto const& sc = sd.GetInstrumentationScope();
  out.set_instrumentation_scope_name(sc.GetName()),
      out.set_instrumentation_scope_version(sc.GetVersion());
  PopulateAttrs(sd.GetAttributes(), out.mutable_attributes());
  for (auto const& ev : sd.GetEvents()) {
    auto* pe = out.add_events();
    pe->set_name(ev.GetName());
    DurationToTimestamp(duration_cast<ns>(ev.GetTimestamp().time_since_epoch()),
                        pe->mutable_timestamp());
    PopulateAttrs(ev.GetAttributes(), pe->mutable_attributes());
  }
  for (auto const& lk : sd.GetLinks()) {
    auto* pl = out.add_links();
    PopulateSpanCtx(lk.GetSpanContext(), pl);
    PopulateAttrs(lk.GetAttributes(), pl->mutable_attributes());
  }
  return out;
}
UnboundedProxySpanExporter::UnboundedProxySpanExporter(SP<TraceStore> s)
    : store_(std::move(s)) {}
UPtr<Recordable> UnboundedProxySpanExporter::MakeRecordable() noexcept {
  return std::make_unique<ot_sdk::SpanData>();
}
ExportRes UnboundedProxySpanExporter::Export(
    ot::nostd::span<UPtr<Recordable>> const& spans) noexcept {
  for (auto const& r : spans)
    if (r)
      store_->AddSpan(
          ConvertSpanData(*static_cast<ot_sdk::SpanData const*>(r.get())));
  return ExportRes::kSuccess;
}
SP<TraceStore> InstallProxyTracerProvider() {
  auto s = std::make_shared<TraceStore>();
  SP<ot_sdk::TracerProvider> sp(ot_sdk::TracerProviderFactory::Create(
      std::make_unique<ot_sdk::SimpleSpanProcessor>(
          std::make_unique<UnboundedProxySpanExporter>(s))));
  return s->SetTracerProvider(sp),
         ot_tr::Provider::SetTracerProvider(ProvPtr(std::move(sp))), s;
}
void ResetGlobalTracerProvider() {
  ot_tr::Provider::SetTracerProvider(ProvPtr{});
}
ScopedProxyTracerProvider::ScopedProxyTracerProvider()
    : previous_(ot_tr::Provider::GetTracerProvider()),
      store_(InstallProxyTracerProvider()) {}
ScopedProxyTracerProvider::~ScopedProxyTracerProvider() {
  if (store_) store_->Shutdown();
  ot_tr::Provider::SetTracerProvider(previous_);
}
ScopedTraceParent::ScopedTraceParent(Str const& tp, Str const& ts) {
  if (tp.empty()) return;
  Carrier c(tp, ts);
  auto cur = ot_ctx::RuntimeContext::GetCurrent();
  auto ext = ot_tr::propagation::HttpTraceContext().Extract(c, cur);
  if (ot_tr::GetSpan(ext)->GetContext().IsValid())
    token_ = ot_ctx::RuntimeContext::Attach(ext);
}
void InjectW3CTraceContext(Str* tp, Str* ts) {
  if (tp) tp->clear();
  if (ts) ts->clear();
  auto cur = ot_ctx::RuntimeContext::GetCurrent();
  if (!ot_tr::GetSpan(cur)->GetContext().IsValid()) return;
  Carrier c("", "");
  ot_tr::propagation::HttpTraceContext().Inject(c, cur);
  if (tp) *tp = c.tp_;
  if (ts) *ts = c.ts_;
}
void DurationToTimestamp(ns d, google::protobuf::Timestamp* ts) {
  auto s = duration_cast<sec>(d);
  auto rem = d - s;
  if (rem.count() < 0) s -= sec(1), rem += sec(1);
  ts->set_seconds(s.count()),
      ts->set_nanos(static_cast<std::int32_t>(rem.count()));
}

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace test_proxy
}  // namespace pubsub
}  // namespace cloud
}  // namespace google

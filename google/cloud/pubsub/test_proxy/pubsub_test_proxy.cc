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

#include "google/cloud/pubsub/test_proxy/pubsub_test_proxy.h"
#include "google/cloud/pubsub/ack_handler.h"
#include "google/cloud/pubsub/backoff_policy.h"
#include "google/cloud/pubsub/exactly_once_ack_handler.h"
#include "google/cloud/pubsub/message.h"
#include "google/cloud/pubsub/options.h"
#include "google/cloud/pubsub/pull_ack_handler.h"
#include "google/cloud/pubsub/pull_response.h"
#include "google/cloud/pubsub/subscriber.h"
#include "google/cloud/common_options.h"
#include "google/cloud/completion_queue.h"
#include "google/cloud/credentials.h"
#include "google/cloud/grpc_options.h"
#include "google/cloud/opentelemetry_options.h"
#include <grpc/compression.h>
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <thread>
#include <utility>
#include <variant>

namespace google {
namespace cloud {
namespace pubsub {
namespace test_proxy {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN

using SC = grpc::StatusCode;
using K = StatusCode;
using gs = grpc::Status;
using ms = std::chrono::milliseconds;
using sec = std::chrono::seconds;
using Clock = std::chrono::steady_clock;
using TP = Clock::time_point;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;
using sz = std::size_t;
using Str = std::string;
using Mutex = std::mutex;
using CV = std::condition_variable;
using AtBool = std::atomic<bool>;
using Lock = std::lock_guard<Mutex>;
using ULock = std::unique_lock<Mutex>;
using StrSet = std::unordered_set<i32>;
using RpPtr = std::shared_ptr<RetryPolicy>;
using AbPtr = std::shared_ptr<AtBool>;
using SCtx = grpc::ServerContext*;
template <class T>
using Opt = std::optional<T>;
template <class T>
using UPtr = std::unique_ptr<T>;
template <class T>
using SP = std::shared_ptr<T>;
template <class T>
using Vec = std::vector<T>;
template <class K1, class V1>
using Map = std::unordered_map<K1, V1>;
using std::chrono::duration_cast;

namespace {
using SessPtr = SP<struct SubState>;
using CallPtr = SP<struct SyncState>;
using StartCmd = pb::StartSubscriberCommand;
using SettleCmd = pb::SettleMessageCommand;
using AckEv = pb::SettlementResultEvent;
using SubEv = pb::SubscriberEvent;
using SubCmd = pb::SubscriberCommand;
using PubResp = pb::PublishResponse;
constexpr auto kReady = std::future_status::ready;
sz PosSz(i64 v) { return static_cast<sz>(std::max<i64>(0, v)); }
sz LimSz(i64 v) {
  return v < 0 ? (std::numeric_limits<sz>::max)() : static_cast<sz>(v);
}
ms PosMs(i64 v) { return ms(std::max<i64>(0, v)); }
template <class V, class P>
void EraseIf(V& v, P p) {
  v.erase(std::remove_if(v.begin(), v.end(), p), v.end());
}
template <class F>
auto WithLock(Mutex& m, F&& f) -> decltype(f()) {
  Lock lk(m);
  return f();
}
template <class Pred>
bool WaitPoll(Mutex& m, CV& cv, Pred done, SCtx c, AtBool const* ab = nullptr) {
  ULock lk(m);
  while (!done()) {
    if ((ab && ab->load()) || (c && c->IsCancelled())) return false;
    cv.wait_for(lk, ms(20));
  }
  return true;
}
struct TraceScope {
  Opt<ScopedTraceParent> s;
  TraceScope(bool mon, Str const& tp, Str const& ts) {
    if (mon && !tp.empty()) s.emplace(tp, ts);
  }
};

template <typename Out>
StatusOr<Out> ParseRes(Str const& name, Str const& proj, Str const& col,
                       Str const& noun) {
  if (name.empty()) return Status(K::kInvalidArgument, noun + " empty");
  if (name.compare(0, 9, "projects/") == 0) {
    auto s1 = name.find('/', 9),
         s2 = s1 == Str::npos ? s1 : name.find('/', s1 + 1);
    if (s1 == Str::npos || s1 == 9 || s2 == Str::npos ||
        name.compare(s1 + 1, s2 - s1 - 1, col) != 0 || s2 + 1 >= name.size() ||
        name.find('/', s2 + 1) != Str::npos)
      return Status(K::kInvalidArgument, "invalid " + noun);
    return Out(name.substr(9, s1 - 9), name.substr(s2 + 1));
  }
  return (name.find('/') != Str::npos || proj.empty())
             ? StatusOr<Out>(
                   Status(K::kInvalidArgument, "invalid short " + noun))
             : Out(proj, name);
}

class CustomRetry final : public RetryPolicy {
 public:
  using RP = UPtr<RetryPolicy>;
  CustomRetry(StrSet codes, Opt<i32> mf, Opt<ms> md, bool ok)
      : codes_(std::move(codes)),
        ok_(ok),
        mf_(mf),
        md_(md),
        dl_(md_ ? Opt<TP>(Clock::now() + *md_) : std::nullopt) {}
  RP clone() const override {
    return std::make_unique<CustomRetry>(codes_, mf_, md_, ok_);
  }
  bool OnFailure(Status const& st) override {
    return !IsPermanentFailure(st) && (++fails_, !IsExhausted());
  }
  bool IsExhausted() const override {
    return (mf_ && fails_ > *mf_) || (dl_ && Clock::now() >= *dl_);
  }
  bool IsPermanentFailure(Status const& st) const override {
    if (ok_ && st.ok()) return true;
    return codes_.empty() ? (!st.ok() && def_.IsPermanentFailure(st))
                          : codes_.count(static_cast<i32>(st.code())) == 0;
  }

 private:
  StrSet codes_;
  bool ok_;
  Opt<i32> mf_;
  Opt<ms> md_;
  LimitedTimeRetryPolicy def_{sec(0)};
  i32 fails_{0};
  Opt<TP> dl_;
};
class AbortRetry final : public RetryPolicy {
 public:
  using RP = UPtr<RetryPolicy>;
  AbortRetry(RpPtr in, AbPtr ab) : in_(std::move(in)), ab_(std::move(ab)) {}
  RP clone() const override {
    return std::make_unique<AbortRetry>(in_->clone(), ab_);
  }
  bool OnFailure(Status const& st) override {
    return !(ab_ && ab_->load()) && in_->OnFailure(st);
  }
  bool IsExhausted() const override {
    return (ab_ && ab_->load()) || in_->IsExhausted();
  }
  bool IsPermanentFailure(Status const& st) const override {
    return in_->IsPermanentFailure(st);
  }

 private:
  RpPtr in_;
  AbPtr ab_;
};
struct SyncState {
  Mutex mu;
  CV cv;
  bool done{false};
  AbPtr aborted = std::make_shared<AtBool>(false);
  Opt<CompletionQueue> cancel_cq;
  void SetCq(Opt<CompletionQueue> c) {
    if (WithLock(mu,
                 [&] { return cancel_cq = std::move(c), aborted->load(); }) &&
        cancel_cq)
      cancel_cq->CancelAll();
  }
  void Abort() {
    aborted->store(true);
    if (auto cq = WithLock(mu, [&] { return cancel_cq; })) cq->CancelAll();
    cv.notify_all();
  }
  void MarkDone() {
    WithLock(mu, [&] { done = true; }), cv.notify_all();
  }
  void WaitAbortable(ms d) {
    ULock lk(mu);
    cv.wait_until(lk, Clock::now() + d, [&] { return aborted->load(); });
  }
};
template <bool HasBudget, typename Req>
void ApplyRetryOpts(Req const& r, Options& opts, bool stop_on_ok) {
  Opt<i32> ma;
  Opt<i64> tt;
  if constexpr (HasBudget) {
    if (r.has_max_attempts()) ma = r.max_attempts();
    if (r.has_total_timeout_ms()) tt = r.total_timeout_ms();
  }
  if (auto const& rc = r.retry_codes(); !rc.empty() || ma || tt || stop_on_ok) {
    auto mf = ma ? Opt<i32>(*ma <= 0 ? -1 : (*ma - 1)) : std::nullopt;
    auto md = tt ? Opt<ms>(PosMs(*tt))
                 : (stop_on_ok && !ma ? Opt<ms>(ms(60000)) : std::nullopt);
    opts.set<RetryPolicyOption>(std::make_shared<CustomRetry>(
        StrSet(rc.begin(), rc.end()), mf, md, stop_on_ok));
  }
  if (r.has_initial_retry_delay_ms() || r.has_max_retry_delay_ms() ||
      r.has_retry_delay_multiplier()) {
    auto i = PosMs(r.has_initial_retry_delay_ms() ? r.initial_retry_delay_ms()
                                                  : 100);
    auto d = std::max(
        i, PosMs(r.has_max_retry_delay_ms() ? r.max_retry_delay_ms() : 60000));
    double m =
        r.has_retry_delay_multiplier() && r.retry_delay_multiplier() > 0.0
            ? r.retry_delay_multiplier()
            : 4.0;
    opts.set<BackoffPolicyOption>(std::make_shared<ExponentialBackoffPolicy>(
        i, d, m <= 1.0 ? 1.0001 : m));
  }
}
gs ValidateEndpoint(Str const& ep, bool use_emu) {
  char const* env = std::getenv("PUBSUB_EMULATOR_HOST");
  bool has = env != nullptr && *env != '\0';
  if (ep.empty() && !use_emu)
    return gs(SC::INVALID_ARGUMENT, "endpoint empty; use_emulator_env=false");
  if (ep.empty() && !has)
    return gs(SC::INVALID_ARGUMENT, "PUBSUB_EMULATOR_HOST unset");
  if (!ep.empty() && !use_emu && has && ep != env)
    return gs(SC::FAILED_PRECONDITION,
              "use_emulator_env=false with PUBSUB_EMULATOR_HOST");
  return gs::OK;
}
template <class Req>
gs ResolveSub(Req const& r, Subscription* out) {
  if (auto st = ValidateEndpoint(r.endpoint(), r.use_emulator_env()); !st.ok())
    return st;
  auto sub = ParseSubscriptionName(r.subscription(), r.project_id());
  return sub ? (*out = *std::move(sub), gs::OK)
             : gs(SC::INVALID_ARGUMENT, sub.status().message());
}
template <typename Req>
void ApplyEpTraceHeaders(Req const& req, Options& opts) {
  opts.set<OpenTelemetryTracingOption>(req.enable_monitoring());
  if (!req.endpoint().empty())
    opts.set<EndpointOption>(req.endpoint()),
        opts.set<UnifiedCredentialsOption>(MakeInsecureCredentials());
  if (!req.custom_headers().empty()) {
    std::unordered_multimap<Str, Str> h;
    for (auto const& kv : req.custom_headers()) h.emplace(kv.first, kv.second);
    opts.set<CustomHeadersOption>(std::move(h));
  }
}
sec ToLeaseSec(i64 v) {
  return v <= 0 ? sec(0) : sec(std::max<i64>(1, (v + 999) / 1000));
}
template <typename Req>
void ApplyLeaseOpts(Req const& r, Options& opts) {
  if (r.has_max_extension_ms())
    opts.set<MaxDeadlineTimeOption>(ToLeaseSec(r.max_extension_ms()));
  if (r.has_max_duration_per_ack_extension_ms())
    opts.set<MaxDeadlineExtensionOption>(
        ToLeaseSec(r.max_duration_per_ack_extension_ms()));
  if (r.has_min_duration_per_ack_extension_ms())
    opts.set<MinDeadlineExtensionOption>(
        ToLeaseSec(r.min_duration_per_ack_extension_ms()));
}
sz DefaultWorkers(sz max_def) {
  auto hw = static_cast<sz>(std::thread::hardware_concurrency());
  return std::clamp<sz>(hw == 0 ? 4 : hw, 4, max_def);
}
struct CqPool {
  explicit CqPool(sz n) : rem(std::max<sz>(1, n)) {
    for (sz i = 0; i < rem; ++i)
      ws.emplace_back([this, c = cq]() mutable {
        c.Run();
        if (WithLock(mu, [&] { return --rem == 0; })) cv.notify_all();
      });
  }
  ~CqPool() { Shutdown(true); }
  void BeginShutdown(bool cancel) {
    if (WithLock(mu, [&] { return std::exchange(down, true); })) return;
    cq.Shutdown();
    if (cancel) cq.CancelAll();
  }
  bool WaitIdle(ms t) {
    ULock lk(mu);
    return cv.wait_for(lk, t, [&] { return rem == 0; });
  }
  void Join() {
    for (auto& t : ws)
      if (t.joinable()) t.join();
    ws.clear();
  }
  void Shutdown(bool cancel, std::function<bool()> const& ab = {},
                ms grace = ms(0)) {
    BeginShutdown(cancel);
    {
      ULock lk(mu);
      for (auto dl = Clock::now() + grace; !cancel && rem > 0;) {
        if ((ab && ab()) || Clock::now() >= dl) {
          lk.unlock(), cq.CancelAll();
          break;
        }
        cv.wait_for(lk, ms(20));
      }
    }
    Join();
  }
  CompletionQueue cq;
  Mutex mu;
  CV cv;
  sz rem{0};
  bool down{false};
  Vec<std::thread> ws;
};
struct PubSlot {
  Mutex mu;
  CV cv;
  bool done{false}, caller_attributes_mutated{false};
  StatusOr<Str> result{Status(K::kInternal, "publish not resolved")};
};
using SlotPtr = SP<PubSlot>;
using SlotVec = Vec<SlotPtr>;
void NotifySlots(SlotVec const& slots) {
  for (auto const& s : slots) WithLock(s->mu, [] {}), s->cv.notify_all();
}
gs PubNotFound(Str const& id) {
  return gs(SC::NOT_FOUND, "publisher not found: " + id);
}
gs ShuttingDown() { return gs(SC::UNAVAILABLE, "proxy shutting down"); }
using PendingMsg = std::variant<AckHandler, ExactlyOnceAckHandler>;
struct SubState {
  using Stream = grpc::ServerReaderWriter<SubEv, SubCmd>;
  SubState(Stream* s, StartCmd cmd) : stream(s), start(std::move(cmd)) {}
  bool SendEvent(SubEv const& ev) {
    return WithLock(wmu, [&] { return !sclosed && stream->Write(ev); });
  }
  void SetFlag(bool SubState::* flag) {
    WithLock(wmu, [&] { this->*flag = true; }), wcv.notify_all();
  }
  void UpdateAcks(int d) {
    if (WithLock(amu, [&] {
          return acks = static_cast<sz>(static_cast<i64>(acks) + d), acks == 0;
        }))
      acv.notify_all();
  }
  bool WaitFinished(ms lim) {
    ULock lk(dmu);
    return dcv.wait_for(lk, lim, [&] { return finished; });
  }
  bool IsFinished() {
    return WithLock(dmu, [&] { return finished; });
  }
  bool CanIdleBail() {
    return WithLock(
        pmu, [&] { return pclosed || (stopping.load() && pending.empty()); });
  }
  void MarkAborted() {
    aborted.store(true), WithLock(wmu, [] {}), wcv.notify_all(),
        WithLock(amu, [] {}), acv.notify_all();
  }
  void Clear() {
    Lock c(clmu);
    WithLock(pmu, [&] { return pclosed = true, std::exchange(pending, {}); });
  }
  Opt<PendingMsg> Take(Str const& tok) {
    Lock lk(pmu);
    auto it = pending.find(tok);
    if (it == pending.end()) return std::nullopt;
    Opt<PendingMsg> out(std::move(it->second));
    pending.erase(it);
    return out;
  }
  void TriggerCancel() {
    if (auto f = WithLock(cmu,
                          [&] {
                            return std::exchange(can_c, false) ? std::move(dfut)
                                                               : future<void>{};
                          });
        f.valid())
      f.cancel();
  }
  void AbortAndCancel() { MarkAborted(), Clear(), TriggerCancel(); }
  Stream* stream;
  StartCmd const start;
  Mutex wmu, pmu, clmu, amu, cmu, dmu;
  bool sclosed{false}, started{false}, can_c{true}, finished{false},
      pclosed{false};
  u64 next_tok{1};
  Map<Str, PendingMsg> pending;
  CV wcv, acv, dcv;
  sz acks{0};
  future<void> dfut;
  Status dst;
  AtBool aborted{false}, stopping{false};
  std::atomic<int> callbacks{0};
  std::atomic<i64> delivered{0};
};

void SendAck(SessPtr const& s, Str const& tok, bool is_nack, Status const& st) {
  SubEv ev;
  auto* r = ev.mutable_settlement_result();
  r->set_delivery_token(tok), r->set_status(ToAckStatus(st)),
      r->set_grpc_error_code(ToGrpcErrorCode(st.code())),
      r->set_error_message(st.message()), r->set_is_nack(is_nack);
  s->SendEvent(ev), s->UpdateAcks(-1);
}
void SettleMsg(SessPtr const& s, Str const& tok, PendingMsg msg, bool nack,
               bool emit) {
  if (emit) s->UpdateAcks(1);
  std::visit(
      [&](auto& h) {
        if constexpr (std::is_same_v<std::decay_t<decltype(h)>, AckHandler>) {
          nack ? std::move(h).nack() : std::move(h).ack();
          if (emit) SendAck(s, tok, nack, {});
        } else {
          auto fut = nack ? std::move(h).nack() : std::move(h).ack();
          if (emit)
            fut.then([s, tok, nack](future<Status> f) {
              SendAck(s, tok, nack, f.get());
            });
          else
            fut.then([](future<Status>) {});
        }
      },
      msg);
}
template <class OutMsg>
void FillMsg(Message const& msg, int att, OutMsg* out) {
  out->set_message_id(msg.message_id()), out->set_data(msg.data());
  if (!msg.ordering_key().empty()) out->set_ordering_key(msg.ordering_key());
  for (auto const& kv : msg.attributes())
    (*out->mutable_attributes())[kv.first] = kv.second;
  DurationToTimestamp(duration_cast<std::chrono::nanoseconds>(
                          msg.publish_time().time_since_epoch()),
                      out->mutable_publish_time());
  if (att > 0)
    out->set_delivery_attempt(att), out->set_has_delivery_attempt(true);
}
template <class H>
void OnSubMsg(SessPtr const& s, Message const& msg, H handler) {
  int const active = ++s->callbacks;
  struct AG {
    std::atomic<int>& c;
    ~AG() { --c; }
  } ag{s->callbacks};
  auto const mode = s->start.auto_settle_mode();
  bool const inter = mode == StartCmd::AUTO_SETTLE_MODE_UNSPECIFIED ||
                     mode == StartCmd::AUTO_SETTLE_MODE_INTERACTIVE;
  SubEv ev;
  auto* d = ev.mutable_message_delivered();
  FillMsg(msg, handler.delivery_attempt(), d);
  d->set_auto_settled(!inter), d->set_active_callbacks(active),
      d->set_is_exactly_once(s->start.use_exactly_once());
  if (s->start.enable_monitoring()) {
    Str tp, ts;
    InjectW3CTraceContext(&tp, &ts);
    if (!tp.empty()) d->set_callback_traceparent(tp);
    if (!ts.empty()) d->set_callback_tracestate(ts);
  }
  Str tok;
  {
    ULock wlk(s->wmu);
    s->wcv.wait(wlk,
                [&] { return s->started || s->sclosed || s->aborted.load(); });
    {
      Lock plk(s->pmu);
      if (s->sclosed || s->pclosed || s->aborted.load() || s->stopping.load())
        return;
      tok = "msg-" + std::to_string(s->next_tok++), d->set_delivery_token(tok);
      if (inter) s->pending.emplace(tok, PendingMsg{std::move(handler)});
    }
    if (!s->stream->Write(ev)) {
      if (inter) s->Take(tok);
      return;
    }
    s->delivered.fetch_add(1);
  }
  if (inter || mode == StartCmd::AUTO_SETTLE_MODE_AUTO_DROP_WITHOUT_SETTLE)
    return;
  bool const nack = mode == StartCmd::AUTO_SETTLE_MODE_AUTO_NACK ||
                    mode == StartCmd::AUTO_SETTLE_MODE_AUTO_NACK_WITH_RESULT;
  bool const emit = mode == StartCmd::AUTO_SETTLE_MODE_AUTO_ACK_WITH_RESULT ||
                    mode == StartCmd::AUTO_SETTLE_MODE_AUTO_NACK_WITH_RESULT;
  SettleMsg(s, tok, PendingMsg{std::move(handler)}, nack, emit);
}
bool DecodeSettle(SettleCmd::SettleAction a, bool* nack, bool* emit) {
  *nack = a == SettleCmd::SETTLE_ACTION_NACK ||
          a == SettleCmd::SETTLE_ACTION_NACK_WITH_RESULT;
  *emit = a == SettleCmd::SETTLE_ACTION_ACK_WITH_RESULT ||
          a == SettleCmd::SETTLE_ACTION_NACK_WITH_RESULT;
  return *nack || a == SettleCmd::SETTLE_ACTION_ACK ||
         a == SettleCmd::SETTLE_ACTION_ACK_WITH_RESULT;
}
void FillPubResp(StatusOr<Str> const& res, bool mutated, PubResp* out) {
  if (res)
    out->set_message_id(*res);
  else
    out->set_error_code(ToGrpcErrorCode(res.status().code())),
        out->set_error_message(res.status().message());
  out->set_caller_attributes_mutated(mutated);
}
gs WaitSlot(SlotPtr const& slot, SCtx ctx, AtBool const& abort, SC code,
            char const* msg, PubResp* out) {
  if (!WaitPoll(slot->mu, slot->cv, [&] { return slot->done; }, ctx, &abort))
    return gs(code, msg);
  if (out) FillPubResp(slot->result, slot->caller_attributes_mutated, out);
  return gs::OK;
}
void HandleSettle(SessPtr const& s, SettleCmd const& cmd) {
  bool nack = false, emit = false;
  if (auto found = s->Take(cmd.delivery_token());
      found && DecodeSettle(cmd.action(), &nack, &emit))
    SettleMsg(s, cmd.delivery_token(), std::move(*found), nack, emit);
}
void RunPull(Subscription sub, Options o, i64 lease_ms,
             SettleCmd::SettleAction sa, CallPtr const& s,
             pb::UnaryPullResponse* resp) {
  CqPool cq(1);
  s->SetCq(cq.cq), o.set<GrpcCompletionQueueOption>(cq.cq);
  {
    auto client = Subscriber(MakeSubscriberConnection(sub, std::move(o)));
    auto res = client.Pull();
    if (!res) {
      if (auto const& e = res.status(); !s->aborted->load()) {
        auto const& m = e.error_info().metadata();
        auto it = m.find("gl-cpp.error.origin");
        if (!(e.code() == K::kUnavailable &&
              e.message() == "no messages returned" && it != m.end() &&
              it->second == "client"))
          resp->set_error_code(ToGrpcErrorCode(e.code())),
              resp->set_error_message(e.message());
      }
    } else {
      auto* p = resp->add_messages();
      FillMsg(res->message, res->handler.delivery_attempt(), p);
      if (lease_ms > 0) s->WaitAbortable(ms(lease_ms));
      bool nack = false, emit = false;
      if (!s->aborted->load() && DecodeSettle(sa, &nack, &emit)) {
        auto fut = nack ? std::move(res->handler).nack()
                        : std::move(res->handler).ack();
        while (!s->aborted->load() && fut.wait_for(ms(20)) != kReady) {
        }
        if (s->aborted->load())
          fut.cancel(), cq.Shutdown(true);
        else {
          Status st = fut.get();
          p->set_settle_status(ToAckStatus(st)),
              p->set_settle_error_code(ToGrpcErrorCode(st.code())),
              p->set_settle_error_message(st.message());
        }
      }
    }
  }
  s->SetCq(std::nullopt),
      cq.Shutdown(
          s->aborted->load(), [&] { return s->aborted->load(); }, sec(5));
}

}  // namespace

struct PubSubTestProxy::SyncCallRegistry {
  using RegPtr = SP<SyncCallRegistry>;
  Mutex mu;
  CV cv;
  bool down{false};
  u64 next_id{1};
  int workers{0};
  Map<u64, CallPtr> active;

  struct Guard {
    RegPtr r;
    CallPtr s;
    ~Guard() {
      if (s) s->MarkDone();
      WithLock(r->mu, [&] { --r->workers; }), r->cv.notify_all();
    }
  };
  void AbortAll() {
    for (auto const& kv : WithLock(mu, [&] { return down = true, active; }))
      kv.second->Abort();
  }
  template <typename Fn>
  static void Reap(RegPtr const& r, Fn fn) {
    if (!r) return std::thread(std::move(fn)).detach();
    WithLock(r->mu, [&] { ++r->workers; });
    std::thread([r, f = std::move(fn)]() mutable {
      Guard g{r, {}};
      auto t = std::move(f);
      t();
    }).detach();
  }
  template <typename Fn>
  static gs Run(RegPtr const& r, SCtx ctx, CallPtr const& s, Fn fn) {
    u64 id = WithLock(r->mu, [&]() -> u64 {
      return r->down ? 0
                     : (r->active.emplace(r->next_id, s), ++r->workers,
                        r->next_id++);
    });
    if (id == 0) return ShuttingDown();
    std::thread w([r, s, f = std::move(fn)]() mutable {
      Guard g{r, s};
      auto t = std::move(f);
      t(s);
    });
    bool const cancelled =
        !WaitPoll(s->mu, s->cv, [&] { return s->done; }, ctx, s->aborted.get());
    bool shut = WithLock(r->mu, [&] { return r->active.erase(id), r->down; });
    if (cancelled)
      return s->Abort(), w.detach(),
             shut ? ShuttingDown() : gs(SC::CANCELLED, "rpc cancelled");
    return w.join(), gs::OK;
  }
};
struct PubSubTestProxy::PublisherHandle
    : std::enable_shared_from_this<PublisherHandle> {
  using RegPtr = SyncCallRegistry::RegPtr;
  using OptPub = Opt<Publisher>;
  using OptTP = Opt<TP>;
  using FutVec = Vec<future<void>>;
  PublisherHandle(Topic t, Options o, bool m, sz n, RegPtr sc)
      : topic(std::move(t)),
        cq_(std::make_unique<CqPool>(n)),
        opts(std::move(o)),
        mon(m),
        calls(std::move(sc)) {
    auto constexpr kMax = (std::numeric_limits<sz>::max)();
    using FPA = FullPublisherAction;
    using FPAO = FullPublisherActionOption;
    may_block = (!opts.has<FPAO>() || opts.get<FPAO>() == FPA::kBlocks) &&
                ((opts.has<MaxPendingMessagesOption>() &&
                  opts.get<MaxPendingMessagesOption>() < kMax) ||
                 (opts.has<MaxPendingBytesOption>() &&
                  opts.get<MaxPendingBytesOption>() < kMax));
    opts.set<GrpcCompletionQueueOption>(cq_->cq);
    auto rp = opts.has<RetryPolicyOption>()
                  ? opts.get<RetryPolicyOption>()
                  : std::make_shared<LimitedTimeRetryPolicy>(sec(60));
    opts.set<RetryPolicyOption>(
        std::make_shared<AbortRetry>(std::move(rp), stop_retry));
    pub.emplace(MakePublisherConnection(topic, opts));
  }
  void OnAdmit(future<void> w) {
    Lock lk(mu);
    EraseIf(watchers, [](auto const& f) { return !f.valid() || f.is_ready(); });
    EraseIf(flight_slots, [](auto const& x) { return x.expired(); });
    if (w.valid()) watchers.push_back(std::move(w));
    --in_flight, cv.notify_all();
  }
  SlotVec PendingSlots() {
    SlotVec out;
    for (auto const& kv : slots) out.push_back(kv.second);
    for (auto const& ws : flight_slots)
      if (auto s = ws.lock()) out.push_back(std::move(s));
    return out;
  }
  SlotVec MarkAborted() {
    stopped = closed = true, aborted.store(true), stop_retry->store(true);
    auto out = PendingSlots();
    return slots.clear(), flight_slots.clear(), cv.notify_all(), out;
  }
  template <class Stop>
  bool DrainFlight(ULock& lk, Stop stop, OptTP dl = {}, bool cancel = false) {
    while (in_flight > 0 && !stop() && (!dl || Clock::now() < *dl)) {
      auto fp = pub;
      lk.unlock();
      if (fp) fp->Flush();
      if (cancel && cq_) cq_->cq.CancelAll();
      if (lk.lock(); in_flight == 0 || stop()) break;
      auto step =
          dl ? std::min(ms(20), duration_cast<ms>(*dl - Clock::now())) : ms(20);
      if (step <= ms(0)) break;
      cv.wait_for(lk, step, [&] { return in_flight == 0 || stop(); });
    }
    return in_flight == 0;
  }
  gs Flush(SCtx ctx, bool stop_pub) {
    OptPub p;
    SlotVec pending;
    {
      ULock lk(mu);
      if (stop_pub) stopped = true;
      DrainFlight(lk, [&] {
        return closed || aborted.load() || (ctx && ctx->IsCancelled());
      });
      if (aborted.load()) return ShuttingDown();
      if (ctx && ctx->IsCancelled()) return gs(SC::CANCELLED, "cancelled");
      if (closed) return gs::OK;
      p = pub, pending = PendingSlots();
    }
    if (p) p->Flush();
    for (auto const& s : pending)
      if (!WaitPoll(s->mu, s->cv, [&] { return s->done; }, ctx, &aborted))
        return aborted.load() ? ShuttingDown()
                              : gs(SC::CANCELLED, "flush cancelled");
    return gs::OK;
  }
  void Abort() {
    OptPub fp;
    SlotVec abort_slots;
    WithLock(mu, [&] { fp = pub, abort_slots = MarkAborted(); });
    NotifySlots(abort_slots);
    if (fp) fp->Flush();
    if (cq_) cq_->cq.CancelAll();
  }
  void Drop(OptPub& p, FutVec& ws) {
    ws.clear(), p.reset(), WithLock(bmu, [&] { bpub.reset(); });
  }
  void Reap() {
    auto self = shared_from_this();
    SyncCallRegistry::Reap(calls, [self] {
      OptPub p;
      FutVec ws;
      {
        ULock lk(self->mu);
        self->DrainFlight(lk, [] { return false; }, {}, true);
        p = std::exchange(self->pub, std::nullopt), ws.swap(self->watchers);
        self->slots.clear(), self->flight_slots.clear();
      }
      self->Drop(p, ws);
      if (self->cq_) self->cq_->Shutdown(true);
    });
  }
  void Shutdown(bool drain, OptTP abort_dl = {}) {
    OptPub p;
    FutVec pw;
    SlotVec abort_slots;
    bool reap = false;
    auto init_dl = [&] {
      return abort_dl ? *abort_dl : (Clock::now() + ms(100));
    };
    OptTP dl;
    {
      ULock lk(mu);
      if (shutting) return;
      shutting = stopped = closed = true;
      if (!drain)
        abort_slots = MarkAborted();
      else if (!aborted.load())
        DrainFlight(lk, [this] { return aborted.load(); });
      if (aborted.load())
        dl = init_dl(), reap = !DrainFlight(lk, [] { return false; }, dl, true);
      if (!reap)
        p = std::exchange(pub, std::nullopt), pw.swap(watchers), slots.clear(),
        flight_slots.clear();
    }
    NotifySlots(abort_slots);
    if (reap) return Reap();
    if (drain && !aborted.load()) {
      if (p) p->Flush();
      for (auto& w : pw)
        while (w.valid() && !aborted.load())
          if (w.wait_for(ms(20)) == kReady) w.get();
    }
    if (Drop(p, pw); !cq_) return;
    if (!aborted.load()) return cq_->Shutdown(true);
    if (!dl) dl = init_dl();
    cq_->BeginShutdown(true);
    auto rem = std::max(ms(0), duration_cast<ms>(*dl - Clock::now()));
    cq_->WaitIdle(rem) ? cq_->Join() : Reap();
  }

  Topic topic;
  UPtr<CqPool> cq_;
  Options opts;
  bool mon, may_block{false};
  RegPtr calls;
  OptPub pub;
  Mutex bmu, mu;
  Opt<BlockingPublisher> bpub;
  CV cv;
  bool stopped{false}, closed{false}, shutting{false};
  AtBool aborted{false};
  AbPtr stop_retry = std::make_shared<AtBool>(false);
  sz in_flight{0};
  u64 next_slot{1};
  Map<Str, SlotPtr> slots;
  Vec<std::weak_ptr<PubSlot>> flight_slots;
  FutVec watchers;
};

Status ValidateListenPort(int p) {
  return (p < 0 || p > 65535) ? Status(K::kInvalidArgument,
                                       "invalid --port " + std::to_string(p) +
                                           ": must be in range [0, 65535]")
                              : Status();
}
StatusOr<Topic> ParseTopicName(Str const& t, Str const& p) {
  return ParseRes<Topic>(t, p, "topics", "topic");
}
StatusOr<Subscription> ParseSubscriptionName(Str const& s, Str const& p) {
  return ParseRes<Subscription>(s, p, "subscriptions", "subscription");
}
i32 ToGrpcErrorCode(StatusCode c) {
  auto v = static_cast<i32>(c);
  return (v < 0 || v > 16) ? static_cast<i32>(SC::UNKNOWN) : v;
}

AckEv::AckStatus ToAckStatus(Status const& st) {
  if (st.ok()) return AckEv::ACK_STATUS_SUCCESS;
  if (st.message().find("INVALID_ACK_ID") != Str::npos)
    return AckEv::ACK_STATUS_INVALID_ACK_ID;
  auto c = st.code();
  return c == K::kPermissionDenied     ? AckEv::ACK_STATUS_PERMISSION_DENIED
         : c == K::kFailedPrecondition ? AckEv::ACK_STATUS_FAILED_PRECONDITION
         : c == K::kInvalidArgument    ? AckEv::ACK_STATUS_INVALID_ACK_ID
         : c == K::kUnavailable        ? AckEv::ACK_STATUS_UNAVAILABLE
         : c == K::kUnknown            ? AckEv::ACK_STATUS_UNKNOWN
                                       : AckEv::ACK_STATUS_OTHER;
}

RpPtr MakeCustomRetryPolicy(Vec<i32> codes, i32 ma, i64 tt, bool ok) {
  return std::make_shared<CustomRetry>(StrSet(codes.begin(), codes.end()),
                                       ma > 0 ? Opt<i32>(ma - 1) : std::nullopt,
                                       tt > 0 ? Opt<ms>(ms(tt)) : std::nullopt,
                                       ok);
}

Options BuildPublisherOptions(pb::CreatePublisherRequest const& r) {
  Options o;
  ApplyEpTraceHeaders(r, o), ApplyRetryOpts<true>(r, o, false);
  if (r.has_count_threshold())
    o.set<MaxBatchMessagesOption>(PosSz(r.count_threshold()));
  if (r.has_byte_threshold())
    o.set<MaxBatchBytesOption>(PosSz(r.byte_threshold()));
  if (r.has_delay_threshold_ms())
    o.set<MaxHoldTimeOption>(PosMs(r.delay_threshold_ms()));
  if (r.has_max_outstanding_messages())
    o.set<MaxPendingMessagesOption>(LimSz(r.max_outstanding_messages()));
  if (r.has_max_outstanding_bytes())
    o.set<MaxPendingBytesOption>(LimSz(r.max_outstanding_bytes()));
  using FPA = FullPublisherAction;
  if (auto b = r.limit_exceeded_behavior();
      b == pb::FLOW_CONTROL_LIMIT_BEHAVIOR_IGNORE)
    o.set<FullPublisherActionOption>(FPA::kIgnored);
  else if (b == pb::FLOW_CONTROL_LIMIT_BEHAVIOR_BLOCK)
    o.set<FullPublisherActionOption>(FPA::kBlocks);
  else if (b == pb::FLOW_CONTROL_LIMIT_BEHAVIOR_SIGNAL_ERROR)
    o.set<FullPublisherActionOption>(FPA::kRejects);
  if (r.has_enable_message_ordering())
    o.set<MessageOrderingOption>(r.enable_message_ordering());
  if (r.enable_compression()) {
    o.set<CompressionThresholdOption>(PosSz(r.compression_bytes_threshold()));
    if (auto a = r.compression_algorithm(); a == pb::COMPRESSION_ALGORITHM_GZIP)
      o.set<CompressionAlgorithmOption>(GRPC_COMPRESS_GZIP);
    else if (a == pb::COMPRESSION_ALGORITHM_DEFLATE)
      o.set<CompressionAlgorithmOption>(GRPC_COMPRESS_DEFLATE);
  }
  if (r.has_num_workers() && r.num_workers() > 0)
    o.set<GrpcBackgroundThreadPoolSizeOption>(PosSz(r.num_workers()));
  return o;
}

Options BuildSubscriberOptions(StartCmd const& c) {
  Options o;
  ApplyEpTraceHeaders(c, o), ApplyRetryOpts<false>(c, o, false);
  if (c.has_num_streams() && c.num_streams() > 0)
    o.set<GrpcNumChannelsOption>(static_cast<int>(c.num_streams()));
  if (c.has_max_concurrency() && c.max_concurrency() >= 0)
    o.set<MaxConcurrencyOption>(PosSz(c.max_concurrency()));
  if (c.has_max_outstanding_messages())
    o.set<MaxOutstandingMessagesOption>(c.max_outstanding_messages());
  if (c.has_max_outstanding_bytes())
    o.set<MaxOutstandingBytesOption>(c.max_outstanding_bytes());
  ApplyLeaseOpts(c, o);
  if (c.has_shutdown_polling_period_ms())
    o.set<ShutdownPollingPeriodOption>(PosMs(c.shutdown_polling_period_ms()));
  return o;
}

Options BuildUnaryPullOptions(pb::UnaryPullRequest const& r) {
  Options o;
  ApplyEpTraceHeaders(r, o), ApplyRetryOpts<true>(r, o, true);
  return ApplyLeaseOpts(r, o), o;
}

PubSubTestProxy::PubSubTestProxy(SP<TraceStore> trace_store,
                                 ShutdownCallback shutdown_callback)
    : trace_store_(std::move(trace_store)),
      shutdown_callback_(std::move(shutdown_callback)),
      sync_calls_(std::make_shared<SyncCallRegistry>()) {}
PubSubTestProxy::~PubSubTestProxy() {
  InitiateShutdown(), WaitForSyncWorkers(ms(2000));
}

gs PubSubTestProxy::GetCapabilities(SCtx, Req<pb::GetCapabilitiesRequest>,
                                    pb::GetCapabilitiesResponse* r) {
  r->set_client_language("cpp"),
      r->set_client_library("google-cloud-cpp/pubsub"),
      r->set_client_version(version_string()), r->set_supports_monitoring(true),
      r->set_supports_unary_pull(true), r->set_supports_blocking_publish(true),
      r->set_supports_publish_flow_control(true),
      r->set_supports_publish_compression(true),
      r->set_supports_ordering_opt_in(true),
      r->set_supports_subscriber_ordering(true),
      r->set_supports_auto_nack_on_drop(true),
      r->set_supports_ack_with_result(true),
      r->set_supports_custom_retry_codes(true),
      r->set_supports_exactly_once_delivery(true),
      r->set_supports_resume_publish(true);
  auto* d = r->mutable_defaults();
  d->set_publish_batch_count_threshold(100), d->set_publish_batch_delay_ms(10),
      d->set_publish_batch_byte_threshold(1024 * 1024),
      d->set_publish_flow_control_max_messages(-1),
      d->set_publish_flow_control_max_bytes(-1),
      d->set_publish_flow_control_behavior(
          pb::FLOW_CONTROL_LIMIT_BEHAVIOR_BLOCK),
      d->set_publish_compression_algorithm(pb::COMPRESSION_ALGORITHM_DEFLATE),
      d->set_publish_total_timeout_ms(60000),
      d->set_retry_initial_delay_ms(100), d->set_retry_max_delay_ms(60000),
      d->set_retry_delay_multiplier(4.0),
      d->set_subscriber_max_outstanding_messages(1000),
      d->set_subscriber_max_outstanding_bytes(104857600),
      d->set_subscriber_min_ack_extension_ms(60000),
      d->set_subscriber_max_ack_extension_ms(600000),
      d->set_subscriber_default_stream_ack_deadline_seconds(10);
  return gs::OK;
}

gs PubSubTestProxy::ResetTraces(SCtx, Req<pb::ResetTracesRequest>,
                                pb::ResetTracesResponse*) {
  return trace_store_ ? (trace_store_->Clear(), gs::OK) : gs::OK;
}
gs PubSubTestProxy::DrainTraces(SCtx, Req<pb::DrainTracesRequest>,
                                pb::DrainTracesResponse* response) {
  if (trace_store_)
    for (auto& s : trace_store_->Drain()) *response->add_spans() = std::move(s);
  return gs::OK;
}
gs PubSubTestProxy::CreatePublisher(SCtx, Req<pb::CreatePublisherRequest> req,
                                    pb::CreatePublisherResponse* resp) {
  if (shutting_down_.load()) return ShuttingDown();
  if (auto st = ValidateEndpoint(req->endpoint(), req->use_emulator_env());
      !st.ok())
    return st;
  auto topic = ParseTopicName(req->topic(), req->project_id());
  if (!topic) return gs(SC::INVALID_ARGUMENT, topic.status().message());
  sz workers = (req->has_num_workers() && req->num_workers() > 0)
                   ? std::min<sz>(64, PosSz(req->num_workers()))
                   : DefaultWorkers(8);
  auto handle = std::make_shared<PublisherHandle>(
      *std::move(topic), BuildPublisherOptions(*req), req->enable_monitoring(),
      workers, sync_calls_);
  Str id = req->client_id();
  auto st = WithLock(publishers_mu_, [&] {
    if (shutting_down_.load()) return ShuttingDown();
    if (id.empty() || publishers_.count(id) > 0)
      id = "pub-" + std::to_string(next_publisher_id_++);
    return publishers_.emplace(id, handle), gs::OK;
  });
  return st.ok() ? (resp->set_publisher_id(id), gs::OK)
                 : (handle->Shutdown(false), st);
}
gs PubSubTestProxy::Publish(SCtx ctx, Req<pb::PublishRequest> req,
                            pb::PublishResponse* resp) {
  auto h = GetPublisher(req->publisher_id());
  if (!h) return PubNotFound(req->publisher_id());
  std::map<Str, Str> attrs(req->attributes().begin(), req->attributes().end());
  for (auto const& raw : req->raw_attributes()) attrs[raw.key()] = raw.value();
  auto const expected_attrs = attrs;
  MessageBuilder builder;
  builder.SetData(req->data()).SetAttributes(attrs.begin(), attrs.end());
  if (!req->ordering_key().empty()) builder.SetOrderingKey(req->ordering_key());
  auto msg = std::move(builder).Build();
  bool const mutated = (attrs != expected_attrs);
  Opt<BlockingPublisher> bp;
  Opt<Publisher> pub;
  SlotPtr slot;
  Str tok;
  {
    Lock lk(h->mu);
    if (h->closed || (!req->use_blocking_publisher() && !h->pub))
      return PubNotFound(req->publisher_id());
    if (h->stopped)
      return resp->set_error_code(ToGrpcErrorCode(K::kFailedPrecondition)),
             resp->set_error_message("publisher is stopped"), gs::OK;
    if (req->use_blocking_publisher()) {
      Lock blk(h->bmu);
      if (!h->bpub) h->bpub.emplace(MakeBlockingPublisherConnection(h->opts));
      bp = h->bpub;
    } else {
      ++h->in_flight, pub = h->pub, slot = std::make_shared<PubSlot>();
      slot->caller_attributes_mutated = mutated;
      if (req->async_fire())
        tok = "pub-" + std::to_string(h->next_slot++),
        h->slots.emplace(tok, slot);
      h->flight_slots.push_back(slot);
    }
  }
  if (bp) {
    auto cs = std::make_shared<SyncState>();
    Options co;
    co.set<RetryPolicyOption>(std::make_shared<AbortRetry>(
        h->opts.get<RetryPolicyOption>(), cs->aborted));
    auto res = std::make_shared<StatusOr<Str>>(
        Status(K::kInternal, "publish not resolved"));
    auto st = SyncCallRegistry::Run(
        sync_calls_, ctx, cs,
        [bp = std::move(bp), topic = h->topic, m = std::move(msg),
         co = std::move(co), mon = h->mon, tp = req->parent_traceparent(),
         ts = req->parent_tracestate(), res](CallPtr const&) mutable {
          TraceScope t(mon, tp, ts);
          *res = bp->Publish(std::move(topic), std::move(m), std::move(co));
        });
    return st.ok() ? (FillPubResp(*res, mutated, resp), gs::OK) : st;
  }
  struct AdmitGuard {
    PubPtr h;
    future<void> w;
    AdmitGuard(AdmitGuard&&) = default;
    ~AdmitGuard() {
      if (h) h->OnAdmit(std::move(w));
    }
  } guard{h, {}};
  auto admit = [pub = std::move(pub), m = std::move(msg), slot, mon = h->mon,
                tp = req->parent_traceparent(), ts = req->parent_tracestate(),
                guard = std::move(guard)](CallPtr const&) mutable {
    if (guard.h->aborted.load()) return;
    TraceScope t(mon, tp, ts);
    guard.w = pub->Publish(std::move(m)).then([slot](auto f) {
      auto res = f.get();
      WithLock(slot->mu,
               [&] { slot->result = std::move(res), slot->done = true; }),
          slot->cv.notify_all();
    });
  };
  if (auto st = h->may_block
                    ? SyncCallRegistry::Run(sync_calls_, ctx,
                                            std::make_shared<SyncState>(),
                                            std::move(admit))
                    : (admit(nullptr), gs::OK);
      !st.ok())
    return st;
  if (req->async_fire())
    return resp->set_publish_token(tok),
           resp->set_caller_attributes_mutated(mutated), gs::OK;
  return WaitSlot(slot, ctx, h->aborted, SC::UNAVAILABLE, "publish aborted",
                  resp);
}
gs PubSubTestProxy::AwaitPublish(SCtx ctx, Req<pb::AwaitPublishRequest> req,
                                 pb::PublishResponse* resp) {
  auto h = GetPublisher(req->publisher_id());
  if (!h) return PubNotFound(req->publisher_id());
  auto slot = WithLock(h->mu, [&]() -> SlotPtr {
    auto it = h->slots.find(req->publish_token());
    return it == h->slots.end() ? nullptr : it->second;
  });
  if (!slot)
    return gs(SC::NOT_FOUND,
              "publish_token not found: " + req->publish_token());
  auto st = WaitSlot(slot, ctx, h->aborted, SC::CANCELLED,
                     "AwaitPublish cancelled", resp);
  return st.ok()
             ? (WithLock(h->mu, [&] { h->slots.erase(req->publish_token()); }),
                gs::OK)
             : st;
}
gs PubSubTestProxy::FlushPublisher(SCtx c, Req<pb::FlushPublisherRequest> r,
                                   pb::FlushPublisherResponse*) {
  auto h = GetPublisher(r->publisher_id());
  return h ? h->Flush(c, false) : PubNotFound(r->publisher_id());
}
gs PubSubTestProxy::StopPublisher(SCtx c, Req<pb::StopPublisherRequest> r,
                                  pb::StopPublisherResponse*) {
  auto h = GetPublisher(r->publisher_id());
  return h ? h->Flush(c, true) : PubNotFound(r->publisher_id());
}
gs PubSubTestProxy::ResumePublish(SCtx, Req<pb::ResumePublishRequest> req,
                                  pb::ResumePublishResponse*) {
  auto h = GetPublisher(req->publisher_id());
  if (!h) return PubNotFound(req->publisher_id());
  auto pub = WithLock(h->mu, [&]() -> Opt<Publisher> {
    return (h->closed || !h->pub) ? std::nullopt : h->pub;
  });
  return pub ? (pub->ResumePublish(req->ordering_key()), gs::OK)
             : PubNotFound(req->publisher_id());
}
gs PubSubTestProxy::ClosePublisher(SCtx, Req<pb::ClosePublisherRequest> req,
                                   pb::ClosePublisherResponse*) {
  auto h = WithLock(publishers_mu_, [&]() -> PubPtr {
    auto it = publishers_.find(req->publisher_id());
    if (it == publishers_.end()) return nullptr;
    auto x = std::move(it->second);
    return publishers_.erase(it), closing_publishers_.insert(x), x;
  });
  if (!h) return PubNotFound(req->publisher_id());
  struct Guard {
    PubSubTestProxy* p;
    PubPtr h;
    ~Guard() {
      WithLock(p->publishers_mu_, [&] { p->closing_publishers_.erase(h); });
    }
  } g{this, h};
  return h->Shutdown(true), gs::OK;
}
gs PubSubTestProxy::SubscriberSession(SCtx ctx, SubState::Stream* stream) {
  if (shutting_down_.load()) return ShuttingDown();
  SubCmd first;
  if (!stream->Read(&first) || !first.has_start())
    return gs(SC::INVALID_ARGUMENT,
              "first command must be StartSubscriberCommand");
  auto const& start = first.start();
  Subscription sub("p", "s");
  if (auto st = ResolveSub(start, &sub); !st.ok()) return st;
  auto cq = std::make_unique<CqPool>(DefaultWorkers(16));
  auto opts = BuildSubscriberOptions(start);
  opts.set<GrpcCompletionQueueOption>(cq->cq);
  Opt<Subscriber> subscriber(MakeSubscriberConnection(sub, std::move(opts)));
  auto s = std::make_shared<SubState>(stream, start);
  u64 sid = WithLock(sessions_mu_, [&]() -> u64 {
    return shutting_down_.load() ? 0 : next_session_id_++;
  });
  if (sid == 0) return subscriber.reset(), cq->Shutdown(true), ShuttingDown();
  auto fut = start.use_exactly_once()
                 ? subscriber->Subscribe(
                       [s](Message const& m, ExactlyOnceAckHandler h) {
                         OnSubMsg(s, m, std::move(h));
                       })
                 : subscriber->Subscribe([s](Message const& m, AckHandler h) {
                     OnSubMsg(s, m, std::move(h));
                   });
  s->dfut = fut.then([s](future<Status> f) {
    Status st(K::kCancelled, "");
    try {
      st = f.get();
    } catch (std::exception const& ex) {
      st = Status(K::kCancelled, ex.what());
    }
    WithLock(s->dmu, [&] { s->dst = std::move(st), s->finished = true; }),
        s->dcv.notify_all();
  });
  WithLock(sessions_mu_, [&] {
    shutting_down_.load()
        ? s->AbortAndCancel()
        : (void)active_sessions_.emplace(sid, [s] { s->AbortAndCancel(); });
  });
  auto cleanup = [&] {
    WithLock(sessions_mu_, [&] { active_sessions_.erase(sid); });
    WithLock(s->cmu, [&] { s->can_c = false; }), s->SetFlag(&SubState::sclosed);
  };
  SubEv started_ev;
  started_ev.mutable_started();
  if (bool const sent = s->SendEvent(started_ev);
      s->SetFlag(&SubState::started), !sent)
    return s->AbortAndCancel(), s->WaitFinished(ms(500)), subscriber.reset(),
           cq->Shutdown(true), cleanup(),
           gs(SC::CANCELLED, "failed to send StartedEvent");
  std::thread reader([stream, s, ctx] {
    for (SubCmd cmd; stream->Read(&cmd);) {
      if (cmd.has_settle()) HandleSettle(s, cmd.settle());
      if (cmd.has_stop())
        WithLock(s->pmu, [&] { s->stopping.store(true); }), s->TriggerCancel();
    }
    if (ctx && ctx->IsCancelled()) s->MarkAborted();
    s->Clear(), s->TriggerCancel();
  });
  auto aborted = [&] {
    return shutting_down_.load() || s->aborted.load() ||
           (ctx && ctx->IsCancelled());
  };
  for (Opt<TP> idle; !s->IsFinished() && !aborted(); s->WaitFinished(ms(50)))
    if (s->CanIdleBail() && s->callbacks.load() == 0) {
      if (!idle) idle = Clock::now();
      if (Clock::now() - *idle >= ms(300)) break;
    } else
      idle.reset();
  if (!s->IsFinished() && aborted())
    s->WaitFinished(shutting_down_.load() ? ms(150) : ms(500));
  WaitPoll(s->amu, s->acv, [&] { return s->acks == 0 || aborted(); }, nullptr);
  s->Clear(), subscriber.reset(),
      cq->Shutdown(aborted() || !s->IsFinished(), aborted, sec(2));
  bool const finished = s->IsFinished(), cancelled = ctx && ctx->IsCancelled();
  if (!cancelled) {
    SubEv ev;
    auto* st = ev.mutable_stopped();
    st->set_error_code(
        ToGrpcErrorCode(finished ? s->dst.code() : K::kCancelled));
    st->set_error_message(finished ? s->dst.message()
                                   : "session cancelled on shutdown");
    st->set_total_delivered(s->delivered.load()), s->SendEvent(ev);
  }
  if (reader.joinable()) reader.join();
  return cleanup(), cancelled ? gs(SC::CANCELLED, "session cancelled") : gs::OK;
}
gs PubSubTestProxy::UnaryPull(SCtx ctx, Req<pb::UnaryPullRequest> req,
                              pb::UnaryPullResponse* resp) {
  if (shutting_down_.load()) return ShuttingDown();
  Subscription sub("p", "s");
  if (auto st = ResolveSub(*req, &sub); !st.ok()) return st;
  auto opts = BuildUnaryPullOptions(*req);
  if (!req->has_total_timeout_ms() && !req->has_max_attempts() && ctx &&
      ctx->deadline() != (std::chrono::system_clock::time_point::max)()) {
    auto rem =
        duration_cast<ms>(ctx->deadline() - std::chrono::system_clock::now());
    opts.set<RetryPolicyOption>(MakeCustomRetryPolicy(
        {req->retry_codes().begin(), req->retry_codes().end()}, 0,
        std::clamp<i64>(rem.count(), 100, 60000), true));
  }
  auto cs = std::make_shared<SyncState>();
  opts.set<RetryPolicyOption>(
      std::make_shared<AbortRetry>(opts.get<RetryPolicyOption>(), cs->aborted));
  auto out = std::make_shared<pb::UnaryPullResponse>();
  auto st = SyncCallRegistry::Run(
      sync_calls_, ctx, cs,
      [sub = std::move(sub), opts = std::move(opts),
       lm = req->lease_extension_ms(), sa = req->settle_action(),
       out](CallPtr const& s) mutable {
        RunPull(std::move(sub), std::move(opts), lm, sa, s, out.get());
      });
  return st.ok() ? (*resp = std::move(*out), gs::OK) : st;
}
void PubSubTestProxy::InitiateShutdown() {
  shutting_down_.store(true);
  for (auto const& kv :
       WithLock(sessions_mu_, [&] { return active_sessions_; }))
    if (kv.second) kv.second();
  if (sync_calls_) sync_calls_->AbortAll();
  auto [pubs, closing] = WithLock(publishers_mu_, [&] {
    return std::pair{std::exchange(publishers_, {}), closing_publishers_};
  });
  for (auto const& h : closing)
    if (h) h->Abort();
  for (auto const& kv : pubs)
    if (kv.second) kv.second->Abort();
  auto dl = Clock::now() + ms(100);
  for (auto const& kv : pubs)
    if (kv.second) kv.second->Shutdown(false, dl);
  if (trace_store_) trace_store_->Shutdown();
}
bool PubSubTestProxy::WaitForSyncWorkers(ms timeout) {
  if (!sync_calls_) return true;
  ULock lk(sync_calls_->mu);
  return sync_calls_->cv.wait_for(lk, timeout,
                                  [&] { return sync_calls_->workers == 0; });
}
gs PubSubTestProxy::ShutdownProxy(SCtx, Req<pb::ShutdownProxyRequest> req,
                                  pb::ShutdownProxyResponse*) {
  InitiateShutdown();
  if (shutdown_callback_)
    shutdown_callback_(ms(req ? std::max<i64>(0, req->grace_period_ms()) : 0));
  return gs::OK;
}
PubSubTestProxy::PubPtr PubSubTestProxy::GetPublisher(Str const& id) {
  return WithLock(publishers_mu_, [&] {
    auto it = publishers_.find(id);
    return it == publishers_.end() ? nullptr : it->second;
  });
}

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace test_proxy
}  // namespace pubsub
}  // namespace cloud
}  // namespace google

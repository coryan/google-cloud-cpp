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
#include "google/cloud/pubsub/options.h"
#include "google/cloud/pubsub/test_proxy/trace_exporter.h"
#include "google/cloud/common_options.h"
#include "google/cloud/grpc_options.h"
#include "google/cloud/testing_util/scoped_environment.h"
#include "google/cloud/version.h"
#include "pubsub/conformance/v1/proxy.grpc.pb.h"
#include <grpc/compression.h>
#include <google/pubsub/v1/pubsub.grpc.pb.h>
#include <gmock/gmock.h>
#include <grpcpp/grpcpp.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span_metadata.h>
#include <condition_variable>
#include <limits>
#include <map>
#include <thread>
#include <tuple>
#include <utility>

namespace google::cloud::pubsub::test_proxy {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN
namespace {

using namespace ::pubsub::conformance::v1;
namespace ps = ::google::pubsub::v1;
namespace ot = opentelemetry::trace;
using opentelemetry::nostd::span;
using opentelemetry::nostd::string_view;
using Act = SettleMessageCommand;
using Mode = StartSubscriberCommand;
using Ack = SettlementResultEvent;
using SC = grpc::StatusCode;
using gs = grpc::Status;
using Str = std::string;
template <class T>
using Vec = std::vector<T>;
using Mutex = std::mutex;
using AtBool = std::atomic<bool>;
using i64 = std::int64_t;
using sz = std::size_t;
using Ctx = grpc::ClientContext;
using ms = std::chrono::milliseconds;
using sec = std::chrono::seconds;
using Clock = std::chrono::steady_clock;
using Lock = std::lock_guard<Mutex>;
using ULock = std::unique_lock<Mutex>;
using StrVec = Vec<Str>;
using RecvVec = Vec<ps::ReceivedMessage>;
constexpr auto kBlk = FLOW_CONTROL_LIMIT_BEHAVIOR_BLOCK;
constexpr auto kInter = Mode::AUTO_SETTLE_MODE_INTERACTIVE;
constexpr auto kAck = Act::SETTLE_ACTION_ACK, kNack = Act::SETTLE_ACTION_NACK,
               kAckR = Act::SETTLE_ACTION_ACK_WITH_RESULT,
               kNackR = Act::SETTLE_ACTION_NACK_WITH_RESULT,
               kDrop = Act::SETTLE_ACTION_RETURN_WITHOUT_SETTLE,
               kUnspec = Act::SETTLE_ACTION_UNSPECIFIED;
constexpr auto kAOk = Ack::ACK_STATUS_SUCCESS,
               kAPerm = Ack::ACK_STATUS_PERMISSION_DENIED,
               kAInv = Ack::ACK_STATUS_INVALID_ACK_ID,
               kAFailPre = Ack::ACK_STATUS_FAILED_PRECONDITION,
               kAUnavail = Ack::ACK_STATUS_UNAVAILABLE,
               kAOth = Ack::ACK_STATUS_OTHER,
               kAUnspec = Ack::ACK_STATUS_UNSPECIFIED;
using C = StatusCode;
using FPA = FullPublisherAction;

template <class F>
auto WithLock(Mutex& m, F&& f) -> decltype(f()) {
  Lock lk(m);
  return f();
}
void Sleep(ms d) { std::this_thread::sleep_for(d); }
int Ec(SC c) { return static_cast<int>(c); }

ps::ReceivedMessage MakeMsg(Str a, Str id, Str d, int att = 0) {
  ps::ReceivedMessage r;
  r.set_ack_id(std::move(a)), r.mutable_message()->set_message_id(id);
  r.mutable_message()->set_data(std::move(d));
  if (att > 0) r.set_delivery_attempt(att);
  return r;
}

struct FakePub final : ps::Publisher::Service {
  void FailNext(int n, gs s) {
    WithLock(mu, [&] { fail_n = n, fst = s; });
  }
  void SetStall(int d) {
    WithLock(mu, [&] { stall_ms = d, rel = false; });
  }
  void Release() {
    WithLock(mu, [&] { rel = true; }), cv.notify_all();
  }
  gs Publish(grpc::ServerContext* ctx, ps::PublishRequest const* req,
             ps::PublishResponse* resp) override {
    ULock l(mu);
    if (stall_ms > 0) ++in_stall;
    ++calls;
    if (stall_ms > 0) {
      for (auto dl = Clock::now() + ms(stall_ms);
           !rel && !ctx->IsCancelled() && Clock::now() < dl;)
        cv.wait_for(l, ms(20));
      --in_stall;
    }
    if (fail_n > 0) return (--fail_n, fst);
    for (auto const& m : req->messages())
      published.push_back(m),
          resp->add_message_ids("i" + std::to_string(nid++));
    return gs::OK;
  }
  Mutex mu;
  std::condition_variable cv;
  std::atomic<int> calls{0}, in_stall{0};
  i64 nid{1};
  int fail_n{0}, stall_ms{0};
  bool rel{false};
  gs fst{gs::OK};
  Vec<ps::PubsubMessage> published;
};

struct FakeSub final : ps::Subscriber::Service {
  using SCtx = grpc::ServerContext*;
  using Empty = google::protobuf::Empty*;
  using RW = grpc::ServerReaderWriter<ps::StreamingPullResponse,
                                      ps::StreamingPullRequest>*;
  void AddPull(Str a, Str d, int att = 0) {
    WithLock(mu, [&] {
      pull_q.push_back(MakeMsg(a, "p" + std::to_string(nid++), d, att));
    });
  }
  void AddBatch(RecvVec b) {
    WithLock(mu, [&] { batches.push_back(std::move(b)); }), cv.notify_all();
  }
  void SetErr(gs* d, gs s) {
    WithLock(mu, [&] { *d = s; });
  }
  void FailPull(int n, gs s) {
    WithLock(mu, [&] { pfn = n, pst = s; });
  }
  StrVec NackedCopy() {
    return WithLock(mu, [&] { return nacked; });
  }
  bool WaitIds(StrVec const& v, sz n) {
    ULock l(mu);
    return cv.wait_for(l, sec(2), [&] { return v.size() >= n; });
  }
  gs Pull(SCtx, ps::PullRequest const*, ps::PullResponse* r) override {
    ++pull_calls;
    Lock l(mu);
    if (pfn > 0) return (--pfn, pst);
    if (!pst.ok()) return std::exchange(pst, gs::OK);
    if (!pull_q.empty())
      *r->add_received_messages() = std::move(pull_q.front()),
      pull_q.erase(pull_q.begin());
    return gs::OK;
  }
  template <class R>
  static void Push(StrVec* d, R const& s) {
    d->insert(d->end(), s.begin(), s.end());
  }
  gs Acknowledge(SCtx, ps::AcknowledgeRequest const* r, Empty) override {
    gs st = WithLock(mu, [&] {
      return ack_st.ok() ? (Push(&acked, r->ack_ids()), gs::OK)
                         : std::exchange(ack_st, gs::OK);
    });
    return cv.notify_all(), st;
  }
  gs ModifyAckDeadline(SCtx, ps::ModifyAckDeadlineRequest const* r,
                       Empty) override {
    if (r->ack_deadline_seconds() == 0)
      WithLock(mu, [&] { Push(&nacked, r->ack_ids()); });
    return cv.notify_all(), gs::OK;
  }
  gs StreamingPull(SCtx ctx, RW s) override {
    ps::StreamingPullRequest init;
    if (!s->Read(&init)) return gs::OK;
    if (gs st = WithLock(mu, [&] { return std::exchange(stream_st, gs::OK); });
        !st.ok())
      return st;
    AtBool rd{false};
    std::thread reader([&] {
      for (ps::StreamingPullRequest q; s->Read(&q); cv.notify_all()) {
        Lock l(mu);
        for (auto const& id : q.ack_ids()) acked.push_back(id);
        for (int i = 0; i < q.modify_deadline_ack_ids_size() &&
                        i < q.modify_deadline_seconds_size();
             ++i)
          if (q.modify_deadline_seconds(i) == 0)
            nacked.push_back(q.modify_deadline_ack_ids(i));
      }
      rd.store(true), cv.notify_all();
    });
    for (RecvVec b; !rd.load() && !ctx->IsCancelled(); b.clear()) {
      if (ULock l(mu);
          cv.wait_for(
              l, ms(20),
              [&] { return !batches.empty() || rd || ctx->IsCancelled(); }),
          !batches.empty())
        b = std::move(batches.front()), batches.erase(batches.begin());
      if (b.empty()) continue;
      ps::StreamingPullResponse resp;
      for (auto& m : b) *resp.add_received_messages() = std::move(m);
      if (!s->Write(resp)) break;
    }
    return reader.join(), gs::OK;
  }
  Mutex mu;
  std::condition_variable cv;
  std::atomic<int> pull_calls{0};
  int pfn{0};
  i64 nid{1};
  gs pst{gs::OK}, ack_st{gs::OK}, stream_st{gs::OK};
  RecvVec pull_q;
  Vec<RecvVec> batches;
  StrVec acked, nacked;
};

struct Env {
  using S = PubSubProxy::Stub;
  template <class T>
  using Cb = std::function<void(T*)>;
  using SubStream = std::unique_ptr<
      grpc::ClientReaderWriter<SubscriberCommand, SubscriberEvent>>;
  struct BgPub {
    SC rc{SC::OK};
    std::int32_t err{-1};
    AtBool done{false};
    std::thread t;
    void Start(Env& e, Str const& pub, Str const& d) {
      t = std::thread([this, &e, pub, d] {
        PublishRequest q;
        q.set_publisher_id(pub), q.set_data(d);
        PublishResponse r;
        rc = e.Call(&S::Publish, q, &r), err = r.error_code(), done.store(true);
      });
      EXPECT_FALSE((Sleep(ms(25)), done.load()));
    }
    void Join() {
      if (t.joinable()) t.join();
    }
    bool Bad() const { return rc != SC::OK || err != 0; }
  };
  Env() {
    auto srv = [](grpc::Service* a, grpc::Service* b, auto& out, int* p) {
      grpc::ServerBuilder sb;
      sb.SetMaxReceiveMessageSize(-1).SetMaxSendMessageSize(-1);
      sb.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), p);
      if (sb.RegisterService(a); b) sb.RegisterService(b);
      out = sb.BuildAndStart();
    };
    int p1 = 0, p2 = 0;
    srv(&fpub, &fsub, ps_srv, &p1), ep = "127.0.0.1:" + std::to_string(p1);
    proxy = std::make_unique<PubSubTestProxy>(tracer.store(), [&](ms g) {
      shutdown_called.store(true), last_grace_ms.store(g.count());
    });
    srv(proxy.get(), nullptr, px_srv, &p2);
    stub = PubSubProxy::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(p2), grpc::InsecureChannelCredentials()));
  }
  ~Env() {
    proxy->InitiateShutdown(), fpub.Release(), StopSrv(px_srv), StopSrv(ps_srv);
    EXPECT_TRUE(proxy->WaitForSyncWorkers(ms(2000)));
  }
  static void StopSrv(std::unique_ptr<grpc::Server>& s) {
    if (s)
      s->Shutdown(std::chrono::system_clock::now() + ms(100)), s->Wait(),
          s.reset();
  }
  SC Shutdown(i64 g) {
    ShutdownProxyRequest r;
    return r.set_grace_period_ms(g), Call(&S::ShutdownProxy, r);
  }
  template <class Pred>
  bool WaitPred(Pred p, ms lim = ms(2000)) {
    for (auto dl = Clock::now() + lim; !p() && Clock::now() < dl;) Sleep(ms(5));
    return p();
  }
  bool WaitCalls(int n) {
    return WaitPred([&] { return fpub.calls >= n; });
  }
  bool WaitAck(sz n) { return fsub.WaitIds(fsub.acked, n); }
  bool WaitNack(sz n) { return fsub.WaitIds(fsub.nacked, n); }
  template <class Q, class R>
  SC Call(gs (S::*m)(Ctx*, Q const&, R*), Q const& q, R* r = nullptr) {
    Ctx c;
    R tmp;
    return (stub.get()->*m)(&c, q, r ? r : &tmp).error_code();
  }
  template <class R, class Q>
  R Exec(gs (S::*m)(Ctx*, Q const&, R*), Q const& q, SC w = SC::OK) {
    R r;
    EXPECT_EQ(Call(m, q, &r), w);
    return r;
  }
  int Spans() {
    return Exec(&S::DrainTraces, DrainTracesRequest()).spans_size();
  }
  sz PubCount() {
    return WithLock(fpub.mu, [&] { return fpub.published.size(); });
  }
  auto SubCounts() {
    return WithLock(fsub.mu, [&] {
      return std::pair{fsub.acked.size(), fsub.nacked.size()};
    });
  }
  Str CreatePub(bool mon = false, bool ord = false,
                Cb<CreatePublisherRequest> fn = {}, SC want = SC::OK) {
    CreatePublisherRequest q;
    q.set_topic("t1"), q.set_project_id("p1"), q.set_endpoint(ep),
        q.set_enable_monitoring(mon), q.set_enable_message_ordering(ord);
    if (fn) fn(&q);
    return Exec(&S::CreatePublisher, q, want).publisher_id();
  }
  Str BlkPub(bool blk, int cnt, int delay_ms, int total_ms = 0,
             int retry_ms = 0) {
    return CreatePub(false, false, [&](auto* r) {
      r->set_count_threshold(cnt), r->set_delay_threshold_ms(delay_ms);
      if (total_ms > 0) r->set_total_timeout_ms(total_ms);
      if (retry_ms > 0)
        r->set_initial_retry_delay_ms(retry_ms),
            r->set_max_retry_delay_ms(retry_ms);
      if (blk)
        r->set_max_outstanding_messages(1),
            r->set_limit_exceeded_behavior(kBlk);
    });
  }
  Str BlkWait(bool blk, int tot = 0, int ret = 0) {
    auto p = BlkPub(blk, 1, 0, tot, ret);
    EXPECT_FALSE(Pub(p, "p1", "", true).publish_token().empty());
    EXPECT_TRUE(WaitCalls(1));
    return p;
  }
  PublishResponse Pub(Str const& id, Str const& d, Str const& k = "",
                      bool async = false, bool blk = false, SC want = SC::OK,
                      Cb<PublishRequest> fn = {}) {
    PublishRequest q;
    q.set_publisher_id(id), q.set_data(d), q.set_async_fire(async),
        q.set_use_blocking_publisher(blk);
    if (!k.empty()) q.set_ordering_key(k);
    if (fn) fn(&q);
    return Exec(&S::Publish, q, want);
  }
  template <class Q, class R>
  SC PubOp(gs (S::*m)(Ctx*, Q const&, R*), Str const& id) {
    Q q;
    return q.set_publisher_id(id), Call(m, q);
  }
  SC Resume(Str const& id, Str const& k = "") {
    ResumePublishRequest q;
    q.set_publisher_id(id);
    if (!k.empty()) q.set_ordering_key(k);
    return Call(&S::ResumePublish, q);
  }
  UnaryPullResponse PullOnce(Str sub, Str proj, Act::SettleAction act = kUnspec,
                             Cb<UnaryPullRequest> fn = {}, SC want = SC::OK) {
    UnaryPullRequest q;
    q.set_subscription(std::move(sub)), q.set_project_id(std::move(proj)),
        q.set_endpoint(ep), q.set_settle_action(act);
    if (fn) fn(&q);
    return Exec(&S::UnaryPull, q, want);
  }
  SubStream StartSub(Ctx* ctx, Mode::AutoSettleMode m = kInter,
                     Cb<Mode> fn = {}) {
    auto s = stub->SubscriberSession(ctx);
    SubscriberCommand cmd;
    auto* c = cmd.mutable_start();
    c->set_subscription("s1"), c->set_project_id("p1"), c->set_endpoint(ep),
        c->set_auto_settle_mode(m);
    if (fn) fn(c);
    SubscriberEvent ev;
    EXPECT_TRUE(s->Write(cmd) && s->Read(&ev) && ev.has_started() &&
                ev.started().client_id().empty());
    return s;
  }
  static MessageDeliveredEvent ReadMsg(SubStream const& s) {
    SubscriberEvent ev;
    EXPECT_TRUE(s->Read(&ev) && ev.has_message_delivered());
    return ev.message_delivered();
  }
  static void Settle(SubStream const& s, Str const& tok,
                     Act::SettleAction act) {
    SubscriberCommand c;
    c.mutable_settle()->set_delivery_token(tok),
        c.mutable_settle()->set_action(act);
    ASSERT_TRUE(s->Write(c));
  }
  static SubscriberCommand StopCmd() {
    SubscriberCommand c;
    return c.mutable_stop(), c;
  }
  static SubscriberEvent StopAndRead(SubStream const& s,
                                     Vec<SubscriberEvent>* extra = nullptr) {
    EXPECT_TRUE(s->Write(StopCmd()) && s->WritesDone());
    SubscriberEvent ev;
    while (s->Read(&ev) && !ev.has_stopped())
      if (extra) extra->push_back(ev);
    EXPECT_TRUE(ev.has_stopped());
    return ev;
  }
  ScopedProxyTracerProvider tracer;
  FakePub fpub;
  FakeSub fsub;
  std::unique_ptr<grpc::Server> ps_srv, px_srv;
  Str ep;
  AtBool shutdown_called{false};
  std::atomic<i64> last_grace_ms{-1};
  std::unique_ptr<PubSubTestProxy> proxy;
  std::unique_ptr<S> stub;
};

TEST(HelpersTest, ParseNamesPortAndStatusMappings) {
  EXPECT_TRUE(ValidateListenPort(0).ok() && ValidateListenPort(65535).ok());
  for (int b : {-1, 65536, 99999}) EXPECT_FALSE(ValidateListenPort(b).ok());
  for (auto b : {"", "a/b", "projects/p/subscriptions/s"})
    EXPECT_FALSE(ParseTopicName(b, "p").ok());
  for (auto b : {"", "projects/p/topics/t", "projects//subscriptions/s"})
    EXPECT_FALSE(ParseSubscriptionName(b, "p").ok());
  auto pt = ParseTopicName;
  auto psn = ParseSubscriptionName;
  EXPECT_TRUE(pt("t1", "p1")->FullName() == "projects/p1/topics/t1" &&
              pt("projects/p2/topics/t2", "x")->FullName() ==
                  "projects/p2/topics/t2" &&
              !pt("t", "").ok() &&
              psn("s1", "p1")->FullName() == "projects/p1/subscriptions/s1" &&
              !psn("s", "").ok() && ToGrpcErrorCode(C::kOk) == 0 &&
              ToGrpcErrorCode(C::kUnavailable) == 14);
  using TC = std::tuple<C, char const*, Ack::AckStatus>;
  for (auto [c, m, s] : {TC{C::kOk, "", kAOk},
                         {C::kPermissionDenied, "x", kAPerm},
                         {C::kFailedPrecondition, "x", kAFailPre},
                         {C::kFailedPrecondition, "INVALID_ACK_ID", kAInv},
                         {C::kUnavailable, "x", kAUnavail},
                         {C::kInternal, "x", kAOth}})
    EXPECT_EQ(ToAckStatus(Status(c, m)), s);
}

TEST(HelpersTest, CustomRetryPolicyAndPresenceOptions) {
  auto pr = MakeCustomRetryPolicy({static_cast<std::int32_t>(C::kUnavailable)},
                                  3, 5000, true);
  auto p = pr->clone(), p3 = pr->clone();
  auto unavail = Status(C::kUnavailable, "");
  EXPECT_TRUE(
      p->IsPermanentFailure(Status()) && !p->OnFailure(Status()) &&
      pr->clone()->IsPermanentFailure(Status(C::kDeadlineExceeded, "")) &&
      p3->OnFailure(unavail) && p3->OnFailure(unavail) &&
      !p3->OnFailure(unavail) && p3->IsExhausted() &&
      !MakeCustomRetryPolicy({}, 1, 0, false)->OnFailure(unavail));
  CreatePublisherRequest pq;
  pq.set_delay_threshold_ms(0), pq.set_count_threshold(0),
      pq.set_byte_threshold(0), pq.set_max_outstanding_messages(0),
      pq.set_max_outstanding_bytes(0), pq.set_enable_compression(false),
      pq.set_initial_retry_delay_ms(50),
      pq.set_limit_exceeded_behavior(FLOW_CONTROL_LIMIT_BEHAVIOR_IGNORE);
  auto po = BuildPublisherOptions(pq);
  pq.set_limit_exceeded_behavior(kBlk);
  auto blk_act = BuildPublisherOptions(pq).get<FullPublisherActionOption>();
  pq.set_enable_message_ordering(false), pq.set_max_outstanding_messages(-1),
      pq.set_enable_compression(true),
      pq.set_compression_algorithm(COMPRESSION_ALGORITHM_GZIP),
      pq.set_limit_exceeded_behavior(FLOW_CONTROL_LIMIT_BEHAVIOR_SIGNAL_ERROR);
  auto po2 = BuildPublisherOptions(pq);
  EXPECT_TRUE(po.get<MaxHoldTimeOption>() == ms(0) &&
              po.get<MaxBatchMessagesOption>() == 0u &&
              po.get<MaxBatchBytesOption>() == 0u &&
              po.get<MaxPendingMessagesOption>() == 0u &&
              po.get<MaxPendingBytesOption>() == 0u &&
              po.get<FullPublisherActionOption>() == FPA::kIgnored &&
              po.has<BackoffPolicyOption>() &&
              !po.has<MessageOrderingOption>() &&
              !po.has<CompressionThresholdOption>() &&
              blk_act == FPA::kBlocks && !po2.get<MessageOrderingOption>() &&
              po2.get<MaxPendingMessagesOption>() == sz(-1) &&
              po2.get<FullPublisherActionOption>() == FPA::kRejects &&
              po2.get<CompressionAlgorithmOption>() == GRPC_COMPRESS_GZIP);
  Mode sc;
  sc.set_max_outstanding_messages(0), sc.set_max_outstanding_bytes(0),
      sc.set_max_extension_ms(0), sc.set_shutdown_polling_period_ms(0),
      sc.set_min_duration_per_ack_extension_ms(1200),
      sc.set_max_duration_per_ack_extension_ms(3400), sc.set_max_concurrency(0),
      sc.set_num_streams(2);
  auto so = BuildSubscriberOptions(sc);
  UnaryPullRequest uq;
  uq.set_max_attempts(0), uq.set_total_timeout_ms(0);
  EXPECT_TRUE(
      so.get<MaxOutstandingMessagesOption>() == 0 &&
      so.get<MaxOutstandingBytesOption>() == 0 &&
      so.get<MaxDeadlineTimeOption>() == sec(0) &&
      so.get<MinDeadlineExtensionOption>() == sec(2) &&
      so.get<MaxDeadlineExtensionOption>() == sec(4) &&
      so.get<ShutdownPollingPeriodOption>() == ms(0) &&
      so.get<MaxConcurrencyOption>() == 0u &&
      so.get<GrpcNumChannelsOption>() == 2 &&
      BuildUnaryPullOptions(uq).get<RetryPolicyOption>()->IsExhausted());
  Env env;
  auto try_pub = [&](Str e, bool emu) {
    CreatePublisherRequest r;
    r.set_topic("t1"), r.set_project_id("p1"), r.set_use_emulator_env(emu);
    if (!e.empty()) r.set_endpoint(std::move(e));
    return env.Call(&Env::S::CreatePublisher, r);
  };
  {
    testing_util::ScopedEnvironment emu("PUBSUB_EMULATOR_HOST", std::nullopt);
    EXPECT_EQ(try_pub("", true), SC::INVALID_ARGUMENT);
  }
  testing_util::ScopedEnvironment emu("PUBSUB_EMULATOR_HOST", env.ep);
  EXPECT_TRUE(try_pub("", false) == SC::INVALID_ARGUMENT &&
              try_pub("127.0.0.1:1", false) == SC::FAILED_PRECONDITION &&
              try_pub("", true) == SC::OK);
}

TEST(TraceTest, CaptureTypesLinksOver1000AndCycleFree) {
  std::weak_ptr<opentelemetry::sdk::trace::TracerProvider> weak_prov;
  std::weak_ptr<TraceStore> weak_store;
  {
    ScopedProxyTracerProvider scoped;
    weak_prov = scoped.store()->tracer_provider(), weak_store = scoped.store();
    EXPECT_FALSE(weak_prov.expired());
    auto tr = ot::Provider::GetTracerProvider()->GetTracer("s", "1");
    for (int i = 0; i < 1025; ++i) tr->StartSpan("bulk")->End();
    ASSERT_EQ(scoped.store()->Drain().size(), 1025u);
    auto tgt = tr->StartSpan("tgt");
    auto tctx = tgt->GetContext();
    tgt->End();
    bool const bs[] = {true, false};
    i64 const is[] = {10, 20};
    double const ds[] = {1.25, 2.5};
    string_view const ss[] = {"a", "b"};
    ot::StartSpanOptions opts;
    opts.kind = ot::SpanKind::kClient;
    auto rich = tr->StartSpan("rich",
                              {{"b", span<bool const>(bs)},
                               {"i", span<i64 const>(is)},
                               {"d", span<double const>(ds)},
                               {"s", span<string_view const>(ss)}},
                              {{tctx, {{"lk", "lv"}}}}, opts);
    rich->AddEvent("ev"), rich->SetStatus(ot::StatusCode::kError, "boom"),
        rich->End();
    Str const tid(32, 'a'), pid(16, 'b');
    {
      ScopedTraceParent bad("bad", ""),
          good("00-" + tid + "-" + pid + "-01", "rojo=1");
      Str tp, ts;
      InjectW3CTraceContext(&tp, &ts);
      EXPECT_TRUE(tp.find(tid) != Str::npos && ts == "rojo=1");
      tr->StartSpan("child")->End();
    }
    auto sp = scoped.store()->Drain();
    ASSERT_GE(sp.size(), 3u);
    auto const& at = sp[1].attributes();
    EXPECT_TRUE(sp[1].kind() == SPAN_KIND_CLIENT &&
                sp[1].status_code() == SPAN_STATUS_CODE_ERROR &&
                sp[1].links(0).attributes().at("lk").string_value() == "lv" &&
                at.at("b").bool_list().values_size() == 2 &&
                at.at("i").int64_list().values(1) == 20 &&
                at.at("d").double_list().values(0) == 1.25 &&
                at.at("s").string_list().values(0) == "a" &&
                sp[2].trace_id() == tid && sp[2].parent_span_id() == pid);
    tr->StartSpan("drop")->End(), scoped.store()->Clear();
    EXPECT_TRUE(scoped.store()->Drain().empty());
  }
  EXPECT_TRUE(weak_prov.expired() && weak_store.expired());
}

TEST(ServiceTest, GetCapabilitiesAndDefaults) {
  Env env;
  GetCapabilitiesResponse r;
  ASSERT_EQ(env.Call(&Env::S::GetCapabilities, GetCapabilitiesRequest{}, &r),
            SC::OK);
  auto const& d = r.defaults();
  EXPECT_TRUE(
      r.client_language() == "cpp" && r.supports_monitoring() &&
      r.client_library() == "google-cloud-cpp/pubsub" &&
      r.client_version() == version_string() && r.supports_unary_pull() &&
      r.supports_blocking_publish() && r.supports_publish_compression() &&
      r.supports_subscriber_ordering() && !r.supports_publish_hedging() &&
      d.publish_batch_count_threshold() == 100 &&
      d.publish_batch_byte_threshold() == 1048576 &&
      d.publish_batch_delay_ms() == 10 &&
      d.publish_flow_control_behavior() == kBlk &&
      d.publish_compression_algorithm() == COMPRESSION_ALGORITHM_DEFLATE &&
      d.retry_delay_multiplier() == 4.0 &&
      d.subscriber_max_outstanding_messages() == 1000 &&
      d.subscriber_max_outstanding_bytes() == 104857600 &&
      d.subscriber_min_ack_extension_ms() == 60000 &&
      d.subscriber_max_ack_extension_ms() == 600000);
}

TEST(ServiceTest, PublisherLifecycleOver10MiBRawAttrsAndErrors) {
  Env env;
  auto const pub = env.CreatePub(false, true);
  auto add_raw = [](auto* q, Str k, Str v) {
    auto* r = q->add_raw_attributes();
    r->set_key(std::move(k)), r->set_value(std::move(v));
  };
  Str const tp = "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01";
  auto r1 = env.Pub(
      pub, Str(11u << 20, 'x'), "k1", false, false, SC::OK, [&](auto* q) {
        (*q->mutable_attributes())["k"] = "v";
        add_raw(q, "raw", "raw-val"), q->set_parent_traceparent(tp);
      });
  auto r2 = env.Pub(
      env.CreatePub(false, false, [](auto* r) { r->set_max_attempts(1); }), "",
      "", false, false, SC::OK,
      [&](auto* q) { add_raw(q, "bin", Str("\xff\xfe\x00\x80", 4)); });
  EXPECT_TRUE(r1.error_code() == 0 && !r1.caller_attributes_mutated() &&
              env.Spans() == 0 && !r2.caller_attributes_mutated() &&
              env.Pub(pub, "blk", "", false, true).error_code() == 0);
  env.fpub.FailNext(1, gs(SC::INVALID_ARGUMENT, "bad"));
  EXPECT_NE(env.Pub(pub, "blk", "", false, true).error_code(), 0);
  auto a = env.Pub(pub, "async", "", true);
  ASSERT_FALSE(a.publish_token().empty());
  auto op = [&](auto m) { return env.PubOp(m, pub); };
  EXPECT_TRUE(op(&Env::S::FlushPublisher) == SC::OK &&
              op(&Env::S::StopPublisher) == SC::OK &&
              env.Pub(pub, "x", "", false, true).error_code() ==
                  Ec(SC::FAILED_PRECONDITION));
  AwaitPublishRequest aq;
  aq.set_publisher_id(pub), aq.set_publish_token(a.publish_token());
  PublishResponse ar;
  EXPECT_TRUE(env.Call(&Env::S::AwaitPublish, aq, &ar) == SC::OK &&
              ar.error_code() == 0 && (aq.set_publish_token("bad-tok"), true) &&
              env.Call(&Env::S::AwaitPublish, aq, &ar) == SC::NOT_FOUND &&
              op(&Env::S::ClosePublisher) == SC::OK &&
              op(&Env::S::ClosePublisher) == SC::NOT_FOUND &&
              op(&Env::S::FlushPublisher) == SC::NOT_FOUND &&
              op(&Env::S::StopPublisher) == SC::NOT_FOUND &&
              env.Resume(pub) == SC::NOT_FOUND);
  env.Pub(pub, "x", "", false, true, SC::NOT_FOUND);
  ASSERT_EQ(env.Call(&Env::S::ResetTraces, ResetTracesRequest{}), SC::OK);
  auto r3 = env.Pub(env.CreatePub(true), "traced", "", false, false, SC::OK,
                    [&](auto* q) { q->set_parent_traceparent(tp); });
  EXPECT_TRUE(r3.error_code() == 0 && env.Spans() > 0);
}

TEST(ServiceTest, ClosePublisherFastUnderLargeDelayAndResumeOrdering) {
  Env env;
  auto const p1 = env.CreatePub(false, true, [](auto* r) {
    r->set_delay_threshold_ms(30000), r->set_count_threshold(100);
  });
  EXPECT_TRUE(!env.Pub(p1, "d0", "k0", true).publish_token().empty() &&
              !env.Pub(p1, "d1", "k1", true).publish_token().empty());
  auto t0 = Clock::now();
  EXPECT_TRUE(env.PubOp(&Env::S::ClosePublisher, p1) == SC::OK &&
              Clock::now() - t0 < sec(2) && env.PubCount() == 2u);
  auto const p2 = env.CreatePub(false, true);
  env.fpub.FailNext(1, gs(SC::INVALID_ARGUMENT, "order fail"));
  EXPECT_TRUE(env.Pub(p2, "m1", "k1").error_code() != 0 &&
              env.Pub(p2, "m1", "k1").error_code() != 0 &&
              env.Resume(p2, "k1") == SC::OK &&
              env.Pub(p2, "m2", "k1").error_code() == 0);
}

TEST(ServiceTest, UnaryPullSettleModesLeaseMonitoringAndErrors) {
  Env env;
  EXPECT_EQ(env.PullOnce("s1", "p1").messages_size(), 0);
  env.fsub.AddPull("u1", "d1", 2);
  auto r1 = env.PullOnce("projects/p1/subscriptions/s1", "", kAck);
  ASSERT_EQ(r1.messages_size(), 1);
  EXPECT_TRUE(r1.messages(0).delivery_attempt() == 2 &&
              r1.messages(0).settle_status() == kAOk);
  auto pr = [&](Str a, Act::SettleAction act, int ext = -1, bool mon = false) {
    env.fsub.AddPull(std::move(a), "d");
    auto r = env.PullOnce("s1", "p1", act, [&](auto* q) {
      if (ext >= 0) q->set_lease_extension_ms(ext);
      if (mon) q->set_enable_monitoring(true);
    });
    return r.messages(0).settle_status();
  };
  env.fsub.SetErr(&env.fsub.ack_st, gs(SC::PERMISSION_DENIED, "denied"));
  EXPECT_TRUE(pr("u3", kAckR, 50, true) == kAPerm && env.Spans() > 0 &&
              pr("u2", kNack) == kAOk && pr("u4", kNackR) == kAOk &&
              pr("u5", kDrop) == kAUnspec);
  env.fsub.SetErr(&env.fsub.pst, gs(SC::PERMISSION_DENIED, "bad sub"));
  EXPECT_EQ(env.PullOnce("s1", "p1").error_code(), Ec(SC::PERMISSION_DENIED));
  env.PullOnce("", "p1", kUnspec, {}, SC::INVALID_ARGUMENT);
  EXPECT_THAT(env.fsub.acked, testing::UnorderedElementsAre("u1"));
  EXPECT_THAT(env.fsub.NackedCopy(),
              testing::UnorderedElementsAre("u2", "u4", "u5"));
}

TEST(ServiceTest, SubscriberSessionUnspecifiedAndAllAutoModes) {
  Env env;
  {
    Ctx ctx;
    auto st = env.StartSub(&ctx, Mode::AUTO_SETTLE_MODE_UNSPECIFIED);
    env.fsub.AddBatch({MakeMsg("a-unspec", "m0", "d0")});
    auto md = Env::ReadMsg(st);
    ASSERT_FALSE(md.auto_settled());
    EXPECT_TRUE((Env::Settle(st, md.delivery_token(), kAck),
                 Env::StopAndRead(st), st->Finish().ok()));
  }
  using M = Mode;
  using TC = std::tuple<M::AutoSettleMode, bool, bool, bool, bool>;
  int i = 0;
  for (auto [mode, eod, err, res, nack] :
       {TC{M::AUTO_SETTLE_MODE_AUTO_ACK, false, false, false, false},
        {M::AUTO_SETTLE_MODE_AUTO_ACK_WITH_RESULT, true, true, true, false},
        {M::AUTO_SETTLE_MODE_AUTO_NACK, false, false, false, true},
        {M::AUTO_SETTLE_MODE_AUTO_NACK_WITH_RESULT, false, false, true, true},
        {M::AUTO_SETTLE_MODE_AUTO_DROP_WITHOUT_SETTLE, false, false, false,
         true}}) {
    auto [pa, pn] = env.SubCounts();
    if (err) env.fsub.SetErr(&env.fsub.ack_st, gs(SC::PERMISSION_DENIED, "e"));
    Ctx ctx;
    auto st = env.StartSub(&ctx, mode,
                           [&](auto* m) { m->set_use_exactly_once(eod); });
    Str const aid = "auto-" + std::to_string(i++);
    env.fsub.AddBatch({MakeMsg(aid, "m", "d")});
    auto dv = Env::ReadMsg(st);
    EXPECT_TRUE(dv.auto_settled() && dv.is_exactly_once() == eod);
    Vec<SubscriberEvent> ex;
    EXPECT_TRUE((Env::StopAndRead(st, &ex), st->Finish().ok()));
    EXPECT_TRUE(!res ||
                (!ex.empty() && ex[0].settlement_result().is_nack() == nack &&
                 ex[0].settlement_result().status() == (err ? kAPerm : kAOk)));
    if (err) continue;
    EXPECT_TRUE(nack ? env.WaitNack(pn + 1) : env.WaitAck(pa + 1));
    auto [na, nn] = env.SubCounts();
    EXPECT_TRUE(na - pa == (nack ? 0u : 1u) && nn - pn == (nack ? 1u : 0u));
    EXPECT_THAT(nack ? env.fsub.NackedCopy() : env.fsub.acked,
                testing::IsSupersetOf({aid}));
  }
}

TEST(ServiceTest, SubscriberSessionPostStopSettleAndZeroNackLeak) {
  Env env;
  {
    Ctx ctx;
    auto st = env.StartSub(&ctx);
    env.fsub.AddBatch({MakeMsg("stream-ack-1", "m-1", "d-1", 4)});
    auto md = Env::ReadMsg(st);
    EXPECT_EQ(md.delivery_attempt(), 4);
    ASSERT_TRUE(st->Write(Env::StopCmd()));
    Sleep(ms(50)), Env::Settle(st, md.delivery_token(), kAck);
    SubscriberEvent stopped;
    ASSERT_TRUE(st->Read(&stopped) &&
                stopped.stopped().total_delivered() == 1 && st->WritesDone() &&
                st->Finish().ok() && env.WaitAck(1));
    EXPECT_EQ(env.fsub.acked[0], "stream-ack-1");
  }
  Ctx ctx;
  auto st = env.StartSub(&ctx);
  env.fsub.AddBatch(
      {MakeMsg("hold-1", "m1", "d1"), MakeMsg("hold-2", "m2", "d2")});
  Env::ReadMsg(st), Env::ReadMsg(st);
  EXPECT_TRUE(Env::StopAndRead(st).stopped().total_delivered() == 2 &&
              st->Finish().ok());
  auto before = env.fsub.NackedCopy();
  EXPECT_THAT(before, testing::IsSupersetOf({"hold-1", "hold-2"}));
  EXPECT_EQ((Sleep(ms(40)), env.fsub.NackedCopy().size()), before.size());
}

TEST(ServiceTest, SubscriberSessionInteractiveSettlesFatalAndErrors) {
  Env env;
  {
    Ctx ctx;
    auto st = env.StartSub(&ctx, kInter, [](auto* c) {
      c->set_client_id("ignored"), c->set_enable_monitoring(true);
    });
    env.fsub.AddBatch({MakeMsg("k1", "m1", "d1"), MakeMsg("k2", "m2", "d2"),
                       MakeMsg("k3", "m3", "d3"), MakeMsg("k4", "m4", "d4")});
    std::map<Str, Str> tok;
    for (int i = 0; i < 4; ++i)
      if (auto d = Env::ReadMsg(st); true) tok[d.data()] = d.delivery_token();
    for (auto [msg, act] :
         {std::pair{"d1", kNack}, {"d2", kAckR}, {"d3", kNackR}, {"d4", kDrop}})
      Env::Settle(st, tok[msg], act);
    Vec<SubscriberEvent> res;
    EXPECT_TRUE(Env::StopAndRead(st, &res).stopped().total_delivered() == 4 &&
                res.size() == 2u && st->Finish().ok() && env.Spans() > 0 &&
                env.WaitAck(1) && env.WaitNack(3));
    EXPECT_THAT(env.fsub.acked, ::testing::UnorderedElementsAre("k2"));
    EXPECT_THAT(env.fsub.NackedCopy(),
                ::testing::IsSupersetOf({"k1", "k3", "k4"}));
  }
  env.fsub.SetErr(&env.fsub.stream_st, gs(SC::PERMISSION_DENIED, "fatal"));
  Ctx ctx;
  auto st = env.StartSub(&ctx);
  SubscriberEvent stopped;
  EXPECT_TRUE(st->Read(&stopped) &&
              stopped.stopped().error_code() == Ec(SC::PERMISSION_DENIED) &&
              st->WritesDone() && st->Finish().ok());
  SubscriberCommand bs;
  bs.mutable_start()->set_endpoint(env.ep);
  for (auto const& cmd : {Env::StopCmd(), bs})
    if (Ctx c; true) {
      auto b = env.stub->SubscriberSession(&c);
      EXPECT_TRUE(b->Write(cmd) && b->WritesDone() &&
                  b->Finish().error_code() == SC::INVALID_ARGUMENT);
    }
}

TEST(ServiceTest, ShutdownProxyDrainsActiveAndRejectsPostShutdown) {
  Env env;
  env.fpub.FailNext(1000, gs(SC::UNAVAILABLE, "retry forever"));
  auto const pub = env.CreatePub(false, false, [](auto* r) {
    r->set_total_timeout_ms(30000), r->set_initial_retry_delay_ms(200),
        r->set_max_retry_delay_ms(1000), r->set_retry_delay_multiplier(2.0);
  });
  EXPECT_FALSE(env.Pub(pub, "retrying", "", true).publish_token().empty());
  Ctx sub_ctx, rctx;
  auto sub = env.StartSub(&sub_ctx);
  auto t0 = Clock::now();
  ASSERT_EQ(env.Shutdown(150), SC::OK);
  EXPECT_TRUE(Clock::now() - t0 < sec(2) && env.last_grace_ms.load() == 150 &&
              env.shutdown_called.load() && sub->WritesDone());
  for (SubscriberEvent ev; sub->Read(&ev);) (void)ev;
  sub->Finish();
  env.CreatePub(false, false, {}, SC::UNAVAILABLE);
  EXPECT_EQ(env.stub->SubscriberSession(&rctx)->Finish().error_code(),
            SC::UNAVAILABLE);
  env.PullOnce("s1", "p1", kUnspec, {}, SC::UNAVAILABLE);
}

TEST(ServiceTest, SubscriberSessionHalfCloseBurstRace) {
  Env env;
  RecvVec batch;
  for (int i = 0; i < 8; ++i)
    batch.push_back(MakeMsg("hc-" + std::to_string(i), "m", "d"));
  for (int iter = 0; iter < 15; ++iter) {
    Ctx ctx;
    auto st =
        env.StartSub(&ctx, kInter, [](auto* m) { m->set_max_concurrency(8); });
    env.fsub.AddBatch(batch), Env::ReadMsg(st), env.fsub.AddBatch(batch);
    ASSERT_TRUE(st->Write(Env::StopCmd()) && st->WritesDone());
    bool got = false;
    for (SubscriberEvent ev; st->Read(&ev);) got = got || ev.has_stopped();
    EXPECT_TRUE(got && st->Finish().ok());
  }
}

TEST(ServiceTest, ShutdownProxyUnblocksSyncBlockingPublishAndUnaryPull) {
  Env env;
  env.fpub.FailNext(1000, gs(SC::UNAVAILABLE, "retry"));
  env.fsub.FailPull(1000, gs(SC::UNAVAILABLE, "retry"));
  auto cfg = [](auto* r) {
    r->set_total_timeout_ms(60000), r->set_initial_retry_delay_ms(100),
        r->set_max_retry_delay_ms(150), r->set_retry_delay_multiplier(1.5);
  };
  auto const pub = env.CreatePub(false, false, cfg);
  SC pub_rc = SC::OK, pull_rc = SC::OK;
  std::thread t_pub([&] {
    PublishRequest q;
    q.set_publisher_id(pub), q.set_data("blk"),
        q.set_use_blocking_publisher(true);
    pub_rc = env.Call(&Env::S::Publish, q);
  });
  std::thread t_pull([&] {
    UnaryPullRequest q;
    q.set_subscription("s1"), q.set_project_id("p1"), q.set_endpoint(env.ep);
    cfg(&q), pull_rc = env.Call(&Env::S::UnaryPull, q);
  });
  EXPECT_TRUE(env.WaitPred(
      [&] { return env.fpub.calls >= 1 && env.fsub.pull_calls >= 1; }));
  Sleep(ms(20));
  auto t0 = Clock::now();
  ASSERT_EQ(env.Shutdown(50), SC::OK);
  t_pub.join(), t_pull.join(), Env::StopSrv(env.px_srv);
  EXPECT_TRUE(Clock::now() - t0 < sec(1) && pub_rc != SC::OK &&
              pull_rc != SC::OK);
}

TEST(ServiceTest, ShutdownProxyUnblocksBlockFlowControlDuringRetry) {
  Env env;
  env.fpub.FailNext(1000, gs(SC::UNAVAILABLE, "retry"));
  auto const pub = env.BlkWait(true, 60000, 5000);
  Env::BgPub p2;
  p2.Start(env, pub, "p2");
  ASSERT_EQ(env.fpub.calls.load(), 1);
  auto t0 = Clock::now();
  ASSERT_EQ(env.Shutdown(50), SC::OK);
  p2.Join(), Env::StopSrv(env.px_srv);
  EXPECT_TRUE(Clock::now() - t0 < sec(1) && p2.Bad());
}

TEST(ServiceTest, SubscriberSessionRapidCloseSendMidStart) {
  Env env;
  for (bool stop_first : {false, true})
    for (int iter = 0; iter < 110; ++iter) {
      Ctx ctx;
      ctx.set_deadline(std::chrono::system_clock::now() + sec(5));
      auto st = env.StartSub(&ctx);
      if (iter % 3 == 1) Sleep(ms(1));
      ASSERT_TRUE(stop_first ? st->Write(Env::StopCmd()) : st->WritesDone())
          << stop_first << " " << iter;
      int code = -1;
      for (SubscriberEvent ev; st->Read(&ev);)
        if (ev.has_stopped()) {
          code = ev.stopped().error_code();
          ASSERT_TRUE(!stop_first || st->WritesDone()) << iter;
        }
      EXPECT_TRUE(code == 0 && st->Finish().ok()) << stop_first << " " << iter;
    }
}

TEST(ServiceTest, BlockFlowControlWithLargeDelayFlushStopClose) {
  for (int op : {0, 1, 2}) {
    Env env;
    auto const pub = env.BlkPub(true, 10, 5000);
    ASSERT_FALSE(env.Pub(pub, "m1", "", true).publish_token().empty());
    Env::BgPub p2;
    p2.Start(env, pub, "m2");
    ASSERT_EQ(env.fpub.calls.load(), 0);
    auto t0 = Clock::now();
    SC rc = op == 0   ? env.PubOp(&Env::S::FlushPublisher, pub)
            : op == 1 ? env.PubOp(&Env::S::StopPublisher, pub)
                      : env.PubOp(&Env::S::ClosePublisher, pub);
    p2.Join();
    EXPECT_TRUE(rc == SC::OK && Clock::now() - t0 < sec(1) && !p2.Bad() &&
                env.PubCount() == 2u)
        << op;
    EXPECT_TRUE(op != 1 ||
                env.Pub(pub, "m3").error_code() == Ec(SC::FAILED_PRECONDITION));
    if (op == 2) env.Pub(pub, "m3", "", false, false, SC::NOT_FOUND);
  }
}

TEST(ServiceTest, ClosePublisherOverlappingRetryAndBlockThenShutdownProxy) {
  for (bool blk : {false, true}) {
    Env env;
    env.fpub.FailNext(1000, gs(SC::UNAVAILABLE, "retry"));
    auto const pub = env.BlkWait(blk, 15000, 5000);
    SC close_rc = SC::UNKNOWN;
    Env::BgPub p2;
    if (blk) p2.Start(env, pub, "p2");
    std::thread tc([&] { close_rc = env.PubOp(&Env::S::ClosePublisher, pub); });
    Sleep(ms(25));
    auto t0 = Clock::now();
    ASSERT_EQ(env.Shutdown(0), SC::OK);
    tc.join(), p2.Join(), Env::StopSrv(env.px_srv);
    EXPECT_TRUE(Clock::now() - t0 < sec(1) && close_rc == SC::OK &&
                env.fpub.calls.load() == 1 && (!blk || p2.Bad()))
        << blk;
  }
}

TEST(ServiceTest, ShutdownProxyWithStalledPublishRpc) {
  for (bool blk : {false, true}) {
    Env env;
    env.fpub.SetStall(5000);
    auto const pub = env.BlkWait(blk);
    Env::BgPub p2;
    if (blk) p2.Start(env, pub, "p2");
    auto t0 = Clock::now();
    ASSERT_EQ(env.Shutdown(0), SC::OK);
    p2.Join(), Env::StopSrv(env.px_srv);
    EXPECT_TRUE(Clock::now() - t0 < sec(1) && env.fpub.in_stall.load() == 1 &&
                (!blk || p2.Bad()))
        << blk;
  }
}

}  // namespace
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace google::cloud::pubsub::test_proxy

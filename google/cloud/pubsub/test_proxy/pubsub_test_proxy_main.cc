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
#include "google/cloud/pubsub/test_proxy/trace_exporter.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

ABSL_FLAG(int, port, 0, "TCP port to listen on (0 for ephemeral port)");

namespace {
int g_signal_write_fd = -1;
void HandleTerminationSignal(int) {
  if (g_signal_write_fd >= 0 && ::write(g_signal_write_fd, "s", 1) < 0) return;
}
}  // namespace

int main(int argc, char* argv[]) {
  namespace tp = google::cloud::pubsub::test_proxy;
  using ms = std::chrono::milliseconds;
  absl::ParseCommandLine(argc, argv);
  int const port = absl::GetFlag(FLAGS_port);
  if (auto st = tp::ValidateListenPort(port); !st.ok())
    return std::cerr << st.message() << "\n", 1;
  std::signal(SIGPIPE, SIG_IGN);
  int sig_pipe[2] = {-1, -1};
  if (::pipe(sig_pipe) != 0) return std::cerr << "Signal pipe failed\n", 1;
  g_signal_write_fd = sig_pipe[1];
  struct sigaction sa{};
  sa.sa_handler = HandleTerminationSignal, sigemptyset(&sa.sa_mask),
  sa.sa_flags = SA_RESTART;
  ::sigaction(SIGTERM, &sa, nullptr), ::sigaction(SIGINT, &sa, nullptr);
  auto store = tp::InstallProxyTracerProvider();
  std::mutex mu;
  std::condition_variable cv;
  bool shut = false;
  ms shut_grace(200);
  auto req_shut = [&](ms g) {
    if (std::lock_guard<std::mutex> lk(mu); !shut) shut = true, shut_grace = g;
    cv.notify_all();
  };
  {
    tp::PubSubTestProxy proxy(store,
                              [&](ms g) { req_shut(std::max(ms(50), g)); });
    grpc::ServerBuilder sb;
    int bound = 0;
    sb.SetMaxReceiveMessageSize(-1)
        .SetMaxSendMessageSize(-1)
        .AddListeningPort("127.0.0.1:" + std::to_string(port),
                          grpc::InsecureServerCredentials(), &bound)
        .RegisterService(&proxy);
    auto server = sb.BuildAndStart();
    if (!server || bound <= 0)
      return std::cerr << "Start failed on port " << port << "\n",
             ::close(sig_pipe[0]), ::close(sig_pipe[1]), 1;
    std::thread watcher([&] {
      for (char ch = 0; ::read(sig_pipe[0], &ch, 1) > 0;) {
        if (ch == 's') proxy.InitiateShutdown(), req_shut(ms(200));
        if (ch == 's' || ch == 'q') break;
      }
    });
    std::cout << "READY 127.0.0.1:" << bound << "\n" << std::flush;
    ms grace = [&] {
      std::unique_lock<std::mutex> lk(mu);
      return cv.wait(lk, [&] { return shut; }), shut_grace;
    }();
    server->Shutdown(std::chrono::system_clock::now() + grace), server->Wait();
    if (::write(sig_pipe[1], "q", 1) < 0) (void)0;
    watcher.join();
    if (!proxy.WaitForSyncWorkers(ms(50))) std::fflush(nullptr), std::_Exit(0);
  }
  if (store) store->Shutdown();
  return tp::ResetGlobalTracerProvider(),
         g_signal_write_fd = -1, ::close(sig_pipe[0]), ::close(sig_pipe[1]), 0;
}

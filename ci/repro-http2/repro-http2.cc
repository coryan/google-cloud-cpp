// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <google/cloud/storage/client.h>
#include <google/cloud/log.h>
#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace {
// Create aliases to make the code easier to read.
namespace gcs = ::google::cloud::storage;

auto constexpr kSplitCount = 128;
auto constexpr kThreads = 32;
auto constexpr kSourcePrefix = "source/";
auto constexpr kSplitPrefix = "splits/";
auto constexpr kLineSize = 128;
// Report progress about every 1,000,000 lines
auto constexpr kLineReportInterval = 1ULL << 20;

std::vector<std::string> CreateObjects(gcs::Client client,
                                       std::string const& bucket_name,
                                       std::size_t count,
                                       std::uint64_t object_size);
std::uint64_t SplitObjects(gcs::Client client, std::string const& bucket_name,
                           std::vector<std::string> const& object_names,
                           std::uint64_t object_size);

}  // namespace

int main(int argc, char* argv[]) try {
  if (argc != 4) {
    std::cerr << "Usage: repro-http2 <bucket-name> <object-count> <object-size>\n";
    return 1;
  }
  auto const bucket_name = std::string(argv[1]);
  auto const object_count = std::stol(argv[2]);
  auto const object_size = std::stoll(argv[3]);

  auto credentials = gcs::oauth2::GoogleDefaultCredentials().value();
  auto client = gcs::Client(gcs::ClientOptions(credentials)
                                .set_enable_raw_client_tracing(true)
                                .set_enable_http_tracing(true));
  auto const object_names = [&] {
    std::vector<std::string> names;
    for (auto& o :
         client.ListObjects(bucket_name, gcs::Prefix(kSourcePrefix))) {
      if (!o) break;
      names.push_back(o->name());
      if (names.size() >= object_count) return names;
    }
    auto more = CreateObjects(client, bucket_name, object_count - names.size(), object_size);
    names.insert(names.end(), std::make_move_iterator(more.begin()),
                 std::make_move_iterator(more.end()));
    return names;
  }();

  std::cout << "Splitting objects" << std::flush;
  auto const line_count = SplitObjects(client, bucket_name, object_names, object_size);
  std::cout << "DONE " << line_count << " lines processed\n";

  return 0;
} catch (std::exception const& ex) {
  std::cerr << "Standard exception throw: " << ex.what() << "\n";
  google::cloud::LogSink::Instance().Flush();
  return 1;
}

namespace {

std::string RandomString(std::mt19937_64& generator, std::size_t n) {
  std::string const population = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::uniform_int_distribution<std::size_t> sampler(0, population.size() - 1);
  std::string s;
  std::generate_n(std::back_inserter(s), n,
                  [&] { return population[sampler(generator)]; });
  return s;
}

std::string RandomObjectName(std::mt19937_64& generator) {
  return RandomString(generator, 32);
}

std::string CreateOneObject(gcs::Client client, std::string const& bucket_name,
                            std::uint32_t seed, std::uint64_t object_size) try {
  std::mt19937_64 generator(seed);
  auto const object_name = kSourcePrefix + RandomObjectName(generator);
  auto const line = RandomString(generator, kLineSize);
  auto writer = client.WriteObject(bucket_name, object_name);
  writer.exceptions(std::ios::badbit);
  auto const line_count = object_size / kLineSize;
  for (int i = 0; i != line_count; ++i) {
    writer << i << ": " << line << "\n";
  }
  writer.Close();
  return writer.metadata().value().name();
} catch(std::exception const& ex) {
  std::cerr << "CreateOneObject: standard exception throw: " << ex.what() << "\n";
  google::cloud::LogSink::Instance().Flush();
  throw;
}


std::vector<std::string> CreateObjects(gcs::Client client,
                                       std::string const& bucket_name,
                                       std::size_t count,
                                       std::uint64_t object_size) {
  std::random_device rd;
  std::seed_seq sq{rd(), rd()};
  std::vector<std::seed_seq::result_type> seeds(count);
  sq.generate(seeds.begin(), seeds.end());

  std::vector<std::future<std::string>> tasks(count);
  std::transform(seeds.begin(), seeds.end(), tasks.begin(),
                 [&](std::seed_seq::result_type seed) {
                   return std::async(std::launch::async, CreateOneObject,
                                     client, bucket_name, seed, object_size);
                 });
  std::vector<std::string> result(count);
  std::transform(std::make_move_iterator(tasks.begin()),
                 std::make_move_iterator(tasks.end()), result.begin(),
                 [](std::future<std::string> f) { return f.get(); });
  return result;
}

std::uint64_t SplitObject(gcs::Client client, std::string const& bucket_name,
                          std::string const& object_name, std::uint32_t seed,
                          std::size_t task_id,// bool report_progress,
                          std::uint64_t object_size) {
  std::mt19937_64 generator(seed);
  std::vector<std::string> splits(kSplitCount);
  std::generate(splits.begin(), splits.end(), [&generator] {
    return kSplitPrefix + RandomObjectName(generator);
  });
  std::vector<gcs::ObjectWriteStream> writers;
  std::transform(splits.begin(), splits.end(), std::back_inserter(writers),
                 [&](std::string const& name) {
                   auto stream = client.WriteObject(bucket_name, name);
                   return stream;
                 });
  auto reader = client.ReadObject(bucket_name, object_name);
  std::string line;
  std::uniform_int_distribution<std::size_t> selector(0, splits.size() - 1);
  std::uint64_t count = 0;
  std::uint64_t offset = 0;
  auto const report_interval = object_size / 20;
  auto next_report = report_interval;
  while (std::getline(reader, line)) {
    auto& stream = writers[selector(generator)];
    stream << line << "\n";
    if (stream.bad() || !stream.last_status().ok()) {
      std::cerr << "SplitTask[" << task_id << "]: status=" << stream.last_status() << "\n";
      google::cloud::LogSink::Instance().Flush();
      break;
    }
    ++count;
    offset += line.size();
    if (task_id == 0 && offset >= next_report) {
      std::cout << '.' << std::flush;
      next_report += report_interval;
    }
  }
  auto error_count = 0;
  for (auto& w : writers) {
    w.Close();
    if (!w.metadata().ok()) ++error_count;
  }
  if (error_count != 0) throw std::runtime_error("splits fail count=" + std::to_string(error_count));
  return count;
}

std::uint64_t SplitTask(gcs::Client client, std::string const& bucket_name,
                        std::vector<std::string> const& object_names,
                        std::size_t task_id, std::uint32_t seed,
                        std::uint64_t object_size) try {
  std::uint64_t count = 0;
  std::size_t pos = 0;
  for (auto& name : object_names) {
    if (pos++ % kThreads != task_id) continue;
    count += SplitObject(client, bucket_name, name, seed, task_id, object_size);
    if (task_id == 0) std::cout << '+' << std::flush;
  }
  return count;
} catch(std::exception const& ex) {
  std::cerr << "SplitTask[" << task_id << "]: standard exception throw: " << ex.what() << "\n";
  google::cloud::LogSink::Instance().Flush();
  throw;
}

std::uint64_t SplitObjects(gcs::Client client, std::string const& bucket_name,
                           std::vector<std::string> const& object_names,
                           std::uint64_t object_size) {
  std::random_device rd;
  std::seed_seq sq{rd(), rd()};
  std::vector<std::seed_seq::result_type> seeds(kThreads);
  sq.generate(seeds.begin(), seeds.end());

  std::vector<std::future<std::uint64_t>> tasks(kThreads);
  std::size_t mod = 0;
  std::transform(seeds.begin(), seeds.end(), tasks.begin(),
                 [&](std::seed_seq::result_type seed) {
                   return std::async(std::launch::async, SplitTask, client,
                                     bucket_name, object_names, mod++, seed,
                                     object_size);
                 });

  std::uint64_t count = 0;
  for (auto& t : tasks) count += t.get();
  return count;
}

}  // namespace

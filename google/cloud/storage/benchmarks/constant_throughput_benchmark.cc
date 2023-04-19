// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/opentelemetry/configure_basic_tracing.h"
#include "google/cloud/storage/benchmarks/benchmark_utils.h"
#include "google/cloud/storage/client.h"
#include "google/cloud/internal/absl_str_join_quiet.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/internal/log_impl.h"
#include "google/cloud/internal/make_status.h"
#include "google/cloud/log.h"
#include "absl/time/time.h"
#include <opentelemetry/trace/provider.h>
#include <algorithm>
#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace g = ::google::cloud;
namespace gcs = ::google::cloud::storage;
namespace gcs_bm = ::google::cloud::storage_benchmarks;
using ::google::cloud::testing_util::FormatSize;
using ::google::cloud::testing_util::Timer;

auto constexpr kDescription = R"""(
This program tries to detect variations in throughput.

The motivation was a problem reported by a customer. In their application they
have many threads downloading objects of difference sizes. Their application
starts a download for these objects and keep the download open. The application
may not read all the data continuously. The application reads objects of
different sizes, but only very large objects seem to cause problems.

In pseudo-code, their code does something like this:

void F(gcs::Client client) {
  auto constexpr kThreshold = std::chrono::milliseconds( ... );
  auto stream = client.ReadObject("some-bucket", "some-large-object");
  std::vector<char> buffer(128 * 1024);
  while (true) {
    auto start = clock.now();
    stream.read(buffer.data(), buffer.size());
    auto elapsed = clock.now() - start();
    if (elapsed > KThreshold) std::cerr << "oh noes!\n";
  }
}

Note that is is not the *average* throughput that is a problem, but the fact
that "some reads are too slow".

This program repeatedly reads N objects from a bucket, using K threads and
reports the number of times a "read was too slow" for each download. The objects
must be created ahead of time, typically this is done with the companion
`create_dataset` program in this directory.

There are command-line options to control
)""";

struct ProgramOptions {
  std::string labels;
  std::string tracing_project_id;
  double tracing_ratio = 1.0;
  std::string bucket_name;
  std::string object_prefix;
  int thread_count = 1;
  int iteration_count = 1;
  int repeats_per_iteration = 1;
  std::size_t read_buffer_size = 128 * gcs_bm::kKiB;
  std::chrono::microseconds slow_read_threshold = std::chrono::seconds(1);
  g::Options client_options;
  bool exit_after_parse = false;
};

g::StatusOr<ProgramOptions> ParseArgs(std::vector<std::string> argv,
                                      std::string const& description = {});

struct IterationOptions {
  std::string labels;
  std::vector<std::string> object_names;
};

struct TaskConfig {
  gcs::Client client;
  std::seed_seq::result_type seed;
};

struct DownloadDetail {
  int iteration;
  std::chrono::system_clock::time_point start_time;
  std::uint64_t object_size;
  std::uint64_t transfer_size;
  std::chrono::microseconds elapsed_time;
  int slow_read_count;
  google::cloud::Status status;
  std::string bucket_name;
  std::string object_name;
  std::int64_t generation;
  std::string peer;
  std::string upload_id;
};

struct TaskResult {
  std::vector<DownloadDetail> details;
};

class Iteration {
 public:
  Iteration(int iteration, ProgramOptions options,
            std::vector<gcs::ObjectMetadata> objects)
      : iteration_(iteration),
        options_(std::move(options)),
        remaining_objects_(std::move(objects)) {}

  TaskResult DownloadTask(TaskConfig const& config);

 private:
  std::mutex mu_;
  int const iteration_;
  ProgramOptions const options_;
  std::vector<gcs::ObjectMetadata> remaining_objects_;
};

void PrintResults(ProgramOptions const& options, std::size_t object_count,
                  std::uint64_t dataset_size,
                  std::vector<TaskResult> const& iteration_results,
                  Timer::Snapshot usage);

}  // namespace

int main(int argc, char* argv[]) {
  auto options = ParseArgs({argv, argv + argc}, kDescription);
  if (!options) {
    std::cerr << options.status() << "\n";
    return 1;
  }
  if (options->exit_after_parse) return 0;

  std::cout << "# Start time: " << gcs_bm::CurrentTime()
            << "\n# Labels: " << options->labels
            << "\n# Tracing Project: " << options->tracing_project_id
            << "\n# Tracing Ratio: " << options->tracing_ratio
            << "\n# Bucket Name: " << options->bucket_name
            << "\n# Object Prefix: " << options->object_prefix
            << "\n# Thread Count: " << options->thread_count
            << "\n# Iterations: " << options->iteration_count
            << "\n# Repeats Per Iteration: " << options->repeats_per_iteration
            << "\n# Read Buffer Size: " << options->read_buffer_size
            << "\n# Slow Read Threshold: "
            << gcs_bm::FormatDuration(options->slow_read_threshold)
            << "\n# Build Info: " << gcs_bm::BuildInfo();

  auto constexpr kCircularBufferSize = 1024;
  g::LogSink::Instance().AddBackend(
      std::make_shared<g::internal::PerThreadCircularBufferBackend>(
          kCircularBufferSize, g::Severity::GCP_LS_WARNING,
          std::make_shared<g::internal::StdClogBackend>(
              g::Severity::GCP_LS_LOWEST)));
  options->client_options.set<g::TracingComponentsOption>({"rpc"});

  // Get the initial object list.
  auto client = gcs::Client(options->client_options);
  std::vector<gcs::ObjectMetadata> dataset;
  std::uint64_t dataset_size = 0;
  for (auto& o : client.ListObjects(options->bucket_name,
                                    gcs::Prefix(options->object_prefix))) {
    if (!o) break;
    dataset_size += o->size();
    dataset.push_back(*std::move(o));
  }
  if (dataset.empty()) {
    std::cerr << "No objects found in bucket " << options->bucket_name
              << " starting with prefix " << options->object_prefix << "\n"
              << "Cannot run the benchmark with an empty dataset\n";
    return 1;
  }

  std::cout << "\n# Object Count: " << dataset.size()
            << "\n# Dataset size: " << FormatSize(dataset_size);
  gcs_bm::PrintOptions(std::cout, "Client Options", options->client_options);
  std::cout << "\n";

  auto configs = [](ProgramOptions const& options, gcs::Client const& client) {
    std::random_device rd;
    std::vector<std::seed_seq::result_type> seeds(options.thread_count);
    std::seed_seq({rd(), rd(), rd()}).generate(seeds.begin(), seeds.end());

    std::vector<TaskConfig> config;
    std::transform(seeds.begin(), seeds.end(), std::back_inserter(config),
                   [&](auto s) {
                     return TaskConfig{client, s};
                   });
    return config;
  }(*options, client);

  // Create N copies of the object list, this simplifies the rest of the code
  // as we can unnest some loops. Note that we do not copy each object
  // consecutively, we want to control the "hotness" of the dataset by
  // going through the objects in a round-robin fashion.
  std::vector<gcs::ObjectMetadata> objects;
  objects.reserve(dataset.size() * options->repeats_per_iteration);
  for (int i = 0; i != options->repeats_per_iteration; ++i) {
    objects.insert(objects.end(), dataset.begin(), dataset.end());
  }

  auto tracing = g::otel::ConfigureBasicTracing(
      g::Project(options->tracing_project_id),
      g::Options{}.set<g::otel::BasicTracingRateOption>(
          options->tracing_ratio));

  // Print the header, so it can be easily loaded using the tools available in
  // our analysis tools (typically Python pandas, but could be R). Flush the
  // header because sometimes we interrupt the benchmark and these tools
  // require a header even for empty files.
  std::cout << "Start,Labels,Iteration,ObjectCount,DatasetSize,ThreadCount"
            << ",RepeatsPerIteration,ReadBufferSize,SlowReadThresholdUs"
            << ",ObjectSize,TransferSize,ElapsedMicroseconds,SlowReadCount"
            << ",StatusCode,BucketName,ObjectName,Generation,Peer,UploadId"
            << ",IterationBytes,IterationElapsedMicroseconds"
            << ",IterationCpuMicroseconds" << std::endl;

  for (int i = 0; i != options->iteration_count; ++i) {
    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    auto tracer = provider->GetTracer("cloud-cpp/benchmark",
                                      google::cloud::version_string());
    opentelemetry::trace::StartSpanOptions span_options;
    span_options.kind = opentelemetry::trace::SpanKind::kClient;
    auto span = tracer->StartSpan("Iteration", span_options);
    auto scope = tracer->WithActiveSpan(span);

    auto timer = Timer::PerProcess();
    Iteration iteration(i, *options, objects);
    auto task = [&iteration, tracer, span](TaskConfig const& c) mutable {
      auto iteration_scope = tracer->WithActiveSpan(span);
      opentelemetry::trace::StartSpanOptions span_options;
      span_options.kind = opentelemetry::trace::SpanKind::kClient;
      auto task_span = tracer->StartSpan("Task", span_options);
      auto task_scope = tracer->WithActiveSpan(task_span);
      return iteration.DownloadTask(c);
    };
    std::vector<std::future<TaskResult>> tasks(configs.size());
    std::transform(configs.begin(), configs.end(), tasks.begin(),
                   [&task](TaskConfig const& c) {
                     return std::async(std::launch::async, task, std::cref(c));
                   });

    std::vector<TaskResult> iteration_results(configs.size());
    std::transform(std::make_move_iterator(tasks.begin()),
                   std::make_move_iterator(tasks.end()),
                   iteration_results.begin(),
                   [](std::future<TaskResult> f) { return f.get(); });

    PrintResults(*options, objects.size(), dataset_size, iteration_results,
                 timer.Sample());

    span->End();
  }

  return 0;
}

namespace {

g::StatusOr<ProgramOptions> ParseArgs(std::vector<std::string> argv,
                                      std::string const& description) {
  using ::google::cloud::testing_util::OptionDescriptor;
  ProgramOptions options;
  g::Status parse_error;
  bool wants_help = false;
  bool wants_description = false;

  std::vector<OptionDescriptor> desc{
      {"--help", "print usage information",
       [&wants_help](std::string const&) { wants_help = true; }},
      {"--description", "print benchmark description",
       [&wants_description](std::string const&) { wants_description = true; }},
      {"--labels", "user-defined labels to tag the results",
       [&options](std::string const& val) { options.labels = val; }},
      {"--tracing-project-id", "the project for tracing",
       [&options](std::string const& val) {
         options.tracing_project_id = val;
       }},
      {"--tracing-ratio", "the ratio of traces that are sampled",
       [&options](std::string const& val) {
         options.tracing_ratio = std::stof(val);
       }},
      {"--bucket-name", "the bucket where the dataset is located",
       [&options](std::string const& val) { options.bucket_name = val; }},
      {"--object-prefix", "the dataset prefix",
       [&options](std::string const& val) { options.object_prefix = val; }},
      {"--thread-count", "set the number of threads in the benchmark",
       [&options](std::string const& val) {
         options.thread_count = std::stoi(val);
       }},
      {"--iteration-count", "set the number of iterations in the benchmark",
       [&options](std::string const& val) {
         options.iteration_count = std::stoi(val);
       }},
      {"--repeats-per-iteration",
       "each iteration downloads the dataset this many times",
       [&options](std::string const& val) {
         options.repeats_per_iteration = std::stoi(val);
       }},
      {"--read-buffer-size", "controls the buffer used in the downloads",
       [&options](std::string const& val) {
         options.read_buffer_size = gcs_bm::ParseBufferSize(val);
       }},
      {"--slow-read-threshold",
       "configure when a read is considered 'too slow'.",
       [&options, &parse_error](std::string const& val) {
         absl::Duration d;
         if (!absl::ParseDuration(val, &d)) {
           parse_error = g::internal::InvalidArgumentError(
               "invalid duration: " + val, GCP_ERROR_INFO());
           return;
         }
         options.slow_read_threshold = absl::ToChronoMicroseconds(d);
       }},
      {"--download-stall-timeout",
       "configure `storage::DownloadStallTimeoutOption`: the maximum time"
       " allowed for data to 'stall' (make insufficient progress) on downloads."
       " This option is intended for troubleshooting. Most of the time the"
       " value is not expected to change the library performance.",
       [&options](std::string const& val) {
         options.client_options.set<gcs::DownloadStallTimeoutOption>(
             gcs_bm::ParseDuration(val));
       }},
      {"--download-stall-minimum-rate",
       "configure `storage::DownloadStallMinimumRateOption`: the transfer"
       " is aborted if the average transfer rate is below this limit for"
       " the period set via `storage::DownloadStallTimeoutOption`.",
       [&options](std::string const& val) {
         options.client_options.set<gcs::DownloadStallMinimumRateOption>(
             static_cast<std::uint32_t>(gcs_bm::ParseBufferSize(val)));
       }},
  };
  auto usage = BuildUsage(desc, argv[0]);

  auto unparsed = OptionsParse(desc, argv);
  if (wants_help) {
    std::cout << usage << "\n";
    options.exit_after_parse = true;
    return options;
  }

  if (wants_description) {
    std::cout << description << "\n";
    options.exit_after_parse = true;
    return options;
  }

  if (unparsed.size() != 1) {
    std::ostringstream os;
    os << "Unknown arguments or options: "
       << absl::StrJoin(std::next(unparsed.begin()), unparsed.end(), ", ")
       << "\n"
       << usage << "\n";
    return g::internal::InvalidArgumentError(std::move(os).str(),
                                             GCP_ERROR_INFO());
  }

  if (!parse_error.ok()) return parse_error;
  return options;
}

std::string Peer(gcs::ObjectReadStream const& stream) {
  auto p = stream.headers().find(":grpc-context-peer");
  if (p == stream.headers().end()) {
    p = stream.headers().find(":curl-peer");
  }
  return p == stream.headers().end() ? std::string{"unknown"} : p->second;
}

std::string SessionId(gcs::ObjectReadStream const& stream) {
  auto p = stream.headers().find("x-guploader-uploadid");
  return p == stream.headers().end() ? std::string{"unknown"} : p->second;
}

DownloadDetail DownloadOneObject(gcs::Client& client,
                                 ProgramOptions const& options,
                                 gcs::ObjectMetadata const& object,
                                 int iteration) {
  using clock = std::chrono::steady_clock;
  using std::chrono::duration_cast;
  using std::chrono::microseconds;

  std::vector<char> buffer(options.read_buffer_size);
  auto const buffer_size = static_cast<std::streamsize>(buffer.size());
  auto const object_start = clock::now();
  auto const start = std::chrono::system_clock::now();
  auto const object_size = static_cast<std::uint64_t>(object.size());

  auto provider = opentelemetry::trace::Provider::GetTracerProvider();
  auto tracer = provider->GetTracer("cloud-cpp/benchmark",
                                    google::cloud::version_string());
  opentelemetry::trace::StartSpanOptions span_options;
  span_options.kind = opentelemetry::trace::SpanKind::kClient;
  auto span = tracer->StartSpan("DownloadOneObject", span_options);
  auto scope = tracer->WithActiveSpan(span);

  auto stream = client.ReadObject(object.bucket(), object.name(),
                                  gcs::Generation(object.generation()));
  auto transfer_size = std::uint64_t{0};
  int slow_read_count = 0;
  while (true) {
    auto const read_start = clock::now();
    stream.read(buffer.data(), buffer_size);
    auto const read_elapsed = clock::now() - read_start;
    if (read_elapsed >= options.slow_read_threshold) {
      ++slow_read_count;
      GCP_LOG(WARNING)
          << "High latency in read() function call, session_id="
          << SessionId(stream) << ", latency="
          << gcs_bm::FormatDuration(
                 std::chrono::duration_cast<std::chrono::microseconds>(
                     read_elapsed));
    }
    transfer_size += stream.gcount();
    if (!stream) break;
  }
  stream.Close();

  span->End();

  // Flush the logs, if any.
  if (!stream.status().ok()) google::cloud::LogSink::Instance().Flush();
  auto const object_elapsed =
      duration_cast<microseconds>(clock::now() - object_start);
  return DownloadDetail{
      iteration,           start,           object_size,
      transfer_size,       object_elapsed,  slow_read_count,
      stream.status(),     object.bucket(), object.name(),
      object.generation(), Peer(stream),    SessionId(stream)};
}

TaskResult Iteration::DownloadTask(TaskConfig const& config) {
  auto client = config.client;
  TaskResult result;
  while (true) {
    std::unique_lock<std::mutex> lk(mu_);
    if (remaining_objects_.empty()) break;
    auto object = std::move(remaining_objects_.back());
    remaining_objects_.pop_back();
    lk.unlock();
    result.details.push_back(
        DownloadOneObject(client, options_, object, iteration_));
  }
  return result;
}

void PrintResults(ProgramOptions const& options, std::size_t object_count,
                  std::uint64_t dataset_size,
                  std::vector<TaskResult> const& iteration_results,
                  Timer::Snapshot usage) {
  auto accumulate_transfer_size = [](std::vector<TaskResult> const& r) {
    return std::accumulate(
        r.begin(), r.end(), std::int64_t{0},
        [](std::int64_t a, TaskResult const& b) {
          auto const subtotal = std::accumulate(
              b.details.begin(), b.details.end(), std::int64_t{0},
              [](auto a, auto const& b) { return a + b.transfer_size; });
          return a + subtotal;
        });
  };

  auto const transfer_size = accumulate_transfer_size(iteration_results);

  auto clean_csv_field = [](std::string v) {
    std::replace(v.begin(), v.end(), ',', ';');
    return v;
  };
  auto const labels = clean_csv_field(options.labels);
  // Print the results after each iteration. Makes it possible to interrupt
  // the benchmark in the middle and still get some data.
  for (auto const& r : iteration_results) {
    for (auto const& d : r.details) {
      // Join the iteration details with the per-download details. That makes
      // it easier to analyze the data in external scripts.
      std::cout << gcs_bm::FormatTimestamp(d.start_time)       //
                << ',' << labels                               //
                << ',' << d.iteration                          //
                << ',' << object_count                         //
                << ',' << dataset_size                         //
                << ',' << options.thread_count                 //
                << ',' << options.repeats_per_iteration        //
                << ',' << options.read_buffer_size             //
                << ',' << options.slow_read_threshold.count()  //
                << ',' << d.object_size                        //
                << ',' << d.transfer_size                      //
                << ',' << d.elapsed_time.count()               //
                << ',' << d.slow_read_count                    //
                << ',' << d.status.code()                      //
                << ',' << d.bucket_name                        //
                << ',' << d.object_name                        //
                << ',' << d.generation                         //
                << ',' << d.peer                               //
                << ',' << d.upload_id                          //
                << ',' << transfer_size                        //
                << ',' << usage.elapsed_time.count()           //
                << ',' << usage.cpu_time.count()               //
                << "\n";
    }
  }
  // After each iteration print a human-readable summary. Flush it because
  // the operator of these benchmarks (coryan@) is an impatient person.
  auto const bandwidth =
      gcs_bm::FormatBandwidthGbPerSecond(transfer_size, usage.elapsed_time);
  std::cout << "# " << gcs_bm::CurrentTime() << " downloaded=" << transfer_size
            << " cpu_time=" << absl::FromChrono(usage.cpu_time)
            << " elapsed_time=" << absl::FromChrono(usage.elapsed_time)
            << " Gbit/s=" << bandwidth << std::endl;
}

}  // namespace

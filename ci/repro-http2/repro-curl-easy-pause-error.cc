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

#include <google/cloud/log.h>
#include <google/cloud/storage/client.h>
#include <curl/curl.h>
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <random>
#include <string>

namespace {
// Create aliases to make the code easier to read.
namespace gcs = ::google::cloud::storage;

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

}  // namespace

int main(int argc, char* argv[]) try {
  if (argc != 3) {
    std::cerr
        << "Usage: repro-curl-easy-pause-error <bucket-name> <object-size>\n";
    return 1;
  }
  auto const bucket_name = std::string(argv[1]);
  auto const object_size = std::stoll(argv[2]);

  auto credentials = gcs::oauth2::GoogleDefaultCredentials().value();
  auto client = gcs::Client(gcs::ClientOptions(credentials)
                                .set_enable_raw_client_tracing(true)
                                .set_enable_http_tracing(true));

  auto generator = std::mt19937_64{std::random_device{}()};
  auto object_name = RandomObjectName(generator);

  // Construct a large object, or at least large enough that it is not
  // downloaded in the first chunk.
  auto const contents = RandomString(generator, object_size);
  auto source_meta = client
                         .InsertObject(bucket_name, object_name, contents,
                                       gcs::IfGenerationMatch(0))
                         .value();

  // Create an iostream to read the object back.
  auto stream = client.ReadObject(bucket_name, object_name);
  auto meta = client.GetObjectMetadata(bucket_name, object_name).value();
  if (!stream.good()) {
    std::cerr << "[1] stream.status=" << stream.status() << std::endl;
    return 1;
  }
  auto const actual = std::string{std::istreambuf_iterator<char>{stream}, {}};
  if (!stream.status().ok()) {
    std::cerr << "[2] stream.status=" << stream.status() << std::endl;
    return 1;
  }
  std::cout << "read successful, received " << actual.size() << " bytes"
            << " stream.status=" << stream.status()
            << " stream.good()=" << stream.good() << "\nmeta=" << meta
            << "\ngoogle-cloud-cpp=" << google::cloud::version_string()
            << "\ncurl=" << curl_version() << "\n";

  (void)client.DeleteObject(source_meta.bucket(), source_meta.name(),
                            gcs::Generation(source_meta.generation()));

  return 0;
} catch (std::exception const& ex) {
  std::cerr << "Standard exception throw: " << ex.what() << "\n";
  google::cloud::LogSink::Instance().Flush();
  return 1;
}

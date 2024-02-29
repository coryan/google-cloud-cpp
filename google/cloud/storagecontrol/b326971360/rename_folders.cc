// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/opentelemetry/configure_basic_tracing.h"
#include "google/cloud/storagecontrol/v2/storage_control_client.h"
#include "google/cloud/credentials.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/opentelemetry_options.h"
#include "google/cloud/options.h"
#include "google/cloud/project.h"
#include <google/storage/control/v2/storage_control.pb.h>
#include <algorithm>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace gc = google::cloud;
using ::google::storage::control::v2::Folder;

void RenameMultipleTimes(std::string const& bucket_name,
                         std::string const& prefix) {
  auto client = gc::storagecontrol_v2::StorageControlClient(
      gc::storagecontrol_v2::MakeStorageControlConnection(
          gc::Options{}.set<gc::OpenTelemetryTracingOption>(true)));

  auto make_id = [prefix](int count) {
    std::ostringstream os;
    os << prefix << "-" << std::this_thread::get_id() << "-" << count;
    return std::move(os).str();
  };
  google::storage::control::v2::CreateFolderRequest create;
  create.set_parent("projects/_/buckets/" + bucket_name);
  create.set_folder_id(make_id(0));
  auto folder = client.CreateFolder(create);
  if (!folder) throw std::move(folder).status();
  auto folder_name = folder->name();
  for (int i = 1; i != 1000; ++i) {
    auto renamed = client.RenameFolder(folder_name, make_id(i)).get();
    if (!renamed) throw std::move(renamed).status();
    folder_name = renamed->name();
  }
}

}  // namespace

int main(int argc, char* argv[]) try {
  if (argc != 4) {
    throw std::runtime_error("Usage: cmd <project-id> <bucket-name> <prefix>");
  }

  auto const project_id = std::string(argv[1]);
  auto bucket_name = std::string(argv[2]);
  auto prefix = std::string(argv[3]);

  auto configuration = gc::otel::ConfigureBasicTracing(
      gc::Project(project_id),
      gc::Options{}.set<gc::otel::BasicTracingRateOption>(0.10));

  std::vector<std::future<void>> tasks;
  std::generate_n(std::back_inserter(tasks), 128, [&] {
    return std::async(std::launch::async, RenameMultipleTimes, bucket_name,
                      prefix);
  });
  for (auto& t : tasks) try {
      t.get();
    } catch (google::cloud::Status const& status) {
      std::cerr << "Status thrown: " << status << "\n";
      return 1;
    }
} catch (google::cloud::Status const& status) {
  std::cerr << "Status thrown: " << status << "\n";
  return 1;
} catch (std::exception const& ex) {
  std::cerr << "Standard exception throw: " << ex.what() << "\n";
  return 1;
} catch (...) {
  std::cerr << "Unknown exception thrown\n";
}

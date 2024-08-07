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

// Generated by the Codegen C++ plugin.
// If you make any local changes, they will be lost.
// source: google/cloud/bigquery/v2/table.proto

#include "google/cloud/bigquerycontrol/v2/internal/table_rest_metadata_decorator.h"
#include "google/cloud/internal/absl_str_cat_quiet.h"
#include "google/cloud/internal/api_client_header.h"
#include "google/cloud/internal/rest_set_metadata.h"
#include "google/cloud/status_or.h"
#include "absl/strings/str_format.h"
#include <memory>
#include <utility>

namespace google {
namespace cloud {
namespace bigquerycontrol_v2_internal {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN

TableServiceRestMetadata::TableServiceRestMetadata(
    std::shared_ptr<TableServiceRestStub> child, std::string api_client_header)
    : child_(std::move(child)),
      api_client_header_(
          api_client_header.empty()
              ? google::cloud::internal::GeneratedLibClientHeader()
              : std::move(api_client_header)) {}

StatusOr<google::cloud::bigquery::v2::Table> TableServiceRestMetadata::GetTable(
    rest_internal::RestContext& rest_context, Options const& options,
    google::cloud::bigquery::v2::GetTableRequest const& request) {
  SetMetadata(rest_context, options);
  return child_->GetTable(rest_context, options, request);
}

StatusOr<google::cloud::bigquery::v2::Table>
TableServiceRestMetadata::InsertTable(
    rest_internal::RestContext& rest_context, Options const& options,
    google::cloud::bigquery::v2::InsertTableRequest const& request) {
  SetMetadata(rest_context, options);
  return child_->InsertTable(rest_context, options, request);
}

StatusOr<google::cloud::bigquery::v2::Table>
TableServiceRestMetadata::PatchTable(
    rest_internal::RestContext& rest_context, Options const& options,
    google::cloud::bigquery::v2::UpdateOrPatchTableRequest const& request) {
  SetMetadata(rest_context, options);
  return child_->PatchTable(rest_context, options, request);
}

StatusOr<google::cloud::bigquery::v2::Table>
TableServiceRestMetadata::UpdateTable(
    rest_internal::RestContext& rest_context, Options const& options,
    google::cloud::bigquery::v2::UpdateOrPatchTableRequest const& request) {
  SetMetadata(rest_context, options);
  return child_->UpdateTable(rest_context, options, request);
}

Status TableServiceRestMetadata::DeleteTable(
    rest_internal::RestContext& rest_context, Options const& options,
    google::cloud::bigquery::v2::DeleteTableRequest const& request) {
  SetMetadata(rest_context, options);
  return child_->DeleteTable(rest_context, options, request);
}

StatusOr<google::cloud::bigquery::v2::TableList>
TableServiceRestMetadata::ListTables(
    rest_internal::RestContext& rest_context, Options const& options,
    google::cloud::bigquery::v2::ListTablesRequest const& request) {
  SetMetadata(rest_context, options);
  return child_->ListTables(rest_context, options, request);
}

void TableServiceRestMetadata::SetMetadata(
    rest_internal::RestContext& rest_context, Options const& options,
    std::vector<std::string> const& params) {
  google::cloud::rest_internal::SetMetadata(rest_context, options, params,
                                            api_client_header_);
}

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace bigquerycontrol_v2_internal
}  // namespace cloud
}  // namespace google

// Copyright 2023 Google LLC
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
// source: google/cloud/osconfig/v1/osconfig_zonal_service.proto

#ifndef GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_OSCONFIG_V1_OS_CONFIG_ZONAL_OPTIONS_H
#define GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_OSCONFIG_V1_OS_CONFIG_ZONAL_OPTIONS_H

#include "google/cloud/osconfig/v1/os_config_zonal_connection.h"
#include "google/cloud/osconfig/v1/os_config_zonal_connection_idempotency_policy.h"
#include "google/cloud/backoff_policy.h"
#include "google/cloud/options.h"
#include "google/cloud/version.h"
#include <memory>

namespace google {
namespace cloud {
namespace osconfig_v1 {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN

/**
 * Use with `google::cloud::Options` to configure the retry policy.
 *
 * @ingroup google-cloud-osconfig-options
 */
struct OsConfigZonalServiceRetryPolicyOption {
  using Type = std::shared_ptr<OsConfigZonalServiceRetryPolicy>;
};

/**
 * Use with `google::cloud::Options` to configure the backoff policy.
 *
 * @ingroup google-cloud-osconfig-options
 */
struct OsConfigZonalServiceBackoffPolicyOption {
  using Type = std::shared_ptr<BackoffPolicy>;
};

/**
 * Use with `google::cloud::Options` to configure which operations are retried.
 *
 * @ingroup google-cloud-osconfig-options
 */
struct OsConfigZonalServiceConnectionIdempotencyPolicyOption {
  using Type = std::shared_ptr<OsConfigZonalServiceConnectionIdempotencyPolicy>;
};

/**
 * Use with `google::cloud::Options` to configure the long-running operations
 * polling policy.
 *
 * @ingroup google-cloud-osconfig-options
 */
struct OsConfigZonalServicePollingPolicyOption {
  using Type = std::shared_ptr<PollingPolicy>;
};

/**
 * The options applicable to OsConfigZonalService.
 *
 * @ingroup google-cloud-osconfig-options
 */
using OsConfigZonalServicePolicyOptionList =
    OptionList<OsConfigZonalServiceRetryPolicyOption,
               OsConfigZonalServiceBackoffPolicyOption,
               OsConfigZonalServicePollingPolicyOption,
               OsConfigZonalServiceConnectionIdempotencyPolicyOption>;

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace osconfig_v1
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_OSCONFIG_V1_OS_CONFIG_ZONAL_OPTIONS_H

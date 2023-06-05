diff --git a/google/cloud/storage/benchmarks/throughput_options.cc b/google/cloud/storage/benchmarks/throughput_options.cc
index 53ef6207d7..54abfd23d2 100644
--- a/google/cloud/storage/benchmarks/throughput_options.cc
+++ b/google/cloud/storage/benchmarks/throughput_options.cc
@@ -14,6 +14,7 @@
 
 #include "google/cloud/storage/benchmarks/throughput_options.h"
 #include "google/cloud/storage/options.h"
+#include "google/cloud/common_options.h"
 #include "google/cloud/grpc_options.h"
 #include "absl/strings/str_split.h"
 #include "absl/time/time.h"
@@ -352,6 +353,11 @@ google::cloud::StatusOr<ThroughputOptions> ParseThroughputOptions(
        [&options](std::string const& val) {
          options.direct_path_options.set<EndpointOption>(val);
        }},
+      {"--gprc-authority-hostname",
+       "sets the ALTS call host for gRPC+DirectPath-based benchmarks",
+       [&options](std::string const& val) {
+         options.direct_path_options.set<AuthorityOption>(val);
+       }},
       {"--transfer-stall-timeout",
        "configure `storage::TransferStallTimeoutOption`: the maximum time"
        " allowed for data to 'stall' (make insufficient progress) on all"

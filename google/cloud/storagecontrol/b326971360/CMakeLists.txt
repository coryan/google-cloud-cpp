# ~~~
# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ~~~

if ((NOT opentelemetry IN_LIST GOOGLE_CLOUD_CPP_ENABLE) OR (NOT GOOGLE_CLOUD_CPP_ENABLE_CXX_EXCEPTIONS))
    return()
endif ()

add_executable(rename_folders rename_folders.cc)
target_link_libraries(
    rename_folders
    PRIVATE google-cloud-cpp::experimental-storagecontrol google-cloud-cpp::opentelemetry
             google-cloud-cpp::common)
google_cloud_cpp_add_common_options(rename_folders)

# Copyright 2021 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

FROM ubuntu:focal AS devtools

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get --no-install-recommends install -y apt-transport-https apt-utils \
        automake build-essential ccache cmake ca-certificates curl git \
        gcc g++ libc-ares-dev libc-ares2 libcurl4-openssl-dev libre2-dev \
        libssl-dev m4 make pkg-config tar wget zlib1g-dev
RUN apt-get update && apt-get --no-install-recommends install -y ninja-build

WORKDIR /var/tmp/build/abseil-cpp
RUN curl -sSL https://github.com/abseil/abseil-cpp/archive/20210324.2.tar.gz | \
    tar -xzf - --strip-components=1 && \
    sed -i 's/^#define ABSL_OPTION_USE_\(.*\) 2/#define ABSL_OPTION_USE_\1 0/' "absl/base/options.h" && \
    cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_CXX_STANDARD=11 \
      -S . -B .build -GNinja && \
    cmake --build .build && \
    cmake --install .build

WORKDIR /var/tmp/build/protobuf
RUN curl -sSL https://github.com/google/protobuf/archive/v3.17.3.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -Dprotobuf_BUILD_TESTS=OFF \
        -S cmake -B .build -GNinja && \
    cmake --build .build && \
    cmake --install .build

WORKDIR /var/tmp/build/grpc
RUN curl -sSL https://github.com/grpc/grpc/archive/v1.38.1.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DgRPC_BUILD_CSHARP_EXT=OFF \
        -DgRPC_INSTALL=ON \
        -DgRPC_BUILD_TESTS=OFF \
        -DgRPC_ABSL_PROVIDER=package \
        -DgRPC_CARES_PROVIDER=package \
        -DgRPC_PROTOBUF_PROVIDER=package \
        -DgRPC_RE2_PROVIDER=package \
        -DgRPC_SSL_PROVIDER=package \
        -DgRPC_ZLIB_PROVIDER=package \
        -S . -B .build -GNinja && \
    cmake --build .build && \
    cmake --install .build

WORKDIR /var/tmp/build/crc32c
RUN curl -sSL https://github.com/google/crc32c/archive/1.1.0.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCRC32C_BUILD_TESTS=OFF \
        -DCRC32C_BUILD_BENCHMARKS=OFF \
        -DCRC32C_USE_GLOG=OFF  \
        -S . -B .build -GNinja && \
    cmake --build .build && \
    cmake --install .build

WORKDIR /var/tmp/build/json
RUN curl -sSL https://github.com/nlohmann/json/archive/v3.9.1.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_TESTING=OFF \
      -S . -B .build -GNinja && \
    cmake --build .build --target install

WORKDIR /var/tmp/build/googletest
RUN curl -sSL https://github.com/google/googletest/archive/release-1.10.0.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
      -DCMAKE_BUILD_TYPE="Release" \
      -DBUILD_SHARED_LIBS=yes \
      -S . -B .build -GNinja && \
    cmake --build .build && \
    cmake --install .build

WORKDIR /var/tmp/build/benchmark
RUN curl -sSL https://github.com/google/benchmark/archive/v1.5.3.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
        -DCMAKE_BUILD_TYPE="Release" \
        -DBUILD_SHARED_LIBS=yes \
        -DBENCHMARK_ENABLE_TESTING=OFF \
      -S . -B .build -GNinja && \
    cmake --build .build && \
    cmake --install .build

FROM devtools AS build

WORKDIR /var/tmp/build/google-cloud-cpp
COPY . /var/tmp/build/google-cloud-cpp
RUN cmake -S . -B .build -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DGOOGLE_CLOUD_CPP_STORAGE_ENABLE_GRPC=ON \
    && cmake --build .build --target storage_storage_throughput_vs_cpu_benchmark \
    && cmake --build .build --target storage_aggregate_throughput_benchmark \
    && cmake --build .build --target storage_create_dataset

WORKDIR /var/tmp/build/dp_check
RUN curl -sSL -o dp_check https://github.com/GoogleCloudPlatform/grpc-gcp-tools/releases/download/v1.3/dp_check
RUN chmod 755 dp_check

FROM ubuntu:focal AS deploy

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get --no-install-recommends install -y  \
        ca-certificates libc-ares2 libcurl4 libre2-5
RUN apt-get update && apt-get upgrade -y

WORKDIR /r
COPY --from=build /var/tmp/build/google-cloud-cpp/.build/google/cloud/storage/benchmarks/*_benchmark /r
COPY --from=build /var/tmp/build/google-cloud-cpp/.build/google/cloud/storage/benchmarks/create_dataset /r
COPY --from=build /var/tmp/build/dp_check /r

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

FROM alpine:edge AS baseline

FROM baseline AS devtools
RUN apk update \
    && apk add \
        autoconf \
        automake \
        cmake \
        curl \
        gcc \
        g++ \
        libc-dev \
        libtool \
        linux-headers \
        make \
        ninja \
        openssl-dev \
        openssl-libs-static \
        tar \
        zlib-dev \
        zlib-static

WORKDIR /var/tmp/build/abseil-cpp
RUN curl -sSL https://github.com/abseil/abseil-cpp/archive/20210324.2.tar.gz \
    | tar -xzf - --strip-components=1 \
    && sed -i 's/^#define ABSL_OPTION_USE_\(.*\) 2/#define ABSL_OPTION_USE_\1 0/' "absl/base/options.h" \
    && cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_CXX_STANDARD=11 \
        -S . -Bcmake-out -GNinja \
    && cmake --build cmake-out \
    && cmake --install cmake-out

WORKDIR /var/tmp/build/nghttp2
RUN curl -sSL https://github.com/nghttp2/nghttp2/archive/refs/tags/v1.43.0.tar.gz \
    | tar -xzf - --strip-components=1 \
    && cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_LIB_ONLY=ON \
        -DENABLE_ASIO_LIB=OFF \
        -DENABLE_STATIC_LIB=ON \
        -DENABLE_SHARED_LIB=OFF \
        -S . -Bcmake-out -GNinja \
    && cmake --build cmake-out \
    && cmake --install cmake-out

WORKDIR /var/tmp/build/crc32c
RUN curl -sSL https://github.com/google/crc32c/archive/1.1.0.tar.gz \
    | tar -xzf - --strip-components=1 \
    && cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCRC32C_BUILD_TESTS=OFF \
        -DCRC32C_BUILD_BENCHMARKS=OFF \
        -DCRC32C_USE_GLOG=OFF \
        -S . -Bcmake-out -GNinja \
    && cmake --build cmake-out \
    && cmake --install cmake-out

WORKDIR /var/tmp/build/json
RUN curl -sSL https://github.com/nlohmann/json/archive/v3.9.1.tar.gz \
    | tar -xzf - --strip-components=1 \
    && cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_TESTING=OFF \
      -S . -Bcmake-out -GNinja \
    && cmake --install cmake-out

RUN apk update \
    && apk add \
        c-ares-dev \
        c-ares-static

WORKDIR /var/tmp/build/googletest
RUN curl -sSL https://github.com/google/googletest/archive/release-1.10.0.tar.gz \
    | tar -xzf - --strip-components=1 \
    && cmake \
        -DCMAKE_BUILD_TYPE="Release" \
        -DBUILD_SHARED_LIBS=OFF \
        -S . -Bcmake-out -GNinja \
    && cmake --build cmake-out \
    && cmake --install cmake-out

WORKDIR /var/tmp/build/benchmark
RUN curl -sSL https://github.com/google/benchmark/archive/v1.5.4.tar.gz \
    | tar -xzf - --strip-components=1 \
    && cmake \
        -DCMAKE_BUILD_TYPE="Release" \
        -DBUILD_SHARED_LIBS=OFF \
        -DBENCHMARK_ENABLE_TESTING=OFF \
        -S . -Bcmake-out -GNinja \
    && cmake --build cmake-out \
    && cmake --install cmake-out

WORKDIR /var/tmp/build/protobuf
RUN curl -sSL https://github.com/google/protobuf/archive/v3.17.1.tar.gz \
    | tar -xzf - --strip-components=1 \
    && cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -Dprotobuf_BUILD_TESTS=OFF \
        -S cmake -Bcmake-out -GNinja \
    && cmake --build cmake-out \
    && cmake --install cmake-out

WORKDIR /var/tmp/build/re2
RUN curl -sSL https://github.com/google/re2/archive/2020-11-01.tar.gz \
    | tar -xzf - --strip-components=1 \
    && cmake -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DRE2_BUILD_TESTING=OFF \
        -S . -Bcmake-out -GNinja \
    && cmake --build cmake-out \
    && cmake --install cmake-out

WORKDIR /var/tmp/build/grpc
RUN curl -sSL https://github.com/grpc/grpc/archive/v1.37.1.tar.gz \
    | tar -xzf - --strip-components=1 \
    && cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DgRPC_INSTALL=ON \
        -DgRPC_STATIC_LINKING=ON \
        -DgRPC_BUILD_CSHARP_EXT=OFF \
        -DgRPC_BUILD_TESTS=OFF \
        -DgRPC_ABSL_PROVIDER=package \
        -DgRPC_CARES_PROVIDER=package \
        -DgRPC_PROTOBUF_PROVIDER=package \
        -DgRPC_RE2_PROVIDER=package \
        -DgRPC_SSL_PROVIDER=package \
        -DgRPC_ZLIB_PROVIDER=package \
        -S . -Bcmake-out -GNinja \
    && cmake --build cmake-out \
    && cmake --install cmake-out

WORKDIR /var/tmp/build/curl
RUN curl -sSL https://curl.haxx.se/download/curl-7.77.0.tar.gz \
    | tar -xzf - --strip-components=1 \
    && cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DUSE_NGHTTP2=ON \
        -DENABLE_ARES=ON \
        -S . -Bcmake-out -GNinja \
    && cmake --build cmake-out \
    && cmake --install cmake-out

WORKDIR /var/tmp/build/google-cloud-cpp
RUN curl -sSL https://github.com/googleapis/google-cloud-cpp/archive/v1.29.0.tar.gz | tar -xzf - --strip-components=1
RUN cmake -DCMAKE_BUILD_TYPE=Debug \
        -DGOOGLE_CLOUD_CPP_STORAGE_ENABLE_GRPC=ON \
        -S . -B cmake-out -GNinja \
    && cmake --build cmake-out \
    && cmake --install cmake-out

FROM devtools AS build

WORKDIR /var/tmp/build/repro
COPY . /var/tmp/build/repro
RUN cmake -S . -B cmake-out -GNinja && cmake --build cmake-out

FROM baseline AS deployment
RUN apk update \
    && apk add \
        c-ares \
        curl \
        libstdc++ \
        zlib

WORKDIR /r
COPY --from=build /var/tmp/build/repro/cmake-out/repro-http2 /r/
COPY --from=build /var/tmp/build/repro/cmake-out/repro-curl-easy-pause-error /r/
COPY --from=build /var/tmp/build/google-cloud-cpp/cmake-out/google/cloud/storage/benchmarks/*_benchmark /r/

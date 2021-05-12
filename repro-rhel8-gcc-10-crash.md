# Steps to reproduce RHEL:8 + GCC-10 Crash

```shell
gcloud compute instances create cloud-cpp-rhel8 \
    --zone=us-east1-b --machine-type=e2-highmem-16 --subnet=default \
    --network-tier=PREMIUM --maintenance-policy=MIGRATE \
    --scopes=https://www.googleapis.com/auth/cloud-platform \
    --image=rhel-8-v20210420 --image-project=rhel-cloud \
    --boot-disk-size=1024GB --boot-disk-type=pd-balanced \
    --boot-disk-device-name=cloud-cpp-rhel8 --no-shielded-secure-boot \
    --shielded-vtpm --shielded-integrity-monitoring --reservation-affinity=any
```

```shell
gcloud compute config-ssh
ssh -A -o ProxyCommand='corp-ssh-helper %h %p' cloud-cpp-rhel8.us-east1-b.coryan-test

sudo dnf install https://dl.fedoraproject.org/pub/epel/epel-release-latest-8.noarch.rpm
sudo yum makecache
sudo yum install -y automake ccache cmake3 curl-devel gcc-toolset-10 \
    git libtool make openssl-devel pkgconfig re2-devel tar wget which \
    zlib-devel ninja-build
sudo ln -sf /usr/bin/cmake3 /usr/bin/cmake && sudo ln -sf /usr/bin/ctest3 /usr/bin/ctest


scl enable gcc-toolset-10 bash
```

The following steps will install libraries and tools in `/usr/local`. By
default RHEL8 does not search for shared libraries in these directories,
there are multiple ways to solve this problem, the following steps are one
solution:

```bash
(echo "/usr/local/lib" ; echo "/usr/local/lib64") | sudo tee /etc/ld.so.conf.d/usrlocal.conf
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig
export PATH=/usr/local/bin:${PATH}
```

#### Abseil

We need a recent version of Abseil.

```bash
mkdir -p $HOME/abseil-cpp && cd $HOME/abseil-cpp
curl -sSL https://github.com/abseil/abseil-cpp/archive/20200923.3.tar.gz | \
    tar -xzf - --strip-components=1 && \
    sed -i 's/^#define ABSL_OPTION_USE_\(.*\) 2/#define ABSL_OPTION_USE_\1 0/' "absl/base/options.h" && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DBUILD_SHARED_LIBS=yes \
        -DCMAKE_CXX_STANDARD=11 \
        -H. -Bcmake-out -GNinja && \
  cmake --build cmake-out
sudo cmake --build cmake-out --target install && sudo ldconfig
```

#### Protobuf

We need to install a version of Protobuf that is recent enough to support the
Google Cloud Platform proto files:

```bash
mkdir -p $HOME/protobuf && cd $HOME/protobuf
curl -sSL https://github.com/google/protobuf/archive/v3.16.0.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=yes \
        -Dprotobuf_BUILD_TESTS=OFF \
        -Hcmake -Bcmake-out -GNinja && \
    cmake --build cmake-out
sudo cmake --build cmake-out --target install && sudo ldconfig
```

#### c-ares

Recent versions of gRPC require c-ares >= 1.11, while CentOS-7
distributes c-ares-1.10. Manually install a newer version:

```bash
mkdir -p $HOME/c-ares && cd $HOME/c-ares
curl -sSL https://github.com/c-ares/c-ares/archive/cares-1_14_0.tar.gz | \
    tar -xzf - --strip-components=1 && \
    ./buildconf && ./configure && make -j ${NCPU:-4} && \
    make
sudo make install && sudo ldconfig
```

#### gRPC

We also need a version of gRPC that is recent enough to support the Google
Cloud Platform proto files. We manually install it using:

```bash
mkdir -p $HOME/grpc && cd $HOME/grpc
curl -sSL https://github.com/grpc/grpc/archive/v1.37.1.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DgRPC_INSTALL=ON \
        -DgRPC_BUILD_TESTS=OFF \
        -DgRPC_ABSL_PROVIDER=package \
        -DgRPC_CARES_PROVIDER=package \
        -DgRPC_PROTOBUF_PROVIDER=package \
        -DgRPC_RE2_PROVIDER=package \
        -DgRPC_SSL_PROVIDER=package \
        -DgRPC_ZLIB_PROVIDER=package \
        -H. -Bcmake-out -GNinja && \
    cmake --build cmake-out
sudo cmake --build cmake-out --target install && sudo ldconfig
```

#### crc32c

The project depends on the Crc32c library, we need to compile this from
source:

```bash
mkdir -p $HOME/crc32c && cd $HOME/crc32c
curl -sSL https://github.com/google/crc32c/archive/1.1.0.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=yes \
        -DCRC32C_BUILD_TESTS=OFF \
        -DCRC32C_BUILD_BENCHMARKS=OFF \
        -DCRC32C_USE_GLOG=OFF \
        -H. -Bcmake-out -GNinja && \
    cmake --build cmake-out
sudo cmake --build cmake-out --target install && sudo ldconfig
```

#### nlohmann_json library

The project depends on the nlohmann_json library. We use CMake to
install it as this installs the necessary CMake configuration files.
Note that this is a header-only library, and often installed manually.
This leaves your environment without support for CMake pkg-config.

```bash
mkdir -p $HOME/json && cd $HOME/json
curl -sSL https://github.com/nlohmann/json/archive/v3.9.1.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=yes \
        -DBUILD_TESTING=OFF \
        -H. -Bcmake-out/nlohmann/json 
sudo cmake --build cmake-out/nlohmann/json --target install && sudo ldconfig
```

```shell
mkdir -p $HOME/gtest && cd $HOME/gtest
curl -sSL https://github.com/google/googletest/archive/release-1.10.0.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
        -DCMAKE_BUILD_TYPE="Release" \
        -DBUILD_SHARED_LIBS=yes \
        -H. -Bcmake-out/googletest -GNinja && \
    cmake --build cmake-out/googletest
sudo cmake --build cmake-out/googletest --target install && sudo ldconfig
```

#### Download and compile Google microbenchmark support library:

```shell
mkdir -p $HOME/benchmark && cd $HOME/benchmark
curl -sSL https://github.com/google/benchmark/archive/v1.5.3.tar.gz | \
    tar -xzf - --strip-components=1 && \
    cmake \
        -DCMAKE_BUILD_TYPE="Release" \
        -DBUILD_SHARED_LIBS=yes \
        -DBENCHMARK_ENABLE_TESTING=OFF \
        -H. -Bcmake-out/benchmark -GNinja && \
    cmake --build cmake-out/benchmark
sudo cmake --build cmake-out/benchmark --target install && sudo ldconfig
```

Some of the above libraries may have installed in /usr/local, so make sure
those library directories will be found.

```shell
sudo ldconfig /usr/local/lib*
```

#### google-cloud-cpp

```shell
cd $HOME
git clone -b repro-crash-with-centos-7-and-gcc-10 git@github.com:coryan/google-cloud-cpp
cd google-cloud-cpp
cmake -S . -B.build -GNinja
cmake --build .build --target storage_create_client_integration_test
```
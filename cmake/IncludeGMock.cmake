# Copyright 2017 Google Inc.
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

# Configure the googletest dependency, this can be found as a submodule, package, or
# installed with pkg-config support.
set(GOOGLE_CLOUD_CPP_GMOCK_PROVIDER "module"
        CACHE STRING "How to find the googlemock library")
set_property(CACHE GOOGLE_CLOUD_CPP_GMOCK_PROVIDER
        PROPERTY STRINGS "module" "package" "external" "vcpkg" "pkg-config")

if ("${GOOGLE_CLOUD_CPP_GMOCK_PROVIDER}" STREQUAL "module")
    # Compile the googlemock library.  This library is rarely installed or
    # pre-compiled because it should be configured with the same flags as the
    # application.
    # TODO(#310) - the name of this target can easily conflict, consider changing it.
    add_library(gmock
            ${PROJECT_THIRD_PARTY_DIR}/googletest/googletest/src/gtest_main.cc
            ${PROJECT_THIRD_PARTY_DIR}/googletest/googletest/src/gtest-all.cc
            ${PROJECT_THIRD_PARTY_DIR}/googletest/googlemock/src/gmock-all.cc)
    target_include_directories(gmock
            PUBLIC ${PROJECT_THIRD_PARTY_DIR}/googletest/googletest/include
            PUBLIC ${PROJECT_THIRD_PARTY_DIR}/googletest/googletest
            PUBLIC ${PROJECT_THIRD_PARTY_DIR}/googletest/googlemock/include
            PUBLIC ${PROJECT_THIRD_PARTY_DIR}/googletest/googlemock)

elseif ("${GOOGLE_CLOUD_CPP_GMOCK_PROVIDER}" STREQUAL "vcpkg")
    find_package(GTest REQUIRED)
    # The FindGTest module finds GTest by default, but does not search for
    # GMock, though they are usually installed together.
    __gtest_find_library(GMOCK_LIBRARY            gmock)
    __gtest_find_library(GMOCK_LIBRARY_DEBUG      gmockd)
    if ("${GMOCK_LIBRARY}" MATCHES "-NOTFOUND")
        message(FATAL_ERROR "Cannot find gmock library ${GMOCK_LIBRARY}.")
    endif ()
    mark_as_advanced(GMOCK_LIBRARY)

    find_path(GMOCK_INCLUDE_DIR gmock/gmock.h
            HINTS $ENV{GTEST_ROOT}/include ${GTEST_ROOT}/include
            DOC "The GoogleTest Mocking Library headers")
    if ("${GMOCK_INCLUDE_DIR}" MATCHES "-NOTFOUND")
        message(FATAL_ERROR "Cannot find gmock library ${GMOCK_INCLUDE_DIR}.")
    endif ()
    mark_as_advanced(GMOCK_INCLUDE_DIR)

    add_library(GMock::GMock STATIC IMPORTED)
    set_target_properties(GMock::GMock PROPERTIES
            IMPORTED_LINK_INTERFACE_LIBRARIES GTest::GTest
            INTERFACE_INCLUDE_DIRECTORIES "${GMOCK_INCLUDE_DIRS}")
    __gtest_import_library(GMock::GMock GMOCK_LIBRARY "")
    __gtest_import_library(GMock::GMock GMOCK_LIBRARY "RELEASE")
    __gtest_import_library(GMock::GMock GMOCK_LIBRARY "DEBUG")
    # TODO(#310) - the name of this target can easily conflict, consider changing it.
    add_library(gmock INTERFACE)
    target_link_libraries(gmock INTERFACE GMock::GMock GTest::Main)

elseif ("${GOOGLE_CLOUD_CPP_GMOCK_PROVIDER}" STREQUAL "package")
    find_package(Threads REQUIRED)
    find_package(GTest REQUIRED)

    find_path(GMOCK_INCLUDE_DIR gmock/gmock.h
            HINTS $ENV{GTEST_ROOT}/include ${GTEST_ROOT}/include
            DOC "The GoogleTest Mocking Library headers")
    if ("${GMOCK_INCLUDE_DIR}" MATCHES "-NOTFOUND")
        message(FATAL_ERROR "Cannot find gmock headers ${GMOCK_INCLUDE_DIR}.")
    endif ()
    mark_as_advanced(GMOCK_INCLUDE_DIR)

    find_library(GMOCK_LIBRARY gmock)
    if ("${GMOCK_LIBRARY}" MATCHES "-NOTFOUND")
        message(FATAL_ERROR "Cannot find gmock library ${GMOCK_LIBRARY}.")
    endif ()
    mark_as_advanced(GMOCK_LIBRARY)

    find_library(GMOCK_MAIN_LIBRARY gmock_main)
    if ("${GMOCK_LIBRARY}" MATCHES "-NOTFOUND")
        message(FATAL_ERROR "Cannot find gmock_main library ${GMOCK_MAIN_LIBRARY}.")
    endif ()
    mark_as_advanced(GMOCK_MAIN_LIBRARY)

    add_library(GMock::GMock UNKNOWN IMPORTED)
    set_target_properties(GMock::GMock PROPERTIES
            IMPORTED_LINK_INTERFACE_LIBRARIES "GTest::GTest;Threads::Threads"
            INTERFACE_INCLUDE_DIRECTORIES "${GMOCK_INCLUDE_DIRS}"
            IMPORTED_LOCATION "${GMOCK_LIBRARY}")

    add_library(GMock::Main UNKNOWN IMPORTED)
    set_target_properties(GMock::Main PROPERTIES
            IMPORTED_LINK_INTERFACE_LIBRARIES "GMock::GMock;Threads::Threads"
            INTERFACE_INCLUDE_DIRECTORIES "${GMOCK_INCLUDE_DIRS}"
            IMPORTED_LOCATION "${GMOCK_MAIN_LIBRARY}")

    # TODO(#310) - the name of this target can easily conflict, consider changing it.
    add_library(gmock INTERFACE)
    target_link_libraries(gmock INTERFACE GMock::Main GMock::GMock GTest::GTest)

elseif ("${GOOGLE_CLOUD_CPP_GMOCK_PROVIDER}" STREQUAL "external")
    find_package(Threads REQUIRED)

    include(ExternalProject)
    set(GOOGLETEST_SHA "d175c8bf823e709d570772b038757fadf63bc632")
    ExternalProject_Add(googletest_project
        PREFIX "${CMAKE_BINARY_DIR}/external/googletest"
        URL https://github.com/google/googletest/archive/${GOOGLETEST_SHA}.tar.gz
        URL_HASH SHA256=39a708e81cf68af02ca20cad879d1dbd055364f3ae5588a5743c919a51d7ad46
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" "-H<SOURCE_DIR>" "-B<BINARY_DIR>" "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build "<BINARY_DIR>" --target all
        INSTALL_COMMAND "${CMAKE_COMMAND}" --build "<BINARY_DIR>" --target install
        LOG_DOWNLOAD ON
        LOG_INSTALL ON)

    ExternalProject_Get_Property(googletest_project INSTALL_DIR)

    message("LIB_PREFIX = ${CMAKE_IMPORT_LIBRARY_PREFIX}")
    function (__gmock_library_name VAR lib)
        set(${VAR} "${INSTALL_DIR}/lib/lib${lib}.a" PARENT_SCOPE)
    endfunction ()
    __gmock_library_name(GMOCK_MAIN_LIBRARY gmock_main)
    __gmock_library_name(GMOCK_LIBRARY gmock)
    __gmock_library_name(GTEST_LIBRARY gtest)

    add_dependencies(google-cloud-cpp-external-projects googletest_project)
    add_library(GTest::GTest UNKNOWN IMPORTED)
    set_target_properties(GTest::GTest PROPERTIES
        IMPORTED_LINK_INTERFACE_LIBRARIES "Threads::Threads"
        INTERFACE_INCLUDE_DIRECTORIES "${INSTALL_DIR}/include"
        IMPORTED_LOCATION "${GTEST_LIBRARY}")
    add_library(GMock::GMock UNKNOWN IMPORTED)
    set_target_properties(GMock::GMock PROPERTIES
        IMPORTED_LINK_INTERFACE_LIBRARIES "Threads::Threads"
        INTERFACE_INCLUDE_DIRECTORIES "${INSTALL_DIR}/include"
        IMPORTED_LOCATION "${GMOCK_LIBRARY}")
    add_library(GMock::Main UNKNOWN IMPORTED)
    set_target_properties(GMock::Main PROPERTIES
        IMPORTED_LINK_INTERFACE_LIBRARIES "GMock::GMock;Threads::Threads"
        INTERFACE_INCLUDE_DIRECTORIES "${INSTALL_DIR}/include"
        IMPORTED_LOCATION "${GMOCK_MAIN_LIBRARY}")

    # TODO(#310) - the name of this target can easily conflict, consider changing it.
    add_library(gmock INTERFACE)
    target_link_libraries(gmock INTERFACE GMock::Main GMock::GMock GTest::GTest)

elseif ("${GOOGLE_CLOUD_CPP_GMOCK_PROVIDER}" STREQUAL "pkg-config")
    # Use pkg-config to find the libraries.
    find_package(PkgConfig REQUIRED)
    # We need a helper function to convert pkg-config(1) output into target
    # properties.
    include(${CMAKE_CURRENT_LIST_DIR}/PkgConfigHelper.cmake)

    pkg_check_modules(gmock_pc REQUIRED gmock_main gmock gtest)
    add_library(GMock::GMock INTERFACE IMPORTED)
    set_library_properties_from_pkg_config(GMock::GMock gmock_pc)
    set_property(TARGET GMock::GMock APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES Threads::Threads)

    # TODO(#310) - the name of this target can easily conflict, consider changing it.
    add_library(gmock INTERFACE)
    target_link_libraries(gmock INTERFACE GMock::GMock)
endif ()

#!/usr/bin/env bash
#
# Copyright (c) 2024-present The Namecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export HOST=aarch64-linux-android
export PACKAGES="unzip openjdk-8-jdk gradle cmake ninja-build"
export CONTAINER_NAME=ci_android
export CI_IMAGE_NAME_TAG="docker.io/amd64/ubuntu:22.04"

export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false

export ANDROID_API_LEVEL=28
export ANDROID_BUILD_TOOLS_VERSION=28.0.3
export ANDROID_NDK_VERSION=23.2.8568313
export ANDROID_TOOLS_URL=https://dl.google.com/android/repository/commandlinetools-linux-8512546_latest.zip
export ANDROID_HOME="${DEPENDS_DIR}/SDKs/android"
export ANDROID_NDK_HOME="${ANDROID_HOME}/ndk/${ANDROID_NDK_VERSION}"
export ANDROID_NDK_ROOT="${ANDROID_NDK_HOME}"
export DEP_OPTS="ANDROID_SDK=${ANDROID_HOME} ANDROID_NDK=${ANDROID_NDK_HOME} ANDROID_API_LEVEL=${ANDROID_API_LEVEL}"

# Disable Qt and GUI-related features for Android
export BITCOIN_CONFIG="--disable-tests --disable-bench --without-gui --without-daemon"
export BUILD_GUI=OFF
export ENABLE_WALLET=OFF
export ENABLE_TESTS=OFF

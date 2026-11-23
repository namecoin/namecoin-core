# Android cross-compilation host configuration
# Requires ANDROID_NDK_ROOT and ANDROID_API_LEVEL environment variables to be set

# Compiler settings - use NDK's Clang toolchain
android_CXX=$(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$(ANDROID_API_LEVEL)-clang++
android_CC=$(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$(ANDROID_API_LEVEL)-clang

# For other architectures, we'll override these in the build
ifeq ($(HOST),armv7a-unknown-linux-androideabi)
android_CXX=$(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi$(ANDROID_API_LEVEL)-clang++
android_CC=$(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi$(ANDROID_API_LEVEL)-clang
endif

android_CFLAGS=-std=$(C_STANDARD) -D__ANDROID_API__=$(ANDROID_API_LEVEL)
android_CXXFLAGS=-std=$(CXX_STANDARD) -D__ANDROID_API__=$(ANDROID_API_LEVEL)

# Archiver and related tools
android_AR=$(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar
android_RANLIB=$(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ranlib
android_STRIP=$(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip

# CMake system settings for Android
android_cmake_system_name=Android
android_cmake_system_version=$(ANDROID_API_LEVEL)
android_cmake_system_processor=$(host_arch)

# CMake toolchain file for Android - this is the NDK's toolchain file
android_cmake_toolchain_file=$(ANDROID_NDK_ROOT)/build/cmake/android.toolchain.cmake

# Android ABI based on host
ifeq ($(HOST),aarch64-unknown-linux-android)
android_abi=arm64-v8a
endif
ifeq ($(HOST),armv7a-unknown-linux-androideabi)
android_abi=armeabi-v7a
endif
ifeq ($(HOST),x86_64-unknown-linux-android)
android_abi=x86_64
endif
ifeq ($(HOST),i686-unknown-linux-android)
android_abi=x86
endif

# ANDROID BUILD NOTES

This guide describes how to build Namecoin Core for Android on Linux and macOS.

## Dependencies

Before proceeding with an Android build, you need:

1. **Android SDK** - Download from [developer.android.com/studio](https://developer.android.com/studio)
2. **Android NDK** - Minimum version r23 or later
3. **CMake** - Version 3.18 or later
4. ** Ninja** - Build system

Use the SDK Manager to install:
- Android SDK Platform (API level 28 or higher)
- Android NDK
- CMake
- NDK side-by-side

## Environment Variables

Set the following environment variables before building:

```bash
export ANDROID_SDK=/path/to/android/sdk
export ANDROID_NDK=/path/to/android/ndk
export ANDROID_API_LEVEL=28  # Or higher, e.g., 29, 30, 31
```

For the NDK path, you may need to adjust based on your NDK installation. For newer NDK versions:

```bash
export ANDROID_NDK_ROOT=$ANDROID_NDK
export ANDROID_TOOLCHAIN_BIN=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin
```

On macOS, replace `linux-x86_64` with `darwin-x86_64`:

```bash
export ANDROID_TOOLCHAIN_BIN=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64/bin
```

## Building with depends

The depends system handles cross-compilation dependencies:

```bash
# Build dependencies for Android
make -C depends HOST=aarch64-linux-android ANDROID_API_LEVEL=28 ANDROID_NDK_ROOT=/path/to/android/ndk

# Or for x86_64 (emulator builds)
make -C depends HOST=x86_64-linux-android ANDROID_API_LEVEL=28 ANDROID_NDK_ROOT=/path/to/android/ndk
```

## Building Namecoin Core

After building dependencies, configure and build:

```bash
# Set up environment
export ANDROID_NDK_ROOT=/path/to/android/ndk
export ANDROID_API_LEVEL=28

# Configure with CMake using the depends toolchain
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=depends/aarch64-linux-android/toolchain.cmake \
  -DENABLE_WALLET=OFF \
  -DBUILD_GUI=OFF \
  -DENABLE_TESTS=OFF \
  -DENABLE_UTILS=ON \
  -DENABLE_DAEMON=ON

# Build
cmake --build build -j$(nproc)

# Install (optional)
cmake --install build --prefix depends/aarch64-linux-android
```

## Supported Android ABIs

Namecoin Core supports the following Android ABIs:

- **arm64-v8a** (aarch64-linux-android) - Recommended for modern devices
- **armeabi-v7a** (armv7a-linux-androideabi) - For older 32-bit ARM devices
- **x86_64** (x86_64-linux-android) - For emulators and x86_64 devices
- **x86** (i686-linux-android) - For older x86 devices

## Building for different ABIs

To build for different ABIs, change the `HOST` parameter:

```bash
# ARM64 (most modern Android devices)
make -C depends HOST=aarch64-linux-android ...

# ARM32 (older devices)
make -C depends HOST=armv7a-linux-androideabi ...

# x86_64 (emulators)
make -C depends HOST=x86_64-linux-android ...

# x86 (older emulators)
make -C depends HOST=i686-linux-android ...
```

## Notes

- Android builds currently only support the daemon (namecoind), not the GUI
- The wallet can be enabled with `-DENABLE_WALLET=ON` but requires additional considerations for Android's security model
- For production builds, consider using `-DCMAKE_BUILD_TYPE=Release` for optimizations
- Android API level 28 (Android 9.0 Pie) is the minimum supported version

## Cross-compilation from macOS

When cross-compiling from macOS, adjust the toolchain path:

```bash
export ANDROID_TOOLCHAIN_BIN=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64/bin
```

The rest of the build process remains the same.

## Known Limitations

- No Qt GUI support (Qt 5.x is incompatible with modern NDK versions)
- Wallet functionality may require additional Android-specific integration
- Testing support is limited; use Android emulator for runtime testing

## Troubleshooting

### Toolchain not found
Ensure `ANDROID_NDK_ROOT` points to the correct NDK directory and that the toolchain bin path exists.

### API level too low
Android API level 28 or higher is recommended. Update `ANDROID_API_LEVEL`.

### Linker errors
Some system libraries may not be available on Android. Ensure you're not using features that require Linux-specific syscalls.

### CMake errors
Ensure CMake version is 3.18 or later. Update with `brew install cmake` (macOS) or `apt install cmake` (Linux).

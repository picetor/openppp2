# OpenPPP2 Android client

> [中文](README_CN.md) · [Technical guide (Chinese)](debug_CN.md) · [Rule assets](android/app/src/main/assets/rules/README.md)

**Status:** Experimental

**Type:** Platform-specific Flutter, Android VPN, and JNI client surface

**Last verified:** 2026-07-28

This directory is the Android client that is shipped with this OpenPPP2 tree. It is a Flutter application backed by an Android `VpnService` and the bundled native `libopenppp2.so`; it is not a standalone SDK or a replacement for the native command-line runtime.

## Fork adaptation

The Flutter UI, Kotlin VPN service, tests, and rule assets are synchronized
from Miaocchi/openppp2 at `4289edf`. This branch keeps its existing native
`libopenppp2.cpp` so the project's IPv6, proxy-mode, and asynchronous DNS fixes
are not replaced by a different core revision.

The native core in this branch exposes the older `statistics` and
`get_link_state` surfaces. A Kotlin compatibility layer combines them into the
runtime snapshot schema expected by the imported UI. Connection phase, traffic,
and liveness remain available; newer-core-only P2P, native OTLP telemetry, and
detailed native error fields are reported as unavailable. This branch also
adds `set_root_path` to JNI so relative rule paths resolve below app
`filesDir`.

## What is here

- Flutter UI and profile/settings code in `lib/`.
- Android activity, VPN service, state mirror, and JNI declarations in `android/app/src/main/`.
- Native packaging from the repository build output at `../bin/android/<ABI>/`.
- Native Android CMake sources and the ABI-oriented `build.sh` helper at this directory level.

The UI currently has Home, launch-options, profiles, and settings areas. Treat the client as an experimental platform surface: validate a real connection and the device's VPN behavior before relying on it operationally.

## Startup and runtime path

```text
Flutter VpnService.connect(configJson, vpnOptions)
  -> MethodChannel "supersocksr.ppp/vpn"
  -> MainActivity requests Android VPN permission when needed
  -> PppVpnService in the private :vpn process
  -> VpnService.Builder establishes a TUN interface
  -> JNI configures and runs libopenppp2.so
  -> native callbacks and a service poller mirror runtime state to app files
  -> Flutter polls the mirror while the app is visible
```

`PppVpnService` is deliberately declared in a separate `:vpn` process. The UI therefore does not read native state directly: it asks the activity for a mirrored runtime snapshot, link state, heartbeat, or last error. See [the technical guide](debug_CN.md) before changing that lifecycle or any JNI signature.

## Work on the Flutter application

From this directory, with a compatible Flutter/Android toolchain installed:

```sh
flutter pub get
flutter test
```

A device run/build also needs native libraries matching the active Android
Gradle configuration. Debug APKs currently package `arm64-v8a` and `x86_64`;
Gradle collects them from `bin/android/arm64-v8a/` and
`bin/android/x86_64/`. This
working tree does not contain prebuilt binaries, so build both required ABIs
first.

```sh
flutter run
```

Use a disposable development device and a non-production configuration. Do not put credentials or private endpoints in documentation, screenshots, or committed test profiles.

### Rebuild the native library (maintainers)

`CMakeLists.txt` builds `libopenppp2.so` from the shared C/C++ runtime and
writes it under `bin/android/<ABI>/`; Gradle consumes `jniLibs` directly from
that directory. `build.sh` selects `x86`, `x64`, `arm`, `arm64`, or `all`; it
requires an Android NDK plus matching prebuilt Boost and OpenSSL libraries.

A path-independent template for an arm64 build is:

```sh
cd android
NDK_ROOT=/path/to/android-ndk \
OTHER_ARGS="-DTHIRD_PARTY_LIBRARY_DIR=/path/to/android-third-party" \
./build.sh arm64
```

The helper removes its temporary `build/` directory. Build each ABI selected by
the active Gradle configuration before packaging. Upstream installation, WSL,
and APK helpers tied to one developer machine are intentionally not imported;
pass real paths to the generic build through environment variables.

## GitHub Actions

`Build OpenPPP2 Android Debug APK` builds the arm64 and x64 native libraries,
runs `flutter test`, packages a Flutter Debug APK, verifies that both
`libopenppp2.so` ABI entries are present, and uploads the APK with its SHA-256
file. It runs for relevant pushes and pull requests targeting `inet6`, and can
also be started manually.

The CI artifact uses Android's standard Debug signing key. It is intended for
testing only and cannot replace an existing installation signed with a
different certificate. Release signing credentials are deliberately not stored
in the workflow or repository.

## Documentation boundary

- [Technical guide (Chinese)](debug_CN.md) documents the current Flutter/Kotlin/JNI implementation and troubleshooting signals.
- [Rule assets](android/app/src/main/assets/rules/README.md) documents the bundled GeoIP/GeoSite fallback files.
- [Work status](WORK_STATUS.md) is a status-bound maintenance note, not a record of current build or device-test success.

Configuration field semantics, protocol behavior, and cross-platform runtime guarantees belong to the canonical project documentation, not to this platform wrapper.

# Building from source

## Dependencies

Arch Linux package names. `packaging/ci/arch-packages.txt` is the full list CI and the release workflow install
(it also holds release-only tools such as `squashfs-tools`, `nasm` and `github-cli`).

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf python sdl3 glm zlib vulkan-headers vulkan-icd-loader \
  shaderc glslang ffmpeg openssl nlohmann-json libsodium curl libarchive qt6-base qt6-svg qt6-wayland
```

| Component | CMake option (default) | Needs |
|---|---|---|
| Game client `joan-native` | `JOA_BUILD_ENGINE` (ON) | SDL3, GLM, zlib, Vulkan headers, `glslc` (shaderc), `glslangValidator` (glslang), Python 3, FFmpeg (libavformat, libavcodec, libavutil, libswscale, libswresample) |
| Importer `joa-install` | always | OpenSSL (crypto), nlohmann-json |
| Updater `joa-updater` | `JOA_BUILD_UPDATER` (ON) | libsodium, libcurl, libarchive, nlohmann-json |
| Launcher `joa-launcher` | `JOA_BUILD_LAUNCHER` (ON) | Qt 6.4+ Widgets and Concurrent; needs the updater |

Other options: `JOA_SANITIZE` (ASan + UBSan), `JOA_DEV_OVERLAY` (Dear ImGui developer overlay, Ctrl+F12 in game),
`JOA_TRACY` (Tracy profiler client, downloaded with FetchContent), `JOA_TEST_XVFB` (see Tests).

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DJOA_GAME_DIR=/path/to/original/game
cmake --build build
```

`JOA_GAME_DIR` is the original installation that asset-backed tests read (never written to). It defaults to the
source directory, because the maintainer keeps the repository inside the game folder. Programs land in
`build/src/engine/joan-native`, `build/launcher/joa-launcher`, `build/updater/joa-updater` and
`build/src/install/joa-install`.

Run the game straight from the build tree:

```sh
./build/src/engine/joan-native --game-dir /path/to/original/game
./build/launcher/joa-launcher        # import, settings, updates
```

Controls and command-line options: [PLAYING.md](PLAYING.md).

## Tests

```sh
ctest --test-dir build --output-on-failure -E '\.(gl|vk)_' -j"$(nproc)"   # non-GPU tests
ctest --test-dir build --output-on-failure -R '\.(gl|vk)_' -j1            # GPU tests
```

- Tests that need the original game data exit 77 (reported as skipped) when `JOA_GAME_DIR` has no game.
- GPU tests run OpenGL under a private Xvfb (llvmpipe) and Vulkan offscreen. They share one Xvfb display, so run
  them with `-j1`. Configure `-DJOA_TEST_XVFB=OFF` to run them as visible windows on the real display instead.
- `ctest -L release` runs the release checks; most of them need `-DJOA_FFMPEG_PREFIX=<LGPL FFmpeg prefix>` and
  exit 77 without it ([RELEASE_PROCESS.md](RELEASE_PROCESS.md)).

## Release builds

Release artifacts (AppImage, signed update tarballs) are built by `packaging/release/build_release.py` against an
LGPL-only FFmpeg: [RELEASE_PROCESS.md](RELEASE_PROCESS.md).

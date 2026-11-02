# Wars and Warriors: Joan of Arc — native Linux port

A clean-room-style native Linux reconstruction of Enlight Software's 2004 game *Wars and Warriors: Joan of Arc*.

## Status

Pre-release (alpha). The port is a new C++20 engine with OpenGL and Vulkan renderers that runs the original game
data: menus, movies, music, the mission scripts, combat, troops and RTS mode, saves, and the eight-mission campaign
through to the ending. An automated test bot wins all eight missions in one chain (missions 1–4 on Normal, 5–8 on
Easy), but no full human play-through has been recorded, and behaviour has been compared against the running
original for mission 1 only. Progress per area: [docs/PROGRESS.md](docs/PROGRESS.md). Known gaps and deliberate
differences: [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).

Beyond the original it adds widescreen support (Hor+ with an anchored HUD), gamepad play, Enhanced and Ultra graphics
presets, optional Vulkan ray tracing and AMD FSR 3 upscaling, HD texture packs, and a launcher with a signed updater.
The Original preset keeps the original look.

## You need the original game

Game data is not included. You supply your own copy of the game: an installed GOG version, or the GOG offline
installer (supported in code but not yet tested with a real installer). *Wars and Warriors: Joan of Arc* and its data are © Enlight Software. This project is not affiliated with
or endorsed by Enlight Software.

The importer recognises the GOG release (`joan.exe` 2004-01-19, GOG build 59908888400692417) by the checksum of
`joan.exe` and refuses unknown versions. Retail CD versions are not supported yet ([docs/INSTALLER.md](docs/INSTALLER.md)).

## Download

Test builds are published as GitHub pre-releases: [releases](https://github.com/sergen213/joan-native/releases).
Each release has an AppImage (`JoanNative-<version>-x86_64.AppImage`) plus signed update packages and `SHA256SUMS`.

```sh
sha256sum -c --ignore-missing SHA256SUMS
chmod +x JoanNative-*-x86_64.AppImage
./JoanNative-*-x86_64.AppImage       # starts the launcher
```

## Requirements

- x86_64 Linux. Release builds are made on Arch Linux and need a glibc at least as new as the build host's
  (0.1.0-alpha.18 needs glibc 2.44) and a current libstdc++.
- A GPU driver with OpenGL 3.3 core or Vulkan 1.3. Ray tracing needs a Vulkan GPU with ray queries.
- For the GOG offline installer: `innoextract` on `PATH`.

## Quick start

1. Start the AppImage. The launcher opens.
2. On the **Game** tab, choose **Import Game Folder…** (an installed copy) or **Install from GOG Installer…**.
   The files are copied to `~/.local/share/joan-native/game/` and checked; the source is never changed.
3. Pick a renderer and preset on the **Graphics** tab, then press **Play**.

The launcher's **Updates** tab installs newer signed releases. All releases so far are pre-releases, which only the
`experimental` channel offers; the default channel is `stable`.

Without the launcher:

```sh
JOA_APPIMAGE_ENTRY=joa-install ./JoanNative-*-x86_64.AppImage import /path/to/original/game
JOA_APPIMAGE_ENTRY=joan-native ./JoanNative-*-x86_64.AppImage --game-dir /path/to/original/game
```

Default controls: WASD move, mouse turns, left / right mouse button light / heavy attack, Space jump, Q defend,
F2 RTS mode (from mission 5), F5 / F6 quick save / quick load, Esc menu. Keys are remappable in Options → Input.
All controls, gamepad layout and command-line options: [docs/PLAYING.md](docs/PLAYING.md).

## Build from source

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DJOA_GAME_DIR=/path/to/original/game
cmake --build build
ctest --test-dir build --output-on-failure -E '\.(gl|vk)_'
./build/src/engine/joan-native --game-dir /path/to/original/game
```

Dependencies, options and GPU tests: [docs/BUILDING.md](docs/BUILDING.md).

## Documentation

For players:
- [docs/PLAYING.md](docs/PLAYING.md): controls, gamepad, saves, screenshots, command-line options.
- [docs/LAUNCHER.md](docs/LAUNCHER.md): launcher tabs and the `settings.ini` reference.
- [docs/INSTALLER.md](docs/INSTALLER.md): importing, verifying and repairing the original game.
- [docs/HD_ASSETS.md](docs/HD_ASSETS.md): HD texture packs.
- [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md): known issues and deliberate differences from the original.

For contributors:
- [docs/BUILDING.md](docs/BUILDING.md): building and testing.
- [docs/PROGRESS.md](docs/PROGRESS.md): status per area and next tasks.
- [docs/GRAPHICS_ROADMAP.md](docs/GRAPHICS_ROADMAP.md), [docs/RAYTRACING.md](docs/RAYTRACING.md),
  [docs/CAMERA_AND_WIDESCREEN.md](docs/CAMERA_AND_WIDESCREEN.md): renderers, presets, ray tracing, widescreen.
- [docs/GAMEPLAY_IMPLEMENTATION.md](docs/GAMEPLAY_IMPLEMENTATION.md), [docs/GAMEPLAY_SYSTEMS.md](docs/GAMEPLAY_SYSTEMS.md),
  [docs/SAVES_AND_CAMPAIGN.md](docs/SAVES_AND_CAMPAIGN.md), [docs/AUDIO_AND_VIDEO.md](docs/AUDIO_AND_VIDEO.md): engine systems.
- [docs/ASSET_FORMATS.md](docs/ASSET_FORMATS.md), [docs/formats/](docs/formats/),
  [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md), [docs/reverse-engineering/](docs/reverse-engineering/):
  file formats and reverse-engineering notes.
- [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md): signed updates and release builds.
- [docs/ASSET_RIGHTS.md](docs/ASSET_RIGHTS.md), [docs/THIRD_PARTY_LICENSES.md](docs/THIRD_PARTY_LICENSES.md):
  asset policy and third-party licenses.

## Contributing

Changes are accepted only as pull requests, reviewed and merged by the maintainer. See
[CONTRIBUTING.md](CONTRIBUTING.md).

## License

Source-available, all rights reserved; see [LICENSE](LICENSE). Contributions via pull request only; see
[CONTRIBUTING.md](CONTRIBUTING.md). Versions published before this change remain under GPL-3.0-or-later.
*Wars and Warriors: Joan of Arc* and its data are © Enlight Software and are not included. Third-party components
keep their own licenses: [docs/THIRD_PARTY_LICENSES.md](docs/THIRD_PARTY_LICENSES.md).

# Third-party licenses

Licenses of libraries the port uses or plans to use, and of the community mods it references. "In tree" = currently referenced by a `CMakeLists.txt` in this repo (2026-10-13). Host versions are from `pacman -Qi` on the build machine; license identifiers are the package metadata cross-checked with upstream.

Release rule: every shipped binary package must include the license text of each linked library (`share/licenses/<name>/`) and, for LGPL components, link them dynamically (or ship object files for relinking) and point to the exact source.

## Libraries

| Component | Status | Host version | License (SPDX) | Obligations / notes |
|---|---|---|---|---|
| SDL3 | planned (engine platform layer) | 3.4.18 | Zlib | notice in docs appreciated, not required |
| GLM | planned (math) | 1.0.3 | MIT | keep notice |
| Vulkan-Headers | planned | 1.4.363 | Apache-2.0 OR MIT | keep notice |
| Vulkan loader | planned (system) | 1.4.363 | Apache-2.0 | system library, not shipped |
| shaderc / glslc | build tool only | 2026.4 | Apache-2.0 | outputs (SPIR-V) are ours |
| Qt 6 (Widgets, Concurrent) | **in tree** (`launcher/`) | 6.12.0 | LGPL-3.0-only (also GPL/commercial) | dynamic linking; ship license + source pointer; users must be able to replace Qt libs (AppImage: keep Qt as separate `.so`) |
| libsodium | **in tree** (`updater/`) | 1.0.22 | ISC | keep notice |
| libcurl | **in tree** (`updater/`) | 8.22.0 | curl (MIT-like) | keep notice |
| libarchive | **in tree** (`updater/`) | 3.8.9 | BSD-2-Clause (some files other BSD/Apache variants) | keep notices |
| nlohmann/json | **in tree** (`src/install`, `updater/`) | 3.12.0 | MIT | keep notice |
| OpenSSL (libcrypto) | **in tree** (`src/install` SHA-256) | 3.6.5 | Apache-2.0 | keep notice |
| FreeType | planned (font rendering) | 2.14.3 | FTL OR GPL-2.0-or-later | choose FTL: credit "Portions of this software are copyright © The FreeType Project" in docs |
| miniaudio | planned (audio mixing/output) | single header | Unlicense OR MIT-0 | none |
| OpenAL Soft | alternative to miniaudio for positional audio | 1.25.2 | LGPL-2.0-or-later | dynamic link only |
| Dear ImGui | planned (debug UI) | — | MIT | keep notice |
| Tracy | planned (profiler, dev builds) | — | BSD-3-Clause | keep notice; disabled in release |
| FFmpeg (libavcodec/libavformat/libavutil/libswresample/libswscale) | **in tree** (`src/engine` audio/video: WMA music, WMV movies), shared only | host 9.0.2; release 8.1.3 | LGPL-2.1-or-later **only if built without `--enable-gpl`/`--enable-nonfree`/`--enable-version3`** | Arch's package is built GPL-3.0 (`pacman -Qi ffmpeg`): fine for local builds only. Release packages bundle the minimal LGPL build from `packaging/ffmpeg/build_ffmpeg.py` (+ `ffmpeg.lock.json`: wmav1/2, wmv1/2/3, asf, file) as replaceable `.so` files, with its license/notice files and the byte-exact signed source archive (docs/AUDIO_AND_VIDEO.md §7). Patent status of WMV/WMA decoding is the distributor's concern. |
| zstd | optional (asset cache) | 1.5.7 | BSD-3-Clause OR GPL-2.0-only | choose BSD |

No GPL-only dependency may be linked into shipped binaries unless the whole project is released under a compatible license (project license is decided by the coordinator).

## Referenced community work (not redistributed)

| Project | License | Use in this project |
|---|---|---|
| AlphaYellow FOV Fix (`WarsAndWarriorsJoanOfArcFOVFix`, <https://github.com/alphayellow1/AlphaYellowWidescreenFixes>) | MIT, © 2024 AlphaYellow (repo `LICENSE`) | Formulas studied (aspect replacement, FOV factor); no code copied. Credit in docs. Its deps: safetyhook (BSL-1.0), spdlog (MIT), inipp (MIT) — not used by us. |
| Ultimate ASI Loader (ThirteenAG) | MIT | not used |
| Camera Fix Patch (Urielik / bgajecki, <https://github.com/bgajecki/joa-camera-fix-patch>) | no license file → all rights reserved | Mechanism documented only; patch files not shipped. |
| Joan of Arc: HD Update (FanSH, <https://www.moddb.com/mods/joan-of-arc-hd-update>) | no license or redistribution permission stated | Never shipped; users may install it separately into the override directory. Contains Enlight's copyrighted files (modified `joan.exe`, 377 unmodified original textures). |
| dgVoodoo 2 (Dege) | freeware, closed source | not used |
| NVIDIA RTX Remix config (Steam guide 3716805017) | not stated | not used |

Original game files are proprietary (Enlight Software); see [ASSET_RIGHTS.md](ASSET_RIGHTS.md).

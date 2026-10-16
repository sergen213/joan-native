# Third-party licenses

This project's own source is licensed GPL-3.0-or-later (`LICENSE`). It contains no original game data;
users supply their own copy (docs/ASSET_RIGHTS.md).

What the port links, bundles and references, and how releases comply. The authoritative per-release
list is generated: `share/doc/joan-native/THIRD_PARTY_INVENTORY.json` inside every AppImage/tarball
(`packaging/release/build_release.py`: every bundled file with SHA-256, owning Arch package, version,
SPDX license, upstream URL and the shipped license texts under `share/licenses/`), checked by
`release.license_audit` (docs/RELEASE_PROCESS.md). `packaging/NOTICE.txt` is the human-readable notice.

Release rules: every shipped library's license text is in `share/licenses/<package>/` (or
`share/licenses/spdx/`); LGPL components are separate shared libraries the user can replace
(programs find `lib/` through `$ORIGIN/../lib`); FFmpeg and AppImage-runtime corresponding source ships
as `joan-native-<version>-third-party-source.tar`; no GPL-only library is linked; no original game data
or third-party mod file is in any artifact (`release.license_audit` checks every shipped file against
all 8073 distinct `known_versions.json` hashes and the game-data name patterns).

## Shipped in the AppImage / update tarballs (inventory of 0.1.0, 2026-10-14, host build)

| License (as shipped) | Components |
|---|---|
| LGPL-2.1-or-later | **FFmpeg 8.1.3** own minimal build (avcodec, avformat, avutil, swresample, swscale; wmav1/2, wmv1/2/3, vc1, pcm_s16le, asf; no GPL/nonfree/version3), glib2, acl (libacl), keyutils (libkeyutils) |
| LGPL-3.0-only (Qt also GPL/commercial) | Qt 6.12 qt6-base (Core, Gui, Widgets, Concurrent, DBus, OpenGL, XcbQpa), qt6-svg, qt6-wayland + platform/shell/decoration/image/input plugins |
| LGPL-3.0-or-later OR GPL-2.0-or-later | libidn2, libunistring |
| Zlib | SDL3 3.4.18 |
| Apache-2.0 | OpenSSL 3.6 (libcrypto/libssl) |
| curl | libcurl 8.22 |
| ISC | libsodium 1.0.22 |
| BSD-2-Clause / BSD-4-Clause-UC / Apache-2.0 OR CC0-1.0 OR OpenSSL | libarchive 3.8.9 |
| MIT family | brotli, nghttp2, nghttp3, ngtcp2, libpsl, libxkbcommon(-x11), libxml2, md4c, xcb-util-cursor, libSM, libICE, libxcb-* utils (X11 variants) |
| BSD-3-Clause / BSD-2-Clause / 0BSD / CC0 | double-conversion, libssh2, pcre2, lz4 (lib BSD-2), zstd (BSD-3 OR GPL-2), xz/liblzma (0BSD), libb2 (CC0), bzip2, util-linux libuuid |
| Unicode-3.0 + BSD | ICU 78 |
| libpng-2.0 | libpng |
| MIT (MIT-SIPB) | e2fsprogs libcom_err (krb5 dependency); krb5 (custom MIT) |
| GPL-3.0-or-later WITH GCC-exception-3.1 | libgomp (GCC runtime, via libb2) |
| Compiled in | GLM (MIT), nlohmann/json (MIT), Vulkan-Headers (Apache-2.0 OR MIT), AMD FidelityFX FSR 1 EASU/RCAS algorithms (MIT, `packaging/licenses/FidelityFX-FSR1-LICENSE.txt`), AMD FidelityFX SDK v1.1.4 subset — FSR 3.1.4 upscaler, frame interpolation, optical flow, Vulkan backend (MIT, vendored in `third_party/fidelityfx-sdk/` with provenance and local patches in its `README.md`, `packaging/licenses/FidelityFX-SDK-LICENSE.txt`; `upscale=fsr3`, Vulkan), SMAA 1x (Jimenez et al., MIT: `SMAA.hlsl` + `AreaTex.h`/`SearchTex.h` vendored unmodified in `third_party/smaa/` from iryoku/smaa 71c806a, `PROVENANCE.txt`, `packaging/licenses/SMAA-LICENSE.txt`; `aa=smaa`), RenderDoc in-application API header 1.46 (MIT, vendored `third_party/renderdoc/renderdoc_app.h`, `packaging/licenses/RenderDoc-app-API-LICENSE.txt`; RenderDoc itself is loaded only from the user's install with `--renderdoc`) |
| AppImage runtime (outside the payload) | AppImage type2-runtime (MIT) statically linking libfuse 3.15.0 (LGPL-2.1, patched), squashfuse (BSD-2), musl, zstd, zlib, mimalloc — notices in `share/licenses/appimage-type2-runtime/`, source in the third-party source tarball |

Host-provided, never bundled: glibc, libstdc++/libgcc_s, libGL/GLX/EGL/OpenGL/GLdispatch, Vulkan
loader, libdrm/gbm, X11/xcb core, Wayland client libraries, fontconfig/freetype/harfbuzz/expat, zlib,
dbus, systemd, udev, ALSA. SDL3 reaches audio/video backends (PipeWire, PulseAudio, Wayland, X11,
Vulkan) through dlopen on the host.

Arch package metadata that is non-SPDX or package-wide is corrected per bundled library in
`packaging/licenses/overrides.json` (texts fetched from the upstream tags: e2fsprogs NOTICE + libcom_err
header, lz4 lib/LICENSE, xz COPYING.0BSD).

Open publication item: the LGPL Arch libraries (Qt 6, glib2, libidn2, libunistring, acl, keyutils,
libgomp) are unmodified distribution builds; the inventory names package/version and the Arch
packaging repository, but their source archives are not yet mirrored next to the release (FFmpeg's
and the runtime's are).

## Build-time only

| Component | License | Notes |
|---|---|---|
| shaderc / glslc, glslang | Apache-2.0 / BSD | SPIR-V outputs are ours |
| CMake, Ninja, GCC, nasm, squashfs-tools | various | not shipped |
| Tracy (`JOA_TRACY=ON`, dev builds) | BSD-3-Clause | disabled in releases |
| Dear ImGui 1.93.0 (`JOA_DEV_OVERLAY=ON`, dev builds) | MIT | vendored in `third_party/imgui` with `LICENSE.txt`; not in releases |

Not used (earlier plans): OpenAL Soft, miniaudio, FreeType (directly).

## Referenced community work (not redistributed)

| Project | License | Use in this project |
|---|---|---|
| AlphaYellow FOV Fix (`WarsAndWarriorsJoanOfArcFOVFix`, <https://github.com/alphayellow1/AlphaYellowWidescreenFixes>) | MIT, © 2024 AlphaYellow (repo `LICENSE`) | Formulas studied (aspect replacement, FOV factor); no code copied. Credit in docs. Its deps: safetyhook (BSL-1.0), spdlog (MIT), inipp (MIT) — not used by us. |
| Ultimate ASI Loader (ThirteenAG) | MIT | not used |
| Camera Fix Patch (Urielik / bgajecki, <https://github.com/bgajecki/joa-camera-fix-patch>) | no license file → all rights reserved | Mechanism documented only; patch files not shipped. |
| Joan of Arc: HD Update (FanSH, <https://www.moddb.com/mods/joan-of-arc-hd-update>) | no license or redistribution permission stated | Never shipped; users import it themselves (`joa-install hd-import`). Contains Enlight's copyrighted files (modified `joan.exe`, 377 unmodified original textures). |
| dgVoodoo 2 (Dege) | freeware, closed source | not used |
| NVIDIA RTX Remix config (Steam guide 3716805017) | not stated | not used |

Original game files are proprietary (Enlight Software); see [ASSET_RIGHTS.md](ASSET_RIGHTS.md).
The project's own license has not been chosen yet (no `LICENSE` at the repository root).

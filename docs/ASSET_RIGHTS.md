# Asset rights policy

Not legal advice; this is the project's working policy. Evidence for the facts is in [SOURCE_AUDIT.md](SOURCE_AUDIT.md) and [EXISTING_MODS_AND_TOOLS.md](EXISTING_MODS_AND_TOOLS.md).

## 1. Original game (Enlight Software)

- "Wars and Warriors: Joan of Arc" is a proprietary commercial game (PCGamingWiki: license `commercial`; current executables are Authenticode-signed by **Enlight Software Limited**, Hong Kong). Sold DRM-free on GOG and GamersGate, with Steam DRM on Steam.
- **Everything in the original install is user-supplied**: executables, DLLs, all data directories, music, movies, manual, `readme.htm`, `CONFIG.DAT`, saves.
- The repository and release packages contain **no** original bytes: no textures, models, sounds, scripts, maps, fonts, text, extracted or converted derivatives, thumbnails, or screenshots of copyrighted art used as test fixtures. The whitelist `.gitignore` enforces this for the repo root.
- Tests that need data read it from `JOA_GAME_DIR` at run time and skip (exit 77) when it is absent. Expected hashes and sizes (facts, not content) may be tracked.
- The installer imports from the user's own install (GOG/Steam/retail) and verifies it against `goggame-1421210170.hashdb` or our SHA-256 manifest. It never downloads game data and never bypasses DRM or copy protection (retail SecuROM/StarForce discs are only supported if the user can already install them).
- Caches derived from user data (decoded textures, converted audio, shader caches) live in the user's data directory and are never uploaded.

## 2. Reverse-engineering material

- Disassembly/decompilation is done locally for interoperability. Decompiler output, Ghidra projects and dumps stay in `work/` (gitignored). Tracked docs may quote short addresses, byte signatures and formulas needed to explain behaviour; tracked code is written independently.

## 3. Third-party mods

| Mod | Rights status | May we redistribute? | How users get it |
|---|---|---|---|
| FOV Fix (AlphaYellow) | MIT | Legally yes, but not needed (Windows ASI). We only cite it. | n/a |
| Camera Fix Patch (Urielik / bgajecki) | no license → all rights reserved; also a patch to Enlight's exe | No | n/a — behaviour reimplemented |
| Joan of Arc: HD Update v0.95 (FanSH) | no license/permission stated; contains a modified Enlight `joan.exe`, an Enlight `VSetting.exe`, 377 unmodified original textures, and AI-upscaled derivatives of Enlight textures | **No.** Not bundled, not mirrored, not auto-downloaded. | User downloads it from ModDB and points the launcher at it. |
| Orlean, Arthur & Bastard replacer, RTX Remix config, translations | no license stated | No | user-installed |

Enhanced-asset support therefore works as an **optional override pack installed separately**:
- The launcher accepts a user-chosen archive or folder (e.g. `HDUpdate.rar`), verifies its MD5/SHA-256 against known values (`HDUpdate.rar` MD5 `73c1eebb67386df510df4123ffc48fd3`), and copies only texture/model files into the user's override directory. It never copies the mod's `joan.exe`, `VSetting.exe` or `CONFIG.DAT`.
- Override files are resolved by original relative path; files identical to the original are skipped.
- We may publish file lists, hashes and statistics about mods (facts), not their content.
- If an author grants explicit written permission, record it here (date, scope, link) before shipping anything.

## 4. Our own enhanced assets

- New assets created by the project (e.g. normal/roughness maps, AI-upscaled textures) are **derivatives of Enlight's art** when generated from original textures, so they are distributed only as a local generation step that runs on the user's machine (tool + model with compatible license), not as downloadable packs.
- Newly authored assets with no original input (shaders, UI icons, placeholder art) are project-licensed and may be shipped.
- Upscaler models must have licenses permitting this use (e.g. Real-ESRGAN code BSD-3-Clause; check each model's weights license before integration).

# Original asset formats — index

One line per format; details, byte layouts, evidence and confidence labels live in the linked documents.
Parsers are in `src/formats/` (library `joa_formats`); the renderer bridge is `src/engine/assets_cache.*`
and the scenario loader `src/engine/world_scene.*` (see `docs/GRAPHICS_ROADMAP.md`, "First original scene").

| Format | Where | What | Parser | Doc |
|---|---|---|---|---|
| `.SMO` | `model/`, `item/`, `unit/` | static mesh v500: submeshes, triangle strips, textures, trailer tags | `model_mesh.hpp` | [MODELS_AND_ANIMATION](formats/MODELS_AND_ANIMATION.md#smo--am1-common-container-version-500) |
| `.AM1` | `unit/`, `item/`, `model/` | skinned mesh v500: per-influence bone-space positions, no bind pose | `model_mesh.hpp` | [MODELS_AND_ANIMATION](formats/MODELS_AND_ANIMATION.md#smo--am1-common-container-version-500) |
| `.AM3` | beside AM1, `anim/` | bone hierarchy (3ds Max Biped names) | `anim_clip.hpp` | [MODELS_AND_ANIMATION](formats/MODELS_AND_ANIMATION.md#am3-skeleton) |
| `.AM2` | `anim/`, `item/**/anim/` | clip table + model-space 3x4 bone keys + events | `anim_clip.hpp` | [MODELS_AND_ANIMATION](formats/MODELS_AND_ANIMATION.md#am2-animation-dwwen3danimdefcpp) |
| `.DDS` | `texture/**`, `int8x6/**` | DXT1/3/5 and bit-mask textures, mips, cubemaps | `dds.hpp` | [MODELS_AND_ANIMATION](formats/MODELS_AND_ANIMATION.md#dds) |
| `.RES` / `.SET` | `Resource/`, `map/scenarioN/`, `item/`, `unit/`, `script/unit/` | indexed containers (Name9 / Name34) | `res_archive.hpp` | [CONTAINERS_WORLD_SAVES §2](formats/CONTAINERS_WORLD_SAVES.md#2-resource-containers) |
| dBase III | inside `*.SET`, `RESOURCE` | game database tables | `res_archive.hpp` | [CONTAINERS_WORLD_SAVES §2.2](formats/CONTAINERS_WORLD_SAVES.md#22-dbase-iii-tables--confirmed-by-data-all-242-tables-formatsres_archive) |
| `.lgt` `.sca` `.day` `.pro` | `map/`, `Resource/`, `item/Object/**` | `Key : value` text (lights, scattering, day stages, item records) | `world_text.hpp` | [CONTAINERS_WORLD_SAVES §3.1](formats/CONTAINERS_WORLD_SAVES.md#31-keyvalue-text) |
| `.stg` `.pla` `.uni` `.wor` `.ter` `.trk` `.cm` `.pth` `.por` | `map/scenarioN/` | block text (`CreateObject {…}`): placements, places, units, camera tracks | `world_text.hpp` | [CONTAINERS_WORLD_SAVES §3.2](formats/CONTAINERS_WORLD_SAVES.md#32-block-text) |
| `.GRI` | `map/scenarioN/` | terrain vertex grid: heights, vertex colour, ground types, sectors | `map_grid.hpp` | [CONTAINERS_WORLD_SAVES §4.1](formats/CONTAINERS_WORLD_SAVES.md#41-gri-terrain-grid--confirmed-formatsmap_grid) |
| `*_scene_sector_N.dds` | `texture/terrain/` | per-sector baked terrain lightmaps | `dds.hpp` | [CONTAINERS_WORLD_SAVES §4.1](formats/CONTAINERS_WORLD_SAVES.md#41-gri-terrain-grid--confirmed-formatsmap_grid) |
| `.PLN` `.map` `.pfd` | `map/scenarioN/` | precomputed sector data / campaign-map placements / pathfinding (not parsed) | — | [CONTAINERS_WORLD_SAVES §4.2](formats/CONTAINERS_WORLD_SAVES.md#42-other-map-files) |
| `.fin` | `font/` | bitmap font metrics (atlas = `int8x6/<stem>.dds`) | `font_fin.hpp` | [CONTAINERS_WORLD_SAVES §5](formats/CONTAINERS_WORLD_SAVES.md#5-fonts-fontfin--confirmed-formatsfont_fin) |
| `CONFIG.DAT` | install root | 0xB05-byte options dump | `config_dat.hpp` | [CONTAINERS_WORLD_SAVES §6](formats/CONTAINERS_WORLD_SAVES.md#6-configdat--confirmed-structure-field-labels-per-row) |
| `.SAV`, `Player.dat` | `Save/` | record-mode saves and profile | `save_sav.hpp` | [CONTAINERS_WORLD_SAVES §7](formats/CONTAINERS_WORLD_SAVES.md#7-saves) |
| `.sce` | `script/scenario/` | scenario script language (C-like text) | `script_ast.hpp` | [SCRIPTS_AND_GAME_DATA](formats/SCRIPTS_AND_GAME_DATA.md#scenario-script-language-sce-unitai-bodies) |
| `*EX.res`, `.obj`, `_map.bm` | `script/scenario/` | string tables, briefing text, briefing 3D-map scenes | `text_strings.hpp`, `world_text.hpp` | [SCRIPTS_AND_GAME_DATA](formats/SCRIPTS_AND_GAME_DATA.md#summary) |
| `script/pscript/*.txt` | `script/pscript/` | particle-system definitions | `script_particle.hpp` | [SCRIPTS_AND_GAME_DATA](formats/SCRIPTS_AND_GAME_DATA.md#particle-systems-scriptpscripttxt) |
| `scenario_0N.scr` | `scenario/` | campaign step table (world/unit/script files per mission) | `scenario_def.hpp` | [SCRIPTS_AND_GAME_DATA](formats/SCRIPTS_AND_GAME_DATA.md#campaign-files-scenarioscenario_0nscr--confirmed) |
| `GAMEPLAY.RES` | `Resource/` | `{SECTION}[KEY]value` tuning tables | `text_vars.hpp` | [SCRIPTS_AND_GAME_DATA](formats/SCRIPTS_AND_GAME_DATA.md#gameplayres-sectionkeyvalue) |
| `unit.res`, `item.res` | `unit/`, `item/` | unit / item records | `world_text.hpp` | [SCRIPTS_AND_GAME_DATA](formats/SCRIPTS_AND_GAME_DATA.md#unitres--itemres-records) |
| `des/*.txt` | `des/`, `item_des/` | descriptions (Latin-1, `~` pages) | `text_strings.hpp` | [SCRIPTS_AND_GAME_DATA](formats/SCRIPTS_AND_GAME_DATA.md#summary) |

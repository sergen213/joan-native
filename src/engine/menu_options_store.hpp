#pragma once
// Options persistence and the remap key tables (remapkey.cpp). GameOptions + key codes live in
// $XDG_CONFIG_HOME/joan-native/options.ini; missing values come from the game's CONFIG.DAT (0xB05-byte dump
// of the config object 0xA57FB8), else the GameOptions/STD.SET DEFAULTK defaults.
// RE notes: work/ui/notes_MenuOptions.md (merged into docs/reverse-engineering/UI_SYSTEM.md).
#include "engine/ui_model.hpp"
#include "formats/config_dat.hpp"
#include "formats/vfs_fs.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace joa::engine {

// STD.SET DEFAULTK rows as KeyMap_load 0x4cae70 builds them (empty hex field = 0xffff / 0xff), code = def.
// Cached per game root; empty without STD.SET.
const std::vector<KeyBindingRow>& default_bindings(const formats::Vfs& vfs);
// KeyMap_get_name 0x4cb5b0: name of the row's binding in the input method's key scheme (0 keyboard -> K_KEY,
// 2 gamepad -> pad button else J_KEY, 1 keyboard & mouse -> FPS_KEY else mouse button), looked up in STD.SET
// KEYDESC. nullptr = code not in KEYDESC.
const std::string* binding_name(const formats::Vfs& vfs, const KeyBindingRow& row, int input_method);
// Current-scheme name of DEFAULTK record `action` (m.bindings, else the defaults); "" if none.
std::string action_key_name(const formats::Vfs& vfs, const MenuModel& m, int action);
// RemapKey_assign 0x4cba40: bind DEFAULTK record `action` (rows[action-1]) in key scheme `scheme` (0 keyboard,
// 1 pad, 3 keyboard & mouse) to device 0 key (DIK) / 1 pad button / 2 mouse button (0-based). Refuses action 34
// (Main Game Menu) and codes absent from KEYDESC. The other action that held the same key/button gets its
// DEFAULTK binding back, or the replaced binding (swap) when its default is taken. false = refused.
bool remap_assign(const formats::Vfs& vfs, std::vector<KeyBindingRow>& rows, int scheme, int action, int device, int code);
int key_scheme(int input_method);  // {0, 3, 1, 2}[input_method] (0x73ca80 + Point & Click -> mode 2)
// SDL scancode -> DirectInput DIK code (the codes K_KEY/FPS_KEY store); 0 = no DIK equivalent.
int sdl_to_dik(int sdl_scancode);

// CONFIG.DAT fields -> options, and its user key table (custom_keys at 0xB4, 0x30-byte entries at 0xB5).
void options_from_config(const formats::GameConfig& cfg, GameOptions& o, std::vector<KeyBindingRow>& rows);

std::filesystem::path options_ini_path();  // $XDG_CONFIG_HOME (else ~/.config) /joan-native/options.ini
// Defaults (GameOptions{}, STD.SET DEFAULTK, then CONFIG.DAT if present), then `ini` if it exists.
void load_options(const formats::Vfs& vfs, const std::filesystem::path& ini, GameOptions& o, std::vector<KeyBindingRow>& rows);
bool save_options(const std::filesystem::path& ini, const GameOptions& o, const std::vector<KeyBindingRow>& rows);

}  // namespace joa::engine

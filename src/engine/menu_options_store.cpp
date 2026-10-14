#include "engine/menu_options_store.hpp"

#include "formats/res_archive.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace joa::engine {
namespace {

struct KeyDesc {
    int device, code;   // device 0 key (DIK), 1 pad button, 2 mouse button
    std::string name;
};

struct KeyTables {
    std::vector<KeyBindingRow> rows;
    std::vector<KeyDesc> desc;
};

int hex_or(const std::string& s, int empty) { return s.empty() ? empty : int(std::strtol(s.c_str(), nullptr, 16)); }

// KeyMap_load 0x4cae70: DEFAULTK {KEY_ACTION, K_KEY, J_KEY, J_JOYPAD, FPS_KEY, FPS_MOUSE} and KEYDESC
// {DEVICE, EVENT, STRING}; hex everywhere except FPS_MOUSE and DEVICE (atoi).
KeyTables load_tables(const formats::Vfs& vfs) {
    KeyTables t;
    const auto path = vfs.resolve("Resource/STD.SET");
    std::optional<formats::ResArchive> set = path ? formats::ResArchive::open(*path) : std::nullopt;
    auto table = [&](const char* name) -> std::optional<formats::DbfTable> {
        const formats::ResEntry* e = set ? set->find(name) : nullptr;
        return e ? formats::DbfTable::parse(set->data(*e)) : std::nullopt;
    };
    if (auto d = table("DEFAULTK")) {
        const int f[6] = {d->field_index("KEY_ACTION"), d->field_index("K_KEY"),   d->field_index("J_KEY"),
                          d->field_index("J_JOYPAD"),   d->field_index("FPS_KEY"), d->field_index("FPS_MOUSE")};
        if (std::find(std::begin(f), std::end(f), -1) == std::end(f))
            for (std::size_t r = 0; r < d->record_count(); ++r) {
                KeyBindingRow row;
                row.action = int(r) + 1;
                row.label = d->text(r, std::size_t(f[0]));
                const std::string mouse = d->text(r, std::size_t(f[5]));
                row.def = {hex_or(d->text(r, std::size_t(f[1])), 0xffff), hex_or(d->text(r, std::size_t(f[2])), 0xffff),
                           hex_or(d->text(r, std::size_t(f[3])), 0xff), hex_or(d->text(r, std::size_t(f[4])), 0xffff),
                           mouse.empty() ? 0xff : std::atoi(mouse.c_str())};
                row.code = row.def;
                row.header = row.action == 1 || row.action == 11 || row.action == 22 || row.action == 33 ||
                             row.action == 42 || row.action == 47 || row.action == 52;
                t.rows.push_back(std::move(row));
            }
    }
    if (auto k = table("KEYDESC")) {
        const int dv = k->field_index("DEVICE"), ev = k->field_index("EVENT"), st = k->field_index("STRING");
        if (dv >= 0 && ev >= 0 && st >= 0)
            for (std::size_t r = 0; r < k->record_count(); ++r)
                t.desc.push_back({std::atoi(k->text(r, std::size_t(dv)).c_str()), hex_or(k->text(r, std::size_t(ev)), 0),
                                  k->text(r, std::size_t(st))});
    }
    return t;
}

const KeyTables& tables(const formats::Vfs& vfs) {
    static std::map<std::filesystem::path, KeyTables> cache;  // ponytail: never evicted, one entry per game dir
    auto it = cache.find(vfs.root());
    if (it == cache.end()) it = cache.emplace(vfs.root(), load_tables(vfs)).first;
    return it->second;
}

// RemapKey_find_desc 0x4cb9b0
const std::string* find_desc(const KeyTables& t, int device, int code) {
    for (const KeyDesc& d : t.desc)
        if (d.device == device && d.code == code) return &d.name;
    return nullptr;
}

}  // namespace

const std::vector<KeyBindingRow>& default_bindings(const formats::Vfs& vfs) { return tables(vfs).rows; }

const std::string* binding_name(const formats::Vfs& vfs, const KeyBindingRow& r, int input_method) {
    const KeyTables& t = tables(vfs);
    switch (input_method) {
        case 0: case 3: return find_desc(t, 0, r.code[0]);  // point & click reads the K_KEY column (0x438b30)
        case 2: return r.code[2] == 0 || r.code[2] == 0xff ? find_desc(t, 0, r.code[1]) : find_desc(t, 1, r.code[2]);
        default: return r.code[3] == 0 || r.code[3] == 0xffff ? find_desc(t, 2, r.code[4]) : find_desc(t, 0, r.code[3]);
    }
}

std::string action_key_name(const formats::Vfs& vfs, const MenuModel& m, int action) {
    const std::vector<KeyBindingRow>& rows = m.bindings.empty() ? default_bindings(vfs) : m.bindings;
    for (const KeyBindingRow& r : rows)
        if (r.action == action) {
            const std::string* n = binding_name(vfs, r, m.options.input_method);
            return n ? *n : std::string();
        }
    return {};
}

// Options combo 0x73ca80 {Keyboard, Keyboard & Mouse, Gamepad} -> modes {0, 3, 1}; Point & Click (port entry) -> 2.
int key_scheme(int input_method) { return input_method == 0 ? 0 : input_method == 2 ? 1 : input_method == 3 ? 2 : 3; }

bool remap_assign(const formats::Vfs& vfs, std::vector<KeyBindingRow>& rows, int scheme, int action, int device, int code) {
    if (action == 0x22 || action < 1 || std::size_t(action) > rows.size()) return false;
    if (!find_desc(tables(vfs), device, device == 0 ? code : code + 1)) return false;
    if (scheme == 0 && device != 0) return true;  // keyboard scheme only takes keys (accepted, nothing changes)
    // Fields of the scheme: key (+0x24 / +0x28 / +0x2c) and button (+0x22 pad / +0x23 mouse; none for keyboard).
    const int kf = scheme == 0 ? 0 : scheme == 1 ? 1 : 3, bf = scheme == 0 ? -1 : scheme == 1 ? 2 : 4;
    const int key = device == 0 ? code : 0, btn = device == (scheme == 1 ? 1 : 2) ? code + 1 : 0;
    KeyBindingRow& r = rows[std::size_t(action - 1)];
    const int old_key = r.code[std::size_t(kf)], old_btn = bf >= 0 ? r.code[std::size_t(bf)] : 0;
    r.code[std::size_t(kf)] = key;
    if (bf >= 0) r.code[std::size_t(bf)] = btn;
    auto field = [&](std::size_t i, int f) { return rows[i].code[std::size_t(f)]; };
    const std::size_t n = std::min<std::size_t>(rows.size(), 0x37);
    std::size_t c = 0;  // last other action (1-based) holding the new key / button
    for (std::size_t i = 0; i < n; ++i)
        if (int(i) + 1 != action && ((key != 0 && field(i, kf) == key) || (bf >= 0 && btn != 0 && field(i, bf) == btn))) c = i + 1;
    if (c == 0) return true;
    KeyBindingRow& o = rows[c - 1];
    // Default free? (keyboard scheme skips the assigned action in this test, the others skip the conflicting one)
    const int skip = scheme == 0 ? action : int(c);
    bool restore = true;
    for (std::size_t i = 0; i < n; ++i)
        if (int(i) + 1 != skip && ((field(i, kf) != 0 && field(i, kf) == o.def[std::size_t(kf)]) ||
                                   (bf >= 0 && field(i, bf) != 0 && field(i, bf) == o.def[std::size_t(bf)])))
            restore = false;
    o.code[std::size_t(kf)] = restore ? o.def[std::size_t(kf)] : old_key;
    if (bf >= 0) o.code[std::size_t(bf)] = restore ? o.def[std::size_t(bf)] : old_btn;
    return true;
}

int sdl_to_dik(int sc) {
    static const std::map<int, int> map = [] {
        std::map<int, int> m;
        const char* letters = "\x1e\x30\x2e\x20\x12\x21\x22\x23\x17\x24\x25\x26\x32\x31\x18\x19\x10\x13\x1f\x14\x16\x2f\x11\x2d\x15\x2c";
        for (int i = 0; i < 26; ++i) m[SDL_SCANCODE_A + i] = (unsigned char)letters[i];
        for (int i = 0; i < 10; ++i) m[SDL_SCANCODE_1 + i] = 2 + i;  // 1..9, 0 -> DIK 2..0x0B
        for (int i = 0; i < 10; ++i) m[SDL_SCANCODE_F1 + i] = 0x3b + i;
        const int pairs[][2] = {
            {SDL_SCANCODE_ESCAPE, 0x01}, {SDL_SCANCODE_MINUS, 0x0c}, {SDL_SCANCODE_EQUALS, 0x0d}, {SDL_SCANCODE_BACKSPACE, 0x0e},
            {SDL_SCANCODE_TAB, 0x0f}, {SDL_SCANCODE_LEFTBRACKET, 0x1a}, {SDL_SCANCODE_RIGHTBRACKET, 0x1b},
            {SDL_SCANCODE_RETURN, 0x1c}, {SDL_SCANCODE_LCTRL, 0x1d}, {SDL_SCANCODE_SEMICOLON, 0x27},
            {SDL_SCANCODE_APOSTROPHE, 0x28}, {SDL_SCANCODE_GRAVE, 0x29}, {SDL_SCANCODE_LSHIFT, 0x2a},
            {SDL_SCANCODE_BACKSLASH, 0x2b}, {SDL_SCANCODE_COMMA, 0x33}, {SDL_SCANCODE_PERIOD, 0x34},
            {SDL_SCANCODE_SLASH, 0x35}, {SDL_SCANCODE_RSHIFT, 0x36}, {SDL_SCANCODE_KP_MULTIPLY, 0x37},
            {SDL_SCANCODE_LALT, 0x38}, {SDL_SCANCODE_SPACE, 0x39}, {SDL_SCANCODE_CAPSLOCK, 0x3a},
            {SDL_SCANCODE_NUMLOCKCLEAR, 0x45}, {SDL_SCANCODE_SCROLLLOCK, 0x46}, {SDL_SCANCODE_KP_7, 0x47},
            {SDL_SCANCODE_KP_8, 0x48}, {SDL_SCANCODE_KP_9, 0x49}, {SDL_SCANCODE_KP_MINUS, 0x4a}, {SDL_SCANCODE_KP_4, 0x4b},
            {SDL_SCANCODE_KP_5, 0x4c}, {SDL_SCANCODE_KP_6, 0x4d}, {SDL_SCANCODE_KP_PLUS, 0x4e}, {SDL_SCANCODE_KP_1, 0x4f},
            {SDL_SCANCODE_KP_2, 0x50}, {SDL_SCANCODE_KP_3, 0x51}, {SDL_SCANCODE_KP_0, 0x52}, {SDL_SCANCODE_KP_PERIOD, 0x53},
            {SDL_SCANCODE_F11, 0x57}, {SDL_SCANCODE_F12, 0x58}, {SDL_SCANCODE_KP_ENTER, 0x9c}, {SDL_SCANCODE_RCTRL, 0x9d},
            {SDL_SCANCODE_KP_DIVIDE, 0xb5}, {SDL_SCANCODE_RALT, 0xb8}, {SDL_SCANCODE_PAUSE, 0xc5}, {SDL_SCANCODE_HOME, 0xc7},
            {SDL_SCANCODE_UP, 0xc8}, {SDL_SCANCODE_PAGEUP, 0xc9}, {SDL_SCANCODE_LEFT, 0xcb}, {SDL_SCANCODE_RIGHT, 0xcd},
            {SDL_SCANCODE_END, 0xcf}, {SDL_SCANCODE_DOWN, 0xd0}, {SDL_SCANCODE_PAGEDOWN, 0xd1}, {SDL_SCANCODE_INSERT, 0xd2},
            {SDL_SCANCODE_DELETE, 0xd3}};
        for (const auto& p : pairs) m[p[0]] = p[1];
        return m;
    }();
    const auto it = map.find(sc);
    return it == map.end() ? 0 : it->second;
}

void options_from_config(const formats::GameConfig& c, GameOptions& o, std::vector<KeyBindingRow>& rows) {
    o.shadow = std::clamp(int(c.shadow_mode), 0, 3);
    o.brightness = c.brightness;
    o.contrast = c.contrast;
    o.gamma = c.gamma;
    o.draw_distance = std::clamp((c.view_distance - 0x401) * 20 / 0x3c00, 0, 20);  // inverse of 0x74a700
    o.dynamic_grass = c.dynamic_grass != 0;
    o.grass_distance = c.grass_distance;
    o.grass_density = c.grass_density;
    o.music_volume = c.music_volume;
    o.environment_volume = c.sfx_volume;   // 0x3C
    o.sound_volume = c.speech_volume;      // 0x41
    o.sound_3d = c.sound_3d != 0;
    // 0x73ca80: {0,3,1}; control_scheme 2 (mode 2, reachable only through CONFIG.DAT) -> Point & Click.
    o.input_method = c.control_scheme == 0 ? 0 : c.control_scheme == 1 ? 2 : c.control_scheme == 2 ? 3 : 1;
    o.mouse_sensitivity = c.mouse_sensitivity;
    o.invert_mouse = c.control_73 != 0;
    o.riding_mode = c.control_74;
    o.melee_auto_target = c.control_75 == 0;
    o.riding_camera_angle = c.camera_turn_deg;
    o.difficulty = std::clamp(int(c.difficulty), 0, 3);
    if (!c.custom_keys) return;
    // KeyMap_apply_config 0x4cc220: per-action +0x22..+0x2c from config +0xb5 + (id-1)*0x30
    auto u32 = [](const std::uint8_t* p) { return int(p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24); };
    for (KeyBindingRow& r : rows) {
        if (r.action < 1 || std::size_t(r.action) > c.keys.size()) continue;
        const std::uint8_t* e = c.keys[std::size_t(r.action - 1)].raw.data();
        r.code = {u32(e + 0x24), u32(e + 0x28), e[0x22], u32(e + 0x2c), e[0x23]};
    }
}

std::filesystem::path options_ini_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = xdg && *xdg ? xdg : std::filesystem::path(home ? home : ".") / ".config";
    return base / "joan-native" / "options.ini";
}

void load_options(const formats::Vfs& vfs, const std::filesystem::path& ini, GameOptions& o, std::vector<KeyBindingRow>& rows) {
    o = GameOptions{};
    rows = default_bindings(vfs);
    if (auto bytes = vfs.read("CONFIG.DAT"))
        if (auto cfg = formats::parse_config_dat(*bytes)) options_from_config(*cfg, o, rows);
    std::ifstream in(ini);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const char* v = line.c_str() + eq + 1;
        const float f = std::strtof(v, nullptr);
        const int i = std::atoi(v);
        if (k == "shadow") o.shadow = i;
        else if (k == "brightness") o.brightness = f;
        else if (k == "contrast") o.contrast = f;
        else if (k == "gamma") o.gamma = f;
        else if (k == "draw_distance") o.draw_distance = i;
        else if (k == "dynamic_grass") o.dynamic_grass = i != 0;
        else if (k == "grass_distance") o.grass_distance = i;
        else if (k == "grass_density") o.grass_density = i;
        else if (k == "music_volume") o.music_volume = i;
        else if (k == "sound_volume") o.sound_volume = i;
        else if (k == "environment_volume") o.environment_volume = i;
        else if (k == "sound_3d") o.sound_3d = i != 0;
        else if (k == "input_method") o.input_method = i;
        else if (k == "mouse_sensitivity") o.mouse_sensitivity = f;
        else if (k == "invert_mouse") o.invert_mouse = i != 0;
        else if (k == "riding_camera_angle") o.riding_camera_angle = f;
        else if (k == "difficulty") o.difficulty = i;
        else if (k == "riding_mode") o.riding_mode = i;
        else if (k == "melee_auto_target") o.melee_auto_target = i != 0;
        else if (k.rfind("key", 0) == 0) {  // keyN=K_KEY,J_KEY,J_JOYPAD,FPS_KEY,FPS_MOUSE (hex)
            const int action = std::atoi(k.c_str() + 3);
            for (KeyBindingRow& r : rows)
                if (r.action == action) {
                    std::istringstream s(v);
                    std::string tok;
                    for (int& c : r.code)
                        if (std::getline(s, tok, ',')) c = int(std::strtol(tok.c_str(), nullptr, 16));
                }
        }
    }
}

bool save_options(const std::filesystem::path& ini, const GameOptions& o, const std::vector<KeyBindingRow>& rows) {
    std::error_code ec;
    std::filesystem::create_directories(ini.parent_path(), ec);
    std::ofstream out(ini, std::ios::trunc);
    out << "# joan-native options (GameOptions, CONFIG.DAT equivalents); keyN = DEFAULTK record N codes\n"
        << "shadow=" << o.shadow << "\nbrightness=" << o.brightness << "\ncontrast=" << o.contrast << "\ngamma=" << o.gamma
        << "\ndraw_distance=" << o.draw_distance << "\ndynamic_grass=" << o.dynamic_grass << "\ngrass_distance=" << o.grass_distance
        << "\ngrass_density=" << o.grass_density << "\nmusic_volume=" << o.music_volume << "\nsound_volume=" << o.sound_volume
        << "\nenvironment_volume=" << o.environment_volume << "\nsound_3d=" << o.sound_3d << "\ninput_method=" << o.input_method
        << "\nmouse_sensitivity=" << o.mouse_sensitivity << "\ninvert_mouse=" << o.invert_mouse
        << "\nriding_camera_angle=" << o.riding_camera_angle << "\ndifficulty=" << o.difficulty << "\nriding_mode=" << o.riding_mode
        << "\nmelee_auto_target=" << o.melee_auto_target << '\n'
        << std::hex;
    for (const KeyBindingRow& r : rows)
        out << "key" << std::dec << r.action << std::hex << '=' << r.code[0] << ',' << r.code[1] << ',' << r.code[2] << ','
            << r.code[3] << ',' << r.code[4] << '\n';
    return bool(out.flush());
}

}  // namespace joa::engine

// Save description / thumbnail / F10 / free-camera mouse look (SaveCapture), headless:
// - the real client (run(Options), NullRenderer) with an autoplay script: F7 -> SAVE NEW -> typed text with caret
//   edits (Backspace, Home, '~' refused, Left, Delete) -> Return: the .jsav META holds the typed description, a
//   512x256 24-bit <stem>.bmp sits next to it, list_saves shows it first; F10 twice -> JOAN000.png + JOAN001.png
//   in $XDG_PICTURES_DIR (never overwritten);
// - GameFlow re-scans the save folder whenever the Load/Save screen opens (files added outside the game appear);
// - save_thumbnail crops the 4:3 centre of the viewport to 320x240;
// - the free camera turns right / looks down with the mouse (invert flips pitch).
#include "check.hpp"
#include "engine/app.hpp"
#include "engine/game_camera.hpp"
#include "engine/game_input.hpp"
#include "engine/game_ui_bridge.hpp"
#include "engine/session_flow.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace joa;
using namespace joa::engine;
namespace fs = std::filesystem;

namespace {

void thumbnail_crop() {
    // 1280x720 frame: green outside the 4:3 centre (x 160..1119), red left half, blue right half of it.
    Image f{1280, 720, std::vector<std::uint8_t>(1280 * 720 * 4)};
    for (int y = 0; y < 720; ++y)
        for (int x = 0; x < 1280; ++x) {
            std::uint8_t* p = &f.rgba[(std::size_t(y) * 1280 + x) * 4];
            p[x < 160 || x >= 1120 ? 1 : x < 640 ? 0 : 2] = 255, p[3] = 255;
        }
    const Image t = save_thumbnail(f, {0, 0, 1280, 720});
    CHECK(t.width == 320 && t.height == 240);
    if (t.width != 320) return;
    int green = 0;
    for (std::size_t i = 0; i < t.rgba.size(); i += 4) green += t.rgba[i + 1] > 0;
    CHECK(green == 0);
    CHECK(t.rgba[0] == 255 && t.rgba[2] == 0);                         // top-left red
    CHECK(t.rgba[(239 * 320 + 319) * 4 + 2] == 255 && t.rgba[(239 * 320 + 319) * 4] == 0);  // bottom-right blue
    CHECK(save_thumbnail(Image{}, {0, 0, 1, 1}).width == 0);
}

void free_camera_mouse() {
    GameCamera cam;
    GameInput in;
    cam.set_free_pose({1000, 2000, 300}, 90, 0);
    const auto dir = [&] {
        const CameraPose p = cam.pose(1.0f);
        return glm::normalize(p.target - p.eye);
    };
    const glm::vec3 d0 = dir();
    const glm::vec3 right = glm::normalize(glm::cross(d0, glm::vec3(0, 1, 0)));  // engine y up
    cam.free_move(in, 0.0f, {100, 0}, GameOptions{});                          // 100 counts right = 18 degrees
    const glm::vec3 d1 = dir();
    CHECK(glm::dot(d1, right) > 0.25f && glm::dot(d1, right) < 0.35f);         // sin 18 = 0.31
    cam.free_move(in, 0.0f, {0, 100}, GameOptions{});                          // mouse down looks down
    CHECK(dir().y < -0.25f);
    GameOptions inv;
    inv.invert_mouse = true;
    cam.free_move(in, 0.0f, {0, 200}, inv);  // inverted: down looks up
    CHECK(dir().y > 0.25f);
}

}  // namespace

int main() {
    thumbnail_crop();
    free_camera_mouse();

    const char* game = std::getenv("JOA_GAME_DIR");
    if (!game || !formats::Vfs(game).exists("scenario/scenario_01.scr")) {
        std::fprintf(stderr, "SKIP: no original data (unit checks ran)\n");
        return g_failures ? 1 : 77;
    }
    const char* out = std::getenv("JOA_TEST_OUT");
    const fs::path xdg = fs::path(out ? out : ".") / "save-capture-xdg";
    fs::remove_all(xdg);
    setenv("XDG_DATA_HOME", (xdg / "data").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (xdg / "config").c_str(), 1);
    setenv("XDG_PICTURES_DIR", (xdg / "pictures").c_str(), 1);

    // In game the Save screen sits 48 px higher (lc = -48): SAVE NEW at (262..412, 664..694).
    const fs::path script = xdg / "save.txt";
    fs::create_directories(xdg);
    std::ofstream(script) << "40 tap Escape\n100 tap F7\n170 click 337 679\n180 type Before Orleanz\n185 tap Backspace\n"
                             "190 type s\n195 tap Home\n200 type X~\n205 tap Left\n210 tap Delete\n220 tap Return\n"
                             "230 tap F10\n240 tap F10\n250 quit\n";
    Options o;
    o.game_dir = game;
    o.headless = true;
    o.new_game = true;
    o.seed = 1;
    o.autoplay = script.string();
    CHECK(run(o) == 0);

    fs::path save;
    for (const auto& e : fs::directory_iterator(game::native_save_dir()))
        if (e.path().extension() == ".jsav" && e.path().stem().string().rfind("Save_", 0) == 0) save = e.path();
    CHECK(!save.empty());
    const auto meta = save.empty() ? std::nullopt : Session::read_meta(save);
    CHECK(meta && meta->description == "Before Orleans" && meta->mission == 1);
    if (meta) std::fprintf(stderr, "saved description '%s'\n", meta->description.c_str());
    fs::path bmp = save;
    bmp.replace_extension(".bmp");
    std::ifstream b(bmp, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(b)), {});
    CHECK(bytes.size() == 393270);  // as the original's Save/*.BMP
    if (bytes.size() >= 30) {
        CHECK(bytes[0] == 'B' && bytes[1] == 'M');
        auto le = [&](std::size_t off, std::size_t n) {
            std::uint32_t v = 0;
            std::memcpy(&v, bytes.data() + off, n);
            return v;
        };
        CHECK(le(18, 4) == 512 && le(22, 4) == 256);
        CHECK(le(28, 2) == 24);
    }
    const formats::Vfs vfs(game);
    const auto saves = list_saves(vfs);
    CHECK(!saves.empty() && saves[0].file == save.string() && saves[0].description == "Before Orleans");
    CHECK(fs::exists(xdg / "pictures" / "JOAN000.png") && fs::exists(xdg / "pictures" / "JOAN001.png"));

    // Folder re-scan: a save copied in from outside appears when the Load screen opens again.
    Session s(vfs);
    MenuModel menu;
    HudModel hud;
    GameFlow flow(s, menu, hud);
    flow.list_saves = [&] { return list_saves(vfs); };
    menu.screen = MenuScreen::Load;  // as the main menu's LOAD A CAMPAIGN sets it
    flow.update();
    const std::size_t n = menu.saves.size();
    CHECK(n == saves.size());
    fs::copy_file(save, save.parent_path() / "Copied.jsav");
    flow.update();
    CHECK(menu.saves.size() == n);  // still open: no re-scan every frame
    menu.screen = MenuScreen::Main, flow.update();
    menu.screen = MenuScreen::Load, flow.update();
    CHECK(menu.saves.size() == n + 1);
    flow.command({UiCommand::Type::DeleteSave, 0, (save.parent_path() / "Copied.jsav").string(), {}});
    CHECK(menu.saves.size() == n);
    return test_result();
}

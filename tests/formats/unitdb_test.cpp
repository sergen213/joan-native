// Gameplay databases and their cross references:
//   unit/unit.res, unit/recruit.res, item/item.res, item/dropitem.res, item/sellitem.res
//   (Name34 RES of "Key : value" records) and Resource/STD.SET (Name9 RES of dBase tables).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

#include "formats/res_archive.hpp"
#include "formats/world_text.hpp"
#include "formats/vfs_fs.hpp"

namespace fs = std::filesystem;
using namespace joa::formats;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

static std::string upper(std::string s) { for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }
static const std::string* get(const std::vector<KvLine>& kv, std::string_view key) {
    for (const auto& l : kv)
        if (upper(l.key) == upper(std::string(key))) return &l.value;
    return nullptr;
}

struct Db {
    std::optional<ResArchive> res;
    std::vector<std::vector<KvLine>> records;
};

static Db load(const fs::path& p, std::size_t want) {
    Db db;
    std::string err;
    db.res = ResArchive::open(p, &err);
    CHECK(db.res && db.res->entries().size() == want, "%s: %s (%zu)", p.c_str(), err.c_str(), db.res ? db.res->entries().size() : 0);
    if (!db.res) return db;
    for (const auto& e : db.res->entries()) {
        auto kv = parse_kv_text(db.res->text(e.name).value_or(""));
        CHECK(get(kv, "Code"), "%s: record without Code", e.name.c_str());
        db.records.push_back(std::move(kv));
    }
    return db;
}

static std::set<std::string> column(const DbfTable& t, const char* field) {
    std::set<std::string> out;
    int f = t.field_index(field);
    for (std::size_t r = 0; f >= 0 && r < t.record_count(); ++r) out.insert(upper(t.text(r, std::size_t(f))));
    return out;
}

int main() {
    const char* env = std::getenv("JOA_GAME_DIR");
    if (!env || !fs::exists(fs::path(env) / "unit" / "unit.res")) { std::puts("SKIP: JOA_GAME_DIR has no game data"); return 77; }
    const fs::path root = env;
    std::string err;

    // STD.SET: 140 dBase tables, all parse.
    auto set = ResArchive::open(root / "Resource" / "STD.SET", &err);
    CHECK(set && set->entries().size() == 140, "STD.SET: %s", err.c_str());
    if (!set) return 1;
    auto table = [&](const char* name) {
        const ResEntry* e = set->find(name);
        CHECK(e, "STD.SET has no %s", name);
        auto t = e ? DbfTable::parse(set->data(*e), &err) : std::nullopt;
        CHECK(t, "%s: %s", name, err.c_str());
        return t;
    };
    for (const auto& e : set->entries()) table(e.name.c_str());
    auto animate = table("ANIMATE"), upgrade = table("UPGRADE"), level = table("LEVEL"), upitem = table("UPITEM");
    auto itemclas = table("ITEMCLAS");
    if (!animate || !upgrade || !level || !upitem || !itemclas) return 1;
    for (const auto& n : column(*animate, "DBFNAME")) CHECK(set->find(n), "ANIMATE -> missing table %s", n.c_str());
    for (const auto& n : column(*upgrade, "DBFNAME")) CHECK(set->find(n), "UPGRADE -> missing table %s", n.c_str());
    CHECK(level->record_count() == 30, "LEVEL rows %zu", level->record_count());
    long prev = 0;
    for (std::size_t r = 0; r < level->record_count(); ++r) {
        long xp = std::stol(level->text(r, 1));
        CHECK(xp > prev, "LEVEL experience not increasing at %zu", r);
        prev = xp;
    }
    // Upgrade lists only name UPITEM codes.
    std::set<std::string> upcodes = column(*upitem, "CODE");
    for (const auto& n : column(*upgrade, "DBFNAME")) {
        auto t = table(n.c_str());
        if (t)
            for (const auto& c : column(*t, "CODE")) CHECK(upcodes.count(c), "%s: unknown upgrade %s", n.c_str(), c.c_str());
    }

    Db units = load(root / "unit" / "unit.res", 117);
    Db recruit = load(root / "unit" / "recruit.res", 38);
    Db items = load(root / "item" / "item.res", 250);
    Db drops = load(root / "item" / "dropitem.res", 37);
    Db sells = load(root / "item" / "sellitem.res", 47);
    if (!units.res || !items.res || !drops.res) return 1;

    std::set<std::string> anims = column(*animate, "CODE"), ups = column(*upgrade, "CODE"), classes = column(*itemclas, "CODE");
    std::set<std::string> unit_codes, item_codes, drop_codes;
    for (const auto& e : units.res->entries()) unit_codes.insert(upper(e.name));
    for (std::size_t i = 0; i < items.records.size(); ++i) {
        item_codes.insert(upper(items.res->entries()[i].name));
        if (auto c = get(items.records[i], "Code")) item_codes.insert(upper(*c));
        auto cls = get(items.records[i], "ItemClass");
        CHECK(cls && classes.count(upper(*cls)), "item %s class", items.res->entries()[i].name.c_str());
    }
    for (const auto& e : drops.res->entries()) drop_codes.insert(upper(e.name));

    for (std::size_t i = 0; i < units.records.size(); ++i) {
        const auto& kv = units.records[i];
        const std::string& name = units.res->entries()[i].name;
        CHECK(get(kv, "Code") && upper(*get(kv, "Code")) == upper(name), "unit %s code", name.c_str());
        for (const char* k : {"HP", "EP", "Strength", "Dexterity", "Defense", "Leadership", "Level", "WalkSpeed", "RunSpeed"})
            CHECK(get(kv, k), "unit %s lacks %s", name.c_str(), k);
        CHECK(get(kv, "Animation") && anims.count(upper(*get(kv, "Animation"))), "unit %s animation", name.c_str());
        CHECK(get(kv, "UpgradeList") && ups.count(upper(*get(kv, "UpgradeList"))), "unit %s upgrade list", name.c_str());
        // Original data quirk: unit ALEXANDER names drop list S2B1_ALEX, which dropitem.res
        // lacks; "NONE" means no drops.
        if (auto d = get(kv, "DropItemList"); d && upper(*d) != "NONE" && upper(*d) != "S2B1_ALEX")
            CHECK(drop_codes.count(upper(*d)), "unit %s drop list %s", name.c_str(), d->c_str());
    }
    // Drop lists and shops reference item codes; recruit lists reference unit codes.
    auto entries_ref = [&](const Db& db, const std::set<std::string>& codes, std::set<std::string> meta) {
        for (std::size_t i = 0; i < db.records.size(); ++i)
            for (const auto& l : db.records[i])
                if (!l.key.empty() && !meta.count(upper(l.key)))
                    CHECK(codes.count(upper(l.key)), "%s: unknown code %s", db.res->entries()[i].name.c_str(), l.key.c_str());
    };
    entries_ref(drops, item_codes, {"CODE"});
    entries_ref(sells, item_codes, {"CODE", "TYPE", "SELLINGRATIO"});
    entries_ref(recruit, unit_codes, {"CODE", "SELLINGRATIO", "LOCATION", "LEVEL", "POPULATION"});

    // Placed world objects: item/Object/**/*.pro are single item records in the same format.
    size_t pros = 0;
    for (const auto& e : fs::recursive_directory_iterator(root / "item" / "Object")) {
        std::string ext = e.path().extension().string();
        if (upper(ext) != ".PRO") continue;
        auto b = read_file(e.path());
        auto kv = parse_kv_text(b ? std::string_view(reinterpret_cast<const char*>(b->data()), b->size()) : "");
        auto cls = get(kv, "ItemClass");
        CHECK(get(kv, "Code") && cls && classes.count(upper(*cls)), "%s", e.path().c_str());
        ++pros;
    }
    CHECK(pros == 819, "expected 819 item/Object .pro records, got %zu", pros);

    std::printf("units %zu, items %zu, drop lists %zu, shops %zu, recruit lists %zu, STD.SET tables %zu\n",
                units.records.size(), items.records.size(), drops.records.size(), sells.records.size(), recruit.records.size(),
                set->entries().size());
    return failures ? 1 : 0;
}

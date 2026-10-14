#include "formats/scenario_def.hpp"

#include <charconv>

#include "formats/vfs_fs.hpp"
#include "formats/world_text.hpp"

namespace joa::formats {

std::optional<ScenarioDef> parse_scenario_def(std::string_view text, std::string* err) {
    auto fail = [&](std::string m) -> std::optional<ScenarioDef> {
        if (err) *err = std::move(m);
        return std::nullopt;
    };
    std::vector<KvLine> kv = parse_kv_text(text);
    ScenarioDef def;
    size_t at = 0;
    for (; at < kv.size() && !iequals(kv[at].key, "StepNo"); ++at)
        if (iequals(kv[at].key, "code")) def.code = kv[at].value;
    if (def.code.empty() || at == kv.size()) return fail("missing code or StepNo");
    int n = 0;
    const std::string& sn = kv[at++].value;
    std::from_chars(sn.data(), sn.data() + sn.size(), n);
    if (n < 1) return fail("bad StepNo");

    static constexpr std::string_view kKeys[] = {"WorldFileName",       "UnitFileName",
                                                 "ScriptFileName",      "DescriptionFileName",
                                                 "ObjectiveFileName",   "ObjectiveMapFileName"};
    for (int s = 0; s < n; ++s) {
        ScenarioStep step;
        std::string* out[] = {&step.world_file,       &step.unit_file,      &step.script_file,
                              &step.description_file, &step.objective_file, &step.objective_map_file};
        for (int k = 0; k < 6; ++k, ++at) {
            if (at >= kv.size() || !iequals(kv[at].key, kKeys[k]))
                return fail("step " + std::to_string(s + 1) + ": expected " + std::string(kKeys[k]));
            *out[k] = kv[at].value;
        }
        def.steps.push_back(std::move(step));
    }
    return def;
}

}  // namespace joa::formats

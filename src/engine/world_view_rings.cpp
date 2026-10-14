// RTS selection rings. Unit +0x325 is a second actor created with the unit's actor (model handle unit +0x217) and
// parented to the unit node (FUN_0060ec70(actor +0x8c)), so it follows the unit position and heading; its model is
// picked by the unit class Special (+0x118) from UnitArray +0x70..+0x7c = MODEL\select, select2, select3, select4
// (UnitArray init): 4 CANNON / 5 FAUCONNEAU / 6 CROSSBOW / 9 BOMBARD -> select2, 8 SIEGE_TOWER / 11 BATTERING_RAM ->
// select3, 10 TREBUCHET -> select4, others -> select. Unit 0x519250(on) shows it for selected units, never for
// HORSE (2) / ANIMAL (7) classes; Unit_process 0x52c490 hides it while dead. CONFIRMED (decomp).
#include "engine/world_view.hpp"

namespace joa::engine {

void WorldView::build_rings(std::vector<DrawItem>& draws) {
    if (!selection || !world_) return;
    for (const int id : *selection) {
        const game::Unit* u = world_->unit(id);
        const auto f = frames_.find(id);
        if (!u || u->deleted || world_->dead(*u) || u->cls < 0 || f == frames_.end()) continue;
        const int sp = world_->data().units[std::size_t(u->cls)].special;
        if (sp == 2 || sp == 7) continue;
        const char* name = sp == 4 || sp == 5 || sp == 6 || sp == 9 ? "model/select2.SMO"
                           : sp == 8 || sp == 11                    ? "model/select3.SMO"
                           : sp == 10                               ? "model/select4.SMO"
                                                                    : "model/select.SMO";
        const Model* m = assets_->model(name);
        if (!m) continue;
        for (const ModelPart& p : m->parts) draws.push_back(DrawItem{m->mesh, p.material, f->second, p.first_index, p.index_count});
    }
}

}  // namespace joa::engine

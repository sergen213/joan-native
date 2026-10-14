// Category of each of the 407 builtins (index = id - 1), from the per-id survey of the FUNC /
// DIALOG / ACTION executors in docs/reverse-engineering/SCRIPT_VM.md "Builtin table".
#include "game/script_vm.hpp"

namespace joa::game {

namespace {
using C = BuiltinCategory;
constexpr C kCategory[407] = {
    /*   1 */ C::Math, C::World, C::World, C::Objective, C::Query, C::Query, C::Query, C::Flags,
    /*   9 */ C::Query, C::Spawn, C::Spawn, C::Spawn, C::Spawn, C::Item, C::Item, C::Item,
    /*  17 */ C::Spawn, C::Query, C::Query, C::Query, C::Query, C::Query, C::Query, C::Query,
    /*  25 */ C::Query, C::World, C::World, C::World, C::World, C::World, C::World, C::World,
    /*  33 */ C::World, C::World, C::World, C::World, C::World, C::World, C::World, C::World,
    /*  41 */ C::Query, C::Spawn, C::Query, C::Query, C::Query, C::Query, C::UnitState, C::UnitState,
    /*  49 */ C::Query, C::Query, C::Query, C::Query, C::Query, C::Query, C::Query, C::Query,
    /*  57 */ C::Query, C::Query, C::Query, C::Query, C::Query, C::Query, C::Query, C::Query,
    /*  65 */ C::Query, C::Query, C::Query, C::UnitState, C::UnitState, C::UnitState, C::Query, C::Query,
    /*  73 */ C::UnitState, C::UnitState, C::Query, C::Query, C::Query, C::UnitState, C::UnitState, C::Query,
    /*  81 */ C::Query, C::UnitState, C::UnitState, C::UnitState, C::Spawn, C::Spawn, C::Spawn, C::UnitState,
    /*  89 */ C::UnitState, C::UnitState, C::Query, C::UnitState, C::UnitState, C::UnitState, C::UnitState, C::UnitState,
    /*  97 */ C::UnitState, C::Query, C::Query, C::UnitState, C::UnitState, C::Query, C::UnitState, C::Query,
    /* 105 */ C::UnitState, C::Query, C::UnitState, C::UnitState, C::Query, C::UnitState, C::UnitState, C::Query,
    /* 113 */ C::Query, C::UnitState, C::UnitState, C::Query, C::UnitState, C::Query, C::UnitState, C::Query,
    /* 121 */ C::UnitState, C::UnitState, C::UnitState, C::Query, C::Query, C::Query, C::UnitState, C::Query,
    /* 129 */ C::UnitState, C::Query, C::Query, C::Query, C::Query, C::Query, C::Query, C::UnitState,
    /* 137 */ C::Query, C::Query, C::Query, C::UnitOrder, C::UnitState, C::Query, C::Query, C::UnitState,
    /* 145 */ C::UnitState, C::Spawn, C::UnitState, C::UnitState, C::UnitOrder, C::UnitState, C::UnitState, C::UnitState,
    /* 153 */ C::UnitState, C::UnitState, C::UnitState, C::Item, C::UnitState, C::Query, C::Spawn, C::Spawn,
    /* 161 */ C::Math, C::Query, C::Query, C::Query, C::UnitState, C::UnitOrder, C::UnitOrder, C::Query,
    /* 169 */ C::Query, C::Query, C::Query, C::Query, C::Query, C::Query, C::Query, C::Query,
    /* 177 */ C::Query, C::Query, C::Item, C::Query, C::World, C::World, C::Query, C::World,
    /* 185 */ C::World, C::Query, C::Spawn, C::Spawn, C::Query, C::World, C::Query, C::Spawn,
    /* 193 */ C::World, C::World, C::World, C::Spawn, C::Item, C::Item, C::World, C::World,
    /* 201 */ C::World, C::World, C::World, C::Objective, C::World, C::Objective, C::Timer, C::Timer,
    /* 209 */ C::World, C::World, C::World, C::World, C::World, C::World, C::World, C::Flags,
    /* 217 */ C::UnitState, C::World, C::Timer, C::Timer, C::Timer, C::Timer, C::Timer, C::Timer,
    /* 225 */ C::Timer, C::Timer, C::Objective, C::Objective, C::Objective, C::Objective, C::Objective, C::Objective,
    /* 233 */ C::Objective, C::Objective, C::Objective, C::Objective, C::Objective, C::Objective, C::Objective, C::Item,
    /* 241 */ C::Objective, C::Objective, C::Item, C::Objective, C::Objective, C::Objective, C::Objective, C::Objective,
    /* 249 */ C::Objective, C::Objective, C::Objective, C::Objective, C::Objective, C::Timer, C::Timer, C::Flags,
    /* 257 */ C::Flags, C::Timer, C::Timer, C::Query, C::UnitState, C::UnitState, C::Objective, C::Objective,
    /* 265 */ C::Objective, C::Objective, C::Objective, C::Objective, C::Objective, C::Objective, C::Objective, C::Objective,
    /* 273 */ C::Objective, C::Objective, C::Objective, C::Objective, C::Flags, C::Flags, C::Flags, C::Flags,
    /* 281 */ C::Flags, C::Flags, C::Flags, C::Flags, C::Flags, C::Flags, C::Flags, C::Flags,
    /* 289 */ C::Flags, C::Flags, C::Flags, C::Flags, C::Sound, C::Camera, C::Camera, C::Camera,
    /* 297 */ C::Camera, C::Camera, C::Camera, C::Camera, C::Camera, C::Camera, C::Camera, C::Ui,
    /* 305 */ C::Ui, C::Ui, C::Ui, C::Ui, C::Ui, C::Query, C::UnitState, C::Query,
    /* 313 */ C::UnitState, C::Item, C::World, C::Dialog, C::Dialog, C::Dialog, C::Dialog, C::Dialog,
    /* 321 */ C::Dialog, C::Dialog, C::UnitOrder, C::UnitOrder, C::Query, C::UnitOrder, C::UnitOrder, C::UnitState,
    /* 329 */ C::Ui, C::Timer, C::Flags, C::World, C::Item, C::Item, C::UnitState, C::Query,
    /* 337 */ C::Query, C::Timer, C::UnitState, C::Query, C::Query, C::Query, C::Query, C::Ui,
    /* 345 */ C::Ui, C::Sound, C::UnitState, C::UnitState, C::UnitState, C::UnitState, C::UnitState, C::Query,
    /* 353 */ C::Query, C::Query, C::Query, C::Ui, C::Ui, C::UnitState, C::UnitState, C::Query,
    /* 361 */ C::Query, C::Item, C::Ui, C::Ui, C::Objective, C::Ui, C::Ui, C::Objective,
    /* 369 */ C::Ui, C::Ui, C::Item, C::Item, C::UnitState, C::Query, C::Query, C::Query,
    /* 377 */ C::Query, C::Ui, C::Ui, C::Query, C::Query, C::Ui, C::Ui, C::Flags,
    /* 385 */ C::Ui, C::Camera, C::UnitState, C::Query, C::UnitState, C::Camera, C::Flags, C::Objective,
    /* 393 */ C::Ui, C::Ui, C::Ui, C::Ui, C::World, C::Ui, C::UnitState, C::UnitState,
    /* 401 */ C::UnitState, C::Objective, C::Query, C::Ui, C::Camera, C::Objective, C::UnitState,
};
}  // namespace

BuiltinCategory builtin_category(std::uint16_t id) { return id >= 1 && id <= 407 ? kCategory[id - 1] : C::Query; }

std::string_view category_name(BuiltinCategory c) {
    static constexpr std::string_view kNames[] = {"math",  "flags",  "timer",  "query",     "unit-order", "unit-state", "spawn",
                                                  "item",  "dialog", "camera", "objective", "sound",      "ui",         "world"};
    return kNames[static_cast<int>(c)];
}

}  // namespace joa::game

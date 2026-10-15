# joa_game: platform-independent game logic (script VM, later world rules). Depends on
# joa_formats only — no SDL/GL/Vulkan. Sources and tests are globbed so contributors add files
# (use a file prefix per subsystem, e.g. script_*) without editing this file.
file(GLOB JOA_GAME_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(joa_game STATIC ${JOA_GAME_SRC})
target_include_directories(joa_game PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(joa_game PUBLIC joa_formats)
target_compile_features(joa_game PUBLIC cxx_std_20)
target_compile_options(joa_game PRIVATE -Wall -Wextra)

# Campaign-bot mission plans: autopilot_plan_<stem>.cpp defines make_plan_<stem>(); the registry (autopilot_plan.cpp)
# includes this generated list.
set(JOA_PLANS_INC "")
foreach(f ${JOA_GAME_SRC})
  get_filename_component(n "${f}" NAME_WE)
  if(n MATCHES "^autopilot_plan_(.+)$")
    string(APPEND JOA_PLANS_INC "JOA_PLAN(${CMAKE_MATCH_1})\n")
  endif()
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/autopilot_plans.inc CONTENT "${JOA_PLANS_INC}")
target_include_directories(joa_game PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# tests/game/<name>_test.cpp -> executable game_<name>_test, CTest name game.<name>.
# Tests get JOA_GAME_DIR in the environment; return 77 to SKIP when data is absent.
file(GLOB JOA_GAME_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/game/*_test.cpp)
foreach(t ${JOA_GAME_TESTS})
  get_filename_component(n "${t}" NAME_WE)
  string(REPLACE "_test" "" short "${n}")
  add_executable(game_${n} ${t})
  target_link_libraries(game_${n} PRIVATE joa_game)
  target_compile_options(game_${n} PRIVATE -Wall -Wextra)
  add_test(NAME game.${short} COMMAND game_${n})
  set_tests_properties(game.${short} PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77)
  if(short MATCHES "^campaign_full_")
    set_tests_properties(game.${short} PROPERTIES TIMEOUT 1500)  # 8 missions x 9000 ticks (x2)
  endif()
endforeach()

# game.campaign_legit: the bot's chain from a new game writes mission-start checkpoints into
# campaign_checkpoints/ here (fixture); game.campaign_legit_mission_N replays mission N from its checkpoint
# (skipped when the chain did not reach it). Iterate on one mission without the chain: ctest -R legit_mission_4 -FS campaign_checkpoints.
if(TARGET game_campaign_legit_test)
  set_tests_properties(game.campaign_legit PROPERTIES TIMEOUT 1500 FIXTURES_SETUP campaign_checkpoints)
  foreach(n RANGE 1 8)
    add_test(NAME game.campaign_legit_mission_${n} COMMAND game_campaign_legit_test ${n})
    set_tests_properties(game.campaign_legit_mission_${n} PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77
                         TIMEOUT 300 FIXTURES_REQUIRED campaign_checkpoints)
  endforeach()
  # game.campaign_S5L1_bot starts from the chain's mission-4 checkpoint: ctest -R S5L1_bot -FS campaign_checkpoints.
  set_tests_properties(game.campaign_S5L1_bot PROPERTIES FIXTURES_REQUIRED campaign_checkpoints)
endif()

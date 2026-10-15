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

# game.campaign_legit: the bot's chain from a new game through missions 1..kExpectedWins (campaign_legit_test.cpp) writes
# mission-start checkpoints into campaign_checkpoints/ here (fixture), the next mission's included (bot-S5L1.jsav: the
# later missions' segment chains start there); game.campaign_legit_mission_N (N = 1..kExpectedWins: keep the RANGE below
# in step) replays mission N from its checkpoint (skipped when the chain did not reach it). Iterate on one mission without the chain: ctest -R legit_mission_4 -FS campaign_checkpoints.
if(TARGET game_campaign_legit_test)
  set_tests_properties(game.campaign_legit PROPERTIES TIMEOUT 1500 FIXTURES_SETUP campaign_checkpoints)
  foreach(n RANGE 1 4)
    add_test(NAME game.campaign_legit_mission_${n} COMMAND game_campaign_legit_test ${n})
    set_tests_properties(game.campaign_legit_mission_${n} PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77
                         TIMEOUT 600 FIXTURES_REQUIRED campaign_checkpoints)  # mission 4: up to 9 lost attempts
  endforeach()
  # game.campaign_S5L1_bot starts from the chain's mission-4 checkpoint: ctest -R S5L1_bot -FS campaign_checkpoints.
  set_tests_properties(game.campaign_S5L1_bot PROPERTIES FIXTURES_REQUIRED campaign_checkpoints)
  # game.campaign_S6L1_bot starts on the road north: segment tests play mission 8 from the chain's mission-5 checkpoint
  # (missions 5-7 forced) and save campaign_checkpoints/bot-S6L1-<segment>.jsav for the next (campaign_S6L1_bot_test.cpp):
  # auxerre (Auxerre captured), hill (Troyes' wall down), troyes (Troyes captured), road (reputation 77). Iterate:
  # ctest -R S6L1 -FS 'campaign_.*'.
  set(s6fix campaign_checkpoints)
  foreach(s auxerre hill troyes road)
    add_test(NAME game.campaign_S6L1_${s} COMMAND game_campaign_S6L1_bot_test --${s} campaign_checkpoints/bot-S6L1-${s}.jsav)
    set_tests_properties(game.campaign_S6L1_${s} PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77
                         TIMEOUT 600 FIXTURES_REQUIRED ${s6fix} FIXTURES_SETUP campaign_S6L1_${s})
    set(s6fix campaign_S6L1_${s})
  endforeach()
  set_tests_properties(game.campaign_S6L1_bot PROPERTIES TIMEOUT 600 FIXTURES_REQUIRED ${s6fix})
  # game.campaign_S7L1_bot starts at La Charité's gates: game.campaign_S7L1_charite plays mission 7 there from the chain's
  # mission-5 checkpoint and saves campaign_checkpoints/bot-S7L1-charite.jsav. Iterate: ctest -R S7L1_bot -FS 'campaign_.*'.
  add_test(NAME game.campaign_S7L1_charite COMMAND game_campaign_S7L1_bot_test --charite campaign_checkpoints/bot-S7L1-charite.jsav)
  set_tests_properties(game.campaign_S7L1_charite PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77
                       TIMEOUT 600 FIXTURES_REQUIRED campaign_checkpoints FIXTURES_SETUP campaign_S7L1_charite)
  set_tests_properties(game.campaign_S7L1_bot PROPERTIES TIMEOUT 600 FIXTURES_REQUIRED campaign_S7L1_charite)
endif()

# game.campaign_S9L1_bot: the bot from the chain's mission-5 start (campaign_checkpoints/bot-S5L1.jsav) through the S5L1
# win into the S9L1 win, cut into fixture segments (each saves campaign_checkpoints/bot-S9L1-seg<k>.jsav for the next; the
# last of them bot-S9L1-last.jsav, which the test proper plays on to the win). Iterate: ctest -R S9L1_bot -FS 'campaign_.*'.
if(TARGET game_campaign_S9L1_bot_test AND TARGET game_campaign_legit_test)
  set(s9in campaign_checkpoints/bot-S5L1.jsav)
  set(s9fix campaign_checkpoints)
  set(s9difficulty 0)  # Easy (the bot also won S9L1 on Normal, not every run: SAVES_AND_CAMPAIGN.md §8.1)
  set(s9n 16)  # segments before the test proper (Easy wins in seg12-14, 60000 kSegmentTicks each; a won chain's later segments pass through)
  foreach(k RANGE 1 ${s9n})
    set(s9out campaign_checkpoints/bot-S9L1-seg${k}.jsav)
    if(k EQUAL s9n)
      set(s9out campaign_checkpoints/bot-S9L1-last.jsav)
    endif()
    add_test(NAME game.campaign_S9L1_seg${k} COMMAND game_campaign_S9L1_bot_test --segment ${s9in} ${s9out} ${s9difficulty})
    set(s9difficulty "")
    set_tests_properties(game.campaign_S9L1_seg${k} PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77
                         TIMEOUT 600 FIXTURES_REQUIRED ${s9fix} FIXTURES_SETUP campaign_S9L1_seg${k})
    set(s9in ${s9out})
    set(s9fix campaign_S9L1_seg${k})
  endforeach()
  set_tests_properties(game.campaign_S9L1_bot PROPERTIES TIMEOUT 600 FIXTURES_REQUIRED ${s9fix})
endif()

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

# The legitimate campaign chain (no forced wins), one CTest fixture chain (SAVES_AND_CAMPAIGN.md §8.1): missions 1-4 on
# Normal, Easy from mission 5; every step writes campaign_checkpoints/*.jsav here for the next. Whole chain:
# ctest -R 'campaign_(legit|S[5-9]L1_)'; one step on the checkpoints already there: ctest -R <test> -FS 'campaign_.*'.
# game.campaign_legit: missions 1..kExpectedWins from a new game (campaign_legit_test.cpp), mission-start checkpoints, the
# next mission's included (bot-S5L1.jsav); game.campaign_legit_mission_N (N = 1..kExpectedWins: keep the RANGE below in
# step) replays mission N from its checkpoint (skipped when the chain did not reach it). Iterate on one mission without
# the chain: ctest -R legit_mission_4 -FS campaign_checkpoints.
if(TARGET game_campaign_legit_test)
  set_tests_properties(game.campaign_legit PROPERTIES TIMEOUT 1500 FIXTURES_SETUP campaign_checkpoints)
  foreach(n RANGE 1 4)
    add_test(NAME game.campaign_legit_mission_${n} COMMAND game_campaign_legit_test ${n})
    set_tests_properties(game.campaign_legit_mission_${n} PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77
                         TIMEOUT 600 FIXTURES_REQUIRED campaign_checkpoints)  # mission 4: up to 9 lost attempts
  endforeach()

  # Missions 5-6 (S5L1, S9L1): fixture segments from bot-S5L1.jsav (each saves campaign_checkpoints/bot-S9L1-seg<k>.jsav,
  # the last bot-S9L1-last.jsav), a win CONTINUEs and ends its segment; game.campaign_S9L1_bot plays on to the S9L1 win
  # and saves the S7L1 start bot-S7L1.jsav. seg1 switches the chain to Easy, as a player does in OPTIONS > GAME (the
  # original's difficulty is a config byte, not saved; the bot has not won S9L1 on Normal: SAVES_AND_CAMPAIGN.md §8.1);
  # the profile carries Easy on into missions 7 and 8.
  set(s9in campaign_checkpoints/bot-S5L1.jsav)
  set(s9fix campaign_checkpoints)
  set(s9difficulty 0)
  set(s9n 16)  # segments before the test proper (60000 kSegmentTicks each; a won chain's later segments pass through)
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
  set_tests_properties(game.campaign_S9L1_bot PROPERTIES TIMEOUT 600 FIXTURES_REQUIRED ${s9fix} FIXTURES_SETUP campaign_S9L1)

  # Mission 7 (S7L1): game.campaign_S7L1_charite plays bot-S7L1.jsav to La Charité's gates (bot-S7L1-charite.jsav);
  # game.campaign_S7L1_bot plays on to the win and saves the S6L1 start bot-S6L1.jsav.
  add_test(NAME game.campaign_S7L1_charite COMMAND game_campaign_S7L1_bot_test --charite campaign_checkpoints/bot-S7L1-charite.jsav)
  set_tests_properties(game.campaign_S7L1_charite PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77
                       TIMEOUT 600 FIXTURES_REQUIRED campaign_S9L1 FIXTURES_SETUP campaign_S7L1_charite)
  set_tests_properties(game.campaign_S7L1_bot PROPERTIES TIMEOUT 600 FIXTURES_REQUIRED campaign_S7L1_charite FIXTURES_SETUP campaign_S7L1)

  # Mission 8 (S6L1): segments from bot-S6L1.jsav, each saving campaign_checkpoints/bot-S6L1-<segment>.jsav for the next
  # (campaign_S6L1_bot_test.cpp): auxerre (Auxerre captured), hill (Troyes' wall down), troyes (Troyes captured), road
  # (reputation 77); game.campaign_S6L1_bot plays on to the win and the Ending event.
  set(s6fix campaign_S7L1)
  foreach(s auxerre hill troyes road)
    add_test(NAME game.campaign_S6L1_${s} COMMAND game_campaign_S6L1_bot_test --${s} campaign_checkpoints/bot-S6L1-${s}.jsav)
    set_tests_properties(game.campaign_S6L1_${s} PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77
                         TIMEOUT 600 FIXTURES_REQUIRED ${s6fix} FIXTURES_SETUP campaign_S6L1_${s})
    set(s6fix campaign_S6L1_${s})
  endforeach()
  set_tests_properties(game.campaign_S6L1_bot PROPERTIES TIMEOUT 600 FIXTURES_REQUIRED ${s6fix})
endif()

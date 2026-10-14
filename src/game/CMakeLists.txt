# joa_game: platform-independent game logic (script VM, later world rules). Depends on
# joa_formats only — no SDL/GL/Vulkan. Sources and tests are globbed so contributors add files
# (use a file prefix per subsystem, e.g. script_*) without editing this file.
file(GLOB JOA_GAME_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(joa_game STATIC ${JOA_GAME_SRC})
target_include_directories(joa_game PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(joa_game PUBLIC joa_formats)
target_compile_features(joa_game PUBLIC cxx_std_20)
target_compile_options(joa_game PRIVATE -Wall -Wextra)

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
  if(short MATCHES "^campaign_full_|^campaign_legit")
    set_tests_properties(game.${short} PROPERTIES TIMEOUT 1500)  # 8 missions x 9000 ticks (x2); legit chain x2
  endif()
endforeach()

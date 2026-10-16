# joa-launcher: Qt6 Widgets front end (import/verify/repair, settings.ini, play, updates). Engine-independent.
if(NOT JOA_BUILD_LAUNCHER)
  return()
endif()
if(NOT JOA_BUILD_UPDATER)
  message(FATAL_ERROR "JOA_BUILD_LAUNCHER needs JOA_BUILD_UPDATER (joa_update: AppImage hand-off to installed updates)")
endif()
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Concurrent)
add_executable(joa-launcher main.cpp)
target_link_libraries(joa-launcher PRIVATE joa_install joa_update Qt6::Widgets Qt6::Concurrent)
target_compile_options(joa-launcher PRIVATE -Wall -Wextra)
install(TARGETS joa-launcher RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  # Offscreen render of the window; private XDG dirs so the user's settings are never touched.
  set(_xdg ${CMAKE_CURRENT_BINARY_DIR}/test-xdg)
  add_test(NAME launcher.screenshot COMMAND joa-launcher --screenshot ${CMAKE_CURRENT_BINARY_DIR}/launcher.png)
  set_tests_properties(launcher.screenshot PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=${_xdg}/data;XDG_CONFIG_HOME=${_xdg}/config;XDG_STATE_HOME=${_xdg}/state")
endif()

# Release pipeline checks: tests/release/release_checks.py (docs/RELEASE_PROCESS.md "Tests").
# release.selftest always runs. The others build/inspect complete release artifacts (minutes): they
# run when JOA_FFMPEG_PREFIX names the LGPL FFmpeg prefix (packaging/ffmpeg/build_ffmpeg.py) and
# exit 77 (skipped) otherwise. `ctest -L release`. release.dist runs first (DEPENDS orders only).
# The release scripts need the `cryptography` module: use the PATH `python3` (distribution interpreter);
# FindPython3 prefers newer-versioned user installs that may lack it.
find_program(JOA_PYTHON3 python3 REQUIRED)
set(JOA_FFMPEG_PREFIX "" CACHE PATH "LGPL FFmpeg prefix for the release.* checks (empty = skip them)")
set(_checks "${CMAKE_SOURCE_DIR}/tests/release/release_checks.py")
set(_out "${CMAKE_CURRENT_BINARY_DIR}/out")

add_test(NAME release.selftest COMMAND "${JOA_PYTHON3}" "${_checks}" selftest)
set_tests_properties(release.selftest PROPERTIES LABELS release)

macro(joa_release_check name)
  add_test(NAME release.${name} COMMAND "${JOA_PYTHON3}" "${_checks}" ${ARGN} --out "${_out}"
    --ffmpeg-prefix "${JOA_FFMPEG_PREFIX}")
  set_tests_properties(release.${name} PROPERTIES LABELS release SKIP_RETURN_CODE 77 TIMEOUT 7200 RUN_SERIAL TRUE)
  if(NOT "${name}" STREQUAL "dist")
    set_tests_properties(release.${name} PROPERTIES DEPENDS release.dist)
  endif()
endmacro()
joa_release_check(dist dist)
joa_release_check(reproducible reproducible)
joa_release_check(appimage appimage)
joa_release_check(license_audit license-audit --game-dir "${JOA_GAME_DIR}")
joa_release_check(install_flow install-flow --game-dir "${JOA_GAME_DIR}")
if(TARGET joa-updater-testhttp)
  joa_release_check(product_update product-update --testhttp "$<TARGET_FILE:joa-updater-testhttp>" --repo "${JOA_UPDATE_REPO}")
endif()

# joa_formats: pure C++20 parsers for original data. No graphics/platform deps.
# Several contributors add files here; sources and tests are globbed so nobody edits this file.
file(GLOB JOA_FORMATS_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(joa_formats STATIC ${JOA_FORMATS_SRC})
target_include_directories(joa_formats PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(joa_formats PRIVATE -Wall -Wextra)

# tests/formats/<name>_test.cpp -> executable <name>_test, CTest name formats.<name>.
# Tests get JOA_GAME_DIR in the environment; return 77 to SKIP when data is absent.
file(GLOB JOA_FORMAT_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/formats/*_test.cpp)
foreach(t ${JOA_FORMAT_TESTS})
  get_filename_component(n "${t}" NAME_WE)
  add_executable(${n} ${t})
  target_link_libraries(${n} PRIVATE joa_formats)
  string(REPLACE "_test" "" short "${n}")
  add_test(NAME formats.${short} COMMAND ${n})
  set_tests_properties(formats.${short} PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77)
endforeach()

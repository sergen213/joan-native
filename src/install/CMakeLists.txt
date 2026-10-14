# joa_install: original-game import/verify/repair + shared settings.ini schema. No Qt.
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(nlohmann_json 3 REQUIRED)

set(JOA_KNOWN_VERSIONS ${CMAKE_CURRENT_SOURCE_DIR}/known_versions.json)
add_library(joa_install STATIC install.cpp settings.cpp)
target_include_directories(joa_install PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(joa_install PRIVATE "JOA_KNOWN_VERSIONS_PATH=\"${JOA_KNOWN_VERSIONS}\"")
set_source_files_properties(install.cpp PROPERTIES OBJECT_DEPENDS ${JOA_KNOWN_VERSIONS})
target_link_libraries(joa_install PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json)
target_compile_options(joa_install PRIVATE -Wall -Wextra)

add_executable(joa-install main.cpp)
target_link_libraries(joa-install PRIVATE joa_install nlohmann_json::nlohmann_json)
target_compile_options(joa-install PRIVATE -Wall -Wextra)
install(TARGETS joa-install RUNTIME DESTINATION bin)

# tests/install/<name>_test.cpp -> CTest install.<name>; exit 77 = skip (no game data).
if(BUILD_TESTING)
  file(GLOB JOA_INSTALL_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/install/*_test.cpp)
  foreach(t ${JOA_INSTALL_TESTS})
    get_filename_component(n "${t}" NAME_WE)
    add_executable(install_${n} ${t})
    target_link_libraries(install_${n} PRIVATE joa_install)
    string(REPLACE "_test" "" short "${n}")
    add_test(NAME install.${short} COMMAND install_${n})
    set_tests_properties(install.${short} PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR}" SKIP_RETURN_CODE 77)
  endforeach()
endif()

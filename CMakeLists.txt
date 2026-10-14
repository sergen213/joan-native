cmake_minimum_required(VERSION 3.24)
project(joan_native VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(JOA_BUILD_ENGINE   "Build native game client (SDL3/OpenGL/Vulkan)" ON)
option(JOA_BUILD_LAUNCHER "Build Qt6 launcher" ON)
option(JOA_BUILD_UPDATER  "Build GitHub Releases updater" ON)
option(JOA_SANITIZE       "Build with ASan+UBSan" OFF)
# Original game data directory used by asset-backed tests (never written to).
set(JOA_GAME_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "Original Joan of Arc installation used by tests")

if(JOA_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
# Release builds (packaging/release/build_release.py): no build/source paths in outputs, so two
# builds in different directories are byte-identical (with SOURCE_DATE_EPOCH set by the caller).
option(JOA_REPRODUCIBLE "Map source/build paths out of compiled outputs" OFF)
if(JOA_REPRODUCIBLE)
  add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/joan-native" "-ffile-prefix-map=${CMAKE_BINARY_DIR}=/joan-native/build")
  add_link_options(-Wl,--build-id=sha1)
endif()

include(CTest)
enable_testing()

# Each component owns its own CMakeLists.txt; added only when present.
foreach(dir src/formats src/game src/engine src/install launcher updater)
  if(EXISTS "${CMAKE_SOURCE_DIR}/${dir}/CMakeLists.txt")
    add_subdirectory(${dir})
  endif()
endforeach()
file(GLOB _tool_dirs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/*/CMakeLists.txt")
foreach(f ${_tool_dirs})
  get_filename_component(d "${f}" DIRECTORY)
  add_subdirectory("${d}")
endforeach()

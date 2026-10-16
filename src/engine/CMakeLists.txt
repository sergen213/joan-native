# joa_engine: native client core (platform, input, simulation, camera, render abstraction, GL + Vulkan backends).
# joan-native: the game executable.
find_package(SDL3 REQUIRED CONFIG)
find_package(glm REQUIRED CONFIG)
find_package(ZLIB REQUIRED)
find_package(Vulkan REQUIRED)  # headers only; the loader is reached through SDL at runtime
# WMA music / WMV movies. Release packages must link an LGPL-only build (packaging/ffmpeg/, docs/AUDIO_AND_VIDEO.md).
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
find_program(JOA_GLSLC glslc REQUIRED)
option(JOA_TRACY "Enable the Tracy profiler client (downloads Tracy via FetchContent)" OFF)

# Embed shaders/gl/<name>.glsl and shaders/common/<name>.glsl (remaster passes, shared with Vulkan) as
# joa::engine::gl_shaders::<name>; reconfigures when a shader changes.
cmake_path(ABSOLUTE_PATH CMAKE_CURRENT_SOURCE_DIR OUTPUT_VARIABLE JOA_ROOT)
cmake_path(GET JOA_ROOT PARENT_PATH JOA_ROOT)
cmake_path(GET JOA_ROOT PARENT_PATH JOA_ROOT)
file(GLOB JOA_GL_SHADERS CONFIGURE_DEPENDS "${JOA_ROOT}/shaders/gl/*.glsl" "${JOA_ROOT}/shaders/common/*.glsl")
set(JOA_GL_SHADER_DEFS "")
foreach(s ${JOA_GL_SHADERS})
  file(READ ${s} body)
  get_filename_component(n ${s} NAME_WE)
  string(APPEND JOA_GL_SHADER_DEFS "const char* const ${n} = R\"GLSL(${body})GLSL\";\n")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${JOA_GL_SHADERS})
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gl_shaders.cpp
  CONTENT "#include \"engine/gl_shaders.hpp\"\nnamespace joa::engine::gl_shaders {\n@JOA_GL_SHADER_DEFS@}\n" @ONLY)

# shaders/vk/*.{vert,frag,comp} -> SPIR-V as C initialiser lists (glslc -mfmt=num), #included by renderer_vk.cpp.
file(GLOB JOA_VK_SHADERS CONFIGURE_DEPENDS "${JOA_ROOT}/shaders/vk/*.vert" "${JOA_ROOT}/shaders/vk/*.frag" "${JOA_ROOT}/shaders/vk/*.comp")
set(JOA_VK_SPIRV "")
foreach(s ${JOA_VK_SHADERS})
  get_filename_component(n ${s} NAME)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/vk_shaders/${n}.inc")
  add_custom_command(OUTPUT "${out}"
    COMMAND ${JOA_GLSLC} --target-env=vulkan1.3 -O -mfmt=num -MD -MF "${out}.d" "${s}" -o "${out}"
    DEPENDS "${s}" DEPFILE "${out}.d" VERBATIM)
  list(APPEND JOA_VK_SPIRV "${out}")
endforeach()

file(GLOB JOA_ENGINE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(REMOVE_ITEM JOA_ENGINE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
add_library(joa_engine STATIC ${JOA_ENGINE_SRC} ${CMAKE_CURRENT_BINARY_DIR}/gl_shaders.cpp ${JOA_VK_SPIRV})
target_include_directories(joa_engine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vk_shaders")
target_include_directories(joa_engine PUBLIC "${JOA_ROOT}/src")
target_link_libraries(joa_engine PUBLIC joa_formats joa_game SDL3::SDL3 glm::glm ZLIB::ZLIB Vulkan::Headers PkgConfig::FFMPEG)
target_compile_options(joa_engine PRIVATE -Wall -Wextra)

if(JOA_TRACY)
  include(FetchContent)
  FetchContent_Declare(tracy GIT_REPOSITORY https://github.com/wolfpld/tracy.git GIT_TAG v0.11.1 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(tracy)
  target_link_libraries(joa_engine PUBLIC Tracy::TracyClient)
  target_compile_definitions(joa_engine PUBLIC JOA_TRACY)
endif()

add_executable(joan-native main.cpp)
target_link_libraries(joan-native PRIVATE joa_engine)
target_compile_definitions(joan-native PRIVATE JOA_VERSION="${JOA_VERSION_STRING}")
install(TARGETS joan-native RUNTIME DESTINATION bin)

# tests/engine/<name>_test.cpp -> CTest engine.<name>; exit 77 = skipped.
file(GLOB JOA_ENGINE_TESTS CONFIGURE_DEPENDS "${JOA_ROOT}/tests/engine/*_test.cpp")
find_program(JOA_XVFB_RUN xvfb-run)
# Vulkan tests: SDL offscreen driver (surfaceless rendering), user implicit layers off, validation on.
# The Khronos layer is not always installed system-wide; point JOA_VK_LAYER_PATH at an extracted copy.
set(_vvl "${JOA_ROOT}/work/vvl/usr/share/vulkan/explicit_layer.d")
if(NOT EXISTS "${_vvl}/VkLayer_khronos_validation.json")
  set(_vvl "")
endif()
set(JOA_VK_LAYER_PATH "${_vvl}" CACHE PATH "Directory holding VkLayer_khronos_validation.json for vk_ tests (empty = system layers)")
find_file(JOA_VK_VALIDATION_JSON VkLayer_khronos_validation.json
          PATHS ${JOA_VK_LAYER_PATH} /usr/share/vulkan/explicit_layer.d /usr/local/share/vulkan/explicit_layer.d NO_DEFAULT_PATH)
set(JOA_VK_TEST_ENV "SDL_VIDEO_DRIVER=offscreen;VK_LOADER_LAYERS_DISABLE=~implicit~;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
if(JOA_VK_LAYER_PATH)
  # Extracted layer packages keep the library in usr/lib next to share/vulkan/explicit_layer.d; the JSON names it
  # without a path, so the loader needs that directory on the library search path.
  cmake_path(SET _vvl_lib NORMALIZE "${JOA_VK_LAYER_PATH}/../../../lib")
  list(APPEND JOA_VK_TEST_ENV "VK_LAYER_PATH=${JOA_VK_LAYER_PATH}" "LD_LIBRARY_PATH=${_vvl_lib}")
endif()
if(JOA_VK_VALIDATION_JSON)
  list(APPEND JOA_VK_TEST_ENV "JOA_REQUIRE_VALIDATION=1")  # layer present: tests fail if it does not load
endif()
foreach(t ${JOA_ENGINE_TESTS})
  get_filename_component(n "${t}" NAME_WE)
  string(REPLACE "_test" "" short "${n}")
  add_executable(engine_${n} ${t})
  target_link_libraries(engine_${n} PRIVATE joa_engine)
  if(short MATCHES "^gl_")
    # GPU tests never touch the user's desktop: private Xvfb server (llvmpipe), else SDL's offscreen/EGL driver.
    if(JOA_XVFB_RUN)
      add_test(NAME engine.${short} COMMAND ${JOA_XVFB_RUN} -a -s "-screen 0 1280x720x24" $<TARGET_FILE:engine_${n}>)
      set(env "SDL_VIDEO_DRIVER=x11")
    else()
      add_test(NAME engine.${short} COMMAND engine_${n})
      set(env "SDL_VIDEO_DRIVER=offscreen")
    endif()
    set_tests_properties(engine.${short} PROPERTIES
                         ENVIRONMENT "${env};JOA_GAME_DIR=${JOA_GAME_DIR};JOA_TEST_OUT=${CMAKE_CURRENT_BINARY_DIR};XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}"
                         SKIP_RETURN_CODE 77 TIMEOUT 120)
  elseif(short MATCHES "^vk_")
    add_test(NAME engine.${short} COMMAND engine_${n})
    set_tests_properties(engine.${short} PROPERTIES
                         ENVIRONMENT "${JOA_VK_TEST_ENV};JOA_GAME_DIR=${JOA_GAME_DIR};JOA_TEST_OUT=${CMAKE_CURRENT_BINARY_DIR};XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}"
                         SKIP_RETURN_CODE 77 TIMEOUT 120)
  else()
    add_test(NAME engine.${short} COMMAND engine_${n})
    set_tests_properties(engine.${short} PROPERTIES ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR};JOA_TEST_OUT=${CMAKE_CURRENT_BINARY_DIR}" SKIP_RETURN_CODE 77)
  endif()
endforeach()

# Ray tracing smoke test renders S1L1 eight times (docs/RAYTRACING.md).
if(TEST engine.vk_rt_smoke)
  set_tests_properties(engine.vk_rt_smoke PROPERTIES TIMEOUT 600)
endif()

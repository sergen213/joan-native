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
option(JOA_DEV_OVERLAY "Dear ImGui developer overlay (third_party/imgui, Ctrl+F12 in game)" OFF)

# Embed shaders/gl/<name>.glsl and shaders/common/<name>.glsl (remaster passes, shared with Vulkan) as
# joa::engine::gl_shaders::<name>; reconfigures when a shader changes.
cmake_path(ABSOLUTE_PATH CMAKE_CURRENT_SOURCE_DIR OUTPUT_VARIABLE JOA_ROOT)
cmake_path(GET JOA_ROOT PARENT_PATH JOA_ROOT)
cmake_path(GET JOA_ROOT PARENT_PATH JOA_ROOT)
file(GLOB JOA_GL_SHADERS CONFIGURE_DEPENDS "${JOA_ROOT}/shaders/gl/*.glsl" "${JOA_ROOT}/shaders/common/*.glsl")
list(APPEND JOA_GL_SHADERS "${JOA_ROOT}/third_party/smaa/SMAA.hlsl")  # gl_shaders::SMAA (MIT, PROVENANCE.txt)
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

# AMD FidelityFX SDK v1.1.4 (third_party/fidelityfx-sdk/README.md: provenance, local patches): FSR 3.1.4 upscaler, frame
# interpolation and optical flow for the Vulkan renderer (fsr_vk.cpp). Only the permutations joa_ffx_clamp_capabilities
# lets the SDK request are compiled (FP32, wave32, render-size unjittered motion vectors, standard depth);
# tools/embed_fsr_shaders.py turns them into the SDK's blob accessor. Build/link recipe from ETI Yami's CMakeLists.txt.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(JOA_FFX_DIR "${JOA_ROOT}/third_party/fidelityfx-sdk")
set(JOA_FFX_GPU "${JOA_FFX_DIR}/include/FidelityFX/gpu")
file(GLOB_RECURSE JOA_FFX_GPU_HEADERS CONFIGURE_DEPENDS "${JOA_FFX_GPU}/*.h")
set(JOA_FFX_SPIRV "")
# glslangValidator (as ETI Yami): glslc -Os strips the resource names the SDK binds its descriptors by
# (ffxFsr3UpscalerContextCreate then fails with FFX_ERROR_INVALID_ARGUMENT).
find_program(JOA_GLSLANG glslangValidator REQUIRED)
# src/backends/vk/shaders/<effect>/<stem>.glsl -> ffx/<stem><suffix>.spv with the extra defines.
function(joa_ffx_shader effect stem suffix)
  set(source "${JOA_FFX_DIR}/src/backends/vk/shaders/${effect}/${stem}.glsl")
  set(spirv "${CMAKE_CURRENT_BINARY_DIR}/ffx/${stem}${suffix}.spv")
  add_custom_command(OUTPUT "${spirv}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/ffx"
    COMMAND ${JOA_GLSLANG} -V --target-env vulkan1.2 -S comp -Os -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_HALF=0 ${ARGN}
            "-I${JOA_FFX_GPU}" "-I${JOA_FFX_GPU}/${effect}" -o "${spirv}" "${source}"
    DEPENDS "${source}" ${JOA_FFX_GPU_HEADERS} VERBATIM)
  set(JOA_FFX_SPIRV ${JOA_FFX_SPIRV} "${spirv}" PARENT_SCOPE)
endfunction()
# Base defines from the SDK's CMakeCompile{FSR3Upscaler,Frameinterpolation,Opticalflow}Shaders.txt.
set(JOA_FSR3_DEFINES
  -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
  -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
  -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0
  -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1 -DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0
  -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0)
foreach(pass IN ITEMS prepare_inputs luma_pyramid shading_change_pyramid shading_change prepare_reactivity luma_instability accumulate
        rcas debug_view autogen_reactive)
  foreach(sharpen 0 1)
    foreach(hdr 0 1)
      joa_ffx_shader(fsr3upscaler ffx_fsr3upscaler_${pass}_pass .h${hdr}s${sharpen} ${JOA_FSR3_DEFINES}
        -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=${hdr} -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=${sharpen})
    endforeach()
  endforeach()
endforeach()
foreach(pass IN ITEMS reconstruct_and_dilate_pass setup_pass reconstruct_previous_depth_pass game_motion_vector_field_pass
        optical_flow_vector_field_pass disocclusion_mask_pass pass compute_inpainting_pyramid_pass inpainting_pass
        compute_game_vector_field_inpainting_pyramid_pass debug_view_pass)
  joa_ffx_shader(frameinterpolation ffx_frameinterpolation_${pass} ""
    -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1
    -DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0 -DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0)
endforeach()
foreach(pass IN ITEMS prepare_luma_pass compute_luminance_pyramid_pass generate_scd_histogram_pass compute_scd_divergence_pass
        compute_optical_flow_advanced_pass_v5 filter_optical_flow_pass_v5 scale_optical_flow_advanced_pass_v5)
  joa_ffx_shader(opticalflow ffx_opticalflow_${pass} "" -DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0)
endforeach()
set(JOA_FFX_BLOBS "${CMAKE_CURRENT_BINARY_DIR}/ffx/ffx_shader_blobs.cpp")
add_custom_command(OUTPUT "${JOA_FFX_BLOBS}"
  COMMAND "${Python3_EXECUTABLE}" "${JOA_ROOT}/tools/embed_fsr_shaders.py" --output "${JOA_FFX_BLOBS}" ${JOA_FFX_SPIRV}
  DEPENDS ${JOA_FFX_SPIRV} "${JOA_ROOT}/tools/embed_fsr_shaders.py" VERBATIM)
set(JOA_FFX_UPSTREAM
  "${JOA_FFX_DIR}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp" "${JOA_FFX_DIR}/src/components/frameinterpolation/ffx_frameinterpolation.cpp"
  "${JOA_FFX_DIR}/src/components/opticalflow/ffx_opticalflow.cpp" "${JOA_FFX_DIR}/src/backends/vk/ffx_vk.cpp"
  "${JOA_FFX_DIR}/src/shared/ffx_assert.cpp" "${JOA_FFX_DIR}/src/shared/ffx_message.cpp"
  "${JOA_FFX_DIR}/src/shared/ffx_breadcrumbs_list.cpp" "${JOA_FFX_DIR}/src/shared/ffx_object_management.cpp")
add_library(joa_fsr3 STATIC ${JOA_FFX_UPSTREAM} "${JOA_FFX_DIR}/joa/joa_ffx_vulkan.cpp" "${JOA_FFX_BLOBS}")
set_target_properties(joa_fsr3 PROPERTIES CXX_STANDARD 17)
target_compile_definitions(joa_fsr3 PRIVATE VK_NO_PROTOTYPES)  # Vulkan resolved at runtime (joa_ffx_load_vulkan)
# joa/ first: it shadows FidelityFX/host/ffx_types.h to size the opaque contexts for 4-byte wchar_t.
# src/components: fsr_vk.cpp reaches the upscaler's private context for its shared resources (as IKV/Kabus 22 do).
target_include_directories(joa_fsr3 SYSTEM PUBLIC "${JOA_FFX_DIR}/joa" "${JOA_FFX_DIR}/include" "${JOA_FFX_DIR}/src/components")
target_include_directories(joa_fsr3 PRIVATE "${JOA_FFX_DIR}/src/shared" "${JOA_FFX_DIR}/src/backends/shared")
target_link_libraries(joa_fsr3 PUBLIC Vulkan::Headers)
target_compile_options(joa_fsr3 PRIVATE "-include${JOA_FFX_DIR}/joa/ffx_compat.h")
set_source_files_properties(${JOA_FFX_UPSTREAM} PROPERTIES COMPILE_OPTIONS -w)

file(GLOB JOA_ENGINE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(REMOVE_ITEM JOA_ENGINE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
if(NOT JOA_DEV_OVERLAY)
  list(REMOVE_ITEM JOA_ENGINE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/dev_overlay.cpp)
endif()
add_library(joa_engine STATIC ${JOA_ENGINE_SRC} ${CMAKE_CURRENT_BINARY_DIR}/gl_shaders.cpp ${JOA_VK_SPIRV})
target_include_directories(joa_engine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vk_shaders" "${JOA_ROOT}/third_party")  # renderdoc/renderdoc_app.h
target_include_directories(joa_engine PUBLIC "${JOA_ROOT}/src")
target_link_libraries(joa_engine PUBLIC joa_formats joa_game SDL3::SDL3 glm::glm ZLIB::ZLIB Vulkan::Headers PkgConfig::FFMPEG ${CMAKE_DL_LIBS})
target_link_libraries(joa_engine PRIVATE joa_fsr3)  # fsr_vk.cpp only
target_compile_options(joa_engine PRIVATE -Wall -Wextra)

if(JOA_DEV_OVERLAY)
  file(GLOB JOA_IMGUI_SRC "${JOA_ROOT}/third_party/imgui/*.cpp")
  add_library(joa_imgui STATIC ${JOA_IMGUI_SRC})
  target_include_directories(joa_imgui PUBLIC "${JOA_ROOT}/third_party/imgui")
  target_link_libraries(joa_engine PRIVATE joa_imgui)
  target_compile_definitions(joa_engine PUBLIC JOA_DEV_OVERLAY)
endif()

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
if(NOT JOA_DEV_OVERLAY)
  list(FILTER JOA_ENGINE_TESTS EXCLUDE REGEX "_dev_overlay_test\\.cpp$")
endif()
option(JOA_TEST_XVFB "GPU tests off the desktop: GL under a private Xvfb (llvmpipe), Vulkan offscreen; OFF = visible windows on the real display/GPU" ON)
if(JOA_TEST_XVFB)
  find_program(JOA_XVFB_RUN xvfb-run)
  set(_vk_driver "SDL_VIDEO_DRIVER=offscreen")
else()
  unset(JOA_XVFB_RUN CACHE)
  set(_vk_driver "JOA_SHOW_WINDOWS=1")
endif()
# Vulkan tests: SDL offscreen driver (surfaceless rendering), user implicit layers off, validation on.
# The Khronos layer is not always installed system-wide; point JOA_VK_LAYER_PATH at an extracted copy.
set(_vvl "${JOA_ROOT}/work/vvl/usr/share/vulkan/explicit_layer.d")
if(NOT EXISTS "${_vvl}/VkLayer_khronos_validation.json")
  set(_vvl "")
endif()
set(JOA_VK_LAYER_PATH "${_vvl}" CACHE PATH "Directory holding VkLayer_khronos_validation.json for vk_ tests (empty = system layers)")
find_file(JOA_VK_VALIDATION_JSON VkLayer_khronos_validation.json
          PATHS ${JOA_VK_LAYER_PATH} /usr/share/vulkan/explicit_layer.d /usr/local/share/vulkan/explicit_layer.d NO_DEFAULT_PATH)
set(JOA_VK_TEST_ENV "${_vk_driver};VK_LOADER_LAYERS_DISABLE=~implicit~;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
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
    # GPU tests: private Xvfb server (llvmpipe), else visible windows on the user's real display (JOA_TEST_XVFB=OFF).
    if(JOA_XVFB_RUN)
      add_test(NAME engine.${short} COMMAND ${JOA_XVFB_RUN} -a -s "-screen 0 1280x720x24" $<TARGET_FILE:engine_${n}>)
      set(env "SDL_VIDEO_DRIVER=x11")
    else()
      add_test(NAME engine.${short} COMMAND engine_${n})
      set(env "JOA_SHOW_WINDOWS=1")
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
    # Never the user's ~/.config (UiRunner loads and saves joan-native/options.ini there).
    set_tests_properties(engine.${short} PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "JOA_GAME_DIR=${JOA_GAME_DIR};JOA_TEST_OUT=${CMAKE_CURRENT_BINARY_DIR};XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}")
  endif()
endforeach()

# Ray tracing smoke test renders S1L1 eight times (docs/RAYTRACING.md).
if(TEST engine.vk_rt_smoke)
  set_tests_properties(engine.vk_rt_smoke PROPERTIES TIMEOUT 600)
endif()

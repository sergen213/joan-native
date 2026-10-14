# hd-upscale: Python tool (no build). CTest tools.hd_upscale: decoders + dry run + fake-upscaler run, no GPU/data.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND BUILD_TESTING)
  add_test(NAME tools.hd_upscale COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_hd_upscale.py)
endif()

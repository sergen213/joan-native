add_executable(joa-model-dump main.cpp)
target_link_libraries(joa-model-dump PRIVATE joa_formats)
target_compile_options(joa-model-dump PRIVATE -Wall -Wextra)

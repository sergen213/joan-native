add_executable(joa-res main.cpp)
target_link_libraries(joa-res PRIVATE joa_formats)
target_compile_options(joa-res PRIVATE -Wall -Wextra)

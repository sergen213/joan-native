add_executable(joa-script main.cpp)
target_link_libraries(joa-script PRIVATE joa_formats)
target_compile_options(joa-script PRIVATE -Wall -Wextra)

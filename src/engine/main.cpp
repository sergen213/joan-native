#include "engine/app.hpp"

#include <cstdio>
#include <string_view>

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::puts("joan-native " JOA_VERSION);
        return 0;
    }
    joa::engine::Options options;
    if (const auto err = joa::engine::parse_options(argc, argv, options); !err.empty()) {
        std::fprintf(stderr, "joan-native: %s\n%s", err.c_str(), joa::engine::usage());
        return 2;
    }
    return joa::engine::run(options);
}

#include "engine/app.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    joa::engine::Options options;
    if (const auto err = joa::engine::parse_options(argc, argv, options); !err.empty()) {
        std::fprintf(stderr, "joan-native: %s\n%s", err.c_str(), joa::engine::usage());
        return 2;
    }
    return joa::engine::run(options);
}

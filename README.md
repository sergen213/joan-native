# Wars and Warriors: Joan of Arc — native Linux port

Clean-room-style native Linux reconstruction of Enlight Software's 2004 *Wars and Warriors: Joan of Arc*.
Users supply their own original game (GOG install or installer); no original data is distributed.

Status and next steps: [docs/PROGRESS.md](docs/PROGRESS.md). Limits: [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DJOA_GAME_DIR=/path/to/original/game
cmake --build build -j
ctest --test-dir build --output-on-failure
```

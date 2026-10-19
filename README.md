# Cardinal

Cliente Trello nativo (C++20, GTK4/libadwaita). Design em `docs/superpowers/specs/`.

## Build

Dependências (Ubuntu): cmake ninja-build pkg-config libcurl4-openssl-dev libsqlite3-dev
nlohmann-json3-dev libsecret-1-dev libgtk-4-dev libadwaita-1-dev libgtest-dev libgmock-dev

Versões usadas no desenvolvimento: GTK 4.22.4, libadwaita 1.9.1, libsecret 0.21.7, CMake 4.2.3, g++ 15.2.

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

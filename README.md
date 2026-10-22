# Cardinal

Cliente Trello nativo (C++20, GTK4/libadwaita). Design em `docs/superpowers/specs/`.

## Build

Dependências (Ubuntu): cmake ninja-build pkg-config libcurl4-openssl-dev libsqlite3-dev
nlohmann-json3-dev libsecret-1-dev libgtk-4-dev libadwaita-1-dev libgtest-dev libgmock-dev

Versões usadas no desenvolvimento: GTK 4.22.4, libadwaita 1.9.1, libsecret 0.21.7, CMake 4.2.3, g++ 15.2.

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

## Build no Windows (WinUI 3)

A interface do Windows fica em `ui-win/`: WinUI 3 em C++/WinRT, sem empacotamento MSIX e
self-contained (não precisa do Windows App Runtime instalado). As telas são montadas em C++,
como na versão GTK, sobre os mesmos view-models do `core/`.

Requisitos:

- Visual Studio 2026 (ou 2022) com as cargas de trabalho "Desenvolvimento para desktop com C++"
  e "Desenvolvimento de aplicativos do Windows", incluindo as ferramentas de app WinUI para C++.
- [vcpkg](https://github.com/microsoft/vcpkg) com `VCPKG_ROOT` apontando para ele. As
  dependências do core (curl, sqlite3, nlohmann-json, gtest) vêm do `vcpkg.json`.

Para compilar o core, rodar os testes e compilar o app:

    tools\build-windows.cmd

O executável fica em `build\ui-win\x64\Debug\Cardinal.exe`. A API key embutida vem de
`msbuild -p:CardinalTrelloApiKey=...`. Em tempo de execução, `--api-key` ou `CARDINAL_API_KEY`
têm prioridade. O token fica no Gerenciador de Credenciais do Windows e o cache fica em
`%LOCALAPPDATA%\cardinal`.

Para desenvolver sem conta do Trello, as mesmas opções da versão GTK funcionam:

    python tools\mock_trello.py --port 8099 --cards 2000
    build\ui-win\x64\Debug\Cardinal.exe --api-base http://127.0.0.1:8099/1 --api-key K --token T0123456789abcdefghij

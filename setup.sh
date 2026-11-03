#!/usr/bin/env bash
# Installs build dependencies and configures CMake (no build).
# Usage: ./setup.sh [--no-deps]
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

install_deps=1
[[ "${1:-}" == "--no-deps" ]] && install_deps=0

sudo_cmd=""
[[ $EUID -ne 0 ]] && command -v sudo >/dev/null && sudo_cmd="sudo"

install_deps_apt() {
    $sudo_cmd apt-get update
    $sudo_cmd apt-get install -y \
        build-essential cmake ninja-build pkg-config clangd \
        libcurl4-openssl-dev libsqlite3-dev nlohmann-json3-dev libsecret-1-dev libmd4c-dev \
        libgtk-4-dev libadwaita-1-dev libgtest-dev libgmock-dev
}

install_deps_dnf() {
    $sudo_cmd dnf install -y \
        gcc-c++ cmake ninja-build pkgconf-pkg-config clang-tools-extra \
        libcurl-devel sqlite-devel json-devel libsecret-devel md4c-devel \
        gtk4-devel libadwaita-devel gtest-devel gmock-devel
}

install_deps_pacman() {
    $sudo_cmd pacman -S --needed --noconfirm \
        base-devel cmake ninja pkgconf clang \
        curl sqlite nlohmann-json libsecret md4c gtk4 libadwaita gtest
}

# MSYS2 (Windows): packages carry the mingw-w64 prefix of the active environment.
install_deps_msys2() {
    local p="${MINGW_PACKAGE_PREFIX:?run this from an MSYS2 MINGW64/UCRT64 shell}"
    pacman -S --needed --noconfirm \
        "$p-toolchain" "$p-cmake" "$p-ninja" "$p-pkgconf" "$p-clang-tools-extra" \
        "$p-curl" "$p-sqlite3" "$p-nlohmann-json" "$p-libsecret" "$p-md4c" \
        "$p-gtk4" "$p-libadwaita" "$p-gtest"
}

if [[ $install_deps -eq 1 ]]; then
    if [[ -n "${MSYSTEM:-}" ]]; then
        install_deps_msys2
    elif command -v apt-get >/dev/null; then
        install_deps_apt
    elif command -v dnf >/dev/null; then
        install_deps_dnf
    elif command -v pacman >/dev/null; then
        install_deps_pacman
    else
        echo "Unsupported package manager; install the dependencies listed in README.md" >&2
        exit 1
    fi
fi

cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo
echo "Configured. Build with:  cmake --build build && ctest --test-dir build"

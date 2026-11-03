#!/usr/bin/env bash
# Builds Cardinal inside a clean Docker container and produces an installable .deb in dist/.
# The package's dependencies are derived from the image's libraries, so an older base image gives a
# package installable on more distros.
# Usage: ./package-deb.sh
# Env:   BASE_IMAGE                  base image (default ubuntu:24.04)
#        RUN_TESTS=1                 run the test suite before packaging
#        CARDINAL_TRELLO_API_KEY     Trello API key compiled into the app
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

base_image="${BASE_IMAGE:-ubuntu:24.04}"
image="cardinal-deb-builder:${base_image//[:\/]/-}"

docker build -t "$image" - <<DOCKERFILE
FROM $base_image
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config dpkg-dev file \
        libcurl4-openssl-dev libsqlite3-dev nlohmann-json3-dev libsecret-1-dev libmd4c-dev \
        libgtk-4-dev libadwaita-1-dev libgtest-dev libgmock-dev \
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE

mkdir -p dist
# Build in a container-only directory so the host's build/ is untouched; output owned by the host user.
docker run --rm \
    -v "$PWD":/src:ro -v "$PWD/dist":/dist \
    -e CARDINAL_TRELLO_API_KEY="${CARDINAL_TRELLO_API_KEY:-}" \
    -e RUN_TESTS="${RUN_TESTS:-}" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$image" bash -euo pipefail -c '
        cmake -S /src -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX=/usr -DCARDINAL_TRELLO_API_KEY="$CARDINAL_TRELLO_API_KEY"
        cmake --build /tmp/build
        [[ -z "$RUN_TESTS" ]] || ctest --test-dir /tmp/build --output-on-failure
        cd /tmp/build && cpack -G DEB
        cp /tmp/build/*.deb /dist/
        chown "$HOST_UID:$HOST_GID" /dist/*.deb
    '

echo
ls -1 dist/*.deb
echo "Install with:  sudo apt install ./dist/<package>.deb"

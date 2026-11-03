find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)

add_library(cardinal_core STATIC
  src/version.cpp
  src/model_json.cpp
  src/store.cpp
  src/scheduler.cpp
  src/http_curl.cpp
  src/trello_api.cpp
  src/executor.cpp
  src/sync.cpp
  src/write_queue.cpp
  src/credentials.cpp
  src/credentials_libsecret.cpp
  src/auth_flow.cpp
  src/loopback_http.cpp
  src/viewmodel.cpp
  src/app_context.cpp
  src/session.cpp
  src/polling_event_source.cpp
  src/board_socket.cpp
  src/ws_curl.cpp
  src/cloudflared.cpp
  src/tunnel_cloudflared.cpp
  src/webhook_source.cpp
  src/time_util.cpp
  src/image_refs.cpp
  src/image_cache.cpp
  src/markdown.cpp
)
target_include_directories(cardinal_core PUBLIC include)
target_link_libraries(cardinal_core PUBLIC
  CURL::libcurl OpenSSL::Crypto SQLite::SQLite3 nlohmann_json::nlohmann_json PkgConfig::LIBSECRET PkgConfig::MD4C)

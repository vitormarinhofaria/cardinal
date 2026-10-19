find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

add_library(cardinal_core STATIC
  src/version.cpp
  src/model_json.cpp
  src/store.cpp
  src/scheduler.cpp
  src/http_curl.cpp
  src/trello_api.cpp
  src/executor.cpp
  src/sync.cpp
)
target_include_directories(cardinal_core PUBLIC include)
target_link_libraries(cardinal_core PUBLIC
  CURL::libcurl SQLite::SQLite3 nlohmann_json::nlohmann_json PkgConfig::LIBSECRET)

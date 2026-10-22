find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json REQUIRED)
if(WIN32)
  set(CARDINAL_CREDENTIALS_SRC src/credentials_wincred.cpp)
  set(CARDINAL_PLATFORM_LIBS ws2_32 advapi32)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
  set(CARDINAL_CREDENTIALS_SRC src/credentials_libsecret.cpp)
  set(CARDINAL_PLATFORM_LIBS PkgConfig::LIBSECRET)
endif()

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
  ${CARDINAL_CREDENTIALS_SRC}
  src/auth_flow.cpp
  src/viewmodel.cpp
  src/app_context.cpp
  src/session.cpp
  src/polling_event_source.cpp
  src/time_util.cpp
)
target_include_directories(cardinal_core PUBLIC include)
target_link_libraries(cardinal_core PUBLIC
  CURL::libcurl SQLite::SQLite3 nlohmann_json::nlohmann_json ${CARDINAL_PLATFORM_LIBS})

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1)

# The app's Trello API key is public (it identifies the app, not a secret) but it is registered
# by whoever builds the app: cmake -DCARDINAL_TRELLO_API_KEY=... Override at run time with
# CARDINAL_API_KEY or --api-key.
set(CARDINAL_TRELLO_API_KEY "" CACHE STRING "Trello API key compiled into the app")

add_executable(cardinal_gtk
  src/main.cpp
  src/platform.cpp
  src/window.cpp
  src/login_page.cpp
  src/app_page.cpp
  src/board_view.cpp
  src/card_panel.cpp
)
target_compile_definitions(cardinal_gtk PRIVATE CARDINAL_TRELLO_API_KEY="${CARDINAL_TRELLO_API_KEY}")
target_link_libraries(cardinal_gtk PRIVATE cardinal_core PkgConfig::GTK4 PkgConfig::ADW)

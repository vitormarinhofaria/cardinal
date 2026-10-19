find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1)

add_executable(cardinal_gtk src/main.cpp)
target_link_libraries(cardinal_gtk PRIVATE cardinal_core PkgConfig::GTK4 PkgConfig::ADW)

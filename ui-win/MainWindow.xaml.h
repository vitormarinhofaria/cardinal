#pragma once

#include "MainWindow.g.h"

namespace winrt::Cardinal::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
};

}  // namespace winrt::Cardinal::implementation

namespace winrt::Cardinal::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::Cardinal::factory_implementation

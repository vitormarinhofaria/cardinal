#pragma once

#include <memory>

#include "App.xaml.g.h"
#include "shell.h"

namespace winrt::Cardinal::implementation {

struct App : AppT<App> {
    App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

private:
    std::unique_ptr<cardinal::win::Shell> shell_;
    Microsoft::UI::Xaml::Window window_{nullptr};
};

}  // namespace winrt::Cardinal::implementation

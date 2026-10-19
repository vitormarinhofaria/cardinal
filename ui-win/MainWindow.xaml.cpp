#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "cardinal/version.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::Cardinal::implementation {

MainWindow::MainWindow() {
    InitializeComponent();
    versionText().Text(L"Cardinal " + to_hstring(std::string(cardinal::version())));
}

}  // namespace winrt::Cardinal::implementation

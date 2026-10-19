#include "pch.h"

#include "login_page.h"

#include "win_util.h"

namespace cardinal::win {
namespace {

using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

const char* errorText(SessionError e) {
    switch (e) {
        case SessionError::None: return "";
        case SessionError::MissingApiKey:
            return "Esta compilação não tem uma API key do Trello embutida. Use a seção abaixo: "
                   "informe a sua própria API key e gere um token.";
        case SessionError::InvalidApiKey:
            return "Essa API key não parece válida. Copie a de trello.com/app-key.";
        case SessionError::BrowserFailed: return "Não foi possível abrir o navegador.";
        case SessionError::LoginTimedOut: return "O login demorou demais. Tente de novo.";
        case SessionError::InvalidToken: return "Esse token não parece válido.";
        case SessionError::SessionExpired: return "Sua sessão expirou. Entre de novo para continuar.";
    }
    return "";
}

}  // namespace

LoginPage::LoginPage(Session& session) : session_(session) {
    auto content = vstack(16);
    content.HorizontalAlignment(HorizontalAlignment::Center);
    content.VerticalAlignment(VerticalAlignment::Center);
    content.MaxWidth(520);
    content.Padding(ThicknessHelper::FromUniformLength(24));

    auto title = text("Cardinal", 40);
    title.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    title.HorizontalAlignment(HorizontalAlignment::Center);
    content.Children().Append(title);
    auto subtitle = text("Um cliente rápido e nativo para o Trello.", 16);
    subtitle.HorizontalAlignment(HorizontalAlignment::Center);
    subtitle.Opacity(0.75);
    content.Children().Append(subtitle);

    signIn_ = button("Entrar com o Trello", true);
    signIn_.HorizontalAlignment(HorizontalAlignment::Center);
    signIn_.Padding(ThicknessHelper::FromLengths(24, 8, 24, 8));
    signIn_.Click([this](auto&&, auto&&) { session_.startLogin(); });
    content.Children().Append(signIn_);

    waiting_ = vstack(8);
    ProgressRing ring;
    ring.IsActive(true);
    ring.Width(32);
    ring.Height(32);
    waiting_.Children().Append(ring);
    auto waitingText = text("Aguardando você autorizar no navegador…");
    waitingText.HorizontalAlignment(HorizontalAlignment::Center);
    waiting_.Children().Append(waitingText);
    auto cancel = button("Cancelar");
    cancel.HorizontalAlignment(HorizontalAlignment::Center);
    cancel.Click([this](auto&&, auto&&) { session_.cancelLogin(); });
    waiting_.Children().Append(cancel);
    content.Children().Append(waiting_);

    error_.Severity(InfoBarSeverity::Error);
    error_.IsClosable(false);
    content.Children().Append(error_);

    auto manualBox = vstack(8);
    manualBox.Children().Append(text(
        "Use isto se o login automático não funcionar, ou para usar a sua própria API key:\n"
        "1. Abra a página de chaves do Trello e copie a sua API key.\n"
        "2. Cole a key abaixo e abra a página de autorização.\n"
        "3. Clique em Permitir e copie o token que o Trello mostra.\n"
        "4. Cole o token abaixo e clique em Usar token.",
        0, true));

    HyperlinkButton keyPage;
    keyPage.Content(winrt::box_value(L"Passo 1 · Abrir trello.com/app-key"));
    keyPage.NavigateUri(winrt::Windows::Foundation::Uri(L"https://trello.com/app-key"));
    manualBox.Children().Append(keyPage);

    Grid keyRow;
    keyRow.ColumnSpacing(8);
    keyRow.ColumnDefinitions().Append(ColumnDefinition());
    ColumnDefinition auto1;
    auto1.Width(GridLengthHelper::Auto());
    keyRow.ColumnDefinitions().Append(auto1);
    keyEntry_.PlaceholderText(L"API key (32 caracteres)");
    auto tokenPage = button("Passo 2 · Abrir autorização");
    tokenPage.Click([this](auto&&, auto&&) { session_.openTokenPage(str(keyEntry_.Text())); });
    Grid::SetColumn(tokenPage, 1);
    keyRow.Children().Append(keyEntry_);
    keyRow.Children().Append(tokenPage);
    manualBox.Children().Append(keyRow);

    Grid tokenRow;
    tokenRow.ColumnSpacing(8);
    tokenRow.ColumnDefinitions().Append(ColumnDefinition());
    ColumnDefinition auto2;
    auto2.Width(GridLengthHelper::Auto());
    tokenRow.ColumnDefinitions().Append(auto2);
    tokenEntry_.PlaceholderText(L"Token que o Trello mostrou");
    auto use = button("Passo 4 · Usar token", true);
    auto submit = [this] { session_.loginWithToken(str(tokenEntry_.Password()), str(keyEntry_.Text())); };
    use.Click([submit](auto&&, auto&&) { submit(); });
    tokenEntry_.KeyDown([submit](auto&&, Input::KeyRoutedEventArgs const& e) {
        if (e.Key() == winrt::Windows::System::VirtualKey::Enter) submit();
    });
    Grid::SetColumn(use, 1);
    tokenRow.Children().Append(tokenEntry_);
    tokenRow.Children().Append(use);
    manualBox.Children().Append(tokenRow);

    manual_.Header(winrt::box_value(L"Entrar com a minha API key e um token"));
    manual_.Content(manualBox);
    manual_.HorizontalAlignment(HorizontalAlignment::Stretch);
    manual_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    content.Children().Append(manual_);

    root_.Content(content);
    root_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    update(session.state().get());
}

void LoginPage::update(const SessionState& state) {
    const bool waiting = state.phase == SessionState::Phase::WaitingForBrowser;
    show(signIn_, !waiting);
    show(waiting_, waiting);
    const char* message = errorText(state.error);
    error_.Message(hs(message));
    error_.IsOpen(*message != '\0');
    if (!waiting) tokenEntry_.Password(L"");
    // Errors about the key or the token are fixed in the manual section: open it.
    if (state.error == SessionError::MissingApiKey || state.error == SessionError::InvalidApiKey ||
        state.error == SessionError::InvalidToken)
        manual_.IsExpanded(true);
}

}  // namespace cardinal::win

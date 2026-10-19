#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cardinal/viewmodel.h"

namespace cardinal::win {

// The card, as a side pane: title, description, labels, members, comments, and the extras
// (checklists, attachments) that load a moment later. Pure view over CardPanelVM.
class CardPanel {
public:
    CardPanel(std::shared_ptr<CardPanelVM> vm, std::function<void()> onClose);
    ~CardPanel();
    winrt::Microsoft::UI::Xaml::UIElement widget() const { return root_; }

private:
    void update(const CardPanelState& s);
    void updateNotice(const CardPanelState& s);
    void updateLabels(const CardPanelState& s);
    void updateMembers(const CardPanelState& s);
    void updateComments(const CardPanelState& s);
    void updateExtras(const CardPanelState& s);
    void commitTitle();
    void sendComment();
    void saveDescription();
    void revertDescription();
    void descriptionEdited();
    std::string descriptionText() const;
    void setDescriptionText(const std::string& text);

    std::shared_ptr<CardPanelVM> vm_;
    std::function<void()> onClose_;
    int conn_ = 0;

    winrt::Microsoft::UI::Xaml::Controls::Grid root_;
    winrt::Microsoft::UI::Xaml::Controls::ProgressRing loading_;
    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer contentScroll_;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel content_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::InfoBar banner_;
    winrt::Microsoft::UI::Xaml::Controls::TextBox title_;
    winrt::Microsoft::UI::Xaml::Controls::TextBlock listName_;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel labelsBox_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button addLabelButton_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Flyout addLabelFlyout_;
    winrt::Microsoft::UI::Xaml::Controls::ListView addLabelList_;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel membersBox_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button addMemberButton_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Flyout addMemberFlyout_;
    winrt::Microsoft::UI::Xaml::Controls::ListView addMemberList_;
    winrt::Microsoft::UI::Xaml::Controls::TextBox description_;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel descButtons_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox comment_;
    winrt::Microsoft::UI::Xaml::Controls::Button sendButton_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel commentsBox_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel extrasSpinner_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel checklistsBox_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel attachmentsBox_{nullptr};

    // what is currently shown, to rebuild only what changed
    std::string shownTitle_;
    std::string shownDescription_;
    bool settingText_ = false;
    bool editingDesc_ = false;               // the user changed the description box
    std::string descBase_;                   // what it said when they started
    std::string titleBase_;                  // same for the title
    bool editingTitle_ = false;
    bool remoteChangedWhileEditing_ = false;
    bool labelPickerBuilt_ = false;
    bool memberPickerBuilt_ = false;
    std::vector<LabelChip> shownLabels_, shownBoardLabels_;
    std::vector<MemberChip> shownMembers_, shownBoardMembers_;
    std::vector<CommentRow> shownComments_;
    std::vector<Checklist> shownChecklists_;
    std::vector<Attachment> shownAttachments_;
    std::vector<PanelNotice> shownNotices_;
    bool shownExtrasLoaded_ = false;
};

}  // namespace cardinal::win

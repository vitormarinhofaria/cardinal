#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cardinal/viewmodel.h"
#include "gtk_util.h"

namespace cardinal::gtk {

// The card, as a side sheet: title, description, labels, members, comments, and the extras
// (checklists, attachments) that load a moment later. Pure view over CardPanelVM.
class CardPanel {
public:
    CardPanel(std::shared_ptr<CardPanelVM> vm, std::function<void()> onClose);
    ~CardPanel();
    GtkWidget* widget() const { return root_; }

    // GTK callbacks reach in here.
    void commitTitle();
    void sendComment();
    void saveDescription();
    void revertDescription();
    void descriptionEdited();

    // Development aid (CARDINAL_SELFTEST=1): drives the same handlers a user would.
    void selfTest();

private:
    void update(const CardPanelState& s);
    void updateNotice(const CardPanelState& s);
    void updateLabels(const CardPanelState& s);
    void updateMembers(const CardPanelState& s);
    void updateComments(const CardPanelState& s);
    void updateExtras(const CardPanelState& s);
    std::string descriptionText() const;
    void setDescriptionText(const std::string& text);

    std::shared_ptr<CardPanelVM> vm_;
    std::function<void()> onClose_;
    int conn_ = 0;
    bool tracedReady_ = false;

    GtkWidget* root_;
    GtkWidget* stack_;
    GtkWidget* banner_;
    GtkWidget* title_;
    GtkWidget* listName_;
    GtkWidget* labelsBox_;
    GtkWidget* addLabelButton_;
    GtkWidget* addLabelList_;
    GtkWidget* membersBox_;
    GtkWidget* addMemberButton_;
    GtkWidget* addMemberList_;
    GtkWidget* descView_;
    GtkTextBuffer* descBuffer_;
    GtkWidget* descButtons_;
    GtkWidget* commentView_;
    GtkTextBuffer* commentBuffer_;
    GtkWidget* sendButton_;
    GtkWidget* commentsBox_;
    GtkWidget* extrasSpinner_;
    GtkWidget* checklistsBox_;
    GtkWidget* attachmentsBox_;
    GtkWidget* content_;

    // what is currently shown, to rebuild only what changed
    std::string shownTitle_;
    std::string shownDescription_;
    bool settingText_ = false;
    std::vector<LabelChip> shownLabels_, shownBoardLabels_;
    std::vector<MemberChip> shownMembers_, shownBoardMembers_;
    std::vector<CommentRow> shownComments_;
    std::vector<Checklist> shownChecklists_;
    std::vector<Attachment> shownAttachments_;
    std::vector<PanelNotice> shownNotices_;
    CardPanelState::Status shownStatus_ = CardPanelState::Status::Loading;
    bool shownExtrasLoaded_ = false;
    std::vector<std::string> labelRowIds_, memberRowIds_;
};

}  // namespace cardinal::gtk

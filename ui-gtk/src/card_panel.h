#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cardinal/viewmodel.h"
#include "gtk_util.h"
#include "image_loader.h"

namespace cardinal::gtk {

// The card's content, shown in a dialog over the board: title, description, labels, members, comments, and the extras
// (checklists, attachments) that load a moment later. Pure view over CardPanelVM.
class CardPanel {
public:
    // `images` (not owned, may be null) shows the pictures of attachments, description, comments.
    CardPanel(std::shared_ptr<CardPanelVM> vm, ImageLoader* images, std::function<void()> onClose);
    ~CardPanel();
    GtkWidget* widget() const { return root_; }
    // Narrow dialog: stack the two columns, labels and members on top.
    void setNarrow(bool narrow);
    bool editingTitle() const;

    // GTK callbacks reach in here.
    void commitTitle();
    void startTitleEdit();
    void finishTitleEdit(bool commit);
    void sendComment();
    void saveDescription();
    void revertDescription();
    void descriptionEdited();

    // Development aid (CARDINAL_SELFTEST=1): drives the same handlers a user would.
    void selfTest();
    void selfTestDraft();  // leave an unsaved description and close the panel

private:
    void update(const CardPanelState& s);
    void updateNotice(const CardPanelState& s);
    void updateLabels(const CardPanelState& s);
    void updateMembers(const CardPanelState& s);
    void updateComments(const CardPanelState& s);
    void updateExtras(const CardPanelState& s);
    void updateDescriptionImages(const CardPanelState& s);
    // The URL to fetch for a thumbnail of `url`: Trello's scaled preview when it is an attachment.
    std::string thumbUrlFor(const std::string& url) const;
    // Pictures of `urls` ready to show now. A Trello attachment waits for the attachment list, so
    // its small preview is fetched rather than the (much bigger) original. Null if none is ready.
    GtkWidget* thumbStrip(const std::vector<std::string>& urls, int side) const;
    std::string descriptionText() const;
    void setDescriptionText(const std::string& text);

    std::shared_ptr<CardPanelVM> vm_;
    ImageLoader* images_;
    std::function<void()> onClose_;
    int conn_ = 0;
    bool tracedReady_ = false;

    GtkWidget* root_;
    GtkWidget* stack_;
    GtkWidget* banner_;
    GtkWidget* title_;  // stack: wrapping label, or a wrapping text view while editing
    GtkWidget* titleLabel_;
    GtkWidget* titleView_;
    GtkTextBuffer* titleBuffer_;
    bool editingTitle_ = false;
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
    GtkWidget* descImagesBox_;
    GtkWidget* content_;
    GtkWidget* columns_;
    GtkWidget* main_;   // description, checklists, attachments, comments
    GtkWidget* mainClamp_;  // caps main_'s width so long text can't squeeze side_
    GtkWidget* side_;   // labels, members

    // what is currently shown, to rebuild only what changed
    std::string shownTitle_;
    std::string shownDescription_;
    bool settingText_ = false;
    bool editingDesc_ = false;               // the user changed the description box
    std::string descBase_;                   // what it said when they started
    std::string titleBase_;                  // same for the title
    bool remoteChangedWhileEditing_ = false;
    bool labelPickerBuilt_ = false;
    bool memberPickerBuilt_ = false;
    std::vector<LabelChip> shownLabels_, shownBoardLabels_;
    std::vector<MemberChip> shownMembers_, shownBoardMembers_;
    std::vector<CommentRow> shownComments_;
    std::vector<Checklist> shownChecklists_;
    std::vector<Attachment> shownAttachments_;
    std::vector<std::string> shownDescImages_;
    // Bumped when the attachments or extrasLoaded change: the pictures in the description and the
    // comments depend on both (see thumbStrip) and are rebuilt when it moves.
    int attachmentsVersion_ = 0;
    int descImagesVersion_ = -1;
    int commentsVersion_ = -1;
    std::vector<PanelNotice> shownNotices_;
    CardPanelState::Status shownStatus_ = CardPanelState::Status::Loading;
    bool shownExtrasLoaded_ = false;
    std::vector<std::string> labelRowIds_, memberRowIds_;
};

}  // namespace cardinal::gtk

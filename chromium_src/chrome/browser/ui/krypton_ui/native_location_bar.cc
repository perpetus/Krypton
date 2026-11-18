// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/native_location_bar.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/numerics/safe_conversions.h"
#include "base/scoped_observation.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/bubble_anchor_util.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/krypton_ui/control_anchors.h"
#include "chrome/browser/ui/krypton_ui/privacy_features_bridge.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/omnibox/chrome_omnibox_client.h"
#include "chrome/browser/ui/omnibox/omnibox_controller.h"
#include "chrome/browser/ui/omnibox/omnibox_edit_model.h"
#include "chrome/browser/ui/omnibox/omnibox_popup_state_manager.h"
#include "chrome/browser/ui/omnibox/omnibox_popup_view.h"
#include "chrome/browser/ui/omnibox/omnibox_view.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/bubble_anchor_util_views.h"
#include "components/omnibox/browser/autocomplete_controller.h"
#include "components/omnibox/browser/autocomplete_match_type.h"
#include "components/omnibox/browser/autocomplete_provider.h"
#include "components/omnibox/browser/location_bar_model.h"
#include "components/omnibox/browser/searchbox_utils.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_provider.h"
#include "ui/display/screen.h"
#include "ui/events/event.h"
#include "ui/gfx/image/image.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/interaction/element_tracker_views.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace krypton_ui {
namespace {
constexpr char kSavedStateKey[] = "KryptonUI.OmniboxState";

KrStr Borrow(const std::string& text) {
  return {text.data(), text.size()};
}

// How an open tab matches typed words: 0 when each word starts a word of its
// lowercased title or address, 1 when each only appears somewhere in them.
std::optional<int> OpenTabRank(const std::vector<std::string_view>& terms,
                               std::string_view text) {
  int rank = 0;
  for (const auto term : terms) {
    size_t at = text.find(term);
    if (at == std::string_view::npos) {
      return std::nullopt;
    }
    while (at != std::string_view::npos && at != 0 &&
           base::IsAsciiAlphaNumeric(text[at - 1])) {
      at = text.find(term, at + 1);
    }
    if (at == std::string_view::npos) {
      rank = 1;
    }
  }
  return rank;
}

class SavedState final : public base::SupportsUserData::Data {
 public:
  SavedState(OmniboxEditModel& model, OmniboxView& editor)
      : model(model.GetStateForTabSwitch()),
        text(editor.GetText()),
        selection(editor.GetSelectionBounds()) {}
  const OmniboxEditModel::State model;
  const std::u16string text;
  const gfx::Range selection;
};

uint32_t SuggestionKind(const AutocompleteMatch& match) {
  if (match.type == omnibox::AutocompleteMatchType::kOpenTab) {
    return KR_SUGGESTION_OPEN_TAB;
  }
  if (AutocompleteMatch::IsSearchType(match.type)) {
    return KR_SUGGESTION_SEARCH;
  }
  if (match.type == omnibox::AutocompleteMatchType::kHistoryUrl ||
      match.type == omnibox::AutocompleteMatchType::kHistoryTitle ||
      match.type == omnibox::AutocompleteMatchType::kHistoryBody) {
    return KR_SUGGESTION_HISTORY;
  }
  if (match.type == omnibox::AutocompleteMatchType::kBookmarkTitle) {
    return KR_SUGGESTION_BOOKMARK;
  }
  return KR_SUGGESTION_URL;
}
}  // namespace

static_assert(static_cast<int>(KR_SUGGESTION_TEXT_URL) ==
              ACMatchClassification::URL);
static_assert(static_cast<int>(KR_SUGGESTION_TEXT_MATCH) ==
              ACMatchClassification::MATCH);
static_assert(static_cast<int>(KR_SUGGESTION_TEXT_DIM) ==
              ACMatchClassification::DIM);
static_assert(static_cast<int>(KR_SUGGESTION_TEXT_TOOLBELT) ==
              ACMatchClassification::TOOLBELT);
struct NativeLocationBar::Snapshot {
  searchbox::AutocompleteSnapshot autocomplete;
  std::vector<size_t> source_indices;
  std::vector<CommandBarEntry> commands;
  std::vector<KrImageId> image_leases;
  base::RepeatingCallback<void(KrImageId)> release_image;
  ~Snapshot() { ReleaseImages(); }
  void ReleaseImages() {
    if (release_image) {
      for (const auto id : image_leases) {
        release_image.Run(id);
      }
    }
    image_leases.clear();
  }
};

class NativeLocationBar::Editor final : public OmniboxView {
 public:
  Editor(NativeLocationBar& owner, OmniboxController& controller)
      : OmniboxView(&controller), owner_(owner) {}

  void ApplyNative(const KrAddressState& state, bool initial = false) {
    if (!initial) {
      OnBeforePossibleChange();
    }
    inline_start_.reset();
    text_ = base::UTF8ToUTF16(std::string_view(
        state.text.data ? state.text.data : "", state.text.size));
    if (KR_HAS_FIELD(&state, KrAddressState, composing)) {
      selection_ = gfx::Range(state.selection_start, state.selection_end);
      composing_ = state.composing;
    } else {
      selection_ = gfx::Range(text_.size());
      composing_ = false;
    }
    if (!initial) {
      OnAfterPossibleChange(!composing_);
    }
  }
  bool composing() const { return composing_; }

  void Update() override {
    if (controller()->edit_model()->ResetDisplayTexts()) {
      RevertAll();
    }
  }
  std::u16string GetText() const override { return text_; }
  void SetWindowTextAndCaretPos(const std::u16string& text,
                                size_t caret,
                                bool update_popup,
                                bool notify_text_changed) override {
    if (composing_) {
      return;
    }
    text_ = text;
    selection_ = gfx::Range(std::min(caret, text_.size()));
    owner_->PublishEdit();
    if (notify_text_changed) {
      TextChanged();
    }
    if (update_popup) {
      UpdatePopup();
    }
  }
  void SetCaretPos(size_t caret) override {
    SetSelectionBounds(gfx::Range(std::min(caret, text_.size())));
  }
  void SetAdditionalText(const std::u16string& text) override {
    // AIM's additional text has no enabled control in the native toolbar.
  }
  void EnterKeywordModeForDefaultSearchProvider() override {
    controller()->edit_model()->EnterKeywordModeForDefaultSearchProvider(
        metrics::OmniboxEventProto::KEYBOARD_SHORTCUT);
  }
  bool IsSelectAll() const override {
    return !text_.empty() &&
           selection_.EqualsIgnoringDirection(gfx::Range(0, text_.size()));
  }
  gfx::Range GetSelectionBounds() const override { return selection_; }
  void SetSelectionBounds(gfx::Range selection) override {
    if (!selection.IsValid() || selection.GetMax() > text_.size() ||
        composing_) {
      return;
    }
    selection_ = selection;
    owner_->PublishEdit();
  }
  bool HasSelection() const override { return !selection_.is_empty(); }
  void SelectAll(bool reversed) override {
    SetSelectionBounds(reversed ? gfx::Range(text_.size(), 0)
                                : gfx::Range(0, text_.size()));
  }
  void UpdatePopup() override {
    if (composing_ || !owner_->open_ || !owner_->request_ ||
        !owner_->focused_) {
      return;
    }
    owner_->query_started_ = true;
    if (owner_->CommandQuery()) {
      controller()->StopAutocomplete(true);
      owner_->ScheduleSuggestions();
      return;
    }
    controller()->edit_model()->UpdateInput(selection_.GetMax() !=
                                            text_.size());
  }
  void SetFocus(bool user_initiated) override {
    owner_->FocusLocation(user_initiated, false);
  }
  bool AimButtonVisible() const override { return false; }
  void ApplyCaretVisibility() override {}
  void OnTemporaryTextMaybeChanged(const std::u16string& text,
                                   const AutocompleteMatch& match,
                                   bool save_selection,
                                   bool notify_text_changed) override {
    if (save_selection) {
      saved_selection_ = selection_;
    }
    SetWindowTextAndCaretPos(text, text.size(), false, notify_text_changed);
  }
  void OnInlineAutocompleteTextMaybeChanged(
      const std::u16string& user_text,
      const std::u16string& inline_text) override {
    if (composing_) {
      return;
    }
    std::u16string display_text = user_text + inline_text;
    // Match OmniboxViewViews: suggestion refreshes with unchanged text must
    // preserve the user's caret and selection, including mid-string deletions.
    if (display_text == text_) {
      return;
    }
    text_ = std::move(display_text);
    inline_start_ = user_text.size();
    selection_ = gfx::Range(user_text.size(), text_.size());
    owner_->PublishEdit();
  }
  void OnInlineAutocompleteTextCleared() override {
    if (inline_start_ && selection_.GetMin() == *inline_start_ &&
        selection_.GetMax() == text_.size()) {
      text_.resize(*inline_start_);
      selection_ = gfx::Range(text_.size());
      owner_->PublishEdit();
    }
    inline_start_.reset();
  }
  void OnRevertTemporaryText(const std::u16string& text,
                             const AutocompleteMatch& match) override {
    SetWindowTextAndCaretPos(text, text.size(), false, false);
    if (saved_selection_.IsValid() &&
        saved_selection_.GetMax() <= text_.size()) {
      SetSelectionBounds(saved_selection_);
    }
  }
  void OnBeforePossibleChange() override {
    before_ = GetState();
    was_composing_ = composing_;
  }
  bool OnAfterPossibleChange(bool allow_keyword) override {
    const State after = GetState();
    auto changes = GetStateChanges(before_, after);
    changes.text_differs |= was_composing_ != composing_;
    const bool changed = controller()->edit_model()->OnAfterPossibleChange(
        changes, allow_keyword && !composing_);
    if (changed && (changes.text_differs || changes.keyword_differs)) {
      TextChanged();
    }
    return changed;
  }
  bool IsImeShowingPopup() const override { return composing_; }

 private:
  // Native XAML owns text painting, caret and its accessibility tree.
  int GetOmniboxTextLength() const override {
    return static_cast<int>(text_.size());
  }
  void EmphasizeURLComponents() override {}
  void SetEmphasis(bool emphasize, const gfx::Range& range) override {}
  void UpdateSchemeStyle(const gfx::Range& range) override {}

  const raw_ref<NativeLocationBar> owner_;
  std::u16string text_;
  gfx::Range selection_{0};
  gfx::Range saved_selection_;
  std::optional<size_t> inline_start_;
  State before_;
  bool composing_ = false;
  bool was_composing_ = false;
};

class NativeLocationBar::Popup final : public OmniboxPopupView,
                                       public OmniboxEditModel::Observer {
 public:
  Popup(NativeLocationBar& owner, OmniboxController& controller)
      : OmniboxPopupView(&controller), owner_(owner) {
    model_observation_.Observe(controller.edit_model());
  }
  bool IsOpen() const override {
    return owner_->open_ && owner_->request_ &&
           owner_->visible_result_count_ != 0;
  }
  void InvalidateLine(size_t line) override {
    // Native selection is already styled by key/index; result updates publish
    // a new immutable generation through UpdatePopupAppearance().
  }
  void UpdatePopupAppearance() override { owner_->PublishSuggestions(); }
  // Chromium delivers result updates through the edit model's observer list;
  // implementing UpdatePopupAppearance alone does not subscribe to them.
  void OnContentsChanged() override { UpdatePopupAppearance(); }
  void OnSelectionChanged(OmniboxPopupSelection,
                          OmniboxPopupSelection) override {
    owner_->PublishSelection();
  }
  void OnMatchIconUpdated(size_t) override { owner_->ScheduleSuggestions(); }
  void OnCharTyped(base::TimeTicks) override {}
  void ProvideButtonFocusHint(size_t line) override {}
  void OnDragCanceled() override {}
  void GetPopupAccessibleNodeData(ui::AXNodeData* node) const override {
    node->role = ax::mojom::Role::kListBox;
  }
  bool IsSelectionPopupControlled() const override { return false; }

 private:
  const raw_ref<NativeLocationBar> owner_;
  base::ScopedObservation<OmniboxEditModel, OmniboxEditModel::Observer>
      model_observation_{this};
};

NativeLocationBar::NativeLocationBar(
    BrowserWindowInterface& browser,
    CommandUpdater& commands,
    LocationBarModel& model,
    WindowSession& session,
    ControlAnchors& anchors,
    views::View& host,
    base::RepeatingCallback<
        base::expected<KrImageId, std::string>(const ui::ImageModel&)> images,
    base::RepeatingCallback<void(KrImageId)> release_image,
    base::RepeatingClosure changed)
    : LocationBar(&commands),
      browser_(browser),
      location_model_(model),
      session_(session),
      anchors_(anchors),
      host_(host),
      changed_(std::move(changed)),
      images_(std::move(images)),
      release_image_(std::move(release_image)) {}

NativeLocationBar::~NativeLocationBar() {
  Close();
}

void NativeLocationBar::Close() {
  weak_factory_.InvalidateWeakPtrs();
  suggestions_pending_ = false;
  open_ = false;
  focused_ = false;
  request_ = 0;
  if (controller_) {
    controller_->popup_state_manager()->SetPopupState(OmniboxPopupState::kNone);
    controller_->StopAutocomplete(true);
  }
  snapshots_.clear();
  if (controller_) {
    controller_->edit_model()->set_popup_view(nullptr);
  }
  popup_.reset();
  editor_.reset();
  controller_.reset();
}

void NativeLocationBar::Initialize() {
  CHECK(!controller_);
  controller_ =
      std::make_unique<OmniboxController>(std::make_unique<ChromeOmniboxClient>(
          this, &*browser_, browser_->GetProfile()));
  editor_ = std::make_unique<Editor>(*this, *controller_);
  popup_ = std::make_unique<Popup>(*this, *controller_);
  controller_->edit_model()->set_popup_view(popup_.get());
  editor_->Update();
}

void NativeLocationBar::OnNativeState(const KrAddressState& state) {
  if (!controller_ || !KR_HAS_FIELD(&state, KrAddressState, text) ||
      (!state.text.data && state.text.size)) {
    return;
  }
  const bool was_open = open_, was_focused = focused_;
  const bool next_open = (state.flags & KR_ADDRESS_OPEN) != 0;
  const auto text = base::UTF8ToUTF16(std::string_view(
      state.text.data ? state.text.data : "", state.text.size));
  const bool steady = text.empty() ||
                      text == location_model_->GetFormattedFullURL() ||
                      text == location_model_->GetURLForDisplay();
  const bool select_all = KR_HAS_FIELD(&state, KrAddressState, selection_end) &&
                          state.selection_start == 0 &&
                          state.selection_end == text.size();
  const bool initial =
      next_open &&
      (!was_open || (request_ != state.request_id && select_all && steady));
  if (initial) {
    controller_->edit_model()->Revert();
  }
  open_ = next_open;
  focused_ = (state.flags & KR_ADDRESS_FOCUSED) != 0;
  launcher_ = (state.flags & KR_ADDRESS_NEW_TAB) != 0;
  if (request_ != state.request_id) {
    selected_command_.reset();
    request_ = state.request_id;
    query_started_ = false;
    initial_steady_ = initial && steady;
  }
  if (KR_HAS_FIELD(&state, KrAddressState, edit_revision)) {
    edit_revision_ = state.edit_revision;
  }
  if (focused_ && !was_focused) {
    controller_->edit_model()->OnSetFocus(false);
  }
  if (open_) {
    editor_->ApplyNative(state, initial);
  }
  if (focused_ && !was_focused) {
    Query(request_, {});
  } else if (!focused_ && was_focused) {
    controller_->edit_model()->OnWillKillFocus();
    controller_->edit_model()->OnKillFocus();
  }
  if (!open_ && was_open) {
    accept_control_ = false;
    snapshots_.clear();
    visible_result_count_ = 0;
    controller_->popup_state_manager()->SetPopupState(OmniboxPopupState::kNone);
    controller_->StopAutocomplete(true);
  }
  if (focused_ != was_focused) {
    NotifyFocusChanged();
  }
}

void NativeLocationBar::Query(uint32_t request, std::string_view) {
  if (request != request_ || !request || query_started_ || !editor_ ||
      editor_->composing() || !focused_) {
    return;
  }
  if (CommandQuery()) {
    controller_->StopAutocomplete(true);
    query_started_ = true;
    ScheduleSuggestions();
    return;
  }
  if (initial_steady_) {
    query_started_ = true;
    controller_->edit_model()->StartZeroSuggestRequest();
  } else {
    controller_->edit_model()->SetUserText(editor_->GetText());
    if (!query_started_) {
      editor_->UpdatePopup();
    }
  }
}

void NativeLocationBar::CancelQuery(uint32_t request) {
  if (request != request_) {
    return;
  }
  request_ = 0;
  snapshots_.clear();
  visible_result_count_ = 0;
  query_started_ = false;
  if (controller_) {
    controller_->popup_state_manager()->SetPopupState(OmniboxPopupState::kNone);
    controller_->StopAutocomplete(!open_);
  }
}

void NativeLocationBar::PublishEdit() {
  const auto* ui = session_->api();
  if (!ui || !open_ || editor_->composing()) {
    return;
  }
  const std::string text = base::UTF16ToUTF8(editor_->GetText());
  const gfx::Range selection = editor_->GetSelectionBounds();
  const KrAddressEdit edit{sizeof(edit),
                           request_,
                           edit_revision_,
                           Borrow(text),
                           static_cast<uint32_t>(selection.GetMin()),
                           static_cast<uint32_t>(selection.GetMax())};
  ui->set_address_edit(ui->self, &edit);
}

void NativeLocationBar::ScheduleSuggestions() {
  if (suggestions_pending_ || !open_ || !request_) {
    return;
  }
  suggestions_pending_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&NativeLocationBar::PublishSuggestions,
                                weak_factory_.GetWeakPtr()));
}
bool NativeLocationBar::CommandQuery() const {
  return editor_ && editor_->GetText().starts_with(u">");
}

void NativeLocationBar::PublishSuggestions() {
  suggestions_pending_ = false;
  const auto* ui = session_->api();
  if (!ui || !open_ || !request_ || editor_->composing()) {
    return;
  }
  auto snapshot = std::make_unique<Snapshot>();
  snapshot->release_image = release_image_;
  snapshot->autocomplete = searchbox::MakeAutocompleteSnapshot(
      controller_->autocomplete_controller());
  const auto& result = snapshot->autocomplete.result;
  const bool only_commands = CommandQuery();
  const std::string query = base::UTF16ToUTF8(editor_->GetText());
  // An unedited address is the current page, not a request: offer tabs and
  // commands only for typed text, as the launcher always is.
  const bool typed =
      launcher_ || controller_->edit_model()->user_input_in_progress();
  if (command_provider_ && (only_commands || (typed && !query.empty()))) {
    snapshot->commands = command_provider_.Run(
        only_commands ? std::string_view(query).substr(1) : query);
    if (!only_commands && !launcher_) {
      // Editing an address offers open tabs to switch to, never commands. Each
      // typed word must appear in a tab's title or address, word starts first;
      // the launcher's subsequence matching would offer unrelated tabs here.
      const std::string normalized = base::ToLowerASCII(query);
      const auto terms = base::SplitStringPiece(
          normalized, " \t", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
      for (auto& entry : snapshot->commands) {
        const auto rank =
            entry.kind == KR_SUGGESTION_OPEN_TAB
                ? OpenTabRank(terms, base::ToLowerASCII(entry.title + " " +
                                                        entry.url.spec()))
                : std::nullopt;
        entry.rank = rank.value_or(-1);
      }
      std::erase_if(snapshot->commands, [](const CommandBarEntry& entry) {
        return entry.rank < 0;
      });
      std::ranges::stable_sort(snapshot->commands, {}, &CommandBarEntry::rank);
      if (snapshot->commands.size() > 4)
        snapshot->commands.resize(4);
    }
    if (!only_commands && snapshot->commands.size() > 8)
      snapshot->commands.resize(8);
  }
  std::vector<std::string> contents;
  std::vector<std::string> descriptions;
  std::vector<KrSuggestion> items;
  std::vector<std::vector<KrSuggestionClassification>> contents_classes,
      description_classes;
  contents.reserve(result.size() + snapshot->commands.size());
  descriptions.reserve(result.size() + snapshot->commands.size());
  items.reserve(result.size() + snapshot->commands.size());
  // Built separately, then ordered for display below.
  std::vector<KrSuggestion> match_items;
  std::vector<size_t> match_sources;
  match_items.reserve(result.size());
  match_sources.reserve(result.size());
  contents_classes.reserve(result.size());
  description_classes.reserve(result.size());
  // A page that is already open is listed once, as its Switch to Tab row.
  // The default match stays: Enter keeps its usual meaning.
  std::vector<GURL> open_tabs;
  for (const auto& command : snapshot->commands) {
    if (command.kind == KR_SUGGESTION_OPEN_TAB && command.url.is_valid()) {
      open_tabs.push_back(command.url.GetWithoutRef());
    }
  }
  const auto classes = [](const ACMatchClassifications& source) {
    std::vector<KrSuggestionClassification> output;
    output.reserve(source.size());
    for (const auto& part : source) {
      output.push_back({sizeof(KrSuggestionClassification),
                        base::checked_cast<uint32_t>(part.offset),
                        static_cast<uint32_t>(part.style)});
    }
    return output;
  };
  for (size_t index = 0; !only_commands && index < result.size(); ++index) {
    const auto& match = result.match_at(index);
    if (controller_->IsSuggestionHidden(match) ||
        match.type == omnibox::AutocompleteMatchType::kNullResultMessage) {
      continue;
    }
    if (&match != result.default_match() &&
        std::ranges::find(open_tabs, match.destination_url.GetWithoutRef()) !=
            open_tabs.end()) {
      continue;
    }
    match_sources.push_back(index);
    const auto display =
        match.GetMatchWithContentsAndDescriptionPossiblySwapped();
    contents.push_back(base::UTF16ToUTF8(display.contents));
    descriptions.push_back(base::UTF16ToUTF8(display.description));
    contents_classes.push_back(classes(display.contents_class));
    description_classes.push_back(classes(display.description_class));
    uint32_t flags =
        &match == result.default_match() ? KR_SUGGESTION_DEFAULT : 0;
    if (match.deletable) {
      flags |= KR_SUGGESTION_DELETABLE;
    }
    if (controller_->edit_model()->GetPopupSelection().line == index) {
      flags |= KR_SUGGESTION_SELECTED;
    }
    KrImageId image = 0;
    const bool rich =
        match.type == omnibox::AutocompleteMatchType::kCalculator ||
        (!match.image_url.is_empty() && match.provider &&
         match.provider->type() !=
             AutocompleteProvider::Type::kUnscopedExtension);
    if (rich) {
      flags |= KR_SUGGESTION_RICH_IMAGE;
    }
    if (images_) {
      const auto* bitmap =
          controller_->edit_model()->GetPopupRichSuggestionBitmap(
              base::checked_cast<int>(index));
      const bool has_bitmap = rich && bitmap && !bitmap->drawsNothing();
      const auto icon =
          has_bitmap ? gfx::Image::CreateFrom1xBitmap(*bitmap)
                     : controller_->edit_model()->GetMatchIcon(
                           match,
                           host_->GetColorProvider()->GetColor(
                               kColorOmniboxResultsIcon),
                           host_->GetNativeTheme()->preferred_color_scheme() ==
                               ui::NativeTheme::PreferredColorScheme::kDark);
      if (!icon.IsEmpty()) {
        const auto acquired = images_.Run(ui::ImageModel::FromImage(icon));
        if (acquired.has_value() && *acquired) {
          image = *acquired;
          snapshot->image_leases.push_back(image);
        }
      }
    }
    match_items.push_back(
        {sizeof(KrSuggestion), SuggestionKind(match), flags,
         Borrow(contents.back()), Borrow(descriptions.back()), image,
         contents_classes.back().data(), contents_classes.back().size(),
         sizeof(KrSuggestionClassification), description_classes.back().data(),
         description_classes.back().size(),
         sizeof(KrSuggestionClassification)});
  }
  const auto add_match = [&](size_t index) {
    snapshot->source_indices.push_back(match_sources[index]);
    items.push_back(match_items[index]);
  };
  const auto add_command = [&](size_t index) {
    auto& command = snapshot->commands[index];
    snapshot->source_indices.push_back(std::numeric_limits<size_t>::max() -
                                       index);
    uint32_t flags = 0;
    if (only_commands && index == 0)
      flags |= KR_SUGGESTION_DEFAULT | KR_SUGGESTION_SELECTED;
    if (command.kind == KR_SUGGESTION_OPEN_TAB)
      flags |= KR_SUGGESTION_TAB_SWITCH;
    KrImageId image = 0;
    if (images_ && !command.icon.IsEmpty()) {
      const auto acquired = images_.Run(command.icon);
      if (acquired.has_value() && *acquired) {
        image = *acquired;
        snapshot->image_leases.push_back(image);
      }
    }
    items.push_back({sizeof(KrSuggestion), command.kind, flags,
                     Borrow(command.title), Borrow(command.description), image,
                     nullptr, 0, sizeof(KrSuggestionClassification), nullptr, 0,
                     sizeof(KrSuggestionClassification)});
  };
  if (only_commands) {
    // Command mode keeps the provider's ranking.
    for (size_t index = 0; index < snapshot->commands.size(); ++index)
      add_command(index);
  } else {
    // Open tabs follow the default match, ahead of the remaining matches and
    // any launcher commands.
    const auto is_tab = [&](size_t index) {
      return snapshot->commands[index].kind == KR_SUGGESTION_OPEN_TAB;
    };
    size_t next = 0;
    if (!match_items.empty())
      add_match(next++);
    for (size_t index = 0; index < snapshot->commands.size(); ++index)
      if (is_tab(index))
        add_command(index);
    while (next < match_items.size())
      add_match(next++);
    for (size_t index = 0; index < snapshot->commands.size(); ++index)
      if (!is_tab(index))
        add_command(index);
  }
  if (++generation_ == 0) {
    ++generation_;
  }
  snapshots_.emplace(generation_, std::move(snapshot));
  visible_result_count_ = items.size();
  controller_->popup_state_manager()->SetPopupState(
      visible_result_count_ ? OmniboxPopupState::kClassic
                            : OmniboxPopupState::kNone);
  ui->set_suggestion_snapshot(ui->self, request_, generation_, items.data(),
                              items.size(), sizeof(KrSuggestion));
  PublishSelection();
}

void NativeLocationBar::SelectSuggestion(uint32_t request,
                                         uint64_t generation,
                                         size_t index) {
  const auto found = snapshots_.find(generation);
  if (!controller_ || request != request_ || found == snapshots_.end() ||
      index >= found->second->source_indices.size()) {
    return;
  }
  const size_t source = found->second->source_indices[index];
  const size_t command = std::numeric_limits<size_t>::max() - source;
  if (command < found->second->commands.size()) {
    if (generation != generation_)
      return;
    selected_command_ = index;
    PublishSelection();
    return;
  }
  selected_command_.reset();
  if (generation == generation_) {
    controller_->edit_model()->SetPopupSelection(OmniboxPopupSelection(source),
                                                 source == 0);
  } else {
    const auto& match = *found->second->autocomplete.result.match_at(source);
    editor_->OnTemporaryTextMaybeChanged(match.fill_into_edit, match, true,
                                         false);
  }
}

void NativeLocationBar::PublishSelection() {
  const auto* api = session_->api();
  const auto found = snapshots_.find(generation_);
  if (!open_ || !api ||
      !KR_HAS_SLOT(api, KrShellUI, set_suggestion_selection) ||
      !api->set_suggestion_selection || found == snapshots_.end()) {
    return;
  }
  if (selected_command_ || CommandQuery()) {
    const size_t index = selected_command_.value_or(0);
    api->set_suggestion_selection(api->self, request_, generation_,
                                  index < found->second->source_indices.size()
                                      ? index
                                      : std::numeric_limits<size_t>::max());
    return;
  }
  const auto source = controller_->edit_model()->GetPopupSelection().line;
  if (source == OmniboxPopupSelection::kNoMatch) {
    api->set_suggestion_selection(api->self, request_, generation_,
                                  std::numeric_limits<size_t>::max());
    return;
  }
  const auto& indices = found->second->source_indices;
  const auto selected = std::ranges::find(indices, source);
  if (selected != indices.end()) {
    api->set_suggestion_selection(
        api->self, request_, generation_,
        static_cast<size_t>(selected - indices.begin()));
  }
}

bool NativeLocationBar::HandleKey(uint32_t key, uint32_t modifiers) {
  if (!open_ || !focused_ || !controller_ || editor_->composing()) {
    return false;
  }
  auto* model = controller_->edit_model();
  const auto snapshot = snapshots_.find(generation_);
  if (snapshot != snapshots_.end() && !snapshot->second->commands.empty()) {
    const auto& indices = snapshot->second->source_indices;
    if (key == KR_ADDRESS_KEY_UP || key == KR_ADDRESS_KEY_DOWN ||
        key == KR_ADDRESS_KEY_PAGE_UP || key == KR_ADDRESS_KEY_PAGE_DOWN ||
        (CommandQuery() && key == KR_ADDRESS_KEY_TAB)) {
      size_t index = selected_command_.value_or(0);
      if (!selected_command_ && !CommandQuery()) {
        const auto found =
            std::ranges::find(indices, model->GetPopupSelection().line);
        if (found != indices.end())
          index = static_cast<size_t>(found - indices.begin());
      }
      const bool previous =
          key == KR_ADDRESS_KEY_UP || key == KR_ADDRESS_KEY_PAGE_UP ||
          (key == KR_ADDRESS_KEY_TAB && (modifiers & KR_ADDRESS_KEY_SHIFT));
      if (!indices.empty())
        index = previous ? (index + indices.size() - 1) % indices.size()
                         : (index + 1) % indices.size();
      SelectSuggestion(request_, generation_, index);
      return true;
    }
    if (CommandQuery() || selected_command_)
      return false;
  }
  switch (key) {
    case KR_ADDRESS_KEY_UP:
    case KR_ADDRESS_KEY_DOWN:
    case KR_ADDRESS_KEY_PAGE_UP:
    case KR_ADDRESS_KEY_PAGE_DOWN:
      model->OnUpOrDownPressed(
          key == KR_ADDRESS_KEY_DOWN || key == KR_ADDRESS_KEY_PAGE_DOWN,
          key == KR_ADDRESS_KEY_PAGE_UP || key == KR_ADDRESS_KEY_PAGE_DOWN);
      return true;
    case KR_ADDRESS_KEY_TAB:
      if (!controller_->IsPopupOpen()) {
        return false;
      }
      model->OnTabPressed((modifiers & KR_ADDRESS_KEY_SHIFT) != 0);
      return true;
    case KR_ADDRESS_KEY_ESCAPE:
      return model->OnEscapeKeyPressed();
    case KR_ADDRESS_KEY_DELETE_MATCH: {
      const auto index = model->GetPopupSelection().line;
      const auto& result = controller_->autocomplete_controller()->result();
      if (index >= result.size() || !result.match_at(index).deletable) {
        return false;
      }
      model->TryDeletingPopupLine(index);
      return true;
    }
    case KR_ADDRESS_KEY_CONTROL:
      accept_control_ = (modifiers & KR_ADDRESS_KEY_CTRL) != 0;
      return false;
    default:
      return false;
  }
}

base::OnceClosure NativeLocationBar::CaptureSuggestion(
    uint32_t request,
    uint64_t generation,
    size_t index,
    WindowOpenDisposition disposition) {
  const auto found = snapshots_.find(generation);
  auto* tab = browser_->GetActiveTabInterface();
  if (!controller_ || !tab || request != request_ ||
      found == snapshots_.end() ||
      index >= found->second->source_indices.size()) {
    return {};
  }
  const auto source = found->second->source_indices[index];
  const size_t command = std::numeric_limits<size_t>::max() - source;
  if (command < found->second->commands.size()) {
    auto execute = std::move(found->second->commands[command].execute);
    snapshots_.erase(found);
    selected_command_.reset();
    return execute;
  }
  if (source == 0) {
    if (const auto bang =
            ExpandBangSearch(base::UTF16ToUTF8(editor_->GetText())))
      return CaptureNavigation(*bang, disposition);
  }
  const bool control = std::exchange(accept_control_, false);
  auto snapshot = std::move(found->second);
  // Navigation needs the captured match, not the popup's image leases.
  // Release before a deferred command can close the window and its cache.
  snapshot->ReleaseImages();
  snapshots_.erase(found);
  return base::BindOnce(
      [](base::WeakPtr<NativeLocationBar> bar,
         base::WeakPtr<tabs::TabInterface> tab,
         std::unique_ptr<Snapshot> snapshot, size_t source,
         WindowOpenDisposition disposition, base::TimeTicks when,
         bool control) {
        if (!bar || !tab ||
            bar->browser_->GetActiveTabInterface() != tab.get()) {
          return;
        }
        if (ShowNativeSurface(&*bar->browser_,
                              snapshot->autocomplete.result.match_at(source)
                                  ->destination_url)) {
          bar->Revert();
          return;
        }
        bar->controller_->edit_model()->OnControlKeyChanged(false);
        if (control) {
          bar->controller_->edit_model()->OnControlKeyChanged(true);
        }
        bar->controller_->edit_model()->OpenSelection(
            OmniboxPopupSelection(source), when, disposition, false,
            &snapshot->autocomplete);
        bar->controller_->edit_model()->OnControlKeyChanged(false);
      },
      weak_factory_.GetWeakPtr(), tab->GetWeakPtr(), std::move(snapshot),
      source, disposition, base::TimeTicks::Now(), control);
}

base::OnceClosure NativeLocationBar::CaptureNavigation(
    std::string_view text,
    WindowOpenDisposition disposition) {
  if (!controller_ || !browser_->GetActiveTabInterface()) {
    return {};
  }
  if (text.starts_with(">"))
    return {};
  const auto expanded = ExpandBangSearch(text);
  if (expanded)
    text = *expanded;
  AutocompleteMatch match;
  controller_->edit_model()->ClassifyString(base::UTF8ToUTF16(text), &match,
                                            nullptr);
  auto snapshot = std::make_unique<searchbox::AutocompleteSnapshot>();
  snapshot->input = AutocompleteInput(
      base::UTF8ToUTF16(text),
      controller_->edit_model()->GetPageClassification(),
      controller_->client()->GetSchemeClassifier(),
      controller_->client()->ShouldDefaultTypedNavigationsToHttps());
  snapshot->result.AppendMatches({match});
  const bool control = std::exchange(accept_control_, false);
  return base::BindOnce(
      [](base::WeakPtr<NativeLocationBar> bar,
         base::WeakPtr<tabs::TabInterface> tab,
         std::unique_ptr<searchbox::AutocompleteSnapshot> snapshot,
         WindowOpenDisposition disposition, base::TimeTicks when,
         bool control) {
        if (bar && tab && bar->browser_->GetActiveTabInterface() == tab.get()) {
          if (ShowNativeSurface(
                  &*bar->browser_,
                  snapshot->result.match_at(0)->destination_url)) {
            bar->Revert();
            return;
          }
          bar->controller_->edit_model()->OnControlKeyChanged(false);
          if (control) {
            bar->controller_->edit_model()->OnControlKeyChanged(true);
          }
          bar->controller_->edit_model()->OpenSelection(
              OmniboxPopupSelection(0), when, disposition, false,
              snapshot.get());
          bar->controller_->edit_model()->OnControlKeyChanged(false);
        }
      },
      weak_factory_.GetWeakPtr(),
      browser_->GetActiveTabInterface()->GetWeakPtr(), std::move(snapshot),
      disposition, base::TimeTicks::Now(), control);
}

void NativeLocationBar::FocusLocation(bool user_initiated,
                                      bool clear_focus_if_failed) {
  if (const auto* ui = session_->api(); ui && toolbar_visible_) {
    ui->focus_address(ui->self, user_initiated);
  } else if (clear_focus_if_failed && GetWebContents()) {
    GetWebContents()->Focus();
  }
}
void NativeLocationBar::FocusSearch() {
  FocusLocation(true, false);
  editor_->EnterKeywordModeForDefaultSearchProvider();
}
void NativeLocationBar::UpdateFocusBehavior(bool visible) {
  toolbar_visible_ = visible;
}
void NativeLocationBar::UpdateContentSettingsIcons() {
  changed_.Run();
}
void NativeLocationBar::SaveStateToContents(content::WebContents* contents) {
  if (controller_ && contents) {
    contents->SetUserData(
        kSavedStateKey,
        std::make_unique<SavedState>(*controller_->edit_model(), *editor_));
  }
}
void NativeLocationBar::Revert() {
  if (editor_) {
    editor_->RevertAll();
  }
}
OmniboxView* NativeLocationBar::GetOmniboxView() {
  return editor_.get();
}
OmniboxPopupView* NativeLocationBar::GetOmniboxPopupView() {
  return popup_.get();
}
OmniboxController* NativeLocationBar::GetOmniboxController() {
  return controller_.get();
}
bool NativeLocationBar::ShouldCloseOmniboxPopup(ui::MouseEvent* event) {
  if (event->type() != ui::EventType::kMousePressed) {
    return false;
  }
  auto* target = static_cast<views::View*>(event->target());
  if (!target) {
    return true;
  }
  auto point = event->location();
  views::View::ConvertPointToScreen(target, &point);
  return !BoundsInScreen().Contains(point);
}
content::WebContents* NativeLocationBar::GetWebContents() {
  return browser_->GetTabStripModel()->GetActiveWebContents();
}
LocationBarModel* NativeLocationBar::GetLocationBarModel() {
  return &*location_model_;
}
std::optional<bubble_anchor_util::AnchorConfiguration>
NativeLocationBar::GetChipAnchor() {
  return std::nullopt;
}
ChipController* NativeLocationBar::GetChipController() {
  return nullptr;
}
void NativeLocationBar::AnnounceAlert(const std::u16string& text) {
  host_->GetViewAccessibility().AnnounceText(text);
}
void NativeLocationBar::OnChanged() {
  changed_.Run();
}
void NativeLocationBar::UpdateWithoutTabRestore() {
  if (editor_) {
    editor_->Update();
  }
  changed_.Run();
}
ui::TrackedElement* NativeLocationBar::GetAnchorOrNull() {
  auto anchor = anchors_->GetAnchor({AnchorKind::kElement, KR_ELEMENT_ADDRESS});
  if (auto* element = anchor.GetIfElement()) {
    return element;
  }
  if (auto* view = anchor.GetIfView()) {
    return views::ElementTrackerViews::GetInstance()->GetElementForView(view);
  }
  return nullptr;
}
BrowserWindowInterface* NativeLocationBar::GetBrowser() {
  return &*browser_;
}
Profile* NativeLocationBar::GetProfile() {
  return browser_->GetProfile();
}
bool NativeLocationBar::IsInitialized() const {
  return controller_ && session_->api();
}
bool NativeLocationBar::IsVisible() const {
  return toolbar_visible_ && !BoundsInScreen().IsEmpty();
}
bool NativeLocationBar::IsDrawn() const {
  return IsVisible() && host_->IsDrawn();
}
bool NativeLocationBar::IsFullscreen() const {
  // Like LocationBarView, ask the hosting widget: Browser has no window yet
  // while its window is being constructed.
  const auto* widget = host_->GetWidget();
  return widget && widget->IsFullscreen();
}
bool NativeLocationBar::IsEditingOrEmpty() const {
  return editor_ && editor_->IsEditingOrEmpty();
}
bool NativeLocationBar::IsMouseHovered() const {
  return BoundsInScreen().Contains(
      display::Screen::Get()->GetCursorScreenPoint());
}
bool NativeLocationBar::IsFocusWithin() const {
  return focused_;
}
void NativeLocationBar::InvalidateLayout() {
  changed_.Run();
}
gfx::Rect NativeLocationBar::Bounds() const {
  auto bounds = BoundsInScreen();
  return views::View::ConvertRectFromScreen(&*host_, bounds);
}
gfx::Rect NativeLocationBar::BoundsInScreen() const {
  const auto anchor =
      anchors_->GetAnchor({AnchorKind::kElement, KR_ELEMENT_ADDRESS});
  // Compact chrome can withdraw the anchor before a mouse press reaches Views.
  return anchor ? anchor.GetAnchorRect() : gfx::Rect();
}
gfx::Size NativeLocationBar::MinimumSize() const {
  return {};
}
gfx::Size NativeLocationBar::PreferredSize() const {
  return BoundsInScreen().size();
}
void NativeLocationBar::Update(content::WebContents* contents) {
  if (!controller_) {
    return;
  }
  if (contents) {
    auto* saved =
        static_cast<SavedState*>(contents->GetUserData(kSavedStateKey));
    controller_->edit_model()->RestoreState(saved ? &saved->model : nullptr);
    if (saved) {
      editor_->SetWindowTextAndCaretPos(saved->text, saved->selection.end(),
                                        false, false);
      editor_->SetSelectionBounds(saved->selection);
    } else {
      editor_->RevertAll();
    }
  } else {
    editor_->Update();
  }
  changed_.Run();
}
void NativeLocationBar::ResetTabState(content::WebContents* contents) {
  contents->RemoveUserData(kSavedStateKey);
}
bool NativeLocationBar::HasSecurityStateChanged() {
  const auto level = location_model_->GetSecurityLevel();
  return std::exchange(security_level_, level) != level;
}
LocationBarTesting* NativeLocationBar::GetLocationBarForTesting() {
  return nullptr;
}
void NativeLocationBar::OnAnchorChanged() {
  NotifyBoundsChanged();
}
}  // namespace krypton_ui

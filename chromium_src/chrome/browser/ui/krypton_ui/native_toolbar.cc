// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/native_toolbar.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/browser/command_updater.h"
#include "chrome/browser/ui/browser_actions.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/control_anchors.h"
#include "chrome/browser/ui/krypton_ui/menu_glyphs.h"
#include "chrome/browser/ui/krypton_ui/menu_model_snapshot.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/toolbar/app_menu_model.h"
#include "chrome/browser/ui/views/extensions/extensions_container_views.h"
#include "chrome/browser/ui/views/page_action/page_action_view_interface.h"
#include "chrome/browser/ui/webui/webui_toolbar/utils/toolbar_button_utils.h"
#include "chrome/grit/generated_resources.h"
#include "ui/actions/actions.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/interaction/element_identifier.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/window_open_disposition_utils.h"
#include "ui/events/event_utils.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/view_class_properties.h"

namespace krypton_ui {
namespace {
DEFINE_LOCAL_ELEMENT_IDENTIFIER_VALUE(kNativeToolbarPaneId);
AnchorKey Element(uint32_t element) {
  return {AnchorKind::kElement, element};
}
KrRect ToAbi(const gfx::RectF& value) {
  return {value.x(), value.y(), value.width(), value.height()};
}
}  // namespace

class NativeToolbar::PageAction final
    : public page_actions::PageActionViewInterface {
 public:
  PageAction(NativeToolbar& owner, actions::ActionId id)
      : owner_(owner), id_(id) {}
  views::BubbleAnchor GetBubbleAnchor() override {
    return owner_->GetBubbleAnchor(id_);
  }
  std::u16string GetTooltipText() const override {
    auto* item = owner_->GetActionItemFor(id_);
    return item ? std::u16string(item->GetTooltipText()) : std::u16string();
  }
  std::u16string GetAccessibleName() const override {
    auto* item = owner_->GetActionItemFor(id_);
    return item ? std::u16string(item->GetText()) : std::u16string();
  }
  void SetVisible(bool visible) override {
    owner_->ShowActionEphemerallyInToolbar(id_, visible);
  }

 private:
  const raw_ref<NativeToolbar> owner_;
  const actions::ActionId id_;
};

struct NativeToolbar::Menu {
  std::unique_ptr<AppMenuModel> model;
  std::vector<base::WeakPtr<ui::MenuModel>> opened;
  MenuModelSnapshot snapshot;

  Menu(std::unique_ptr<AppMenuModel> source,
       MenuModelSnapshot::PublishImage images,
       base::RepeatingClosure invalidated,
       base::RepeatingCallback<void(KrImageId)> release_image)
      : model(std::move(source)),
        snapshot(std::move(images),
                 std::move(invalidated),
                 std::move(release_image)) {}
  ~Menu() {
    snapshot.Clear();
    for (auto it = opened.rbegin(); it != opened.rend(); ++it) {
      if (*it) {
        (*it)->MenuWillClose();
      }
    }
  }
  void Open(ui::MenuModel& source) {
    const auto weak = source.AsWeakPtr();
    if (std::ranges::none_of(
            opened, [&](const auto& item) { return item.get() == &source; })) {
      source.MenuWillShow();
      if (!weak) {
        return;
      }
      opened.push_back(weak);
    }
    for (size_t index = 0; weak && index < source.GetItemCount(); ++index) {
      const auto type = source.GetTypeAt(index);
      if (type == ui::MenuModel::TYPE_SUBMENU ||
          type == ui::MenuModel::TYPE_ACTIONABLE_SUBMENU) {
        if (auto* child = source.GetSubmenuModelAt(index)) {
          Open(*child);
        }
      }
    }
  }
};

NativeToolbar::NativeToolbar(
    BrowserWindowInterface& browser,
    CommandUpdater& commands,
    ui::AcceleratorProvider& accelerators,
    WindowSession& session,
    ControlAnchors& anchors,
    MenuModelSnapshot::PublishImage images,
    base::RepeatingCallback<void(KrImageId)> release_image,
    base::RepeatingClosure changed)
    : browser_(browser),
      commands_(commands),
      accelerators_(accelerators),
      session_(session),
      anchors_(anchors),
      pinned_model_(*PinnedToolbarActionsModel::Get(browser.GetProfile())),
      changed_(std::move(changed)),
      images_(std::move(images)),
      release_image_(std::move(release_image)),
      registration_(browser.GetUnownedUserDataHost(), *this) {
  SetProperty(views::kElementIdentifierKey, kNativeToolbarPaneId);
  SetFocusBehavior(FocusBehavior::ALWAYS);
  SetCanProcessEventsWithinSubtree(false);
  GetViewAccessibility().SetRole(ax::mojom::Role::kToolbar);
}

NativeToolbar::~NativeToolbar() {
  weak_factory_.InvalidateWeakPtrs();
  observation_.Reset();
  menu_.reset();
}

void NativeToolbar::Initialize() {
  observation_.Observe(&*pinned_model_);
  pinned_model_->MaybeMigrateExistingPinnedStates();
  anchors_->Track(Element(KR_ELEMENT_APP_MENU), kToolbarAppMenuButtonElementId);
  anchors_->Track(Element(KR_ELEMENT_ADDRESS), kLocationBarElementId);
  anchors_->Track(Element(KR_ELEMENT_BACK), kToolbarBackButtonElementId);
  anchors_->Track(Element(KR_ELEMENT_FORWARD), kToolbarForwardButtonElementId);
}

KrActionId NativeToolbar::ActionIdentity(actions::ActionId id) {
  return (uint64_t{1} << 56) | static_cast<uint32_t>(id);
}

base::expected<void, std::string> NativeToolbar::PresentAppMenu(
    uint32_t request) {
  const auto* api = session_->api();
  if (!api) {
    return base::unexpected("The native shell is closed.");
  }
  CloseMenu();
  auto model = std::make_unique<AppMenuModel>(&*accelerators_, &*browser_);
  model->Init();
  menu_ = std::make_unique<Menu>(
      std::move(model), images_,
      base::BindRepeating(&NativeToolbar::InvalidateMenu,
                          weak_factory_.GetWeakPtr(), request),
      release_image_);
  menu_->Open(*menu_->model);
  auto captured = menu_->snapshot.Capture(*menu_->model, GetColorProvider(),
                                          &BrowserCommandGlyph);
  if (!captured.has_value()) {
    menu_.reset();
    return captured;
  }
  menu_request_ = request;
  auto published = menu_->snapshot.Publish(*api, request);
  if (!published.has_value()) {
    menu_.reset();
    menu_request_ = 0;
    return published;
  }
  menu_open_ = true;
  for (auto& observer : menu_observers_) {
    observer.AppMenuShown();
  }
  return {};
}

base::OnceClosure NativeToolbar::CaptureMenuCommand(uint32_t request,
                                                    int32_t command) {
  if (!menu_ || request != menu_request_) {
    return {};
  }
  auto execute = menu_->snapshot.CaptureCommand(request, command);
  if (!execute) {
    return {};
  }
  return base::BindOnce(
      [](std::unique_ptr<Menu> menu, base::OnceClosure execute) {
        std::move(execute).Run();
      },
      std::move(menu_), std::move(execute));
}

void NativeToolbar::NotifyMenuClosed() {
  if (!std::exchange(menu_open_, false)) {
    return;
  }
  for (auto& observer : menu_observers_) {
    observer.AppMenuClosed();
  }
}

void NativeToolbar::OnNativeMenuClosed(uint32_t request) {
  if (request != menu_request_) {
    return;
  }
  menu_request_ = 0;
  menu_.reset();
  NotifyMenuClosed();
}

void NativeToolbar::CloseMenu() {
  const auto request = std::exchange(menu_request_, 0);
  if (request) {
    if (const auto* api = session_->api()) {
      api->dismiss_menu(api->self, request);
    }
  }
  menu_.reset();
  NotifyMenuClosed();
}

void NativeToolbar::InvalidateMenu(uint32_t request) {
  // Model notifications may be on the source's stack. Defer the new capture
  // and retain the request identity so an old notification cannot touch a new
  // menu.
  std::ignore = session_->Defer(base::BindOnce(
      [](base::WeakPtr<NativeToolbar> owner, uint32_t request) {
        if (owner && owner->menu_request_ == request) {
          owner->RepublishMenu();
        }
      },
      weak_factory_.GetWeakPtr(), request));
}

void NativeToolbar::RepublishMenu() {
  // Submenus rebuild after the app menu opens, as when recently closed tabs
  // finish loading. Follow them, as the Views app menu does, rather than close.
  const auto* api = session_->api();
  if (!api || !menu_) {
    CloseMenu();
    return;
  }
  menu_->Open(*menu_->model);
  auto published = menu_->snapshot.Capture(*menu_->model, GetColorProvider(),
                                           &BrowserCommandGlyph);
  if (published.has_value()) {
    published = menu_->snapshot.Publish(*api, menu_request_);
  }
  if (!published.has_value()) {
    LOG(ERROR) << published.error();
    CloseMenu();
  }
}

void NativeToolbar::ShowMenu() {
  if (const auto* api = session_->api()) {
    const auto bounds = anchors_->GetBounds(Element(KR_ELEMENT_APP_MENU));
    api->request_context_menu(api->self, KR_MENU_APP, 0,
                              bounds ? ToAbi(*bounds) : KrRect{});
  }
}

void NativeToolbar::ShowMenuWithFlags(int) {
  // MenuRunner flags apply to Views menus; WinUI handles native popup input.
  ShowMenu();
}

views::MenuItemView* NativeToolbar::GetRootMenuItemViewForTesting() {
  // The app menu is a WinUI popup, not a Views menu.
  return nullptr;
}

void NativeToolbar::ExecuteCommandForTesting(int command_id,
                                             int mouse_event_flags) {
  ExecuteCommand(command_id, mouse_event_flags);
}

void NativeToolbar::SetMenuTimerForTesting(base::ElapsedTimer) {}

void NativeToolbar::RequestNativeFocus(uint32_t element) {
  if (const auto* api = session_->api()) {
    const KrNativeFocus request{sizeof(request), KR_ANCHOR_ELEMENT, element};
    api->focus_native(api->self, &request);
  }
}

void NativeToolbar::OnFocus() {
  RequestNativeFocus(KR_ELEMENT_TOOLBAR);
}
void NativeToolbar::RequestFocus() {
  // The actual controls live in WinUI, so this Views proxy has no children.
  // AccessiblePaneView's focus restoration searches those empty children.
  // Focus the proxy itself; OnFocus forwards to the native toolbar.
  views::View::RequestFocus();
}
bool NativeToolbar::SetPaneFocusAndFocusDefault() {
  FocusToolbar();
  return HasFocus();
}
void NativeToolbar::FocusToolbar() {
  RequestFocus();
  RequestNativeFocus(KR_ELEMENT_TOOLBAR);
}
void NativeToolbar::Focus(views::AccessiblePaneView*) {
  RequestFocus();
  RequestNativeFocus(KR_ELEMENT_APP_MENU);
}
bool NativeToolbar::HasFocus() const {
  return views::View::HasFocus();
}
bool NativeToolbar::IsDrawn() const {
  return static_cast<bool>(anchors_->GetAnchor(Element(KR_ELEMENT_APP_MENU)));
}
bool NativeToolbar::IsMenuShowing() const {
  return menu_open_;
}
views::DialogDelegate* NativeToolbar::GetDialogDelegate() {
  return nullptr;
}
views::View* NativeToolbar::GetFocusablePaneView() {
  return this;
}
void NativeToolbar::AddObserver(AppMenuButtonObserver* observer) {
  menu_observers_.AddObserver(observer);
}
void NativeToolbar::RemoveObserver(AppMenuButtonObserver* observer) {
  menu_observers_.RemoveObserver(observer);
}
void NativeToolbar::SetTypeAndSeverity(
    AppMenuIconController::TypeAndSeverity state) {
  app_menu_state_ = state;
  changed_.Run();
}
void NativeToolbar::SetIsMaximizedOrFullscreen(bool maximized) {}
views::BubbleAnchor NativeToolbar::GetAnchor() {
  auto anchor = anchors_->GetAnchor(Element(KR_ELEMENT_APP_MENU));
  return anchor ? anchor : views::BubbleAnchor(this);
}
ExtensionsContainerViews* NativeToolbar::GetExtensionsContainerViews() {
  return extensions_container_;
}
PinnedToolbarActions* NativeToolbar::GetPinnedToolbarActions() {
  return this;
}
gfx::Size NativeToolbar::GetToolbarButtonSize() const {
  const auto bounds = anchors_->GetBounds(Element(KR_ELEMENT_RELOAD));
  return bounds ? gfx::ToEnclosingRect(*bounds).size() : gfx::Size();
}
views::BubbleAnchor NativeToolbar::GetDefaultExtensionDialogAnchor() {
  return extensions_container_
             ? extensions_container_->GetExtensionsButtonAnchor()
             : GetAnchor();
}
page_actions::PageActionViewInterface*
NativeToolbar::GetPageActionViewInterface(actions::ActionId id) {
  if (!GetActionItemFor(id)) {
    return nullptr;
  }
  auto& view = page_actions_[id];
  if (!view) {
    view = std::make_unique<PageAction>(*this, id);
  }
  return view.get();
}
AppMenuControl* NativeToolbar::GetAppMenuControl() {
  return this;
}
gfx::Rect NativeToolbar::GetFindBarBoundingBox(int contents_bottom) {
  const auto content = session_->geometry().content();
  auto bounds = gfx::ToEnclosedRect(
      gfx::RectF(content.x, content.y, content.width, content.height));
  bounds.set_height(std::max(0, contents_bottom - bounds.y()));
  return bounds;
}
views::AccessiblePaneView* NativeToolbar::GetAsAccessiblePaneView() {
  return this;
}
views::BubbleAnchor NativeToolbar::GetBubbleAnchor(
    std::optional<actions::ActionId> id) {
  return id ? GetBubbleAnchor(*id) : GetAnchor();
}
views::BubbleAnchor NativeToolbar::GetPageActionBubbleAnchor(
    actions::ActionId id) {
  return GetBubbleAnchor(id);
}
void NativeToolbar::ZoomChangedForActiveTab(bool can_show_bubble) {
  changed_.Run();
}
AvatarToolbarButtonInterface* NativeToolbar::GetAvatarToolbarButtonInterface() {
  return nullptr;
}
ToolbarButton* NativeToolbar::GetBackButton() {
  return nullptr;
}
ReloadControl* NativeToolbar::GetReloadButton() {
  return this;
}
ToolbarButton* NativeToolbarButtonProvider::GetDownloadButton() {
  return nullptr;
}
DownloadButton* NativePinnedToolbarActions::GetDownloadButton() {
  return nullptr;
}
WebUIToolbarWebView* NativeToolbar::GetWebUIToolbarViewForTesting() {
  return nullptr;
}
void NativeToolbar::ChangeMode(Mode mode, bool force) {
  if (force || mode_ != mode) {
    mode_ = mode;
    changed_.Run();
  }
}
bool NativeToolbar::GetDevToolsStatusForTesting() const {
  return dev_tools_;
}
void NativeToolbar::SetDevToolsStatus(bool connected) {
  dev_tools_ = connected;
}
bool NativeToolbar::IsCommandIdChecked(int command) const {
  return false;
}
bool NativeToolbar::IsCommandIdEnabled(int command) const {
  return commands_->IsCommandEnabled(command);
}
bool NativeToolbar::IsCommandIdVisible(int command) const {
  return true;
}
bool NativeToolbar::GetAcceleratorForCommandId(
    int command,
    ui::Accelerator* accelerator) const {
  return accelerators_->GetAcceleratorForCommandId(command, accelerator);
}
void NativeToolbar::ExecuteCommand(int command, int event_flags) {
  const auto weak = weak_factory_.GetWeakPtr();
  const bool queued = session_->Defer(base::BindOnce(
      [](base::WeakPtr<NativeToolbar> view, int command, int flags) {
        if (view && view->commands_->IsCommandEnabled(command)) {
          chrome::BrowserCommandController::From(&*view->browser_)
              ->ExecuteCommandWithDisposition(
                  command, ui::DispositionFromEventFlags(flags));
        }
      },
      weak, command, event_flags));
  if (!queued) {
    CloseMenu();
  }
}
actions::ActionItem* NativeToolbar::GetActionItemFor(actions::ActionId id) {
  return actions::ActionManager::Get().FindAction(
      id, BrowserActions::From(&*browser_)->root_action_item());
}
const std::vector<actions::ActionId>& NativeToolbar::PinnedActionIds() const {
  return pinned_model_->PinnedActionIds();
}
bool NativeToolbar::IsActionPinned(actions::ActionId id) {
  return std::ranges::contains(PinnedActionIds(), id);
}
bool NativeToolbar::IsActionPoppedOut(actions::ActionId id) {
  return std::ranges::contains(popped_out_, id);
}
bool NativeToolbar::IsActionPinnedOrPoppedOut(actions::ActionId id) {
  return IsActionPinned(id) || IsActionPoppedOut(id);
}
bool NativeToolbar::IsActionHighlighted(actions::ActionId id) {
  return std::ranges::contains(active_, id);
}
bool NativeToolbar::IsOverflowed(actions::ActionId id) {
  return !anchors_->GetBounds({AnchorKind::kAction, ActionIdentity(id)});
}
views::View* NativeToolbar::GetContainerView() {
  return this;
}
bool NativeToolbar::ShouldAnyButtonsOverflow(gfx::Size available) const {
  return false;
}
void NativeToolbar::UpdateActionState(actions::ActionId id, bool active) {
  const auto found = std::ranges::find(active_, id);
  if (active && found == active_.end()) {
    active_.push_back(id);
  } else if (!active && found != active_.end()) {
    active_.erase(found);
  }
  ShowActionEphemerallyInToolbar(id, active);
}
void NativeToolbar::ShowActionEphemerallyInToolbar(actions::ActionId id,
                                                   bool show) {
  const auto found = std::ranges::find(popped_out_, id);
  if (show && found == popped_out_.end()) {
    popped_out_.push_back(id);
    changed_.Run();
  } else if (!show && found != popped_out_.end()) {
    popped_out_.erase(found);
    changed_.Run();
  }
}
void NativeToolbar::PostOrQueueActionAfterAnimation(base::OnceClosure action) {
  const bool queued = session_->Defer(std::move(action));
  (void)queued;
}
views::BubbleAnchor NativeToolbar::GetBubbleAnchor(actions::ActionId id) {
  const AnchorKey key{AnchorKind::kAction, ActionIdentity(id)};
  if (auto identifier = webui_toolbar::ActionIdToElementIdentifier(id)) {
    anchors_->Track(key, identifier);
  }
  return anchors_->GetAnchor(key);
}
void NativeToolbar::GetBubbleAnchorAsync(
    actions::ActionId id,
    base::OnceCallback<void(BubbleAnchorResult)> callback) {
  if (auto anchor = GetBubbleAnchor(id)) {
    std::move(callback).Run(anchor);
    return;
  }
  if (auto* item = GetActionItemFor(id); !item || !item->GetVisible()) {
    std::move(callback).Run(
        base::unexpected(GetAnchorFailureReason::kAnchorNotFound));
    return;
  }
  pending_anchors_.push_back({id, std::move(callback)});
  ShowActionEphemerallyInToolbar(id, true);
}
void NativeToolbar::OnAnchorsChanged() {
  auto waiting = std::move(pending_anchors_);
  pending_anchors_.clear();
  auto weak = weak_factory_.GetWeakPtr();
  for (auto& request : waiting) {
    auto anchor = GetBubbleAnchor(request.id);
    if (anchor) {
      std::move(request.callback).Run(anchor);
    } else {
      pending_anchors_.push_back(std::move(request));
    }
    if (!weak) {
      return;
    }
  }
}
PinnedActionToolbarButton* NativeToolbar::GetChromeLabsButton() {
  return nullptr;
}
void NativeToolbar::UpdatePinnedStateAndAnnounce(actions::ActionId id,
                                                 bool pin) {
  auto* item = GetActionItemFor(id);
  if (!item || !item->GetProperty(actions::kActionItemPinnableKey) ||
      pin == IsActionPinned(id)) {
    return;
  }
  pinned_model_->UpdatePinnedState(id, pin);
  GetViewAccessibility().AnnounceAlert(l10n_util::GetStringUTF16(
      pin ? IDS_TOOLBAR_BUTTON_PINNED : IDS_TOOLBAR_BUTTON_UNPINNED));
}
void NativeToolbar::MovePinnedAction(actions::ActionId id, int index) {
  pinned_model_->MovePinnedAction(id, index);
}
void NativeToolbar::MovePinnedActionBy(actions::ActionId id, int delta) {
  const auto& ids = PinnedActionIds();
  const auto found = std::ranges::find(ids, id);
  if (found != ids.end()) {
    MovePinnedAction(id, static_cast<int>(found - ids.begin()) + delta);
  }
}
void NativeToolbar::OnActionsChanged() {
  changed_.Run();
}
BEGIN_METADATA(NativeToolbar)
END_METADATA
}  // namespace krypton_ui

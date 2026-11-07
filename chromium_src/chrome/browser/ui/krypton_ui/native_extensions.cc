// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/native_extensions.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/accelerator_utils.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/krypton_ui/action_menu_model.h"
#include "chrome/browser/ui/krypton_ui/control_anchors.h"
#include "chrome/browser/ui/krypton_ui/site_data_menu_model.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/toolbar/app_menu_model.h"
#include "chrome/browser/ui/views/extensions/extension_action_delegate_desktop.h"
#include "chrome/grit/generated_resources.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/extension_action.h"
#include "ui/base/interaction/element_identifier.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/color/color_provider.h"
#include "ui/gfx/color_utils.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace krypton_ui {
namespace {
DEFINE_LOCAL_ELEMENT_IDENTIFIER_VALUE(kNativeExtensionButton);
DEFINE_LOCAL_ELEMENT_IDENTIFIER_VALUE(kNativeExtensionsMenuButton);
constexpr AnchorKey kMenuAnchor{AnchorKind::kAction,
                                NativeExtensions::kMenuAction};
KrStr Borrow(std::string_view text) {
  return {text.data(), text.size()};
}

class ExtensionsMenu final : public ui::SimpleMenuModel {
 public:
  ExtensionsMenu() : ui::SimpleMenuModel(nullptr) {}
  std::vector<std::unique_ptr<ui::MenuModel>> actions;
  std::unique_ptr<AppMenuModel> app;
};
}  // namespace

struct NativeExtensions::Action {
  KrActionId key = 0;
  std::string extension_id;
  raw_ptr<ExtensionActionViewModel> source = nullptr;
  base::CallbackListSubscription icon_changed;
  std::string label;
  std::string tooltip;
  KrImageId icon = 0;
  // KryptonUI draws the badge over an unbadged icon.
  std::string badge;
  SkColor badge_color = SK_ColorTRANSPARENT;
  SkColor badge_text_color = SK_ColorTRANSPARENT;
  bool dirty = true;
  bool registered = false;
};

NativeExtensions::NativeExtensions(
    BrowserWindowInterface& browser,
    views::View& host,
    WindowSession& session,
    ControlAnchors& anchors,
    MenuModelSnapshot::PublishImage images,
    base::RepeatingCallback<void(KrImageId)> release,
    base::RepeatingClosure changed,
    base::RepeatingClosure close_menus)
    : browser_(browser),
      host_(host),
      session_(session),
      anchors_(anchors),
      images_(std::move(images)),
      release_(std::move(release)),
      changed_(std::move(changed)),
      close_menus_(std::move(close_menus)) {
  menu_label_ = base::UTF16ToUTF8(
      l10n_util::GetStringUTF16(IDS_TOOLTIP_EXTENSIONS_BUTTON));
  model_ = std::make_unique<ExtensionsToolbarViewModel>(
      this, &*browser_, ToolbarActionsModel::Get(browser.GetProfile()));
  registration_.emplace(browser.GetUnownedUserDataHost(), *model_);
  model_->AddObserver(this);
  OnActionsInitialized();
}
NativeExtensions::~NativeExtensions() {
  Close();
}
void NativeExtensions::Attach() {
  if (attached_ || closed_)
    return;
  attached_ = true;
  anchors_->Track(kMenuAnchor, kNativeExtensionsMenuButton);
  for (auto& [id, action] : actions_) {
    anchors_->Track({AnchorKind::kAction, action->key}, kNativeExtensionButton);
    action->source->RegisterCommand();
    action->registered = true;
  }
  changed_.Run();
}
void NativeExtensions::Close() {
  if (std::exchange(closed_, true))
    return;
  weak_factory_.InvalidateWeakPtrs();
  HideActivePopup();
  model_->RemoveObserver(this);
  registration_.reset();
  for (auto& [id, action] : actions_) {
    if (action->registered)
      action->source->UnregisterCommand();
    release_.Run(action->icon);
    anchors_->Untrack({AnchorKind::kAction, action->key});
  }
  actions_.clear();
  anchors_->Untrack(kMenuAnchor);
  model_.reset();
}
std::unique_ptr<ExtensionActionViewModel>
NativeExtensions::CreateActionViewModel(const ToolbarActionsModel::ActionId& id,
                                        ExtensionsContainer* container) {
  return ExtensionActionViewModel::Create(
      id, &*browser_,
      std::make_unique<ExtensionActionDelegateDesktop>(&*browser_, container,
                                                       this));
}
void NativeExtensions::EnsureAction(const std::string& id) {
  if (actions_.contains(id))
    return;
  auto* source = model_->GetActionForId(id);
  if (!source)
    return;
  auto action = std::make_unique<Action>();
  CHECK_LT(next_id_, (uint64_t{1} << 56) - 1);
  action->key = (uint64_t{4} << 56) | ++next_id_;
  action->extension_id = id;
  // CreateActionViewModel() creates every action the model holds.
  action->source = static_cast<ExtensionActionViewModel*>(source);
  action->icon_changed = source->RegisterIconUpdateObserver(base::BindRepeating(
      &NativeExtensions::OnActionUpdated, weak_factory_.GetWeakPtr(), id));
  if (attached_) {
    anchors_->Track({AnchorKind::kAction, action->key}, kNativeExtensionButton);
    source->RegisterCommand();
    action->registered = true;
  }
  actions_.emplace(id, std::move(action));
}
void NativeExtensions::AppendAction(const std::string& id,
                                    std::vector<KrAction>& output) {
  const auto found = actions_.find(id);
  if (found == actions_.end())
    return;
  auto& action = *found->second;
  auto* contents = browser_->GetTabStripModel()->GetActiveWebContents();
  if (action.dirty) {
    action.label = base::UTF16ToUTF8(action.source->GetActionName());
    action.tooltip = base::UTF16ToUTF8(action.source->GetTooltip(contents));
    const auto icon = action.source->GetIconWithoutBadge(
        contents, gfx::Size(kDefaultIconSizeChromeRefresh,
                            kDefaultIconSizeChromeRefresh));
    // An empty icon means the extension is unloading; its action may be gone.
    if (icon.IsEmpty())
      action.badge.clear();
    else
      UpdateBadge(action, contents);
    const auto image = images_.Run(icon);
    if (image.has_value()) {
      release_.Run(action.icon);
      action.icon = *image;
      action.dirty = false;
    }
  }
  uint32_t flags = KR_ACTION_VISIBLE;
  if (contents && action.source->IsEnabled(contents))
    flags |= KR_ACTION_ENABLED;
  if (action.source->IsShowingPopup())
    flags |= KR_ACTION_ACTIVE;
  output.push_back({sizeof(KrAction),
                    action.key,
                    KR_ACTION_EXTENSION,
                    flags,
                    Borrow(action.label),
                    Borrow(action.tooltip),
                    action.icon,
                    {},
                    Borrow(action.badge),
                    action.badge_color,
                    -1,
                    action.badge_text_color});
}
void NativeExtensions::UpdateBadge(Action& action,
                                   content::WebContents* contents) {
  // Resolve colours as IconWithBadgeImageSource does: an unset background
  // uses the theme default, any other is made opaque, and an unset text
  // colour contrasts with the background.
  const int tab = sessions::SessionTabHelper::IdForTab(contents).id();
  const auto& source = *action.source->extension_action();
  action.badge = source.GetDisplayBadgeText(tab);
  const ui::ColorProvider* colors =
      contents ? &contents->GetColorProvider() : host_->GetColorProvider();
  SkColor background = source.GetBadgeBackgroundColor(tab);
  if (SkColorGetA(background) != SK_AlphaTRANSPARENT)
    background = SkColorSetA(background, SK_AlphaOPAQUE);
  else if (colors)
    background = colors->GetColor(kColorExtensionIconBadgeBackgroundDefault);
  const SkColor text = source.GetBadgeTextColor(tab);
  action.badge_color = background;
  action.badge_text_color =
      SkColorGetA(text) == SK_AlphaTRANSPARENT
          ? color_utils::GetColorWithMaxContrast(background)
          : text;
}
void NativeExtensions::AppendActions(std::vector<KrAction>& output) {
  if (closed_ || !attached_)
    return;
  for (const auto& id : model_->GetPinnedActionIds())
    AppendAction(id, output);
  if (popped_ && !std::ranges::contains(model_->GetPinnedActionIds(), *popped_))
    AppendAction(*popped_, output);
  // Site tools already contain the extensions list and management commands.
  // Retain the aggregate action for host commands without a second URL button.
  auto menu_action = ExtensionMenuAction(Borrow(menu_label_));
  menu_action.flags = KR_ACTION_ENABLED;
  output.push_back(menu_action);
}
void NativeExtensions::RefreshImages() {
  for (auto& [id, action] : actions_)
    action->dirty = true;
  changed_.Run();
}
NativeExtensions::Action* NativeExtensions::Find(KrActionId key) const {
  for (const auto& [id, action] : actions_) {
    if (action->key == key)
      return action.get();
  }
  return nullptr;
}
bool NativeExtensions::CanInvoke(KrActionId key) const {
  auto* action = Find(key);
  auto* contents = browser_->GetTabStripModel()->GetActiveWebContents();
  return !closed_ && action && contents && action->source->IsEnabled(contents);
}
void NativeExtensions::Invoke(KrActionId key) {
  if (key == kMenuAction) {
    ToggleExtensionsMenu();
  } else if (CanInvoke(key)) {
    auto* action = Find(key);
    model_->ExecuteUserAction(
        action->extension_id,
        ToolbarActionViewModel::InvocationSource::kToolbarButton);
  }
}
void NativeExtensions::InvokeFromMenu(KrActionId key) {
  if (CanInvoke(key)) {
    model_->ExecuteUserAction(
        Find(key)->extension_id,
        ToolbarActionViewModel::InvocationSource::kMenuEntry);
  }
}
std::unique_ptr<ui::MenuModel> NativeExtensions::CreateMenu(KrActionId key,
                                                            bool panel) {
  if (closed_)
    return nullptr;
  using Source = extensions::ExtensionContextMenuModel::ContextMenuSource;
  if (key != kMenuAction) {
    auto* action = Find(key);
    auto* menu = action ? action->source->GetContextMenu(Source::kToolbarAction)
                        : nullptr;
    return menu ? std::make_unique<ActionMenuModel>(*menu) : nullptr;
  }
  auto root = std::make_unique<ExtensionsMenu>();
  auto* contents = browser_->GetTabStripModel()->GetActiveWebContents();
  int command = 1;
  for (const auto& id : model_->GetAllActionIds()) {
    const auto found = actions_.find(id);
    if (found == actions_.end())
      continue;
    const auto& action = *found->second;
    // These submenus have no separate pin button like Chromium's extension
    // list.
    auto* context = action.source->GetContextMenu(Source::kToolbarAction);
    if (!context)
      continue;
    const auto image = action.source->GetIcon(
        contents, gfx::Size(kDefaultIconSizeChromeRefresh,
                            kDefaultIconSizeChromeRefresh));
    auto label = action.source->GetActionName();
    base::ReplaceSubstringsAfterOffset(&label, 0, u"&", u"&&");
    auto menu = std::make_unique<ActionMenuModel>(
        *context,
        ActionMenuModel::Action{
            action.source->GetActionTitle(contents), image,
            base::BindRepeating(
                [](base::WeakPtr<NativeExtensions> owner, KrActionId key) {
                  return owner && owner->CanInvoke(key);
                },
                weak_factory_.GetWeakPtr(), action.key),
            base::BindRepeating(&NativeExtensions::InvokeFromMenu,
                                weak_factory_.GetWeakPtr(), action.key)});
    root->AddSubMenu(command, label, menu.get());
    root->SetIcon(root->GetItemCount() - 1, image);
    root->actions.push_back(std::move(menu));
    ++command;
  }
  root->app = std::make_unique<AppMenuModel>(
      AcceleratorProviderForBrowser(&*browser_), &*browser_);
  root->app->Init();
  ui::MenuModel* parent = root->app.get();
  size_t index = 0;
  if (ui::MenuModel::GetModelAndIndexForCommandId(IDC_EXTENSIONS_SUBMENU,
                                                  &parent, &index) &&
      parent->IsVisibleAt(index) && parent->IsEnabledAt(index)) {
    if (auto* child = parent->GetSubmenuModelAt(index)) {
      if (root->GetItemCount() && !panel)
        root->AddSeparator(ui::NORMAL_SEPARATOR);
      root->AddSubMenu(panel ? SiteDataMenuModel::kManageExtensions : command,
                       parent->GetLabelAt(index), child);
    }
  }
  return root;
}
void NativeExtensions::RequestMenu(KrActionId key) {
  if (const auto* api = session_->api()) {
    const auto bounds = anchors_->GetBounds({AnchorKind::kAction, key});
    auto fallback = anchors_->GetBounds(kMenuAnchor);
    if (!fallback)
      fallback = anchors_->GetBounds({AnchorKind::kAction, kSiteToolsAction});
    const auto rect = bounds ? bounds : fallback;
    if (rect)
      api->request_context_menu(
          api->self, KR_MENU_ACTION, key,
          {rect->x(), rect->y(), rect->width(), rect->height()});
  }
}
void NativeExtensions::ToggleExtensionsMenu() {
  RequestMenu(kMenuAction);
}
void NativeExtensions::ShowContextMenuAsFallback(
    const extensions::ExtensionId& id) {
  if (const auto found = actions_.find(id); found != actions_.end())
    RequestMenu(found->second->key);
}
void NativeExtensions::HideActivePopup() {
  if (popup_owner_)
    popup_owner_->HidePopup();
}
void NativeExtensions::CloseExtensionsMenuIfOpen() {
  close_menus_.Run();
}
bool NativeExtensions::CanShowToolbarActionPopupForAPICall(
    const std::string& id) {
  return !closed_ && model_->GetActionForId(id) && IsVisible() &&
         host_->GetWidget() && host_->GetWidget()->IsActive();
}
void NativeExtensions::OnActionsInitialized() {
  for (const auto& id : model_->GetAllActionIds())
    EnsureAction(id);
  changed_.Run();
}
void NativeExtensions::OnActionAdded(const std::string& id) {
  EnsureAction(id);
  changed_.Run();
}
void NativeExtensions::OnActionRemoved(const std::string& id) {
  close_menus_.Run();
  const auto found = actions_.find(id);
  if (found != actions_.end()) {
    const auto& action = *found->second;
    if (popup_owner_ == action.source)
      HideActivePopup();
    if (action.registered)
      action.source->UnregisterCommand();
    release_.Run(action.icon);
    anchors_->Untrack({AnchorKind::kAction, action.key});
    actions_.erase(found);
  }
  if (popped_ == id)
    popped_.reset();
  changed_.Run();
}
void NativeExtensions::OnActionUpdated(const std::string& id) {
  if (auto found = actions_.find(id); found != actions_.end())
    found->second->dirty = true;
  changed_.Run();
}
void NativeExtensions::OnPinnedActionsChanged() {
  close_menus_.Run();
  changed_.Run();
}
void NativeExtensions::OnActiveWebContentsChanged(
    bool same_document,
    content::WebContents* contents) {
  HideActivePopup();
  close_menus_.Run();
  RefreshImages();
}
void NativeExtensions::OnToolbarControlStateUpdated() {
  changed_.Run();
}
bool NativeExtensions::IsVisible() const {
  return anchors_->GetBounds(kMenuAnchor).has_value() ||
         anchors_->GetBounds({AnchorKind::kAction, kSiteToolsAction})
             .has_value();
}
std::optional<extensions::ExtensionId> NativeExtensions::GetPoppedOutActionId()
    const {
  return popped_;
}
bool NativeExtensions::IsActionVisibleOnToolbar(const std::string& id) const {
  return model_ && (std::ranges::contains(model_->GetPinnedActionIds(), id) ||
                    popped_ == id);
}
void NativeExtensions::UndoPopOut() {
  popped_.reset();
  changed_.Run();
}
void NativeExtensions::SetPopupOwner(ToolbarActionViewModel* owner) {
  popup_owner_ = owner;
}
void NativeExtensions::PopOutAction(const extensions::ExtensionId& id,
                                    base::OnceClosure ready) {
  popped_ = id;
  changed_.Run();
  std::move(ready).Run();
}
void NativeExtensions::ShowWidgetForExtension(views::Widget* widget,
                                              const std::string& id) {
  if (auto* bubble = widget->widget_delegate()->AsBubbleDialogDelegate())
    bubble->SetAnchor(GetReferenceButtonForPopup(id));
  widget->Show();
}
void NativeExtensions::OnPopupShown(const extensions::ExtensionId& id,
                                    bool by_user) {
  changed_.Run();
}
void NativeExtensions::OnPopupClosed(const extensions::ExtensionId& id) {
  changed_.Run();
}
views::FocusManager* NativeExtensions::GetFocusManagerForAccelerator() {
  return host_->GetFocusManager();
}
views::BubbleAnchor NativeExtensions::GetReferenceButtonForPopup(
    const extensions::ExtensionId& id) {
  const auto found = actions_.find(id);
  if (found != actions_.end()) {
    auto anchor =
        anchors_->GetAnchor({AnchorKind::kAction, found->second->key});
    if (anchor)
      return anchor;
  }
  return GetExtensionsButtonAnchor();
}
views::BubbleAnchor NativeExtensions::GetExtensionsButtonAnchor() {
  auto anchor = anchors_->GetAnchor(kMenuAnchor);
  if (!anchor)
    anchor = anchors_->GetAnchor({AnchorKind::kAction, kSiteToolsAction});
  return anchor ? anchor : views::BubbleAnchor(&*host_);
}
views::BubbleBorder::Arrow NativeExtensions::GetPopupArrow() const {
  return views::BubbleBorder::TOP_RIGHT;
}
}  // namespace krypton_ui

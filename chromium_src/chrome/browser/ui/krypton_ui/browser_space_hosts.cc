// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
//
// Profiles per Space: frame and host coordination for BrowserAdapter. A
// window's own adapter (the frame) owns the caption, frame material and Space
// catalog. Each other persistent profile that a presented Space uses is hosted
// by a real Browser of that profile whose widget is a child of the frame (see
// space_hosts.h and hosted_browser_widget.h). Every tab, popup, permission,
// credential, extension and history entry therefore belongs to the Browser and
// profile that created it; nothing here moves tabs between profiles, copies
// profile data or substitutes one profile for another.
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_active_state_manager/browser_active_state_manager.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/hosted_browser_widget.h"
#include "chrome/browser/ui/krypton_ui/native_menus.h"
#include "chrome/browser/ui/krypton_ui/native_toolbar.h"
#include "chrome/browser/ui/krypton_ui/space_hosts.h"
#include "chrome/browser/ui/krypton_ui/space_profiles.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/extensions/extension_keybinding_registry_views.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "components/prefs/pref_service.h"
#include "components/sessions/core/session_id.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/aura/window.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"

namespace krypton_ui {
namespace {
constexpr std::string_view kSpaceProfileUnavailable =
    "This Space's profile is unavailable.";
constexpr std::string_view kSpaceProfileLocked =
    "Close this Space's tabs to change its profile.";

KrStr Borrow(std::string_view text) {
  return {text.data(), text.size()};
}

// The presented profile's Browser is the active, most recently activated one:
// links from other apps, extension window APIs and new windows follow it.
// Hidden profiles are inactive even though their widgets share the frame.
void SetBrowserActive(BrowserWindowInterface& browser, bool active) {
  auto* state = BrowserActiveStateManager::From(&browser);
  if (!state) {
    return;
  }
  if (!active) {
    state->DidBecomeInactive();
    return;
  }
  // A child widget paints as active with its parent, so a hidden host may
  // already count as active. Re-announce the presented one so it moves to the
  // front of the activation order.
  if (state->IsActive() &&
      GetLastActiveBrowserWindowInterfaceWithAnyProfile() != &browser) {
    state->DidBecomeInactive();
  }
  state->DidBecomeActive();
}
}  // namespace

void BrowserAdapter::InitializeSpaceHost() {
  const auto* restored = RestoredWindowData();
  if (restored) {
    if (const auto found = restored->find(kWindowIdSessionKey);
        found != restored->end() &&
        base::Uuid::ParseLowercase(found->second).is_valid()) {
      window_id_ = found->second;
    }
  }
  if (window_id_.empty()) {
    window_id_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
  }
  auto* profile = browser_->GetProfile();
  // Off-the-record windows never present other profiles and never host them.
  if (profile->IsOffTheRecord()) {
    return;
  }
  auto frame = PendingSpaceHostFrame();
  std::string frame_id;
  if (!frame && restored) {
    if (const auto found = restored->find(kSpaceHostSessionKey);
        found != restored->end() &&
        base::Uuid::ParseLowercase(found->second).is_valid()) {
      frame_id = found->second;
      frame = FindFrameWindow(frame_id);
    }
  }
  if (frame && frame.get() != this && !frame->hosted_ && !frame->closed_ &&
      !frame->browser_->GetProfile()->IsOffTheRecord() &&
      frame->browser_->GetProfile() != profile) {
    hosted_ = true;
    frame_ = std::move(frame);
    // Hosted shells start hidden; the frame presents them.
    presented_ = false;
  } else if (!frame_id.empty()) {
    orphan_of_ = std::move(frame_id);
  }
}

void BrowserAdapter::AttachSpaceHost() {
  if (hosted_) {
    if (frame_ && !frame_->closed_) {
      frame_->EnsureSpaceHosts().AddHost(*this);
    }
  } else {
    RegisterFrameWindow(window_id_, weak_factory_.GetWeakPtr());
    if (!orphan_of_.empty()) {
      RegisterOrphanHost(orphan_of_, weak_factory_.GetWeakPtr());
    }
    // Hosts of this window restored before it adopt into it once it exists.
    std::ignore = session_.Defer(base::BindOnce(
        &BrowserAdapter::AdoptOrphanHosts, weak_factory_.GetWeakPtr()));
  }
  PersistWindowData();
  PublishSpaceProfiles();
}

void BrowserAdapter::CloseSpaceHost() {
  profile_prefs_.RemoveAll();
  profile_directory_.reset();
  ClearWindowSessionData(browser_->GetSessionID().id());
  if (hosted_) {
    if (frame_ && frame_->space_hosts_) {
      frame_->space_hosts_->RemoveHost(*this);
    }
    frame_ = nullptr;
    return;
  }
  UnregisterFrameWindow(window_id_);
  if (space_hosts_) {
    space_hosts_->DetachHosts();
    space_hosts_.reset();
  }
}

void BrowserAdapter::DetachFromFrame() {
  if (!hosted_) {
    return;
  }
  // The frame's profile preferences may go away with it.
  layout_prefs_.RemoveAll();
  frame_ = nullptr;
  if (closed_) {
    return;
  }
  presented_ = false;
  session_.SetPresented(false);
  CloseHostedWindow();
}

PrefService& BrowserAdapter::WindowPrefs() const {
  return *CatalogProfile().GetPrefs();
}

Profile& BrowserAdapter::CatalogProfile() const {
  return hosted_ && frame_ ? *frame_->browser_->GetProfile()
                           : *browser_->GetProfile();
}

std::string BrowserAdapter::CatalogKey() const {
  return hosted_ && frame_
             ? frame_->browser_->GetProfile()->GetBaseName().AsUTF8Unsafe()
             : std::string();
}

const Workspace* BrowserAdapter::FindSpace(KrSpaceId id) const {
  const auto found = std::ranges::find(workspaces_.spaces, id, &Workspace::id);
  return found == workspaces_.spaces.end() ? nullptr : &*found;
}

bool BrowserAdapter::HostsSpace(KrSpaceId id) const {
  if (!hosted_) {
    return CatalogHostsSpace(workspaces_, *browser_->GetProfile(), id);
  }
  return frame_ && SpaceUsesProfile(id, browser_->GetProfile()->GetPath());
}

KrSpaceId BrowserAdapter::FallbackSpace() const {
  if (HostsSpace(active_space_)) {
    return active_space_;
  }
  if (!hosted_) {
    return kDefaultWorkspace;
  }
  for (const auto& space : workspaces_.spaces) {
    if (HostsSpace(space.id)) {
      return space.id;
    }
  }
  return 0;
}

bool BrowserAdapter::SpaceUsesProfile(KrSpaceId id,
                                      const base::FilePath& path) const {
  const auto* space = FindSpace(id);
  if (!space) {
    return false;
  }
  const auto resolved = ResolveSpaceProfile(CatalogProfile(), *space);
  return (resolved.status == SpaceProfileStatus::kOwner ||
          resolved.status == SpaceProfileStatus::kAvailable) &&
         resolved.path == path;
}

bool BrowserAdapter::ActiveSpaceUses(const base::FilePath& path) const {
  return SpaceUsesProfile(active_space_, path);
}

Profile* BrowserAdapter::HostProfile() const {
  return browser_->GetProfile();
}

views::Widget* BrowserAdapter::FrameWidget() const {
  if (hosted_) {
    return frame_ ? frame_->FrameWidget() : nullptr;
  }
  return view_->GetWidget();
}

gfx::NativeView BrowserAdapter::HostParent() const {
  const auto* widget = FrameWidget();
  return widget ? widget->GetNativeView() : gfx::NativeView();
}

gfx::Rect BrowserAdapter::HostBounds() const {
  const auto parent = HostParent();
  return parent ? gfx::Rect(parent->bounds().size()) : gfx::Rect();
}

bool BrowserAdapter::HostPresented(const BrowserAdapter& host) const {
  return space_hosts_ && space_hosts_->IsPresented(host);
}

bool BrowserAdapter::HostActive(const BrowserAdapter& host) const {
  const auto* widget = FrameWidget();
  return widget && widget->IsActive() && space_hosts_ &&
         space_hosts_->IsPresented(host);
}

SpaceHosts& BrowserAdapter::EnsureSpaceHosts() {
  CHECK(!hosted_);
  if (!space_hosts_) {
    space_hosts_ = std::make_unique<SpaceHosts>(*this);
  }
  return *space_hosts_;
}

void BrowserAdapter::PresentHost(BrowserAdapter& host, bool activate) {
  if (!hosted_ && !closed_ && space_hosts_) {
    space_hosts_->PresentHost(host, activate);
  }
}

void BrowserAdapter::PresentForBrowserUI() {
  if (closed_ || !browser_initialized_ || !ready() || presented_) {
    return;
  }
  if (hosted_) {
    if (frame_) {
      frame_->PresentHost(*this, /*activate=*/false);
    }
  } else if (space_hosts_) {
    space_hosts_->PresentFrame();
  }
}

void BrowserAdapter::OnBrowserWindowInitialized() {
  if (closed_ || browser_initialized_) {
    return;
  }
  browser_initialized_ = true;
  if (hosted_) {
    // These services now exist. A new host stays inactive until the coordinator
    // explicitly presents it after CreateBrowserWindow has returned.
    SetHostActive(false);
    FitToFrame();
  }
}

void BrowserAdapter::HostShowedSpace(KrSpaceId space) {
  if (hosted_ || closed_ || !FindSpace(space)) {
    return;
  }
  if (active_space_ != space) {
    menus_->CloseAll();
    active_space_ = space;
    RememberWorkspace(WindowPrefs(), space);
    PublishWorkspaces();
  }
  SyncHostsActiveSpace();
}

void BrowserAdapter::SyncHostsActiveSpace() {
  if (hosted_ || !space_hosts_) {
    return;
  }
  // Every shell of the window shows the same Space selection.
  space_hosts_->ForEachHost([this](BrowserAdapter& host) {
    if (host.active_space_ != active_space_) {
      host.active_space_ = active_space_;
      host.PublishWorkspaces();
    }
  });
}

bool BrowserAdapter::FollowSpace(KrSpaceId space) {
  if (!HostsSpace(space)) {
    return false;
  }
  if (hosted_) {
    if (frame_) {
      frame_->HostShowedSpace(space);
    } else if (active_space_ != space) {
      menus_->CloseAll();
      active_space_ = space;
      PublishWorkspaces();
    }
    return true;
  }
  if (active_space_ != space) {
    menus_->CloseAll();
    active_space_ = space;
    RememberWorkspace(WindowPrefs(), space);
    PublishWorkspaces();
  }
  SyncHostsActiveSpace();
  return true;
}

void BrowserAdapter::SelectSpaceTab(KrSpaceId space) {
  auto* model = browser_->GetTabStripModel();
  auto* selected = browser_->GetActiveTabInterface();
  if (selected && (EssentialForTab(selected->GetContents()) ||
                   WorkspaceForTab(selected->GetContents()) == space)) {
    return;
  }
  tabs::TabInterface* target = nullptr;
  for (auto* tab : *model) {
    if (WorkspaceForTab(tab->GetContents()) == space &&
        (!target || tab->GetContents()->GetLastActiveTime() >
                        target->GetContents()->GetLastActiveTime())) {
      target = tab;
    }
  }
  if (target) {
    model->ActivateTabAt(model->GetIndexOfTab(target));
  } else {
    // A workspace always has a real foreground tab. This also makes Chromium's
    // selected-tab session record sufficient to restore the active workspace.
    const base::AutoReset<KrSpaceId> creating(&new_tab_space_, space);
    chrome::NewTab(&*browser_, NewTabTypes::kNewTabCommand);
  }
}

bool BrowserAdapter::ShowSpace(KrSpaceId space) {
  if (closed_ || !browser_initialized_ || !hosted_) {
    return false;
  }
  if (!HostsSpace(space)) {
    NotifySpaceUnavailable();
    return false;
  }
  menus_->CloseAll();
  if (frame_) {
    frame_->HostShowedSpace(space);
  }
  SelectSpaceTab(space);
  return true;
}

void BrowserAdapter::FollowActiveTab() {
  if (closed_ || !hosted_) {
    return;
  }
  auto* tab = browser_->GetActiveTabInterface();
  const auto space = tab && !EssentialForTab(tab->GetContents())
                         ? WorkspaceForTab(tab->GetContents())
                         : active_space_;
  if (!FollowSpace(space)) {
    if (const auto fallback = FallbackSpace()) {
      std::ignore = FollowSpace(fallback);
    }
  }
  PublishSelection();
}

void BrowserAdapter::SetPresented(bool presented) {
  if (closed_ || presented_ == presented) {
    return;
  }
  presented_ = presented;
  if (!presented) {
    SetBrowserActive(*browser_, false);
    if (auto* keys = ExtensionKeybindingRegistryViews::From(&*browser_)) {
      keys->OnHostActivationChanged(false);
    }
    menus_->CloseAll();
    toolbar_->CloseMenu();
    DropPreviews();
    // Views bubbles are separate widgets, so hiding BrowserView does not
    // necessarily hide them. Dismiss bubbles anchored to this profile before
    // another profile is presented. Never close another host's dialogs.
    auto* owner = view_->GetWidget();
    if (owner) {
      views::Widget::ForEachOwnedWidget(
          owner->GetNativeView(), [owner](views::Widget* widget) {
            auto* delegate = widget->widget_delegate();
            auto* bubble =
                delegate ? delegate->AsBubbleDialogDelegate() : nullptr;
            auto* anchor = bubble ? bubble->GetAnchorView() : nullptr;
            if (anchor && anchor->GetWidget() == owner && !widget->IsClosed()) {
              widget->CloseWithReason(views::Widget::ClosedReason::kLostFocus);
            }
          });
    }
  }
  session_.SetPresented(presented);
  if (hosted_) {
    if (presented && !HostedWidgetWasShown(view_->GetWidget())) {
      // Chromium's first show: OnWindowDidShow and initial page focus. The
      // hosted widget shows itself because this host is now presented.
      view_->Show();
    } else {
      SetHostedWidgetPresented(view_->GetWidget(), presented);
    }
  } else {
    // Hide the complete profile-owned Views tree, including the find bar and
    // side panel. Hiding just the page leaves the inactive profile's controls
    // and accelerators reachable through the shared top-level FocusManager.
    // Hosted widgets are Aura siblings, not descendants of this Views tree.
    view_->SetVisible(presented);
  }
  if (!presented) {
    return;
  }
  PublishWindowState();
  PublishSelection();
  PublishNavigation();
  if (hosted_ && frame_ && browser_initialized_ &&
      frame_->command_theme_space_ == active_space_) {
    std::ignore = session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> self, KrSpaceId space) {
          if (!self || !self->presented_ || !self->browser_initialized_ ||
              !self->frame_ || self->active_space_ != space ||
              self->frame_->command_theme_space_ != space)
            return;
          self->frame_->command_theme_space_ = 0;
          self->ShowThemeEditor(space);
        },
        weak_factory_.GetWeakPtr(), active_space_));
  }
  if (const auto* widget = FrameWidget(); widget && widget->IsActive()) {
    view_->RestoreFocus();
  }
}

void BrowserAdapter::SetHostActive(bool active) {
  if (closed_ || !browser_initialized_ || !hosted_) {
    return;
  }
  SetHostedWidgetActive(view_->GetWidget(), active);
  SetBrowserActive(*browser_, active);
  // A newly initialized hidden widget can already have active_ == false, so
  // its delegate notification is suppressed. Gate its newly created registry
  // explicitly as well as on subsequent activation changes.
  if (auto* keys = ExtensionKeybindingRegistryViews::From(&*browser_)) {
    keys->OnHostActivationChanged(active);
  }
}

void BrowserAdapter::SetFrameActive(bool active) {
  if (closed_ || hosted_) {
    return;
  }
  SetBrowserActive(*browser_, active);
  // Global extension shortcuts belong to the presented profile.
  if (auto* keys = ExtensionKeybindingRegistryViews::From(&*browser_)) {
    keys->OnHostActivationChanged(active);
  }
}

void BrowserAdapter::FitToFrame() {
  auto* widget = view_->GetWidget();
  if (!hosted_ || !browser_initialized_ || !frame_ || !widget) {
    return;
  }
  if (auto* window = widget->GetNativeView()) {
    // Parent coordinates: the host fills the frame's client area.
    window->SetBounds(frame_->HostBounds());
  }
}

void BrowserAdapter::CloseHostedWindow() {
  if (!closed_) {
    // The ordinary close path: beforeunload, then every tab, then the Browser.
    view_->Close();
  }
}

void BrowserAdapter::CloseFrameWindow() {
  if (!closed_ && !hosted_) {
    view_->Close();
  }
}

bool BrowserAdapter::CanCloseFrame() {
  if (hosted_ || closed_ || !space_hosts_) {
    return true;
  }
  return space_hosts_->CloseHostsForFrameClose(
      browser_shutdown::IsTryingToQuit());
}

void BrowserAdapter::RecoverActiveSpace() {
  if (hosted_ || closed_) {
    return;
  }
  std::ignore = session_.Defer(base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> self) {
        if (!self || self->closed_ || self->HostsSpace(self->active_space_) ||
            (self->space_hosts_ && self->space_hosts_->presented())) {
          return;
        }
        self->SwitchWorkspace(kDefaultWorkspace);
      },
      weak_factory_.GetWeakPtr()));
}

void BrowserAdapter::NotifyPresented(std::string_view text) {
  if (hosted_) {
    if (frame_) {
      frame_->NotifyPresented(text);
    } else {
      Notify(text);
    }
    return;
  }
  auto* target = space_hosts_ ? space_hosts_->presented() : nullptr;
  (target ? *target : *this).Notify(text);
}

void BrowserAdapter::NotifySpaceUnavailable() {
  NotifyPresented(kSpaceProfileUnavailable);
}

void BrowserAdapter::PersistWindowData() {
  std::map<std::string, std::string> data{{kWindowIdSessionKey, window_id_}};
  const std::string host_of =
      hosted_ ? (frame_ ? frame_->window_id_ : std::string()) : orphan_of_;
  if (!host_of.empty()) {
    data.emplace(kSpaceHostSessionKey, host_of);
  }
  // Re-emitted whenever Chromium rebuilds this window's session commands.
  SetWindowSessionData(browser_->GetSessionID().id(), data);
  // The session service tracks the window only after its construction.
  std::ignore = session_.Defer(base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> self,
         std::map<std::string, std::string> data) {
        if (!self || self->closed_) {
          return;
        }
        auto* service =
            SessionServiceFactory::GetForProfile(self->browser_->GetProfile());
        if (!service) {
          return;
        }
        for (const auto& [key, value] : data) {
          service->AddWindowExtraData(self->browser_->GetSessionID(),
                                      key.c_str(), value);
        }
      },
      weak_factory_.GetWeakPtr(), std::move(data)));
}

void BrowserAdapter::AdoptOrphanHosts() {
  if (closed_ || hosted_) {
    return;
  }
  const auto catalog = browser_->GetProfile()->GetBaseName().AsUTF8Unsafe();
  for (const auto& orphan : TakeOrphanHosts(window_id_)) {
    if (!orphan || orphan.get() == this || orphan->closed_ || orphan->hosted_) {
      continue;
    }
    auto* profile = orphan->browser_->GetProfile();
    if (profile == browser_->GetProfile() || profile->IsOffTheRecord()) {
      continue;
    }
    auto& hosts = EnsureSpaceHosts();
    auto* host = hosts.HostFor(profile->GetPath());
    if (!host) {
      // Same profile as the orphan: its tabs only change Browser, never
      // profile. The new host starts hidden.
      const ScopedSpaceHostCreation scope(weak_factory_.GetWeakPtr());
      std::ignore = CreateBrowserWindow(
          BrowserWindowCreateParams(*profile, /*from_user_gesture=*/false));
      host = hosts.HostFor(profile->GetPath());
    }
    if (host) {
      host->AdoptOrphanTabs(*orphan, catalog);
    }
  }
}

void BrowserAdapter::AdoptOrphanTabs(BrowserAdapter& orphan,
                                     const std::string& catalog) {
  if (closed_ || !hosted_ ||
      orphan.browser_->GetProfile() != browser_->GetProfile()) {
    return;
  }
  auto* from = orphan.browser_->GetTabStripModel();
  auto* to = browser_->GetTabStripModel();
  std::vector<std::pair<base::WeakPtr<tabs::TabInterface>, KrSpaceId>> moving;
  for (auto* tab : *from) {
    const auto found = orphan.orphan_spaces_.find(Identity(*tab));
    if (found != orphan.orphan_spaces_.end() &&
        found->second.second == catalog) {
      moving.emplace_back(tab->GetWeakPtr(), found->second.first);
    }
  }
  for (const auto& [tab, space] : moving) {
    const int index =
        tab ? from->GetIndexOfTab(tab.get()) : TabStripModel::kNoTab;
    if (index == TabStripModel::kNoTab) {
      continue;
    }
    orphan.orphan_spaces_.erase(Identity(*tab));
    auto* contents = tab->GetContents();
    const bool pinned = from->IsTabPinned(index);
    auto detached = from->DetachTabAtForInsertion(index);
    // Restore the Space recorded for this window before insertion, so the
    // host keeps it. Folders are not hosted for other profiles.
    SetWorkspaceForTab(contents, space);
    SetWorkspaceCatalogForTab(contents, catalog);
    SetFolderForTab(contents, 0);
    to->InsertDetachedTabAt(
        to->count(), std::move(detached),
        pinned ? AddTabTypes::ADD_PINNED : AddTabTypes::ADD_NONE);
  }
  // An emptied orphan closes through Chromium's ordinary empty-strip path.
}

SpaceProfileDirectory& BrowserAdapter::ProfileDirectory() {
  if (!profile_directory_) {
    profile_directory_ = std::make_unique<SpaceProfileDirectory>(
        base::BindRepeating(&BrowserAdapter::SchedulePublishSpaceProfiles,
                            weak_factory_.GetWeakPtr()));
  }
  return *profile_directory_;
}

void BrowserAdapter::SchedulePublishSpaceProfiles() {
  if (space_profiles_scheduled_ || closed_) {
    return;
  }
  space_profiles_scheduled_ = session_.Defer(base::BindOnce(
      &BrowserAdapter::PublishSpaceProfiles, weak_factory_.GetWeakPtr()));
}

void BrowserAdapter::PublishSpaceProfiles() {
  space_profiles_scheduled_ = false;
  const auto* api = session_.api();
  if (closed_ || !api || !KR_HAS_SLOT(api, KrShellUI, set_space_profiles) ||
      !api->set_space_profiles) {
    return;
  }
  auto& owner = CatalogProfile();
  // In-memory attributes only: listing never loads or creates a profile.
  const auto choices = owner.IsOffTheRecord()
                           ? std::vector<SpaceProfileChoice>()
                           : ListSpaceProfiles(owner);
  std::vector<std::string> names;
  names.reserve(choices.size());
  for (const auto& choice : choices) {
    names.push_back(base::UTF16ToUTF8(choice.name));
  }
  std::vector<KrProfileChoice> values;
  values.reserve(choices.size());
  for (size_t index = 0; index < choices.size(); ++index) {
    values.push_back(
        {sizeof(KrProfileChoice), SpaceProfileIdFor(choices[index].path),
         Borrow(names[index]),
         choices[index].owner ? uint32_t{KR_PROFILE_CURRENT} : uint32_t{0}});
  }
  const auto in_use = SpacesInUse(owner);
  std::vector<KrSpaceProfile> spaces;
  spaces.reserve(workspaces_.spaces.size());
  for (const auto& space : workspaces_.spaces) {
    KrSpaceProfile value{sizeof(KrSpaceProfile), space.id, 0,
                         KR_SPACE_PROFILE_READY, 0};
    const auto resolved = ResolveSpaceProfile(owner, space);
    switch (resolved.status) {
      case SpaceProfileStatus::kOwner:
      case SpaceProfileStatus::kAvailable:
        value.profile = SpaceProfileIdFor(resolved.path);
        break;
      case SpaceProfileStatus::kMissing:
        value.state = KR_SPACE_PROFILE_MISSING;
        break;
      case SpaceProfileStatus::kUnavailable:
        value.state = KR_SPACE_PROFILE_UNAVAILABLE;
        break;
    }
    // Default always uses the catalog owner.
    if (space.id == kDefaultWorkspace || in_use.contains(space.id)) {
      value.flags |= KR_SPACE_PROFILE_IN_USE;
    }
    spaces.push_back(value);
  }
  api->set_space_profiles(api->self, values.data(), values.size(),
                          sizeof(KrProfileChoice), spaces.data(), spaces.size(),
                          sizeof(KrSpaceProfile));
}

KrResult BrowserAdapter::SetSpaceProfileCommand(KrSpaceId space,
                                                KrProfileId profile,
                                                std::u16string new_profile) {
  std::optional<base::FilePath> path;
  if (new_profile.empty()) {
    path = SpaceProfilePath(profile);
    if (!path) {
      return KR_ERR_INVALID_ARGUMENT;
    }
  }
  return session_.Defer(base::BindOnce(&BrowserAdapter::SetSpaceProfile,
                                       weak_factory_.GetWeakPtr(), space,
                                       std::move(path), std::move(new_profile)))
             ? KR_OK
             : KR_ERR_UNSUPPORTED;
}

KrResult BrowserAdapter::CreateSpaceCommand(Workspace value,
                                            KrProfileId profile,
                                            std::u16string new_profile) {
  std::optional<base::FilePath> path;
  if (new_profile.empty()) {
    path = SpaceProfilePath(profile);
    if (!path) {
      return KR_ERR_INVALID_ARGUMENT;
    }
  }
  return session_.Defer(base::BindOnce(
             &BrowserAdapter::CreateSpace, weak_factory_.GetWeakPtr(),
             std::move(value), std::move(path), std::move(new_profile)))
             ? KR_OK
             : KR_ERR_UNSUPPORTED;
}

void BrowserAdapter::SetSpaceProfile(KrSpaceId space,
                                     std::optional<base::FilePath> profile,
                                     std::u16string new_profile) {
  if (closed_) {
    return;
  }
  if (hosted_) {
    // The catalog owner decides.
    if (frame_) {
      frame_->SetSpaceProfile(space, std::move(profile),
                              std::move(new_profile));
    }
    return;
  }
  if (browser_->GetProfile()->IsOffTheRecord()) {
    return;
  }
  if (new_profile.empty()) {
    AssignSpaceProfileAndSwitch(space, /*switch_to=*/false, std::move(profile));
    return;
  }
  // Refuse before creating a profile the Space could not use.
  if (SpacesInUse(*browser_->GetProfile()).contains(space)) {
    NotifyPresented(kSpaceProfileLocked);
    SchedulePublishSpaceProfiles();
    return;
  }
  ProfileDirectory().Create(
      std::move(new_profile),
      base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> self, KrSpaceId space,
             std::optional<base::FilePath> path) {
            if (!self) {
              return;
            }
            if (!path) {
              self->NotifySpaceUnavailable();
              return;
            }
            self->AssignSpaceProfileAndSwitch(space, /*switch_to=*/false,
                                              std::move(path));
          },
          weak_factory_.GetWeakPtr(), space));
}

void BrowserAdapter::CreateSpace(Workspace value,
                                 std::optional<base::FilePath> profile,
                                 std::u16string new_profile) {
  if (closed_) {
    return;
  }
  if (hosted_) {
    if (frame_) {
      frame_->CreateSpace(std::move(value), std::move(profile),
                          std::move(new_profile));
    }
    return;
  }
  if (browser_->GetProfile()->IsOffTheRecord()) {
    return;
  }
  auto create = base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> self, Workspace value,
         std::optional<base::FilePath> path) {
        if (!self || self->closed_) {
          return;
        }
        if (!path) {
          self->customize_new_space_ = false;
          self->NotifySpaceUnavailable();
          return;
        }
        value.id = 0;
        value.profile.clear();
        const auto id = SaveWorkspace(self->WindowPrefs(), std::move(value));
        if (!id) {
          return;
        }
        const bool customize = std::exchange(self->customize_new_space_, false);
        if (*path == self->browser_->GetProfile()->GetPath()) {
          self->SwitchWorkspace(*id);
          if (customize)
            self->ShowThemeEditor(*id);
          return;
        }
        if (customize)
          self->command_theme_space_ = *id;
        self->AssignSpaceProfileAndSwitch(*id, /*switch_to=*/true,
                                          std::move(path));
      },
      weak_factory_.GetWeakPtr(), std::move(value));
  if (new_profile.empty()) {
    std::move(create).Run(std::move(profile));
    return;
  }
  ProfileDirectory().Create(std::move(new_profile), std::move(create));
}

void BrowserAdapter::AssignSpaceProfileAndSwitch(
    KrSpaceId space,
    bool switch_to,
    std::optional<base::FilePath> profile) {
  if (closed_ || hosted_ || !profile) {
    return;
  }
  auto& owner = *browser_->GetProfile();
  // The catalog owner's own profile is stored as "no mapping".
  const auto target = *profile == owner.GetPath() ? base::FilePath() : *profile;
  // The preference observers republish the catalog and profile states.
  const auto result = AssignSpaceProfile(owner, space, target);
  if (result == SpaceProfileAssignment::kAssigned) {
    if (switch_to) {
      SwitchWorkspace(space);
    }
    return;
  }
  if (switch_to) {
    // A Space created for a profile must never quietly use another one.
    std::ignore = RemoveWorkspace(WindowPrefs(), space);
  }
  NotifyPresented(result == SpaceProfileAssignment::kSpaceInUse
                      ? kSpaceProfileLocked
                      : kSpaceProfileUnavailable);
  SchedulePublishSpaceProfiles();
}
}  // namespace krypton_ui

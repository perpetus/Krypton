// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_ADAPTER_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_ADAPTER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/containers/flat_map.h"
#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "chrome/browser/ui/krypton_ui/control_anchors.h"
#include "chrome/browser/ui/krypton_ui/folder_prefs.h"
#include "chrome/browser/ui/krypton_ui/native_images.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/krypton_ui/workspace_prefs.h"
#include "chrome/browser/ui/krypton_ui/workspace_theme_prefs.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/prefs/pref_change_registrar.h"
#include "ui/actions/action_id.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_ui_types.h"

class BrowserView;
class BrowserWindowInterface;
class PrefService;
class Profile;
struct NavigateParams;
namespace content {
class NavigationHandle;
struct DropData;
}  // namespace content
namespace ui {
class ImageModel;
class MenuModel;
class OSExchangeData;
}  // namespace ui
namespace gfx {
class Point;
class PointF;
}  // namespace gfx
namespace views {
class Widget;
}

namespace krypton_ui {
class NativeLocationBar;
class NativeBackdrop;
class PageCornerOverlays;
class NativeGlassBackdrop;
class NativeToolbar;
class NativeMenus;
class PrivacyFeaturesBridge;
class BrowserServices;
class SidebarServices;
class NativeExtensions;
class SiteCustomization;
class TranslationActions;
class DictationController;
class SitePermissions;
class MultiPageController;
class LiveFolders;
class LiveFolderService;
struct CommandBarEntry;
class PageCapture;
enum class PageCaptureMode;
enum class DocumentKind;
class SpaceHosts;
class SpaceProfileDirectory;

// BrowserView owns this adapter before constructing browser chrome. It keeps
// Chromium objects host-owned and publishes their state through the public ABI.
class BrowserAdapter final : public TabStripModelObserver {
 public:
  [[nodiscard]] MultiPageController* multi_page() const {
    return multi_page_.get();
  }
  [[nodiscard]] bool MultiPageEnabled() const;
  [[nodiscard]] SiteCustomization* site_customization() {
    return site_customization_.get();
  }
  [[nodiscard]] bool SnapLayoutsEnabled() const;
  [[nodiscard]] SitePermissions* site_permissions() const {
    return site_permissions_.get();
  }
  [[nodiscard]] bool PrepareMultiPageSelection(content::WebContents* contents);
  [[nodiscard]] bool IsWorkspaceCloseCandidate(
      const tabs::TabInterface& tab) const;
  [[nodiscard]] bool NeedsWorkspaceCloseTarget(
      const tabs::TabInterface& closing) const;
  void PrepareWorkspaceCloseTarget(tabs::TabInterface& closing);
  [[nodiscard]] KrResult MultiPageCommand(const KrMultiPageCommand& command);
  // Chromium's own split entry points join Krypton's groups instead. Both
  // return false when multi-page is unavailable, leaving Chromium's path.
  [[nodiscard]] bool JoinNativeSplit(KrTabId pivot,
                                     KrTabId other,
                                     KrSplitLayout layout);
  [[nodiscard]] bool ShowSplitChooser();
  [[nodiscard]] bool ShowSplitOptions(KrTabId tab);
  [[nodiscard]] content::WebContents* OpenGlance(
      NavigateParams& params,
      base::OnceCallback<void(content::NavigationHandle&)> callback = {});
  static bool IsEligible(const BrowserWindowInterface& browser);
  explicit BrowserAdapter(BrowserView& view);
  ~BrowserAdapter() override;
  BrowserAdapter(const BrowserAdapter&) = delete;
  BrowserAdapter& operator=(const BrowserAdapter&) = delete;

  // Service registration precedes BrowserView's AutofillBubbleHandler. HWND
  // attachment follows Widget::Init and always precedes showing the window.
  void InitializeServices();
  [[nodiscard]] base::expected<void, std::string> Attach();
  void Close();
  [[nodiscard]] bool ready() const { return session_.api() != nullptr; }
  [[nodiscard]] WindowSession& session() { return session_; }
  [[nodiscard]] NativeLocationBar& location_bar() { return *location_; }
  [[nodiscard]] NativeToolbar& toolbar() { return *toolbar_; }
  [[nodiscard]] base::WeakPtr<BrowserAdapter> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  [[nodiscard]] base::ScopedClosureRunner RequestPageMenu(
      ui::MenuModel& model,
      const gfx::Point& screen_point);
  void RefreshPageMenu(const ui::MenuModel& model);
  [[nodiscard]] uint32_t WindowFlags() const;
  void PublishTheme();
  // Follows the module's frame request: opaque theme or DWM acrylic.
  void UpdateFrameMaterial();
  void UpdatePageCornerOverlays();
  [[nodiscard]] KrResult FolderCommand(const KrFolderCommand& command);
  [[nodiscard]] bool CanFolders() const;
  void PublishWindowState();
  void PublishNavigation();
  void ScheduleActions();
  void ShowTranslationMenu();
  void ShowVoiceMenu();
  [[nodiscard]] bool OpenCommandLauncher();
  [[nodiscard]] bool CanCreateDocument() const;
  void NewDocument(DocumentKind kind);
  void UpdateDocumentPageTheme(content::WebContents* contents);
  bool ShowOnboarding();
  static bool CanDropUrl(const ui::OSExchangeData& data);
  bool UrlDragChanged(const gfx::PointF& point, bool active);
  void UrlDragExited();
  bool PreviewUrlDrag(const content::DropData& data, const gfx::PointF& point);
  base::OnceClosure CaptureUrlDrop(const ui::OSExchangeData& data,
                                   const gfx::PointF& point);
  void Fail(std::string error);

  // Profiles per Space (browser_space_hosts.cc, space_hosts.h). A window's own
  // adapter, the frame, presents Spaces of its own profile and coordinates
  // hosts. A hosted adapter belongs to a Browser of another persistent profile
  // whose widget is a child of the frame; it shows the frame's Space catalog
  // and holds only tabs of Spaces mapped to its profile.
  [[nodiscard]] bool hosted() const { return hosted_; }
  [[nodiscard]] BrowserAdapter* frame_adapter() const { return frame_.get(); }
  [[nodiscard]] bool presented() const { return presented_; }
  [[nodiscard]] bool browser_initialized() const {
    return browser_initialized_;
  }
  // Called by Chromium's final post-window feature initialization hook.
  void OnBrowserWindowInitialized();
  [[nodiscard]] Profile* HostProfile() const;
  // The frame's widget; for a hosted adapter, the widget it shares.
  [[nodiscard]] views::Widget* FrameWidget() const;
  // Frame only: where host widgets are parented, and their bounds in it.
  [[nodiscard]] gfx::NativeView HostParent() const;
  [[nodiscard]] gfx::Rect HostBounds() const;
  [[nodiscard]] bool HostActive(const BrowserAdapter& host) const;
  [[nodiscard]] bool HostPresented(const BrowserAdapter& host) const;
  // Frame only: Chromium asked a host to show or activate.
  void PresentHost(BrowserAdapter& host, bool activate);
  // Explicit Chromium UI must be shown with its own profile's page and shell.
  void PresentForBrowserUI();
  // Frame only: a host now shows `space`.
  void HostShowedSpace(KrSpaceId space);
  void SetPresented(bool presented);
  // Derived activation while the shared frame is the active window.
  void SetHostActive(bool active);
  void SetFrameActive(bool active);
  // Host only: selects a Space mapped to this profile. False when this host
  // cannot show it.
  [[nodiscard]] bool ShowSpace(KrSpaceId space);
  // Host only: the frame follows this host's selected Space.
  void FollowActiveTab();
  void FitToFrame();
  void CloseHostedWindow();
  void CloseFrameWindow();
  void RecoverActiveSpace();
  void NotifySpaceUnavailable();
  [[nodiscard]] bool SpaceUsesProfile(KrSpaceId space,
                                      const base::FilePath& path) const;
  [[nodiscard]] bool ActiveSpaceUses(const base::FilePath& path) const;
  // BrowserView asks before closing the window. A frame first closes each
  // host through that host's own unload handling.
  [[nodiscard]] bool CanCloseFrame();

 private:
  friend class MultiPageController;
  friend class SiteCustomization;
  friend class LiveFolders;
  friend class LiveFolderService;
  friend class SpaceHosts;
  class Tab;
  class TabPreview;
  class SwitcherPreview;
  struct Action;
  static KrTabId Identity(const tabs::TabInterface& tab);
  KrBrowser MakeBrowser();
  void BindBrowserServices(KrBrowser& host);
  void Insert(tabs::TabInterface& tab, size_t index);
  void PublishTab(tabs::TabInterface& tab);
  void PublishSelection();
  // New Tab Pages show the window's theme: their page view hides its
  // background and the patched page draws none. Runs after Chromium has bound
  // tabs to views, once per task.
  void SchedulePageBackgrounds();
  void UpdatePageBackgrounds();
  void PublishProgress(tabs::TabInterface& tab);
  void PublishActions();
  void PublishLayout();
  void PublishWorkspaces();
  void PublishFolders();
  void OnFoldersChanged();
  void AssignFolder(tabs::TabInterface& tab, KrFolderId folder);
  void PublishWorkspaceThemes();
  [[nodiscard]] uint32_t ThemeScheme() const;
  void ApplyWorkspaceTheme(const KrWorkspaceTheme& theme, bool commit);
  void ShowThemeEditor(KrSpaceId space);
  void OnWorkspacesChanged();
  void SwitchWorkspace(KrSpaceId space);
  void AssignWorkspace(tabs::TabInterface& tab, KrSpaceId space);
  void AssignEssential(tabs::TabInterface& tab, uint64_t id);
  void SetEssential(tabs::TabInterface& tab, bool enabled);
  void AlignEssentialPin(tabs::TabInterface& tab, int to_index);
  void OnEssentialsChanged();
  void UpdateEssential(tabs::TabInterface& tab);
  void EnsureWorkspace(tabs::TabInterface& tab);
  void EditWorkspace(uint32_t kind, Workspace value);
  [[nodiscard]] std::unique_ptr<ui::MenuModel> CreateWorkspaceMenu(
      KrSpaceId target);
  [[nodiscard]] std::unique_ptr<ui::MenuModel> CreateContextMenu(
      uint32_t kind,
      tabs::TabInterface* target,
      uint64_t zone = 0);
  [[nodiscard]] std::unique_ptr<ui::MenuModel> CreateSiteDataMenu(
      tabs::TabInterface& target);
  [[nodiscard]] base::expected<void, std::string> ShowPageInfo(
      tabs::TabInterface* tab);
  void ExecuteCommand(int command);
  std::vector<CommandBarEntry> CommandBarEntries(std::string_view query);
  void AppendExtraCommands(std::string_view query,
                           std::vector<CommandBarEntry>& entries);
  void RecordPinnedHome(tabs::TabInterface& tab);
  bool customize_new_space_ = false;
  KrSpaceId command_theme_space_ = 0;
  bool CanTakeScreenshot() const;
  void TakeScreenshot(tabs::TabInterface* tab,
                      PageCaptureMode mode,
                      bool add_to_easel = false);
  void CopyCurrentLink();
  void Notify(std::string_view text);
  [[nodiscard]] base::OnceClosure CaptureCommand(int command, bool tab_scoped);
  void Activate(KrTabId id);
  void CloseTab(KrTabId id);
  void MoveTab(KrTabId id, KrSpaceId space, size_t index);
  void SetTabFlags(KrTabId id, uint32_t mask, uint32_t values);
  bool HandleUnhandledKey(const MSG& message);
  void TakeKeyboardFocus();
  void WindowCommand(uint32_t command);
  [[nodiscard]] tabs::TabInterface* FindTab(KrTabId id) const;
  [[nodiscard]] base::expected<KrImageId, std::string> AcquireImage(
      const ui::ImageModel& image);
  void OnTabStripModelChanged(
      TabStripModel* model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab, TabChangeType type) override;
  void OnTabPinnedStateChanged(tabs::TabInterface* tab, int index) override;

  // Profiles per Space (browser_space_hosts.cc).
  void InitializeSpaceHost();
  void AttachSpaceHost();
  void CloseSpaceHost();
  // Window-level preferences: appearance, layout and the Space catalog belong
  // to the frame's profile; Essentials and privacy settings to this profile.
  [[nodiscard]] PrefService& WindowPrefs() const;
  [[nodiscard]] Profile& CatalogProfile() const;
  // Session key of the catalog owner for this browser's tabs; empty when the
  // catalog belongs to this browser's own profile.
  [[nodiscard]] std::string CatalogKey() const;
  [[nodiscard]] bool HostsSpace(KrSpaceId space) const;
  // Where a tab without a valid Space goes; 0 when no Space uses this
  // browser's profile.
  [[nodiscard]] KrSpaceId FallbackSpace() const;
  [[nodiscard]] const Workspace* FindSpace(KrSpaceId space) const;
  [[nodiscard]] SpaceHosts& EnsureSpaceHosts();
  // Follows the active tab's Space. Returns false when `space` is not shown by
  // this window.
  bool FollowSpace(KrSpaceId space);
  void SyncHostsActiveSpace();
  void SelectSpaceTab(KrSpaceId space);
  // Shows a notification in whichever profile's shell is presented.
  void NotifyPresented(std::string_view text);
  // Drops tab and switcher preview requests; their types are local to
  // browser_adapter.cc.
  void DropPreviews();
  void SchedulePublishSpaceProfiles();
  void PublishSpaceProfiles();
  [[nodiscard]] KrResult SetSpaceProfileCommand(KrSpaceId space,
                                                KrProfileId profile,
                                                std::u16string new_profile);
  [[nodiscard]] KrResult CreateSpaceCommand(Workspace value,
                                            KrProfileId profile,
                                            std::u16string new_profile);
  void SetSpaceProfile(KrSpaceId space,
                       std::optional<base::FilePath> profile,
                       std::u16string new_profile);
  void CreateSpace(Workspace value,
                   std::optional<base::FilePath> profile,
                   std::u16string new_profile);
  void AssignSpaceProfileAndSwitch(KrSpaceId space,
                                   bool switch_to,
                                   std::optional<base::FilePath> profile);
  [[nodiscard]] SpaceProfileDirectory& ProfileDirectory();
  void PersistWindowData();
  void AdoptOrphanHosts();
  void AdoptOrphanTabs(BrowserAdapter& orphan, const std::string& catalog);
  // The frame is closing: release its preferences and close this host.
  void DetachFromFrame();

  const raw_ref<BrowserView> view_;
  const raw_ref<BrowserWindowInterface> browser_;
  WindowSession session_;
  ControlAnchors anchors_;
  NativeImages images_;
  std::unique_ptr<TabPreview> tab_preview_;
  uint32_t switcher_request_ = 0;
  std::vector<std::unique_ptr<SwitcherPreview>> switcher_previews_;
  std::unique_ptr<MultiPageController> multi_page_;
  bool multi_page_capable_ = false;
  raw_ptr<NativeBackdrop> backdrop_ = nullptr;  // BrowserView's background.
  std::unique_ptr<PageCornerOverlays> page_corner_overlays_;
  std::unique_ptr<NativeGlassBackdrop> glass_;
  std::unique_ptr<NativeLocationBar> location_;
  raw_ptr<NativeToolbar> toolbar_ = nullptr;  // Child of BrowserView.
  std::unique_ptr<NativeMenus> menus_;
  std::unique_ptr<PrivacyFeaturesBridge> privacy_features_;
  std::unique_ptr<BrowserServices> browser_services_;
  std::unique_ptr<SitePermissions> site_permissions_;
  std::unique_ptr<PageCapture> screenshot_;
  std::unique_ptr<SidebarServices> sidebar_;
  std::unique_ptr<NativeExtensions> extensions_;
  std::unique_ptr<SiteCustomization> site_customization_;
  std::unique_ptr<LiveFolders> live_folders_;
  PrefChangeRegistrar layout_prefs_;
  base::CallbackListSubscription essential_restore_;
  base::CallbackListSubscription fullscreen_changed_;
  WorkspaceCatalog workspaces_;
  FolderCatalog folders_;
  std::vector<TabFolder> published_folders_;
  KrFolderId new_folder_ = 0;
  bool editing_folders_ = false;
  WorkspaceThemeState workspace_themes_;
  KrSpaceId active_space_ = kDefaultWorkspace;
  KrSpaceId new_tab_space_ = 0;
  uint64_t new_essential_ = 0;
  bool restoring_essentials_ = false;
  base::flat_map<KrTabId, std::unique_ptr<Tab>> tabs_;
  base::flat_map<KrActionId, std::unique_ptr<Action>> actions_;
  std::vector<KrAction> action_views_;
  std::vector<actions::ActionId> action_ids_;
  std::string display_url_;
  std::string full_url_;
  std::string site_tools_label_;
  std::string copy_link_label_;
  std::string translation_tooltip_;
  std::string translation_locale_;
  std::string zoom_label_;
  std::string zoom_tooltip_;
  std::unique_ptr<TranslationActions> translation_actions_;
  std::unique_ptr<DictationController> dictation_;
  uint32_t security_ = KR_SECURITY_NONE;
  KrTabId active_tab_ = 0;
  bool back_ = false;
  bool forward_ = false;
  bool loading_ = false;
  bool navigation_published_ = false;
  bool actions_scheduled_ = false;
  bool page_backgrounds_scheduled_ = false;
  bool services_initialized_ = false;
  bool closed_ = false;
  bool url_drag_preview_active_ = false;
  float image_scale_ = 0;
  // Profiles per Space. Durable identity of this window across restarts.
  std::string window_id_;
  bool hosted_ = false;
  bool browser_initialized_ = false;
  base::WeakPtr<BrowserAdapter> frame_;  // Hosted adapters only.
  // A host restored before its frame runs standalone until adopted.
  std::string orphan_of_;
  std::map<KrTabId, std::pair<KrSpaceId, std::string>> orphan_spaces_;
  bool presented_ = true;
  bool space_profiles_scheduled_ = false;
  std::unique_ptr<SpaceHosts> space_hosts_;  // Frames only, on first use.
  std::unique_ptr<SpaceProfileDirectory> profile_directory_;
  PrefChangeRegistrar profile_prefs_;  // This browser's own profile.
  base::WeakPtrFactory<BrowserAdapter> weak_factory_{this};
};
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_ADAPTER_H_

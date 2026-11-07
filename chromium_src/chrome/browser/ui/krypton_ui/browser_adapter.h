// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_ADAPTER_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_ADAPTER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/containers/flat_map.h"
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

class BrowserView;
class BrowserWindowInterface;
struct NavigateParams;
namespace content {
class NavigationHandle;
}
namespace image_editor {
class ScreenshotFlow;
}
namespace ui {
class ImageModel;
class MenuModel;
}  // namespace ui
namespace gfx {
class Point;
}

namespace krypton_ui {
class NativeLocationBar;
class NativeBackdrop;
class NativeGlassBackdrop;
class NativeToolbar;
class NativeMenus;
class PrivacyFeaturesBridge;
class BrowserServices;
class SidebarServices;
class NativeExtensions;
class SiteCustomization;
class MultiPageController;

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
  [[nodiscard]] KrResult FolderCommand(const KrFolderCommand& command);
  [[nodiscard]] bool CanFolders() const;
  void PublishWindowState();
  void PublishNavigation();
  void ScheduleActions();
  bool ShowOnboarding();
  void Fail(std::string error);

 private:
  friend class MultiPageController;
  friend class SiteCustomization;
  class Tab;
  class TabPreview;
  struct Action;
  static KrTabId Identity(const tabs::TabInterface& tab);
  KrBrowser MakeBrowser();
  void BindBrowserServices(KrBrowser& host);
  void Insert(tabs::TabInterface& tab, size_t index);
  void PublishTab(tabs::TabInterface& tab);
  void PublishSelection();
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
  void OnEssentialsChanged();
  void UpdateEssential(tabs::TabInterface& tab);
  void EnsureWorkspace(tabs::TabInterface& tab);
  void EditWorkspace(uint32_t kind, Workspace value);
  [[nodiscard]] std::unique_ptr<ui::MenuModel> CreateWorkspaceMenu(
      KrSpaceId target);
  [[nodiscard]] std::unique_ptr<ui::MenuModel> CreateContextMenu(
      uint32_t kind,
      tabs::TabInterface* target);
  [[nodiscard]] std::unique_ptr<ui::MenuModel> CreateSiteDataMenu(
      tabs::TabInterface& target);
  [[nodiscard]] base::expected<void, std::string> ShowPageInfo(
      tabs::TabInterface* tab);
  void ExecuteCommand(int command);
  bool CanTakeScreenshot() const;
  void TakeScreenshot(tabs::TabInterface* tab);
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

  const raw_ref<BrowserView> view_;
  const raw_ref<BrowserWindowInterface> browser_;
  WindowSession session_;
  ControlAnchors anchors_;
  NativeImages images_;
  std::unique_ptr<TabPreview> tab_preview_;
  std::unique_ptr<MultiPageController> multi_page_;
  bool multi_page_capable_ = false;
  raw_ptr<NativeBackdrop> backdrop_ = nullptr;  // BrowserView's background.
  std::unique_ptr<NativeGlassBackdrop> glass_;
  std::unique_ptr<NativeLocationBar> location_;
  raw_ptr<NativeToolbar> toolbar_ = nullptr;  // Child of BrowserView.
  std::unique_ptr<NativeMenus> menus_;
  std::unique_ptr<PrivacyFeaturesBridge> privacy_features_;
  std::unique_ptr<BrowserServices> browser_services_;
  std::unique_ptr<image_editor::ScreenshotFlow> screenshot_;
  std::unique_ptr<SidebarServices> sidebar_;
  std::unique_ptr<NativeExtensions> extensions_;
  std::unique_ptr<SiteCustomization> site_customization_;
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
  uint32_t security_ = KR_SECURITY_NONE;
  KrTabId active_tab_ = 0;
  bool back_ = false;
  bool forward_ = false;
  bool loading_ = false;
  bool navigation_published_ = false;
  bool actions_scheduled_ = false;
  bool services_initialized_ = false;
  bool closed_ = false;
  float image_scale_ = 0;
  base::WeakPtrFactory<BrowserAdapter> weak_factory_{this};
};
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_ADAPTER_H_

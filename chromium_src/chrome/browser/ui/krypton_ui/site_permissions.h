// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SITE_PERMISSIONS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SITE_PERMISSIONS_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/containers/flat_map.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/content_settings/core/browser/content_settings_observer.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/page_info/page_info_ui.h"
#include "components/permissions/permission_prompt.h"
#include "content/public/browser/web_contents_observer.h"

class BrowserWindowInterface;
class ChromePageInfoUiDelegate;

namespace krypton_ui {
class WindowSession;

// Observes only while the permission surface is open. PageInfo remains the
// authority for effective settings, policy, temporary grants and revocation.
class SitePermissions final : public PageInfoUI,
                              public content_settings::Observer,
                              public content::WebContentsObserver {
 public:
  SitePermissions(BrowserWindowInterface& browser, WindowSession& session);
  ~SitePermissions() override;
  base::WeakPtr<SitePermissions> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  void Visibility(std::string origin, bool visible);
  KrResult SetPermission(KrPermissionId id, uint32_t setting);
  KrResult Reset(std::string origin);
  std::unique_ptr<permissions::PermissionPrompt> CreatePrompt(
      content::WebContents* contents,
      permissions::PermissionPrompt::Delegate* delegate);
  void Respond(uint64_t request, uint32_t decision);
  void DismissPrompt(uint64_t request);

 private:
  void Close(bool dismiss_surface);
  void ApplyVisibility(std::string origin, bool visible, uint64_t request);
  bool TargetValid() const;
  void Refresh(uint64_t generation);
  void Apply(KrPermissionId id, uint32_t setting, uint64_t generation);
  void ApplyReset(std::string origin, uint64_t generation);
  void ApplyResponse(uint64_t request, uint32_t decision);
  void SetPermissionInfo(const PermissionInfoList& permissions,
                         ChosenObjectInfoList objects) override;
  void OnContentSettingChanged(const ContentSettingsPattern& primary,
                               const ContentSettingsPattern& secondary,
                               ContentSettingsTypeSet types) override;
  void DidFinishNavigation(content::NavigationHandle* handle) override;
  void WebContentsDestroyed() override;
  void OnVisibilityChanged(content::Visibility visibility) override;

  const raw_ref<BrowserWindowInterface> browser_;
  const raw_ref<WindowSession> session_;
  base::ScopedObservation<HostContentSettingsMap, content_settings::Observer>
      settings_observation_{this};
  std::unique_ptr<ChromePageInfoUiDelegate> ui_delegate_;
  std::unique_ptr<PageInfo> presenter_;
  base::flat_map<KrPermissionId, PageInfo::PermissionInfo> permissions_;
  ChosenObjectInfoList objects_;
  std::string origin_;
  GURL page_url_;
  uint64_t generation_ = 0;
  uint64_t visibility_request_ = 0;
  KrPermissionId next_id_ = 0;
  bool refresh_pending_ = false;
  uint64_t next_prompt_ = 0, prompt_ = 0;
  bool prompt_allow_once_ = false;
  base::WeakPtr<permissions::PermissionPrompt::Delegate> prompt_delegate_;
  base::WeakPtr<content::WebContents> prompt_contents_;
  base::WeakPtrFactory<SitePermissions> weak_factory_{this};
};

// Returns null for non-Krypton windows and request variants whose specialized
// Chromium UI (quiet prompts, embedded elements, accuracy choices) must remain.
std::unique_ptr<permissions::PermissionPrompt> CreatePermissionPrompt(
    content::WebContents* contents,
    permissions::PermissionPrompt::Delegate* delegate);
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_SITE_PERMISSIONS_H_

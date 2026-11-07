// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SITE_CUSTOMIZATION_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SITE_CUSTOMIZATION_H_

#include <memory>
#include <string>
#include <string_view>

#include "krypton_ui/abi.h"

class GURL;

namespace content {
class NavigationHandle;
class RenderFrameHost;
}  // namespace content
namespace tabs {
class TabInterface;
}
namespace ui {
class MenuModel;
class ImageModel;
}  // namespace ui
namespace user_prefs {
class PrefRegistrySyncable;
}
namespace krypton_ui {
class BrowserAdapter;
inline constexpr char kSiteCustomizationPref[] =
    "krypton_ui.site_customization";
void RegisterSiteCustomizationPrefs(user_prefs::PrefRegistrySyncable* registry);

// Profile-owned records, with per-window observers and Chromium-owned editors.
// No timers, page scripts, or native focus requests are used to apply styles.
class SiteCustomization {
 public:
  struct BoostState {
    bool available = false;
    bool active = false;
  };
  explicit SiteCustomization(BrowserAdapter& owner);
  ~SiteCustomization();
  void Initialize();
  std::string Glyph(tabs::TabInterface& tab);
  SiteCustomization(const SiteCustomization&) = delete;
  SiteCustomization& operator=(const SiteCustomization&) = delete;
  std::unique_ptr<ui::MenuModel> RoutingMenu(KrLibraryItemId item = 0);
  std::unique_ptr<ui::MenuModel> BoostMenu(KrLibraryItemId item = 0);
  std::unique_ptr<ui::MenuModel> SiteBoostsMenu();
  BoostState BoostForSite(const GURL& url) const;
  void OpenBoost();
  void NavigationReady(content::NavigationHandle& navigation);
  void NavigationFinished(tabs::TabInterface& tab,
                          content::NavigationHandle& navigation);
  void ApplyStyles(content::RenderFrameHost& frame);
  void PublishBoosts();
  void LibraryCommand(uint32_t section, KrLibraryItemId item, uint32_t command);
  void EditTabIcon(tabs::TabInterface& tab);
  KrResult EditBoost(uint32_t kind, const KrBoost* value);
  KrResult SaveRoutingRule(const KrRoutingRule* value);
  void SetTabIcon(KrTabId tab, std::string_view glyph);
  ui::ImageModel Icon(tabs::TabInterface& tab, const ui::ImageModel& fallback);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace krypton_ui
#endif

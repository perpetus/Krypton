#include "chrome/browser/ui/krypton_ui/privacy_features_bridge.h"

#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/browser_service_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "url/gurl.h"

namespace krypton_ui {
bool ShowNativeSurface(BrowserWindowInterface* browser, const GURL& url) {
  if (!url.is_valid() || !(url.SchemeIs("krypton") || url.SchemeIs("helium") ||
                           url.SchemeIs("chrome")))
    return false;
  auto* view = BrowserView::GetBrowserViewForBrowser(browser);
  if (!view || !view->krypton_ui())
    return false;
  if (url.host() == "onboarding" && (url.path().empty() || url.path() == "/"))
    return view->krypton_ui()->ShowOnboarding();
  const auto section = NativeSettingsSection(url);
  return section && OpenNativeSettings(view->krypton_ui()->session().api(),
                                       *section) == KR_OK;
}

bool ShowNativeSettings(BrowserWindowInterface* browser,
                        std::string_view sub_page) {
  const auto section = NativeSettingsSection(
      GURL("krypton://settings/" + std::string(sub_page)));
  if (!section)
    return false;
  auto* view = BrowserView::GetBrowserViewForBrowser(browser);
  if (!view || !view->krypton_ui())
    return false;
  const auto* api = view->krypton_ui()->session().api();
  return OpenNativeSettings(api, *section) == KR_OK;
}
}  // namespace krypton_ui

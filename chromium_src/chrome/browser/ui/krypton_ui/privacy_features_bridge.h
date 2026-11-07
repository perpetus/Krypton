#ifndef CHROME_BROWSER_UI_KRYPTON_UI_PRIVACY_FEATURES_BRIDGE_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_PRIVACY_FEATURES_BRIDGE_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/krypton_privacy_features/registry.h"
#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/prefs/pref_change_registrar.h"

class BrowserWindowInterface;
class GURL;
class PrefService;
namespace krypton_ui {
// Profile choices share the native privacy surface, not the process-wide
// component registry. Keep their IDs outside the registry's range.
inline constexpr KrPrivacyFeatureId kOnlineSearchSuggestionsId = 0x10000;
class PrivacyFeaturesBridge {
 public:
  PrivacyFeaturesBridge(krypton::privacy::Registry* registry,
                        base::RepeatingCallback<const KrShellUI*()> api,
                        base::RepeatingCallback<std::string(int)> strings = {},
                        PrefService* profile_prefs = nullptr);
  ~PrivacyFeaturesBridge();
  void SetVisible(bool visible);
  KrResult SetEnabled(KrPrivacyFeatureId id, bool enabled);

 private:
  void Publish(krypton::privacy::Id id);
  void PublishOnlineSuggestions();
  const base::WeakPtr<krypton::privacy::Registry> registry_;
  base::RepeatingCallback<const KrShellUI*()> api_;
  base::RepeatingCallback<std::string(int)> strings_;
  base::CallbackListSubscription subscription_;
  const raw_ptr<PrefService> profile_prefs_;
  PrefChangeRegistrar profile_observer_;
  bool visible_ = false;
};
std::optional<uint32_t> NativeSettingsSection(const GURL& url);
bool ShowNativeSettings(BrowserWindowInterface* browser,
                        std::string_view sub_page);
bool ShowNativeSurface(BrowserWindowInterface* browser, const GURL& url);
}  // namespace krypton_ui
#endif

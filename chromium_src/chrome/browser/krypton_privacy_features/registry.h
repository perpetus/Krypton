#ifndef CHROME_BROWSER_KRYPTON_PRIVACY_FEATURES_REGISTRY_H_
#define CHROME_BROWSER_KRYPTON_PRIVACY_FEATURES_REGISTRY_H_

#include <cstdint>
#include <string_view>

#include "base/callback_list.h"
#include "base/containers/span.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "components/krypton_privacy_features/privacy_features.h"
#include "components/prefs/pref_change_registrar.h"

namespace krypton::privacy {
struct Feature {
  Id id;
  std::string_view key;
  unsigned group, risk;
  bool restart, onboarding;
  int title, summary, data_shared, risk_detail, when_off;
  std::string_view manage_url;
};
base::span<const Feature> Catalog();
class Registry {
 public:
  explicit Registry(PrefService& prefs);
  ~Registry();
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;
  const Feature* Find(Id id) const;
  uint32_t Flags(Id id) const;
  bool SetEnabled(Id id, bool enabled);
  base::CallbackListSubscription Observe(
      base::RepeatingCallback<void(Id)> callback);
  base::WeakPtr<Registry> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  const raw_ref<PrefService> prefs_;
  PrefChangeRegistrar registrar_;
  base::RepeatingCallbackList<void(Id)> changed_;
  base::WeakPtrFactory<Registry> weak_factory_{this};
};
// Owned by the browser process, independent of native windows and shells.
void InitializeRegistry(PrefService& prefs);
Registry* GetRegistry();
void ShutdownRegistry();
}  // namespace krypton::privacy
#endif

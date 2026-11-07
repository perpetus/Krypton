#include "chrome/browser/krypton_privacy_features/registry.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/grit/generated_resources.h"
#include "components/prefs/pref_service.h"
#include "third_party/widevine/cdm/buildflags.h"

namespace krypton::privacy {
namespace {
constexpr Feature kCatalog[]{
#if BUILDFLAG(ENABLE_WIDEVINE_CDM_COMPONENT)
    {Id::kWidevine, "widevine", 0, 2, true, true,
     IDS_KRYPTON_PRIVACY_WIDEVINE_TITLE, IDS_KRYPTON_PRIVACY_WIDEVINE_SUMMARY,
     IDS_KRYPTON_PRIVACY_WIDEVINE_SHARED, IDS_KRYPTON_PRIVACY_WIDEVINE_RISK,
     IDS_KRYPTON_PRIVACY_WIDEVINE_OFF, ""},
#endif
    {Id::kLiveCaption, "live_caption", 0, 2, true, false,
     IDS_KRYPTON_PRIVACY_CAPTION_TITLE, IDS_KRYPTON_PRIVACY_CAPTION_SUMMARY,
     IDS_KRYPTON_PRIVACY_CAPTION_SHARED, IDS_KRYPTON_PRIVACY_CAPTION_RISK,
     IDS_KRYPTON_PRIVACY_CAPTION_OFF, ""},
    {Id::kAutoplayList, "autoplay_list", 0, 1, true, false,
     IDS_KRYPTON_PRIVACY_AUTOPLAY_TITLE, IDS_KRYPTON_PRIVACY_AUTOPLAY_SUMMARY,
     IDS_KRYPTON_PRIVACY_AUTOPLAY_SHARED, IDS_KRYPTON_PRIVACY_AUTOPLAY_RISK,
     IDS_KRYPTON_PRIVACY_AUTOPLAY_OFF, ""}};
std::unique_ptr<Registry>& Instance() {
  static base::NoDestructor<std::unique_ptr<Registry>> instance;
  return *instance;
}
}  // namespace
base::span<const Feature> Catalog() {
  return kCatalog;
}
Registry::Registry(PrefService& prefs) : prefs_(prefs) {
  registrar_.Init(&prefs);
  for (const auto& feature : Catalog()) {
    if (!prefs.FindPreference(PrefName(feature.id)))
      continue;
    registrar_.Add(
        PrefName(feature.id),
        base::BindRepeating(
            [](Registry* registry, Id id) { registry->changed_.Notify(id); },
            base::Unretained(this), feature.id));
  }
}
Registry::~Registry() = default;
const Feature* Registry::Find(Id id) const {
  if (PrefName(id).empty() || !prefs_->FindPreference(PrefName(id)))
    return nullptr;
  const auto catalog = Catalog();
  const auto it = std::ranges::find(catalog, id, &Feature::id);
  return it == catalog.end() ? nullptr : &*it;
}
uint32_t Registry::Flags(Id id) const {
  const auto* feature = Find(id);
  if (!feature)
    return 0;
  const bool enabled = IsEnabled(*prefs_, id);
  uint32_t flags = enabled ? 1u : 0u;
  if (feature->restart) {
    flags |= 1u << 1;
    if (enabled != IsStartupEnabled(id))
      flags |= 1u << 2;
  }
  if (prefs_->FindPreference(PrefName(id))->IsManaged())
    flags |= 1u << 3;
  if (feature->onboarding)
    flags |= 1u << 4;
  return flags;
}
bool Registry::SetEnabled(Id id, bool enabled) {
  if (!Find(id) || prefs_->FindPreference(PrefName(id))->IsManaged())
    return false;
  prefs_->SetBoolean(PrefName(id), enabled);
  return true;
}
base::CallbackListSubscription Registry::Observe(
    base::RepeatingCallback<void(Id)> callback) {
  return changed_.Add(std::move(callback));
}
void InitializeRegistry(PrefService& prefs) {
  if (!Instance())
    Instance() = std::make_unique<Registry>(prefs);
}
Registry* GetRegistry() {
  return Instance().get();
}
void ShutdownRegistry() {
  Instance().reset();
}
}  // namespace krypton::privacy

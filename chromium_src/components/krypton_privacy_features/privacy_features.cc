#include "components/krypton_privacy_features/privacy_features.h"

#include <algorithm>
#include <optional>

#include "components/krypton_privacy_features/speech_component_ids.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace krypton::privacy {
namespace {
constexpr std::array<std::string_view, kFeatureCount> kPrefs{
    "krypton.privacy_features.widevine",
    "krypton.privacy_features.live_caption",
    "krypton.privacy_features.password_manager",
    "krypton.privacy_features.touch_ui",
    "krypton.privacy_features.fedcm_prompts",
    "krypton.privacy_features.download_warnings",
    "krypton.privacy_features.qr_code",
    "krypton.privacy_features.reading_mode",
    "krypton.privacy_features.reading_list",
    "krypton.privacy_features.autoplay_list",
    "krypton.privacy_features.cast",
    "krypton.privacy_features.web_push",
    "krypton.privacy_features.screen_ai"};
std::optional<StartupSnapshot>& Snapshot() {
  static std::optional<StartupSnapshot> snapshot;
  return snapshot;
}
}  // namespace
std::string_view PrefName(Id id) {
  const auto index = static_cast<unsigned>(id);
  return index && index <= kPrefs.size() ? kPrefs[index - 1]
                                         : std::string_view{};
}
void RegisterLocalState(PrefRegistrySimple* registry) {
  for (const auto name : kPrefs)
    registry->RegisterBooleanPref(name, false);
}
bool IsEnabled(const PrefService& prefs, Id id) {
  const auto name = PrefName(id);
  const auto* pref = name.empty() ? nullptr : prefs.FindPreference(name);
  return pref && pref->GetValue()->is_bool() && prefs.GetBoolean(name);
}
StartupSnapshot::StartupSnapshot(const PrefService& prefs) {
  for (unsigned i = 0; i < values_.size(); ++i)
    values_[i] = privacy::IsEnabled(prefs, static_cast<Id>(i + 1));
}
bool StartupSnapshot::IsEnabled(Id id) const {
  const auto index = static_cast<unsigned>(id);
  return index && index <= values_.size() && values_[index - 1];
}
void CaptureStartupSnapshot(const PrefService& prefs) {
  if (!Snapshot())
    Snapshot().emplace(prefs);
}
bool IsStartupEnabled(Id id) {
  return Snapshot() && Snapshot()->IsEnabled(id);
}
bool ComponentAllowed(std::string_view id, bool helium_allowed) {
  return ComponentAllowed(id, helium_allowed, IsStartupEnabled(Id::kWidevine),
                          IsStartupEnabled(Id::kLiveCaption),
                          IsStartupEnabled(Id::kAutoplayList));
}
bool ComponentAllowed(std::string_view id,
                      bool helium_allowed,
                      bool widevine_enabled,
                      bool live_caption_enabled,
                      bool autoplay_enabled) {
  if (id == "laoigpblnllgcgjnjnllmfolckpjlhki")
    return autoplay_enabled;
  if (id == "oimompecagnajdejgnnjijobebaeigek")
    return widevine_enabled;
  if (std::ranges::find(kLiveCaptionComponents, id) !=
      kLiveCaptionComponents.end())
    return live_caption_enabled;
  return helium_allowed;
}
bool UsesUpstreamComponentServer(bool helium_consent) {
  return UsesUpstreamComponentServer(
      helium_consent, IsStartupEnabled(Id::kWidevine),
      IsStartupEnabled(Id::kLiveCaption), IsStartupEnabled(Id::kAutoplayList));
}
bool UsesUpstreamComponentServer(bool helium_consent,
                                 bool widevine_enabled,
                                 bool live_caption_enabled,
                                 bool autoplay_enabled) {
  return helium_consent || widevine_enabled || live_caption_enabled ||
         autoplay_enabled;
}
}  // namespace krypton::privacy

#ifndef COMPONENTS_KRYPTON_PRIVACY_FEATURES_PRIVACY_FEATURES_H_
#define COMPONENTS_KRYPTON_PRIVACY_FEATURES_PRIVACY_FEATURES_H_

#include <array>
#include <cstddef>
#include <string_view>
#include "base/component_export.h"

class PrefService;
class PrefRegistrySimple;

namespace krypton::privacy {
// Stable catalog ids, including reserved ids for surfaces not yet supported.
enum class Id : unsigned {
  kWidevine = 1,
  kLiveCaption = 2,
  kPasswordManager = 3,
  kTouchUi = 4,
  kFedcmPrompts = 5,
  kDownloadWarnings = 6,
  kQrCode = 7,
  kReadingMode = 8,
  kReadingList = 9,
  kAutoplayList = 10,
  kCast = 11,
  kWebPush = 12,
  kScreenAi = 13
};
inline constexpr size_t kFeatureCount = 13;
COMPONENT_EXPORT(KRYPTON_PRIVACY_FEATURES) std::string_view PrefName(Id id);
COMPONENT_EXPORT(KRYPTON_PRIVACY_FEATURES)
void RegisterLocalState(PrefRegistrySimple* registry);
COMPONENT_EXPORT(KRYPTON_PRIVACY_FEATURES)
bool IsEnabled(const PrefService& prefs, Id id);

// Missing startup state fails closed. Once captured, edits to saved choices
// cannot change decisions that require a restart during the running session.
class COMPONENT_EXPORT(KRYPTON_PRIVACY_FEATURES) StartupSnapshot {
 public:
  explicit StartupSnapshot(const PrefService& prefs);
  bool IsEnabled(Id id) const;

 private:
  std::array<bool, kFeatureCount> values_{};
};
COMPONENT_EXPORT(KRYPTON_PRIVACY_FEATURES)
void CaptureStartupSnapshot(const PrefService& prefs);
COMPONENT_EXPORT(KRYPTON_PRIVACY_FEATURES) bool IsStartupEnabled(Id id);
COMPONENT_EXPORT(KRYPTON_PRIVACY_FEATURES)
bool ComponentAllowed(std::string_view id, bool helium_allowed);
COMPONENT_EXPORT(KRYPTON_PRIVACY_FEATURES)
bool ComponentAllowed(std::string_view id,
                      bool helium_allowed,
                      bool widevine_enabled,
                      bool live_caption_enabled,
                      bool autoplay_enabled = false);
COMPONENT_EXPORT(KRYPTON_PRIVACY_FEATURES)
bool UsesUpstreamComponentServer(bool helium_consent);
COMPONENT_EXPORT(KRYPTON_PRIVACY_FEATURES)
bool UsesUpstreamComponentServer(bool helium_consent,
                                 bool widevine_enabled,
                                 bool live_caption_enabled,
                                 bool autoplay_enabled);
}  // namespace krypton::privacy
#endif

#include "chrome/browser/ui/krypton_ui/privacy_features_bridge.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/generated_resources.h"
#include "components/prefs/pref_service.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/gurl.h"

namespace krypton_ui {
namespace {
KrStr Borrow(const std::string& text) {
  return {text.data(), text.size()};
}
}  // namespace
PrivacyFeaturesBridge::PrivacyFeaturesBridge(
    krypton::privacy::Registry* registry,
    base::RepeatingCallback<const KrShellUI*()> api,
    base::RepeatingCallback<std::string(int)> strings,
    PrefService* profile_prefs)
    : registry_(registry ? registry->GetWeakPtr() : nullptr),
      api_(std::move(api)),
      strings_(strings ? std::move(strings)
                       : base::BindRepeating(&l10n_util::GetStringUTF8)),
      profile_prefs_(profile_prefs) {}
PrivacyFeaturesBridge::~PrivacyFeaturesBridge() = default;
void PrivacyFeaturesBridge::SetVisible(bool visible) {
  if (visible_ == visible)
    return;
  visible_ = visible;
  subscription_ = {};
  profile_observer_.RemoveAll();
  if (!visible_)
    return;
  const auto* api = api_.Run();
  if (!api || !KR_HAS_SLOT(api, KrShellUI, upsert_privacy_feature) ||
      !api->upsert_privacy_feature)
    return;
  if (registry_) {
    subscription_ = registry_->Observe(base::BindRepeating(
        &PrivacyFeaturesBridge::Publish, base::Unretained(this)));
    for (const auto& feature : krypton::privacy::Catalog())
      Publish(feature.id);
  }
  if (profile_prefs_ &&
      profile_prefs_->FindPreference(prefs::kSearchSuggestEnabled)) {
    if (!profile_observer_.prefs())
      profile_observer_.Init(profile_prefs_);
    profile_observer_.Add(
        prefs::kSearchSuggestEnabled,
        base::BindRepeating(&PrivacyFeaturesBridge::PublishOnlineSuggestions,
                            base::Unretained(this)));
    PublishOnlineSuggestions();
  }
}
void PrivacyFeaturesBridge::PublishOnlineSuggestions() {
  if (!visible_ || !profile_prefs_)
    return;
  const auto* pref =
      profile_prefs_->FindPreference(prefs::kSearchSuggestEnabled);
  const auto* api = api_.Run();
  if (!pref || !api || !KR_HAS_SLOT(api, KrShellUI, upsert_privacy_feature) ||
      !api->upsert_privacy_feature)
    return;
  uint32_t flags = profile_prefs_->GetBoolean(prefs::kSearchSuggestEnabled)
                       ? KR_PRIVACY_FEATURE_ENABLED
                       : 0;
  if (!pref->IsUserModifiable())
    flags |= KR_PRIVACY_FEATURE_MANAGED;
  const auto title = strings_.Run(IDS_KRYPTON_PRIVACY_SEARCH_SUGGEST_TITLE);
  const auto summary = strings_.Run(IDS_KRYPTON_PRIVACY_SEARCH_SUGGEST_SUMMARY);
  const auto shared = strings_.Run(IDS_KRYPTON_PRIVACY_SEARCH_SUGGEST_SHARED);
  const auto risk = strings_.Run(IDS_KRYPTON_PRIVACY_SEARCH_SUGGEST_RISK);
  const auto off = strings_.Run(IDS_KRYPTON_PRIVACY_SEARCH_SUGGEST_OFF);
  const KrPrivacyFeature wire{sizeof(KrPrivacyFeature),
                              kOnlineSearchSuggestionsId,
                              KR_PRIVACY_GROUP_REMOTE,
                              KR_PRIVACY_RISK_MODERATE,
                              flags,
                              20,
                              Borrow(title),
                              Borrow(summary),
                              Borrow(shared),
                              Borrow(risk),
                              Borrow(off),
                              {nullptr, 0}};
  api->upsert_privacy_feature(api->self, &wire);
}
void PrivacyFeaturesBridge::Publish(krypton::privacy::Id id) {
  if (!visible_ || !registry_)
    return;
  const auto* feature = registry_->Find(id);
  const auto* api = api_.Run();
  if (!feature || !api ||
      !KR_HAS_SLOT(api, KrShellUI, upsert_privacy_feature) ||
      !api->upsert_privacy_feature)
    return;
  const auto title = strings_.Run(feature->title);
  const auto summary = strings_.Run(feature->summary);
  const auto shared = strings_.Run(feature->data_shared);
  const auto risk = strings_.Run(feature->risk_detail);
  const auto off = strings_.Run(feature->when_off);
  const KrPrivacyFeature wire{
      sizeof(KrPrivacyFeature),
      static_cast<uint64_t>(id),
      feature->group,
      feature->risk,
      registry_->Flags(id),
      static_cast<uint32_t>(id),
      Borrow(title),
      Borrow(summary),
      Borrow(shared),
      Borrow(risk),
      Borrow(off),
      {feature->manage_url.data(), feature->manage_url.size()}};
  api->upsert_privacy_feature(api->self, &wire);
}
KrResult PrivacyFeaturesBridge::SetEnabled(KrPrivacyFeatureId id,
                                           bool enabled) {
  if (id == kOnlineSearchSuggestionsId) {
    if (!visible_ || !profile_prefs_)
      return KR_ERR_INVALID_ARGUMENT;
    const auto* pref =
        profile_prefs_->FindPreference(prefs::kSearchSuggestEnabled);
    if (!pref || !pref->IsUserModifiable())
      return KR_ERR_INVALID_ARGUMENT;
    const bool previous =
        profile_prefs_->GetBoolean(prefs::kSearchSuggestEnabled);
    profile_prefs_->SetBoolean(prefs::kSearchSuggestEnabled, enabled);
    if (previous == enabled)
      PublishOnlineSuggestions();
    return KR_OK;
  }
  if (!registry_ || id > krypton::privacy::kFeatureCount)
    return KR_ERR_INVALID_ARGUMENT;
  const auto feature = static_cast<krypton::privacy::Id>(id);
  const auto previous_flags = registry_->Flags(feature);
  if (!visible_ || !registry_->SetEnabled(feature, enabled))
    return KR_ERR_INVALID_ARGUMENT;
  // Changed choices are confirmed by the registry observer. A no-op write
  // emits no notification and needs its own acknowledgment.
  if (previous_flags == registry_->Flags(feature))
    Publish(feature);
  return KR_OK;
}
std::optional<uint32_t> NativeSettingsSection(const GURL& url) {
  if (!url.is_valid() || url.host() != "settings" ||
      !(url.SchemeIs("krypton") || url.SchemeIs("helium") ||
        url.SchemeIs("chrome")))
    return std::nullopt;
  const auto path = url.path();
  if (path == "/" || path.empty())
    return KR_SETTINGS_CURRENT;
  if (path == "/privacy" || path == "/privacyAndSecurity")
    return KR_SETTINGS_PRIVACY;
  if (path == "/appearance")
    return KR_SETTINGS_APPEARANCE;
  if (path == "/general")
    return KR_SETTINGS_GENERAL;
  if (path == "/workspaces")
    return KR_SETTINGS_WORKSPACES;
  if (path == "/search")
    return KR_SETTINGS_SEARCH;
  if (path == "/downloads")
    return KR_SETTINGS_DOWNLOADS;
  if (path == "/languages")
    return KR_SETTINGS_LANGUAGES;
  if (path == "/accessibility")
    return KR_SETTINGS_ACCESSIBILITY;
  if (path == "/help" || path == "/about")
    return KR_SETTINGS_ABOUT;
  return std::nullopt;
}
}  // namespace krypton_ui

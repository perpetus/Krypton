#include "chrome/browser/ui/krypton_ui/privacy_features_bridge.h"

#include <cstddef>
#include <limits>
#include <memory>

#include "base/functional/bind.h"
#include "chrome/common/pref_names.h"
#include "components/krypton_privacy_features/speech_component_ids.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "net/net_buildflags.h"
#include "testing/gtest/include/gtest/gtest.h"
#if BUILDFLAG(ENABLE_MDNS)
#include "net/base/address_family.h"
#include "net/base/net_errors.h"
#include "net/dns/mdns_client.h"
#include "net/socket/datagram_server_socket.h"
#endif
#include "url/gurl.h"

namespace krypton_ui {
namespace {
class OnlineSuggestionsTest : public testing::Test {
 protected:
  void SetUp() override {
    // Match Helium's registration; the bridge must never set a default or
    // create a user value merely because the settings panel was opened.
    profile_.registry()->RegisterBooleanPref(prefs::kSearchSuggestEnabled,
                                             false);
    api_.struct_size = sizeof(api_);
    api_.self = this;
    api_.upsert_privacy_feature = [](void* self,
                                     const KrPrivacyFeature* value) {
      auto& state = *static_cast<OnlineSuggestionsTest*>(self);
      EXPECT_EQ(value->id, kOnlineSearchSuggestionsId);
      state.flags_ = value->flags;
      ++state.count_;
      return KR_OK;
    };
    bridge_ = MakeBridge(profile_);
  }
  std::unique_ptr<PrivacyFeaturesBridge> MakeBridge(PrefService& profile) {
    return std::make_unique<PrivacyFeaturesBridge>(
        nullptr,
        base::BindRepeating([](const KrShellUI* api) { return api; }, &api_),
        base::BindRepeating(
            [](int) { return std::string("Suggestion privacy"); }),
        &profile);
  }
  TestingPrefServiceSimple profile_;
  KrShellUI api_{};
  uint32_t flags_ = 0;
  int count_ = 0;
  std::unique_ptr<PrivacyFeaturesBridge> bridge_;
};
TEST_F(OnlineSuggestionsTest, DefaultsOffWithoutChangingTheProfile) {
  EXPECT_EQ(count_, 0);
  bridge_->SetVisible(true);
  EXPECT_EQ(count_, 1);
  EXPECT_EQ(flags_, 0u);
  EXPECT_FALSE(profile_.GetBoolean(prefs::kSearchSuggestEnabled));
  EXPECT_EQ(profile_.GetUserPrefValue(prefs::kSearchSuggestEnabled), nullptr);
}
TEST_F(OnlineSuggestionsTest, ChangesPersistImmediatelyAndArePerProfile) {
  bridge_->SetVisible(true);
  EXPECT_EQ(bridge_->SetEnabled(kOnlineSearchSuggestionsId, true), KR_OK);
  EXPECT_TRUE(profile_.GetBoolean(prefs::kSearchSuggestEnabled));
  EXPECT_EQ(flags_, static_cast<uint32_t>(KR_PRIVACY_FEATURE_ENABLED));
  EXPECT_EQ(count_, 2);
  auto reopened = MakeBridge(profile_);
  reopened->SetVisible(true);
  EXPECT_EQ(flags_, static_cast<uint32_t>(KR_PRIVACY_FEATURE_ENABLED));
  TestingPrefServiceSimple other;
  other.registry()->RegisterBooleanPref(prefs::kSearchSuggestEnabled, false);
  auto other_bridge = MakeBridge(other);
  other_bridge->SetVisible(true);
  EXPECT_EQ(flags_, 0u);
  EXPECT_EQ(bridge_->SetEnabled(kOnlineSearchSuggestionsId, false), KR_OK);
  EXPECT_FALSE(profile_.GetBoolean(prefs::kSearchSuggestEnabled));
}
TEST_F(OnlineSuggestionsTest, ManagedChoicesCannotBeOverridden) {
  profile_.SetManagedPref(prefs::kSearchSuggestEnabled, base::Value(true));
  bridge_->SetVisible(true);
  EXPECT_EQ(flags_, static_cast<uint32_t>(KR_PRIVACY_FEATURE_ENABLED |
                                          KR_PRIVACY_FEATURE_MANAGED));
  EXPECT_EQ(bridge_->SetEnabled(kOnlineSearchSuggestionsId, false),
            KR_ERR_INVALID_ARGUMENT);
  EXPECT_TRUE(profile_.GetBoolean(prefs::kSearchSuggestEnabled));
  profile_.SetManagedPref(prefs::kSearchSuggestEnabled, base::Value(false));
  EXPECT_EQ(flags_, static_cast<uint32_t>(KR_PRIVACY_FEATURE_MANAGED));
  EXPECT_EQ(bridge_->SetEnabled(kOnlineSearchSuggestionsId, true),
            KR_ERR_INVALID_ARGUMENT);
}
TEST_F(OnlineSuggestionsTest, ObservationFollowsPanelAndBridgeLifetime) {
  bridge_->SetVisible(true);
  profile_.SetBoolean(prefs::kSearchSuggestEnabled, true);
  EXPECT_EQ(count_, 2);
  bridge_->SetVisible(false);
  profile_.SetBoolean(prefs::kSearchSuggestEnabled, false);
  EXPECT_EQ(count_, 2);
  bridge_->SetVisible(true);
  EXPECT_EQ(count_, 3);
  EXPECT_EQ(flags_, 0u);
  EXPECT_EQ(bridge_->SetEnabled(kOnlineSearchSuggestionsId, false), KR_OK);
  EXPECT_EQ(count_, 4);  // A no-op still confirms the native pending toggle.
  bridge_.reset();
  profile_.SetBoolean(prefs::kSearchSuggestEnabled, true);
  EXPECT_EQ(count_, 4);
}
TEST_F(OnlineSuggestionsTest, UnregisteredProfileFailsClosed) {
  TestingPrefServiceSimple unregistered;
  auto bridge = MakeBridge(unregistered);
  bridge->SetVisible(true);
  EXPECT_EQ(count_, 0);
  EXPECT_EQ(bridge->SetEnabled(kOnlineSearchSuggestionsId, true),
            KR_ERR_INVALID_ARGUMENT);
}
using krypton::privacy::Id;
class PrivacyFeaturesTest : public testing::Test {
 protected:
  void SetUp() override {
    krypton::privacy::RegisterLocalState(prefs_.registry());
  }
  TestingPrefServiceSimple prefs_;
};
TEST_F(PrivacyFeaturesTest, DefaultsOffAndUnknownIdsFailClosed) {
  for (unsigned i = 1; i <= krypton::privacy::kFeatureCount; ++i)
    EXPECT_FALSE(krypton::privacy::IsEnabled(prefs_, static_cast<Id>(i)));
  EXPECT_FALSE(krypton::privacy::IsEnabled(prefs_, static_cast<Id>(0)));
  EXPECT_FALSE(krypton::privacy::IsEnabled(prefs_, static_cast<Id>(999)));
}
TEST_F(PrivacyFeaturesTest, CompiledMdnsCannotCreateSocketsWithoutConsent) {
#if BUILDFLAG(ENABLE_MDNS)
  net::MDnsClient::SetKryptonDiscoveryEnabledForProcess(false);
  auto client = net::MDnsClient::CreateDefault();
  ASSERT_TRUE(client);
  // No factory exists: any attempt to reach socket construction would fail.
  EXPECT_EQ(client->StartListening(nullptr), net::ERR_NOT_IMPLEMENTED);
  EXPECT_FALSE(client->IsListening());
  // The local-discovery implementation also creates sockets directly rather
  // than through MDnsClient::StartListening. Its factory must fail closed too.
  EXPECT_FALSE(
      net::CreateAndBindMDnsSocket(net::ADDRESS_FAMILY_IPV4, 1, nullptr));
  net::MDnsClient::SetKryptonDiscoveryEnabledForProcess(true);
  EXPECT_TRUE(net::MDnsClient::IsKryptonDiscoveryEnabledForProcess());
  net::MDnsClient::SetKryptonDiscoveryEnabledForProcess(false);
#else
  GTEST_SKIP() << "mDNS support is not compiled in this configuration";
#endif
}
TEST_F(PrivacyFeaturesTest, SnapshotRemainsUnchangedAfterSavedEdits) {
  krypton::privacy::StartupSnapshot snapshot(prefs_);
  prefs_.SetBoolean(krypton::privacy::PrefName(Id::kWidevine), true);
  EXPECT_FALSE(snapshot.IsEnabled(Id::kWidevine));
  krypton::privacy::StartupSnapshot next_start(prefs_);
  EXPECT_TRUE(next_start.IsEnabled(Id::kWidevine));
}
TEST_F(PrivacyFeaturesTest, ProcessSnapshotIsCapturedOnlyOnce) {
  krypton::privacy::CaptureStartupSnapshot(prefs_);
  prefs_.SetBoolean(krypton::privacy::PrefName(Id::kWidevine), true);
  krypton::privacy::CaptureStartupSnapshot(prefs_);
  EXPECT_FALSE(krypton::privacy::IsStartupEnabled(Id::kWidevine));
}
TEST(KryptonPrivacyRegistry, MissingLocalStatePreferencesAreUnavailable) {
  TestingPrefServiceSimple prefs;
  krypton::privacy::Registry registry(prefs);
  EXPECT_EQ(registry.Find(Id::kWidevine), nullptr);
  EXPECT_FALSE(registry.SetEnabled(Id::kWidevine, true));
  EXPECT_EQ(registry.Flags(Id::kWidevine), 0u);
}
TEST_F(PrivacyFeaturesTest,
       OffGatesPreserveHeliumExceptExplicitWidevineDefault) {
  EXPECT_FALSE(krypton::privacy::ComponentAllowed("unknown", false));
  EXPECT_TRUE(krypton::privacy::ComponentAllowed(
      "hfnkpimlhhgieaddgfemjhofmfblmnib", true));
  EXPECT_FALSE(krypton::privacy::ComponentAllowed(
      "oimompecagnajdejgnnjijobebaeigek", true));
  EXPECT_FALSE(krypton::privacy::UsesUpstreamComponentServer(false));
  EXPECT_TRUE(krypton::privacy::UsesUpstreamComponentServer(true));
}
TEST_F(PrivacyFeaturesTest, ComponentsAreAllowedOnlyForTheirNamedFeature) {
  EXPECT_FALSE(krypton::privacy::UsesUpstreamComponentServer(false, false,
                                                             false, false));
  EXPECT_TRUE(
      krypton::privacy::UsesUpstreamComponentServer(true, false, false, false));
  EXPECT_TRUE(
      krypton::privacy::UsesUpstreamComponentServer(false, true, false, false));
  EXPECT_TRUE(
      krypton::privacy::UsesUpstreamComponentServer(false, false, true, false));
  EXPECT_TRUE(
      krypton::privacy::UsesUpstreamComponentServer(false, false, false, true));
  constexpr auto autoplay = "laoigpblnllgcgjnjnllmfolckpjlhki";
  EXPECT_FALSE(krypton::privacy::ComponentAllowed(autoplay, true));
  EXPECT_FALSE(krypton::privacy::ComponentAllowed(autoplay, true, true, true));
  EXPECT_TRUE(
      krypton::privacy::ComponentAllowed(autoplay, false, false, false, true));
  EXPECT_FALSE(krypton::privacy::ComponentAllowed("unrelated-component", false,
                                                  false, false, true));
  for (const auto id : krypton::privacy::kLiveCaptionComponents) {
    EXPECT_FALSE(krypton::privacy::ComponentAllowed(id, false, false, false));
    EXPECT_FALSE(krypton::privacy::ComponentAllowed(id, false, true, false));
    EXPECT_TRUE(krypton::privacy::ComponentAllowed(id, false, false, true));
  }
  EXPECT_FALSE(krypton::privacy::ComponentAllowed(
      "oimompecagnajdejgnnjijobebaeigek", true, false, true));
  EXPECT_TRUE(krypton::privacy::ComponentAllowed(
      "oimompecagnajdejgnnjijobebaeigek", true, true, false));
  EXPECT_FALSE(krypton::privacy::ComponentAllowed("unrelated-component", false,
                                                  true, true));
}
TEST_F(PrivacyFeaturesTest,
       OnlySupportedCatalogCanBeChangedAndManagedChoicesAreRejected) {
  krypton::privacy::Registry registry(prefs_);
  EXPECT_NE(registry.Find(Id::kWidevine), nullptr);
  EXPECT_EQ(registry.Find(Id::kReadingMode), nullptr);
  EXPECT_FALSE(registry.SetEnabled(Id::kReadingMode, true));
  EXPECT_TRUE(registry.SetEnabled(Id::kWidevine, true));
  EXPECT_NE(registry.Flags(Id::kWidevine) & KR_PRIVACY_FEATURE_RESTART_PENDING,
            0u);
  prefs_.SetManagedPref(std::string(krypton::privacy::PrefName(Id::kWidevine)),
                        base::Value(false));
  EXPECT_FALSE(registry.SetEnabled(Id::kWidevine, true));
  EXPECT_NE(registry.Flags(Id::kWidevine) & KR_PRIVACY_FEATURE_MANAGED, 0u);
}
TEST_F(PrivacyFeaturesTest, BridgePublishesOnDemandConfirmsAndStopsObserving) {
  krypton::privacy::Registry registry(prefs_);
  struct State {
    int count = 0;
    uint32_t flags = 0;
  } state;
  KrShellUI api{.struct_size = sizeof(api), .self = &state};
  api.upsert_privacy_feature = [](void* self, const KrPrivacyFeature* value) {
    auto& state = *static_cast<State*>(self);
    ++state.count;
    if (value->id == 1)
      state.flags = value->flags;
    EXPECT_TRUE(value->id == 1 || value->id == 2 || value->id == 10);
    EXPECT_EQ(value->group, static_cast<uint32_t>(KR_PRIVACY_GROUP_REMOTE));
    EXPECT_EQ(value->risk, static_cast<uint32_t>(
                               value->id == 10 ? KR_PRIVACY_RISK_LOW
                                               : KR_PRIVACY_RISK_MODERATE));
    return KR_OK;
  };
  PrivacyFeaturesBridge bridge(
      &registry,
      base::BindRepeating([](const KrShellUI* api) { return api; }, &api),
      base::BindRepeating([](int) { return std::string("Feature text"); }));
  EXPECT_EQ(state.count, 0);
  bridge.SetVisible(true);
  EXPECT_EQ(state.count, 3);
  EXPECT_EQ(bridge.SetEnabled(1, true), KR_OK);
  EXPECT_NE(state.flags & KR_PRIVACY_FEATURE_ENABLED, 0u);
  EXPECT_EQ(bridge.SetEnabled(8, true), KR_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(bridge.SetEnabled(std::numeric_limits<uint64_t>::max(), true),
            KR_ERR_INVALID_ARGUMENT);
  const auto before = state.count;
  bridge.SetVisible(false);
  prefs_.SetBoolean(krypton::privacy::PrefName(Id::kWidevine), false);
  EXPECT_EQ(state.count, before);
  bridge.SetVisible(true);
  EXPECT_EQ(state.count, before + 3);
  EXPECT_EQ(state.flags & KR_PRIVACY_FEATURE_ENABLED, 0u);
}
TEST_F(PrivacyFeaturesTest, OldShellDoesNotReceiveOptionalRecords) {
  krypton::privacy::Registry registry(prefs_);
  KrShellUI api{.struct_size = offsetof(KrShellUI, upsert_privacy_feature)};
  PrivacyFeaturesBridge bridge(
      &registry,
      base::BindRepeating([](const KrShellUI* api) { return api; }, &api));
  bridge.SetVisible(true);
  EXPECT_EQ(bridge.SetEnabled(1, true), KR_OK);
}
TEST_F(PrivacyFeaturesTest, NativeSettingsRecognizesInternalPrivacyUrlsOnly) {
  EXPECT_EQ(NativeSettingsSection(GURL("krypton://settings/")),
            KR_SETTINGS_CURRENT);
  EXPECT_EQ(NativeSettingsSection(GURL("krypton://settings/workspaces")),
            KR_SETTINGS_WORKSPACES);
  EXPECT_EQ(NativeSettingsSection(GURL("krypton://settings/downloads")),
            KR_SETTINGS_DOWNLOADS);
  EXPECT_EQ(NativeSettingsSection(GURL("krypton://settings/search")),
            KR_SETTINGS_SEARCH);
  EXPECT_EQ(NativeSettingsSection(GURL("krypton://settings/languages")),
            KR_SETTINGS_LANGUAGES);
  EXPECT_EQ(NativeSettingsSection(GURL("krypton://settings/accessibility")),
            KR_SETTINGS_ACCESSIBILITY);
  EXPECT_EQ(NativeSettingsSection(GURL("krypton://settings/help")),
            KR_SETTINGS_ABOUT);
  EXPECT_EQ(NativeSettingsSection(GURL("krypton://settings/privacy")),
            KR_SETTINGS_PRIVACY);
  EXPECT_EQ(NativeSettingsSection(GURL("helium://settings/privacy")),
            KR_SETTINGS_PRIVACY);
  EXPECT_FALSE(NativeSettingsSection(GURL("https://settings/privacy")));
  EXPECT_FALSE(NativeSettingsSection(GURL("krypton://settings/unknown")));
}
}  // namespace
}  // namespace krypton_ui

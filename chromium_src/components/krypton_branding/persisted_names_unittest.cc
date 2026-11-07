#include "components/krypton_branding/persisted_names.h"

#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace krypton::branding {
namespace {
constexpr char kName[] = "profile.name";
constexpr char kDefault[] = "profile.using_default_name";

TEST(KryptonPersistedBranding,
     GeneratedNamesRetainTranslatedPrefixesAndSuffixes) {
  EXPECT_EQ(RebrandDefaultProfileName("Your Helium", true), "Your Krypton");
  EXPECT_EQ(RebrandDefaultProfileName("Heliumin", true), "Kryptonin");
  EXPECT_EQ(RebrandDefaultProfileName("\xE1\x8B\xA8Helium", true),
            "\xE1\x8B\xA8Krypton");
  EXPECT_FALSE(RebrandDefaultProfileName("Your Krypton", true));
  EXPECT_FALSE(RebrandDefaultProfileName("Person 2", true));
}

TEST(KryptonPersistedBranding,
     CustomNamesArePreservedEvenWhenTheyMatchTheOldDefault) {
  EXPECT_FALSE(RebrandDefaultProfileName("Your Helium", false));
  EXPECT_FALSE(RebrandDefaultProfileName("Helium research", false));
}

TEST(KryptonPersistedBranding, ProfileAndCacheMigrateConsistentlyAndOnlyOnce) {
  TestingPrefServiceSimple prefs;
  prefs.registry()->RegisterStringPref(kName, "");
  prefs.registry()->RegisterBooleanPref(kDefault, false);
  prefs.SetString(kName, "Your Helium");
  prefs.SetBoolean(kDefault, true);
  base::DictValue cache;
  cache.Set("name", "Your Helium");
  cache.Set("is_using_default_name", true);
  cache.Set("enterprise_label", "Helium company");
  ASSERT_TRUE(MigrateDefaultProfileName(prefs, kName, kDefault));
  ASSERT_TRUE(
      MigrateCachedDefaultProfileName(cache, "name", "is_using_default_name"));
  EXPECT_EQ(prefs.GetString(kName), *cache.FindString("name"));
  EXPECT_EQ(prefs.GetString(kName), "Your Krypton");
  EXPECT_TRUE(prefs.GetBoolean(kDefault));
  EXPECT_EQ(*cache.FindString("enterprise_label"), "Helium company");
  EXPECT_FALSE(MigrateDefaultProfileName(prefs, kName, kDefault));
  EXPECT_FALSE(
      MigrateCachedDefaultProfileName(cache, "name", "is_using_default_name"));
}

TEST(KryptonPersistedBranding, RegisteredDefaultDoesNotRequireASerializedFlag) {
  TestingPrefServiceSimple prefs;
  prefs.registry()->RegisterStringPref(kName, "");
  prefs.registry()->RegisterBooleanPref(kDefault, true);
  prefs.SetString(kName, "Your Helium");
  EXPECT_EQ(prefs.GetUserPrefValue(kDefault), nullptr);
  EXPECT_TRUE(MigrateDefaultProfileName(prefs, kName, kDefault));
  EXPECT_EQ(prefs.GetString(kName), "Your Krypton");
  EXPECT_EQ(prefs.GetUserPrefValue(kDefault), nullptr);
}

TEST(KryptonPersistedBranding,
     UnknownMetadataAndExplicitCustomNamesAreNotChanged) {
  base::DictValue cache;
  cache.Set("name", "Your Helium");
  EXPECT_FALSE(
      MigrateCachedDefaultProfileName(cache, "name", "is_using_default_name"));
  cache.Set("is_using_default_name", false);
  EXPECT_FALSE(
      MigrateCachedDefaultProfileName(cache, "name", "is_using_default_name"));
  EXPECT_EQ(*cache.FindString("name"), "Your Helium");
  TestingPrefServiceSimple prefs;
  EXPECT_FALSE(MigrateDefaultProfileName(prefs, kName, kDefault));
  prefs.registry()->RegisterStringPref(kName, "");
  prefs.registry()->RegisterBooleanPref(kDefault, false);
  prefs.SetString(kName, "Your Helium");
  EXPECT_FALSE(MigrateDefaultProfileName(prefs, kName, kDefault));
  EXPECT_EQ(prefs.GetString(kName), "Your Helium");
}

TEST(KryptonPersistedBranding, PolicyNamesAndPolicyStateArePreserved) {
  TestingPrefServiceSimple prefs;
  prefs.registry()->RegisterStringPref(kName, "");
  prefs.registry()->RegisterBooleanPref(kDefault, false);
  prefs.SetString(kName, "Your Helium");
  prefs.SetBoolean(kDefault, true);
  prefs.SetManagedPref(kName, base::Value("Helium company"));
  EXPECT_FALSE(MigrateDefaultProfileName(prefs, kName, kDefault));
  EXPECT_EQ(prefs.GetString(kName), "Helium company");
  prefs.RemoveManagedPref(kName);
  EXPECT_EQ(prefs.GetString(kName), "Your Helium");
}

TEST(KryptonPersistedBranding, OnlyMatchingGeneratedShortcutsAreRefreshed) {
  EXPECT_TRUE(ShouldRefreshDefaultProfileShortcut(u"Your Helium",
                                                  u"Your Krypton", true));
  EXPECT_FALSE(ShouldRefreshDefaultProfileShortcut(u"Your Helium",
                                                   u"Your Krypton", false));
  EXPECT_FALSE(
      ShouldRefreshDefaultProfileShortcut(u"Your Helium", u"Work", true));
  EXPECT_FALSE(
      ShouldRefreshDefaultProfileShortcut(u"Work", u"Your Krypton", true));
  EXPECT_FALSE(ShouldRefreshDefaultProfileShortcut(u"", u"Your Krypton", true));
  EXPECT_FALSE(ShouldRefreshDefaultProfileShortcut(u"Your Krypton",
                                                   u"Your Krypton", true));
}

TEST(KryptonPersistedBranding, RecommendedProfileLabelsAreNotOverridden) {
  TestingPrefServiceSimple prefs;
  prefs.registry()->RegisterStringPref(kName, "");
  prefs.registry()->RegisterBooleanPref(kDefault, true);
  prefs.SetRecommendedPref(kName, base::Value("Helium company"));
  EXPECT_FALSE(MigrateDefaultProfileName(prefs, kName, kDefault));
  EXPECT_EQ(prefs.GetString(kName), "Helium company");
  EXPECT_EQ(prefs.GetUserPrefValue(kName), nullptr);
}
}  // namespace
}  // namespace krypton::branding

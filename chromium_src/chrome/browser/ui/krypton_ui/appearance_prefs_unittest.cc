#include "chrome/browser/ui/krypton_ui/layout_prefs.h"

#include <cstddef>
#include <cstring>

#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
TEST(KryptonAppearancePrefs, DefaultsAndChoicesRoundTripPerProfile) {
  sync_preferences::TestingPrefServiceSyncable first, second;
  RegisterAppearanceProfilePrefs(first.registry());
  RegisterAppearanceProfilePrefs(second.registry());
  const auto defaults = ReadAppearancePrefs(first);
  EXPECT_EQ(defaults.glass_mode, static_cast<uint64_t>(KR_GLASS_OFF));
  EXPECT_EQ(defaults.icon_pack, static_cast<uint64_t>(KR_ICON_PACK_FLUENT));
  const KrAppearancePrefs chosen{
      sizeof(chosen),           KR_COLOR_DARK,
      KR_MENU_MATERIAL_ACRYLIC, KR_APPEARANCE_HIDE_HOVER_MEMORY,
      KR_ICON_PACK_TABLER,      KR_GLASS_QUALITY};
  ASSERT_TRUE(WriteAppearancePrefs(first, &chosen));
  const auto saved = ReadAppearancePrefs(first);
  EXPECT_EQ(saved.color_scheme, static_cast<uint32_t>(KR_COLOR_DARK));
  EXPECT_EQ(saved.icon_pack, static_cast<uint64_t>(KR_ICON_PACK_TABLER));
  EXPECT_EQ(saved.glass_mode, static_cast<uint64_t>(KR_GLASS_QUALITY));
  EXPECT_EQ(ReadAppearancePrefs(second).icon_pack,
            static_cast<uint64_t>(KR_ICON_PACK_FLUENT));
}
TEST(KryptonAppearancePrefs, GlassModesValidateAndLegacyRecordsDefaultToOff) {
  sync_preferences::TestingPrefServiceSyncable prefs;
  RegisterAppearanceProfilePrefs(prefs.registry());
  for (uint64_t mode = KR_GLASS_OFF; mode <= KR_GLASS_QUALITY; ++mode) {
    KrAppearancePrefs value{
        sizeof(value),       KR_COLOR_DARK, KR_MENU_MATERIAL_DEFAULT, 0,
        KR_ICON_PACK_FLUENT, mode};
    ASSERT_TRUE(WriteAppearancePrefs(prefs, &value));
    EXPECT_EQ(ReadAppearancePrefs(prefs).glass_mode, mode);
  }
  auto invalid = ReadAppearancePrefs(prefs);
  invalid.glass_mode = uint64_t{1} << 32;
  invalid.color_scheme = KR_COLOR_LIGHT;
  EXPECT_FALSE(WriteAppearancePrefs(prefs, &invalid));
  EXPECT_EQ(ReadAppearancePrefs(prefs).color_scheme,
            static_cast<uint32_t>(KR_COLOR_DARK));
  invalid.struct_size = offsetof(KrAppearancePrefs, glass_mode);
  ASSERT_TRUE(WriteAppearancePrefs(prefs, &invalid));
  EXPECT_EQ(ReadAppearancePrefs(prefs).glass_mode,
            static_cast<uint64_t>(KR_GLASS_OFF));
}

TEST(KryptonAppearancePrefs, MalformedRecordsAreRejectedWithoutPartialWrites) {
  sync_preferences::TestingPrefServiceSyncable prefs;
  RegisterAppearanceProfilePrefs(prefs.registry());
  KrAppearancePrefs chosen{sizeof(chosen), KR_COLOR_DARK,
                           KR_MENU_MATERIAL_DEFAULT, 0, KR_ICON_PACK_RADIX + 1};
  EXPECT_FALSE(WriteAppearancePrefs(prefs, nullptr));
  EXPECT_FALSE(WriteAppearancePrefs(prefs, &chosen));
  EXPECT_EQ(ReadAppearancePrefs(prefs).color_scheme,
            static_cast<uint32_t>(KR_COLOR_SYSTEM));
  chosen.icon_pack = uint64_t{1} << 32;
  EXPECT_FALSE(WriteAppearancePrefs(prefs, &chosen));
  EXPECT_EQ(ReadAppearancePrefs(prefs).color_scheme,
            static_cast<uint32_t>(KR_COLOR_SYSTEM));
  chosen.icon_pack = KR_ICON_PACK_LUCIDE;
  chosen.struct_size = offsetof(KrAppearancePrefs, flags);
  EXPECT_FALSE(WriteAppearancePrefs(prefs, &chosen));
}
TEST(KryptonAppearancePrefs, LegacyPaddingDoesNotBecomeAnIconPack) {
  struct LegacyAppearance {
    size_t struct_size;
    uint32_t color_scheme, menu_material, flags;
  };
  static_assert(offsetof(KrAppearancePrefs, icon_pack) >=
                sizeof(LegacyAppearance));
  sync_preferences::TestingPrefServiceSyncable prefs;
  RegisterAppearanceProfilePrefs(prefs.registry());
  KrAppearancePrefs legacy{};
  legacy.struct_size = sizeof(LegacyAppearance);
  legacy.color_scheme = KR_COLOR_DARK;
  constexpr auto padding =
      offsetof(KrAppearancePrefs, flags) + sizeof(legacy.flags);
  if (sizeof(LegacyAppearance) > padding)
    std::memset(reinterpret_cast<unsigned char*>(&legacy) + padding, 0xff,
                sizeof(LegacyAppearance) - padding);
  EXPECT_TRUE(WriteAppearancePrefs(prefs, &legacy));
  EXPECT_EQ(ReadAppearancePrefs(prefs).icon_pack,
            static_cast<uint64_t>(KR_ICON_PACK_FLUENT));
}
}  // namespace krypton_ui

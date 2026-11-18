// Copyright 2026 The Krypton Authors. GPL-3.0.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_TRANSLATION_MENU_MODEL_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_TRANSLATION_MENU_MODEL_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/krypton_translate/installer.h"
#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "chrome/browser/ui/krypton_ui/menu_panel_layout.h"
#include "components/prefs/pref_change_registrar.h"

namespace content {
class WebContents;
}
namespace ui {
class MenuModel;
}
namespace krypton_ui {
inline constexpr KrActionId kTranslateAction = uint64_t{7} << 56;
std::optional<KrAction> TranslationAction(content::WebContents* contents,
                                          std::string& tooltip,
                                          std::string& locale_chip);
base::span<const MenuPanelRow> TranslationPanelLayout();
bool TranslationMenuKeepsOpen(int command);
bool CanShowTranslationPanel(content::WebContents* contents);
std::string_view TranslationMenuGlyph(int command);
std::unique_ptr<ui::MenuModel> CreateTranslationMenu(
    content::WebContents& contents,
    base::RepeatingClosure changed);

class TranslationActions final
    : public on_device_translation::OnDeviceTranslationInstaller::Observer {
 public:
  TranslationActions(Profile& profile, base::RepeatingClosure changed);
  ~TranslationActions() override;
  void OnLanguagePackInstalled(krypton::translation::PackKey) override;
  void OnLanguagePackInstallationChanged(
      krypton::translation::PackKey) override;
  void OnInstallationChanged() override;
  void OnLanguagePackProgress(krypton::translation::PackKey, int) override;

 private:
  base::WeakPtr<krypton::translation::Installer> installer_;
  base::RepeatingClosure changed_;
  PrefChangeRegistrar prefs_;
};
}  // namespace krypton_ui
#endif  // CHROME_BROWSER_UI_KRYPTON_UI_TRANSLATION_MENU_MODEL_H_

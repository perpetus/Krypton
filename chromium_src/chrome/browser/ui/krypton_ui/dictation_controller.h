// Copyright 2026 The Krypton Authors. GPL-3.0.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_DICTATION_CONTROLLER_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_DICTATION_CONTROLLER_H_
#include <memory>
#include <optional>
#include <string>
#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/krypton_speech/installer.h"
#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/prefs/pref_change_registrar.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/views/widget/widget_observer.h"
class BrowserWindowInterface;
class BrowserView;
class Profile;
namespace ui {
class MenuModel;
}
namespace krypton::speech {
class RecognizerClient;
enum class RecognizerError;
}  // namespace krypton::speech
namespace krypton_ui {
class WindowSession;
const krypton_voice::ModelPack* SelectedVoicePack(const Profile& profile);
std::u16string VoiceLanguageName(std::string_view language);
std::u16string VoicePackName(const krypton_voice::ModelPack& pack);
inline constexpr KrActionId kDictationAction = uint64_t{8} << 56;
class DictationController final : public content::WebContentsObserver,
                                  public views::WidgetObserver,
                                  public speech::SodaInstaller::Observer {
 public:
  DictationController(BrowserWindowInterface& browser,
                      BrowserView& view,
                      WindowSession& session,
                      base::RepeatingCallback<bool()> allowed,
                      base::RepeatingClosure changed,
                      base::RepeatingClosure show_menu);
  ~DictationController() override;
  std::optional<KrAction> Action();
  void Start(uint32_t request);
  void Stop(uint32_t request = 0);
  std::unique_ptr<ui::MenuModel> CreateMenu();
  const krypton_voice::ModelPack* SelectedPack() const;
  void SelectPack(std::string id);
  void DownloadSelected();
  void RemoveSelected();
  void OnSodaInstalled(speech::LanguageCode) override;
  void OnSodaInstallError(speech::LanguageCode,
                          speech::SodaInstaller::ErrorCode) override;
  void OnSodaProgress(speech::LanguageCode, int) override;

 private:
  void Prepared(uint64_t generation, uint32_t request, bool ready);
  void Result(std::string text, bool final);
  void Finished(krypton::speech::RecognizerError error);
  void Emit(uint32_t request,
            uint32_t state,
            std::string_view text = {},
            bool final = false,
            std::string_view error = {});
  void DidStartNavigation(content::NavigationHandle* handle) override;
  void OnWidgetActivationChanged(views::Widget*, bool active) override;
  void OnWidgetDestroying(views::Widget*) override;
  const raw_ref<BrowserWindowInterface> browser_;
  const raw_ref<BrowserView> view_;
  const raw_ref<WindowSession> session_;
  base::RepeatingCallback<bool()> allowed_;
  base::RepeatingClosure changed_, show_menu_;
  base::WeakPtr<krypton::speech::Installer> installer_;
  PrefChangeRegistrar prefs_;
  raw_ptr<views::Widget> widget_ = nullptr;
  std::unique_ptr<krypton::speech::RecognizerClient> recognizer_;
  uint32_t request_ = 0;
  uint64_t generation_ = 0;
  bool listening_ = false;
  std::string tooltip_, error_;
  base::WeakPtrFactory<DictationController> weak_factory_{this};
};
}  // namespace krypton_ui
#endif

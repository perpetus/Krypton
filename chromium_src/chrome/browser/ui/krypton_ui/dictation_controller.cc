// Copyright 2026 The Krypton Authors. GPL-3.0.
#include "chrome/browser/ui/krypton_ui/dictation_controller.h"
#include <utility>
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/krypton_speech/recognizer_client.h"
#include "chrome/browser/permissions/system/system_permission_settings.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/local_model_prefs.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/common/pref_names.h"
#include "components/language/core/browser/pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/navigation_handle.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/widget/widget.h"

namespace krypton_ui {
namespace local = krypton::speech;
std::u16string VoiceLanguageName(std::string_view language) {
  return speech::GetLanguageDisplayName(
      local::Locale(language), g_browser_process->GetApplicationLocale());
}
std::u16string VoicePackName(const krypton_voice::ModelPack& pack) {
  return VoiceLanguageName(pack.language) +
         (pack.id.ends_with("-medium") ? u" — Medium"
          : pack.lightweight           ? u" — Tiny (lightweight)"
                                       : u" — Small");
}
namespace {
class DictationMenu final : public ui::SimpleMenuModel,
                            public ui::SimpleMenuModel::Delegate {
 public:
  DictationMenu(base::WeakPtr<DictationController> owner,
                const krypton_voice::ModelPack* selected)
      : ui::SimpleMenuModel(this),
        owner_(std::move(owner)),
        choices_(this),
        selected_(selected) {
    size_t i = 0;
    for (const auto& pack : krypton_voice::kPacks)
      choices_.AddRadioItem(100 + static_cast<int>(i++), VoicePackName(pack),
                            1);
    AddSubMenu(1, u"Voice language", &choices_);
    if (selected_) {
      auto* installer = local::Installer::Get();
      const bool ready = installer && installer->Ready(selected_->id);
      if (ready)
        AddItem(3, u"Remove this voice pack");
      else
        AddItem(2, u"Download from Moonshine AI (≈" +
                       base::NumberToString16(
                           (selected_->download_size + 500000) / 1000000) +
                       u" MB)");
      AddItem(4, u"Voice pack status");
    }
    AddSeparator(ui::NORMAL_SEPARATOR);
    AddItem(5, u"Audio stays on this device. Click the microphone to listen.");
  }
  bool IsCommandIdChecked(int id) const override {
    const int index = id - 100;
    return selected_ && index >= 0 &&
           static_cast<size_t>(index) < std::size(krypton_voice::kPacks) &&
           selected_ == &krypton_voice::kPacks[index];
  }
  bool IsCommandIdEnabled(int id) const override {
    if (id == 2 || id == 3) {
      const auto* installer = local::Installer::Get();
      return owner_ && installer && !installer->IsDownloading() && selected_ &&
             (id == 3 ? installer->Ready(selected_->id)
                      : !installer->Ready(selected_->id));
    }
    return owner_ && id != 4 && id != 5;
  }
  bool IsItemForCommandIdDynamic(int id) const override {
    return id == 2 || id == 4;
  }
  std::u16string GetLabelForCommandId(int id) const override {
    auto* installer = local::Installer::Get();
    if (!selected_ || !installer)
      return u"Voice pack unavailable";
    const int progress = installer->Progress(selected_->id);
    if (progress >= 0)
      return u"Downloading: " + base::NumberToString16(progress) + u"%";
    if (id == 2)
      return installer->Ready(selected_->id)
                 ? u"Voice pack installed"
                 : u"Download from Moonshine AI (≈" +
                       base::NumberToString16(
                           (selected_->download_size + 999999) / 1000000) +
                       u" MB)";
    const auto error = installer->Error(selected_->id);
    return !error.empty() ? base::UTF8ToUTF16(error)
           : installer->Ready(selected_->id)
               ? u"Voice pack ready"
               : u"A local voice pack is required";
  }
  void ExecuteCommand(int id, int) override {
    if (!IsCommandIdEnabled(id))
      return;
    const int index = id - 100;
    if (index >= 0 &&
        static_cast<size_t>(index) < std::size(krypton_voice::kPacks))
      owner_->SelectPack(std::string(krypton_voice::kPacks[index].id));
    else if (id == 2)
      owner_->DownloadSelected();
    else if (id == 3)
      owner_->RemoveSelected();
  }

 private:
  base::WeakPtr<DictationController> owner_;
  ui::SimpleMenuModel choices_;
  const krypton_voice::ModelPack* selected_;
};
}  // namespace
DictationController::DictationController(
    BrowserWindowInterface& browser,
    BrowserView& view,
    WindowSession& session,
    base::RepeatingCallback<bool()> allowed,
    base::RepeatingClosure changed,
    base::RepeatingClosure show_menu)
    : browser_(browser),
      view_(view),
      session_(session),
      allowed_(std::move(allowed)),
      changed_(std::move(changed)),
      show_menu_(std::move(show_menu)) {
  if (auto* installer = local::Installer::Get()) {
    installer_ = installer->GetWeakPtr();
    installer_->AddObserver(this);
  }
  prefs_.Init(browser_->GetProfile()->GetPrefs());
  prefs_.Add(kVoicePackPref, base::BindRepeating(
                                 [](base::WeakPtr<DictationController> self) {
                                   if (!self)
                                     return;
                                   self->Stop();
                                   self->changed_.Run();
                                 },
                                 weak_factory_.GetWeakPtr()));
  prefs_.Add(prefs::kAudioCaptureAllowed,
             base::BindRepeating(
                 [](base::WeakPtr<DictationController> self) {
                   if (!self)
                     return;
                   if (!self->browser_->GetProfile()->GetPrefs()->GetBoolean(
                           prefs::kAudioCaptureAllowed))
                     self->Stop();
                   self->changed_.Run();
                 },
                 weak_factory_.GetWeakPtr()));
}
DictationController::~DictationController() {
  Stop();
  if (installer_)
    installer_->RemoveObserver(this);
}
const krypton_voice::ModelPack* SelectedVoicePack(const Profile& profile) {
  const auto& prefs = *profile.GetPrefs();
  const std::string selected = prefs.FindPreference(kVoicePackPref)
                                   ? prefs.GetString(kVoicePackPref)
                                   : std::string();
  for (const auto& pack : krypton_voice::kPacks)
    if (pack.id == selected)
      return &pack;
  // Detect the spoken language: the UI language, then the user's preferred
  // page languages in order, then English. Nothing downloads until chosen.
  if (const auto* pack =
          local::DefaultPack(g_browser_process->GetApplicationLocale()))
    return pack;
  if (prefs.FindPreference(language::prefs::kAcceptLanguages)) {
    for (const auto& language : base::SplitString(
             prefs.GetString(language::prefs::kAcceptLanguages), ",",
             base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY))
      if (const auto* pack = local::DefaultPack(language))
        return pack;
  }
  return local::DefaultPack("en");
}
const krypton_voice::ModelPack* DictationController::SelectedPack() const {
  return SelectedVoicePack(*browser_->GetProfile());
}
std::optional<KrAction> DictationController::Action() {
  if (browser_->GetProfile()->IsOffTheRecord() || !installer_)
    return {};
  const auto* pack = SelectedPack();
  uint32_t flags = KR_ACTION_VISIBLE;
  if (browser_->GetProfile()->GetPrefs()->GetBoolean(
          prefs::kAudioCaptureAllowed))
    flags |= KR_ACTION_ENABLED;
  if (listening_)
    flags |= KR_ACTION_ACTIVE | KR_ACTION_ATTENTION;
  const int progress = pack ? installer_->Progress(pack->id) : -1;
  tooltip_ = listening_ ? "Listening locally. Click again or press Esc to stop."
             : !error_.empty()
                 ? error_
                 : "Type with your voice privately on this device";
  constexpr static char label[] = "Voice typing";
  KrAction action{};
  action.struct_size = sizeof(action);
  action.id = kDictationAction;
  action.kind = KR_ACTION_DICTATION;
  action.flags = flags;
  action.label = {label, sizeof(label) - 1};
  action.tooltip = {tooltip_.data(), tooltip_.size()};
  action.progress = progress < 0 ? -1 : static_cast<float>(progress) / 100;
  return action;
}
void DictationController::Start(uint32_t request) {
  Stop();
  if (!request)
    return;
  request_ = request;
  if (!allowed_.Run()) {
    Stop(request);
    return;
  }
  if (!installer_ || browser_->GetProfile()->IsOffTheRecord() ||
      !browser_->GetProfile()->GetPrefs()->GetBoolean(
          prefs::kAudioCaptureAllowed)) {
    Emit(request, KR_DICTATION_ERROR, {}, false,
         "Voice typing is disabled in this profile.");
    Stop(request);
    return;
  }
  if (system_permission_settings::IsDenied(
          ContentSettingsType::MEDIASTREAM_MIC)) {
    Finished(local::RecognizerError::kRecognitionFailed);
    return;
  }
  const auto* pack = SelectedPack();
  if (!pack) {
    Emit(request, KR_DICTATION_NEEDS_MODEL, {}, false,
         "Choose a supported voice language.");
    show_menu_.Run();
    return;
  }
  error_.clear();
  Observe(browser_->GetTabStripModel()->GetActiveWebContents());
  widget_ = view_->GetWidget();
  if (!widget_ || !widget_->IsActive()) {
    Stop(request);
    return;
  }
  widget_->AddObserver(this);
  const auto generation = ++generation_;
  installer_->Prepare(
      std::string(pack->id),
      base::BindOnce(&DictationController::Prepared, weak_factory_.GetWeakPtr(),
                     generation, request));
}
void DictationController::Prepared(uint64_t generation,
                                   uint32_t request,
                                   bool ready) {
  if (generation != generation_ || request != request_ || !allowed_.Run()) {
    Stop(request);
    return;
  }
  if (!ready) {
    Emit(request, KR_DICTATION_NEEDS_MODEL, {}, false,
         "Download a voice pack to use local voice typing. Nothing downloads "
         "automatically.");
    show_menu_.Run();
    return;
  }
  const auto* pack = SelectedPack();
  if (!pack) {
    Stop(request);
    return;
  }
  recognizer_ = std::make_unique<local::RecognizerClient>(
      *browser_->GetProfile(), local::RecognitionLanguage(*pack),
      base::BindRepeating(&DictationController::Result,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&DictationController::Finished,
                     weak_factory_.GetWeakPtr()));
  listening_ = true;
  Emit(request, KR_DICTATION_LISTENING);
  changed_.Run();
  if (request_ == request && recognizer_)
    recognizer_->Start();
}
void DictationController::Stop(uint32_t request) {
  if (request && request != request_)
    return;
  if (!request_ && !recognizer_ && !widget_)
    return;
  const auto previous = std::exchange(request_, 0);
  ++generation_;
  listening_ = false;
  recognizer_.reset();
  Observe(nullptr);
  if (widget_) {
    widget_->RemoveObserver(this);
    widget_ = nullptr;
  }
  if (previous)
    Emit(previous, KR_DICTATION_IDLE);
  changed_.Run();
}
void DictationController::Result(std::string text, bool final) {
  if (!request_ || !allowed_.Run()) {
    Stop();
    return;
  }
  if (text.size() > 32768 || !base::IsStringUTF8(text)) {
    Finished(local::RecognizerError::kRecognitionFailed);
    return;
  }
  Emit(request_, KR_DICTATION_LISTENING, text, final);
  if (final)
    Stop();
}
void DictationController::Finished(local::RecognizerError error) {
  const auto request = request_;
  if (request && error != local::RecognizerError::kNone) {
    if (system_permission_settings::IsDenied(
            ContentSettingsType::MEDIASTREAM_MIC)) {
      error_ =
          "Windows has blocked desktop microphone access. Enable it in "
          "Settings > Privacy & security > Microphone.";
    } else if (error == local::RecognizerError::kNoInputDevice) {
      error_ =
          "No usable microphone was found. Check the Windows input device.";
    } else if (error == local::RecognizerError::kServiceUnavailable) {
      error_ = "The local speech service could not start.";
    } else {
      error_ =
          "Voice typing could not start or continue. Check the microphone "
          "and the installed voice pack.";
    }
    Emit(request, KR_DICTATION_ERROR, {}, false, error_);
  }
  Stop();
}
void DictationController::Emit(uint32_t request,
                               uint32_t state,
                               std::string_view text,
                               bool final,
                               std::string_view error) {
  const auto* api = session_->api();
  if (!api || !KR_HAS_SLOT(api, KrShellUI, set_dictation) ||
      !api->set_dictation)
    return;
  const KrDictationUpdate update{sizeof(KrDictationUpdate),
                                 request,
                                 state,
                                 final ? KR_DICTATION_FINAL : 0u,
                                 {text.data(), text.size()},
                                 {error.data(), error.size()}};
  std::ignore = api->set_dictation(api->self, &update);
}
void DictationController::SelectPack(std::string id) {
  Stop();
  for (const auto& pack : krypton_voice::kPacks) {
    if (pack.id != id)
      continue;
    browser_->GetProfile()->GetPrefs()->SetString(kVoicePackPref, id);
    if (installer_)
      installer_->Prepare(
          id, base::BindOnce(
                  [](base::WeakPtr<DictationController> self, bool) {
                    if (self)
                      self->changed_.Run();
                  },
                  weak_factory_.GetWeakPtr()));
    return;
  }
}
void DictationController::DownloadSelected() {
  const auto* pack = SelectedPack();
  if (installer_ && pack)
    installer_->RequestDownload(*browser_->GetProfile(), std::string(pack->id));
}
void DictationController::RemoveSelected() {
  Stop();
  const auto* pack = SelectedPack();
  if (installer_ && pack)
    installer_->RemovePack(std::string(pack->id));
}
std::unique_ptr<ui::MenuModel> DictationController::CreateMenu() {
  return std::make_unique<DictationMenu>(weak_factory_.GetWeakPtr(),
                                         SelectedPack());
}
void DictationController::DidStartNavigation(
    content::NavigationHandle* handle) {
  if (handle->IsInPrimaryMainFrame())
    Stop();
}
void DictationController::OnWidgetActivationChanged(views::Widget*,
                                                    bool active) {
  if (!active)
    Stop();
}
void DictationController::OnWidgetDestroying(views::Widget*) {
  widget_ = nullptr;
  Stop();
}
void DictationController::OnSodaInstalled(speech::LanguageCode) {
  changed_.Run();
}
void DictationController::OnSodaInstallError(speech::LanguageCode,
                                             speech::SodaInstaller::ErrorCode) {
  changed_.Run();
}
void DictationController::OnSodaProgress(speech::LanguageCode, int) {
  changed_.Run();
}
}  // namespace krypton_ui

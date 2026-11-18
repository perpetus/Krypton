// Copyright 2026 The Krypton Authors. GPL-3.0.
#include "chrome/browser/ui/krypton_ui/translation_menu_model.h"

#include <algorithm>
#include <array>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/i18n/language_tag.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/krypton_translate/model_packs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/browser_service_model.h"
#include "chrome/browser/ui/krypton_ui/translation_bridge.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/language/core/browser/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/translate/core/browser/language_state.h"
#include "components/translate/core/browser/translate_manager.h"
#include "components/translate/core/browser/translate_pref_names.h"
#include "components/translate/core/browser/translate_prefs.h"
#include "components/translate/core/browser/translate_ui_delegate.h"
#include "components/translate/core/browser/translation_provider.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {
namespace {
namespace local = krypton::translation;
const char kChoiceKey = 0;
struct Choices final : base::SupportsUserData::Data {
  content::WeakDocumentPtr document;
  std::string source, target;
  bool failed = false;
  bool automatic_checked = false;
  bool automatic_pending = false;
  bool automatic_attempted = false;
  bool automatic_suppressed = false;
};
Choices& Choice(content::WebContents& contents) {
  auto* value = static_cast<Choices*>(contents.GetUserData(&kChoiceKey));
  if (!value || value->document.AsRenderFrameHostIfValid() !=
                    contents.GetPrimaryMainFrame()) {
    auto next = std::make_unique<Choices>();
    next->document = contents.GetPrimaryMainFrame()->GetWeakDocumentPtr();
    value = next.get();
    contents.SetUserData(&kChoiceKey, std::move(next));
  }
  return *value;
}
translate::TranslateManager* Manager(content::WebContents& contents) {
  return ChromeTranslateClient::GetManagerFromWebContents(&contents);
}
std::pair<std::string, std::string> Languages(content::WebContents& contents) {
  auto& choice = Choice(contents);
  std::string source, target;
  if (auto* client = ChromeTranslateClient::FromWebContents(&contents))
    client->GetTranslateLanguages(&contents, &source, &target);
  if (!choice.source.empty())
    source = choice.source;
  if (!choice.target.empty())
    target = choice.target;
  if (target.empty())
    target = "en";
  return {local::CanonicalLanguage(source), local::CanonicalLanguage(target)};
}
std::u16string Name(std::string_view code) {
  if (code.empty() || code == "und" || code == "auto")
    return u"Unknown";
  return l10n_util::GetDisplayNameForLocale(
      std::string(code), g_browser_process->GetApplicationLocale(), true);
}
bool Eligible(content::WebContents* contents) {
  if (!translate::HasTranslationProvider() || !contents ||
      !Manager(*contents) || !contents->GetPrimaryMainFrame() ||
      !contents->GetLastCommittedURL().SchemeIsHTTPOrHTTPS())
    return false;
  auto* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
  return !profile->IsOffTheRecord() &&
         translate::TranslatePrefs(profile->GetPrefs())
             .IsTranslateAllowedByPolicy();
}
bool IsForeignLanguage(const translate::LanguageState& state,
                       const translate::TranslatePrefs& prefs) {
  const auto detected = local::CanonicalLanguage(state.source_language());
  if (!state.page_level_translation_criteria_met() || detected.empty() ||
      detected == "und" || detected == "auto" ||
      detected ==
          local::CanonicalLanguage(g_browser_process->GetApplicationLocale())) {
    return false;
  }
  return std::ranges::none_of(
      prefs.GetLanguageList(), [&](const auto& language) {
        return local::CanonicalLanguage(language.tag_string()) == detected;
      });
}

// The base browser registers offer-to-translate as off by default, so only an
// explicit user or policy value declines automatic local translation.
// Eligible() separately enforces the translate policy.
bool OffersTranslation(const PrefService& prefs) {
  const auto* pref =
      prefs.FindPreference(translate::prefs::kOfferTranslateEnabled);
  return !pref || pref->IsDefaultValue() || pref->GetValue()->GetBool();
}

bool CanAutomaticallyTranslate(content::WebContents& contents) {
  if (!Eligible(&contents))
    return false;
  const auto& state = *Manager(contents)->GetLanguageState();
  auto& choices = Choice(contents);
  auto* profile = Profile::FromBrowserContext(contents.GetBrowserContext());
  translate::TranslatePrefs prefs(profile->GetPrefs());
  const auto [source, target] = Languages(contents);
  return !choices.automatic_suppressed && !choices.failed &&
         !state.translation_declined() && !state.translation_pending() &&
         !state.IsPageTranslated() && IsForeignLanguage(state, prefs) &&
         OffersTranslation(*profile->GetPrefs()) &&
         !prefs.IsBlockedLanguage(source) &&
         !prefs.IsSiteOnNeverPromptList(
             contents.GetLastCommittedURL().host()) &&
         local::RequiredPacks(source, target).has_value();
}

void MaybeTranslateInstalled(content::WebContents& contents) {
  if (!CanAutomaticallyTranslate(contents))
    return;
  auto* installer = local::Installer::Get();
  auto& choices = Choice(contents);
  if (!installer || choices.automatic_pending || choices.automatic_attempted)
    return;
  const auto [source, target] = Languages(contents);
  const auto required = local::RequiredPacks(source, target);
  const auto installed = installer->InstalledLanguagePacks();
  if (choices.automatic_checked &&
      !std::ranges::all_of(*required,
                           [&](auto key) { return installed.contains(key); }))
    return;
  choices.automatic_pending = true;
  installer->PreparePair(
      source, target,
      base::BindOnce(
          [](content::WeakDocumentPtr document, std::string source,
             std::string target, bool ready) {
            auto* frame = document.AsRenderFrameHostIfValid();
            if (!frame)
              return;
            auto* contents = content::WebContents::FromRenderFrameHost(frame);
            if (!contents || frame != contents->GetPrimaryMainFrame())
              return;
            auto& choices = Choice(*contents);
            choices.automatic_pending = false;
            choices.automatic_checked = true;
            if (!ready || !CanAutomaticallyTranslate(*contents) ||
                Languages(*contents) != std::pair(source, target))
              return;
            choices.automatic_attempted = true;
            translate::TranslateUIDelegate(Manager(*contents)->GetWeakPtr(),
                                           source, target)
                .Translate();
          },
          contents.GetPrimaryMainFrame()->GetWeakDocumentPtr(), source,
          target));
}

enum Command {
  kHeading = 41000,
  kSettingsMenu,
  kSourceMenu,
  kTargetMenu,
  kFooter,
  kTranslate,
  kOriginal,
  kAlways,
  kNeverLanguage,
  kNeverSite,
  kManageLanguages,
  kStatus,
  kSourceCaption,
  kTargetCaption,
  kProblem,
  kNotNow,
  kSourceStart = 41200,
  kTargetStart = 41300,
  kDownloadStart = 41400,
};
constexpr uint64_t kMegabyte = 1024 * 1024;

// Captions and notices are panel titles: always readable, never commands.
bool IsTitle(int command) {
  return command == kSourceCaption || command == kTargetCaption ||
         command == kStatus || command == kProblem;
}
std::u16string Megabytes(uint64_t bytes) {
  return base::NumberToString16((bytes + kMegabyte - 1) / kMegabyte) + u" MB";
}
// Settings > Languages manages local packs. The WebUI page remains for a UI
// module that cannot open native settings.
void OpenLanguageSettings(content::WebContents& contents) {
  auto* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(&contents);
  auto* view =
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
  if (view && view->krypton_ui() &&
      OpenNativeSettings(view->krypton_ui()->session().api(),
                         KR_SETTINGS_LANGUAGES) == KR_OK) {
    return;
  }
  contents.OpenURL(content::OpenURLParams(
                       GURL("chrome://krypton-models/"), content::Referrer(),
                       WindowOpenDisposition::kNewForegroundTab,
                       ui::PAGE_TRANSITION_LINK, false),
                   {});
}

// Layout follows Zen's panel: header and settings, captioned language
// dropdowns, one message box and the footer buttons. Downloads stay separate
// rows, because only that named action may use the network.
class TranslationMenu final : public ui::SimpleMenuModel,
                              public ui::SimpleMenuModel::Delegate {
 public:
  TranslationMenu(content::WebContents& contents,
                  base::RepeatingClosure changed)
      : ui::SimpleMenuModel(this),
        document_(contents.GetPrimaryMainFrame()->GetWeakDocumentPtr()),
        contents_(&contents),
        changed_(std::move(changed)),
        heading_(this),
        settings_(this),
        source_menu_(this),
        target_menu_(this),
        footer_(this) {
    languages_.push_back("en");
    for (const auto& pack : local::ModelPacks())
      languages_.push_back(std::string(pack.language));
    std::ranges::sort(languages_, [](const auto& a, const auto& b) {
      return Name(a) < Name(b);
    });
    auto recent = translate::TranslatePrefs(
                      Profile::FromBrowserContext(contents.GetBrowserContext())
                          ->GetPrefs())
                      .GetRecentTargetLanguages();
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
      const auto found =
          std::ranges::find(languages_, local::CanonicalLanguage(*it));
      if (found != languages_.end())
        std::rotate(languages_.begin(), found, found + 1);
    }
    for (size_t i = 0; i < languages_.size(); ++i) {
      source_menu_.AddCheckItem(kSourceStart + static_cast<int>(i),
                                Name(languages_[i]));
      target_menu_.AddCheckItem(kTargetStart + static_cast<int>(i),
                                Name(languages_[i]));
    }
    settings_.AddCheckItem(kAlways, u"Always translate");
    settings_.AddCheckItem(kNeverLanguage, u"Never translate");
    settings_.AddCheckItem(kNeverSite, u"Never translate this site");
    settings_.AddSeparator(ui::NORMAL_SEPARATOR);
    settings_.AddItem(kManageLanguages, u"Manage languages");
    heading_.AddSubMenu(kSettingsMenu, u"Translation settings", &settings_);
    AddSubMenu(kHeading, u"Translate this page?", &heading_);
    AddItem(kSourceCaption, u"Translate from");
    AddSubMenu(kSourceMenu, u"Translate from", &source_menu_);
    AddItem(kTargetCaption, u"Translate to");
    AddSubMenu(kTargetMenu, u"Translate to", &target_menu_);
    AddItem(kProblem, u"Translation problem");
    AddItem(kStatus, u"Language packs");
    // Each pack keeps its own command ID even when the selected pair changes.
    for (const auto& pack : local::ModelPacks()) {
      AddItem(kDownloadStart + static_cast<int>(pack.key),
              u"Download language pack");
    }
    footer_.AddItem(kOriginal, u"Show original");
    footer_.AddItem(kNotNow, u"Not now");
    footer_.AddItem(kTranslate, u"Translate");
    AddSubMenu(kFooter, u"", &footer_);
    UpdatePair();
  }

  ItemType GetTypeAt(size_t index) const override {
    return IsTitle(GetCommandIdAt(index))
               ? TYPE_TITLE
               : ui::SimpleMenuModel::GetTypeAt(index);
  }
  std::u16string GetSecondaryLabelAt(size_t index) const override {
    const int command = GetCommandIdAt(index);
    if (!Valid() || (command != kStatus && command != kProblem))
      return ui::SimpleMenuModel::GetSecondaryLabelAt(index);
    return command == kProblem ? ProblemDetail() : StatusDetail();
  }
  std::u16string GetAccessibleNameAt(size_t index) const override {
    const int command = GetCommandIdAt(index);
    if (Valid() && command == kSourceMenu)
      return u"Translate from: " + Name(source_);
    if (Valid() && command == kTargetMenu)
      return u"Translate to: " + Name(target_);
    return ui::SimpleMenuModel::GetAccessibleNameAt(index);
  }

  bool IsCommandIdChecked(int command) const override {
    if (!Valid())
      return false;
    if (auto language = SelectedLanguage(command, kTargetStart))
      return *language == target_;
    if (auto language = SelectedLanguage(command, kSourceStart))
      return *language == source_;
    if (command == kAlways)
      return delegate_->ShouldAlwaysTranslate();
    if (command == kNeverLanguage)
      return delegate_->IsLanguageBlocked();
    if (command == kNeverSite)
      return delegate_->IsSiteOnNeverPromptList();
    return false;
  }
  bool IsCommandIdVisible(int command) const override {
    if (!Valid())
      return false;
    if (command == kOriginal)
      return State().IsPageTranslated();
    if (command == kProblem)
      return !ProblemHeadline().empty();
    if (command == kStatus)
      return ProblemHeadline().empty() &&
             (State().translation_pending() || !Ready());
    if (command >= kDownloadStart) {
      const auto key = DownloadKey(command);
      const auto* installer = local::Installer::Get();
      return keys_.contains(key) && installer &&
             !installer->InstalledLanguagePacks().contains(key);
    }
    return true;
  }
  bool IsCommandIdEnabled(int command) const override {
    if (!Valid())
      return false;
    // Titles are drawn at full strength; the renderer never makes them
    // interactive.
    if (IsTitle(command) || command == kNotNow)
      return true;
    if (command == kOriginal)
      return State().IsPageTranslated() && !State().translation_pending();
    if (command == kTranslate) {
      return Ready() && !State().translation_pending() &&
             (!State().IsPageTranslated() ||
              local::CanonicalLanguage(State().current_language()) != target_ ||
              local::CanonicalLanguage(State().source_language()) != source_);
    }
    if (command == kAlways)
      return Ready() || delegate_->ShouldAlwaysTranslate();
    if (command == kNeverLanguage)
      return source_ != "und" && !source_.empty();
    if (command == kNeverSite)
      return delegate_->CanAddSiteToNeverPromptList();
    if (command >= kDownloadStart)
      return IsCommandIdVisible(command) &&
             !local::Installer::Get()->IsDownloading();
    return true;
  }
  bool IsItemForCommandIdDynamic(int command) const override {
    return command == kStatus || command == kProblem || command == kHeading ||
           command == kSourceMenu || command == kTargetMenu ||
           command == kTranslate || command == kAlways ||
           command == kNeverLanguage || command >= kDownloadStart;
  }
  std::u16string GetLabelForCommandId(int command) const override {
    if (!Valid())
      return u"Page unavailable";
    if (command == kHeading) {
      if (State().IsPageTranslated())
        return u"This page is translated from " +
               Name(State().source_language()) + u" to " +
               Name(State().current_language());
      return u"Translate this page?";
    }
    if (command == kSourceMenu)
      return Name(source_);
    if (command == kTargetMenu)
      return Name(target_);
    if (command == kTranslate)
      return State().translation_pending() ? u"Translating…" : u"Translate";
    if (command == kAlways)
      return u"Always translate " + Name(source_);
    if (command == kNeverLanguage)
      return u"Never translate " + Name(source_);
    if (command == kProblem)
      return ProblemHeadline();
    if (command == kStatus)
      return StatusHeadline();
    if (command >= kDownloadStart && keys_.contains(DownloadKey(command))) {
      const auto key = DownloadKey(command);
      auto* installer = local::Installer::Get();
      const auto* pack = local::FindPack(key);
      if (!installer || !pack)
        return u"Language pack unavailable";
      const int progress = installer->Progress(key);
      if (progress >= 0)
        return u"Downloading " + Name(pack->language) + u"… " +
               base::NumberToString16(progress) + u"%";
      const auto size = Megabytes(pack->download_bytes);
      if (!installer->Error(key).empty())
        return u"Retry " + Name(pack->language) + u" download (" + size + u")";
      return u"Download " + Name(pack->language) + u" (" + size + u")";
    }
    return {};
  }
  void ExecuteCommand(int command, int) override {
    if (IsTitle(command) || !IsCommandIdEnabled(command))
      return;
    auto& choices = Choice(*contents_);
    if (command == kNotNow || command == kOriginal)
      choices.automatic_suppressed = true;
    if (command == kNotNow)
      return;
    if (auto target = SelectedLanguage(command, kTargetStart)) {
      choices.target = *target;
      choices.failed = false;
      UpdatePair();
    } else if (auto source = SelectedLanguage(command, kSourceStart)) {
      choices.source = *source;
      choices.failed = false;
      UpdatePair();
    } else if (command >= kDownloadStart) {
      local::Installer::Get()->RequestDownload(
          *Profile::FromBrowserContext(contents_->GetBrowserContext()),
          DownloadKey(command));
    } else if (command == kManageLanguages) {
      OpenLanguageSettings(*contents_);
    } else if (command == kTranslate) {
      choices.failed = false;
      // Reapply to the original DOM when changing languages on an already
      // translated page, so runs never translate the previous output.
      if (State().IsPageTranslated())
        delegate_->RevertTranslation();
      delegate_->Translate();
    } else if (command == kOriginal) {
      choices.failed = false;
      delegate_->RevertTranslation();
    } else if (command == kAlways) {
      delegate_->SetAlwaysTranslate(!delegate_->ShouldAlwaysTranslate());
    } else if (command == kNeverLanguage) {
      delegate_->SetLanguageBlocked(!delegate_->IsLanguageBlocked());
    } else if (command == kNeverSite) {
      delegate_->SetNeverPromptSite(!delegate_->IsSiteOnNeverPromptList());
    }
    changed_.Run();
  }

 private:
  bool Valid() const {
    const auto* frame = document_.AsRenderFrameHostIfValid();
    return frame && frame == contents_->GetPrimaryMainFrame() &&
           Eligible(contents_);
  }
  const translate::LanguageState& State() const {
    return *Manager(*contents_)->GetLanguageState();
  }
  static local::PackKey DownloadKey(int command) {
    return static_cast<local::PackKey>(command - kDownloadStart);
  }
  std::optional<std::string_view> SelectedLanguage(int command,
                                                   int first) const {
    if (command < first ||
        command - first >= static_cast<int>(languages_.size()))
      return std::nullopt;
    return languages_[command - first];
  }
  void UpdatePair() {
    std::tie(source_, target_) = Languages(*contents_);
    delegate_ = std::make_unique<translate::TranslateUIDelegate>(
        Manager(*contents_)->GetWeakPtr(), source_, target_);
    keys_ = local::RequiredPacks(source_, target_)
                .value_or(std::set<local::PackKey>{});
    // This checks existing files only. Downloads require the named action.
    if (auto* installer = local::Installer::Get();
        installer && !keys_.empty()) {
      installer->PreparePair(source_, target_,
                             base::BindOnce([](base::RepeatingClosure changed,
                                               bool) { changed.Run(); },
                                            changed_));
    }
  }
  bool Ready() const {
    const auto* installer = local::Installer::Get();
    if (!installer || !installer->IsInit() || keys_.empty())
      return false;
    const auto installed = installer->InstalledLanguagePacks();
    return std::ranges::all_of(
        keys_, [&](auto key) { return installed.contains(key); });
  }
  // The selected pair's packs that are not on disk, in pack order.
  std::vector<const local::ModelPack*> MissingPacks() const {
    std::vector<const local::ModelPack*> missing;
    const auto* installer = local::Installer::Get();
    if (!installer)
      return missing;
    const auto installed = installer->InstalledLanguagePacks();
    for (auto key : keys_) {
      if (const auto* pack = local::FindPack(key);
          pack && !installed.contains(key))
        missing.push_back(pack);
    }
    return missing;
  }
  std::u16string PackNames(
      const std::vector<const local::ModelPack*>& packs) const {
    std::u16string names;
    for (size_t i = 0; i < packs.size(); ++i) {
      if (i)
        names += i + 1 == packs.size() ? u" and " : u", ";
      names += Name(packs[i]->language);
    }
    return names;
  }
  const local::ModelPack* FailedPack() const {
    if (const auto* installer = local::Installer::Get()) {
      for (auto key : keys_) {
        if (!installer->Error(key).empty())
          return local::FindPack(key);
      }
    }
    return nullptr;
  }
  std::u16string ProblemHeadline() const {
    if (State().translation_pending())
      return {};
    if (const auto* pack = FailedPack())
      return u"Couldn't download the " + Name(pack->language) +
             u" language pack";
    if (Choice(*contents_).failed)
      return u"There was a problem translating. Please try again.";
    return {};
  }
  std::u16string ProblemDetail() const {
    const auto* pack = FailedPack();
    const auto* installer = local::Installer::Get();
    return pack && installer && !State().translation_pending()
               ? base::UTF8ToUTF16(installer->Error(pack->key))
               : std::u16string();
  }
  std::u16string StatusHeadline() const {
    if (State().translation_pending())
      return u"Translating on this device…";
    if (keys_.empty()) {
      return source_ == target_
                 ? u"Choose a different language to translate to."
                 : u"Translation isn't available for this language pair.";
    }
    const auto missing = MissingPacks();
    if (missing.empty())
      return u"The on-device translation engine is unavailable.";
    if (local::Installer::Get()->IsDownloading())
      return u"Downloading " + PackNames(missing) + u"…";
    return u"Download " + PackNames(missing) + u" to translate this page";
  }
  std::u16string StatusDetail() const {
    if (State().translation_pending() || keys_.empty())
      return {};
    const auto missing = MissingPacks();
    if (missing.empty())
      return {};
    uint64_t bytes = 0;
    for (const auto* pack : missing)
      bytes += pack->download_bytes;
    return u"A one-time " + Megabytes(bytes) +
           u" download from Mozilla. Pages are then translated on this "
           u"device.";
  }

  content::WeakDocumentPtr document_;
  const raw_ptr<content::WebContents> contents_;
  base::RepeatingClosure changed_;
  ui::SimpleMenuModel heading_, settings_, source_menu_, target_menu_, footer_;
  std::unique_ptr<translate::TranslateUIDelegate> delegate_;
  std::string source_, target_;
  std::vector<std::string> languages_;
  std::set<local::PackKey> keys_;
};
}  // namespace

bool UpdateTranslationUI(content::WebContents* contents,
                         bool failed,
                         bool open_menu) {
  if (!contents)
    return false;
  auto* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(contents);
  auto* view =
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
  if (!view || !view->krypton_ui())
    return false;
  if (contents->GetPrimaryMainFrame())
    Choice(*contents).failed = failed;
  view->krypton_ui()->ScheduleActions();
  if (open_menu &&
      browser->GetTabStripModel()->GetActiveWebContents() == contents)
    view->krypton_ui()->ShowTranslationMenu();
  return true;
}
std::optional<KrAction> TranslationAction(content::WebContents* contents,
                                          std::string& tooltip,
                                          std::string& locale_chip) {
  if (!Eligible(contents))
    return std::nullopt;
  const auto& state = *Manager(*contents)->GetLanguageState();
  auto* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
  translate::TranslatePrefs prefs(profile->GetPrefs());
  if (!state.IsPageTranslated() && !state.translation_pending() &&
      !IsForeignLanguage(state, prefs)) {
    return std::nullopt;
  }
  const auto [source, target] = Languages(*contents);
  MaybeTranslateInstalled(*contents);
  uint32_t flags = KR_ACTION_VISIBLE | KR_ACTION_ENABLED;
  if (state.IsPageTranslated())
    flags |= KR_ACTION_ACTIVE;
  else if (OffersTranslation(*profile->GetPrefs()) &&
           local::RequiredPacks(source, target) &&
           !prefs.IsBlockedLanguage(source) &&
           !prefs.IsSiteOnNeverPromptList(
               contents->GetLastCommittedURL().host()))
    flags |= KR_ACTION_ATTENTION;
  float progress = -1;
  if (auto required = local::RequiredPacks(source, target);
      required && local::Installer::Get())
    for (auto key : *required) {
      const int percent = local::Installer::Get()->Progress(key);
      if (percent >= 0)
        progress = static_cast<float>(percent) / 100;
    }
  if (state.translation_pending())
    tooltip = "Translating locally…";
  else if (Choice(*contents).failed)
    tooltip = "Translation failed. Open the panel for details.";
  else if (state.IsPageTranslated())
    tooltip = "Page translated. Show original or change languages.";
  else
    tooltip = "Translate page privately on this device";
  static constexpr char label[] = "Translate page";
  KrAction action{};
  action.struct_size = sizeof(action);
  action.id = kTranslateAction;
  action.kind = KR_ACTION_TRANSLATE;
  action.flags = flags;
  action.progress = progress;
  action.label = {label, sizeof(label) - 1};
  action.tooltip = {tooltip.data(), tooltip.size()};
  if (state.IsPageTranslated()) {
    locale_chip = local::CanonicalLanguage(state.current_language());
    action.badge = {locale_chip.data(), locale_chip.size()};
  }
  return action;
}
base::span<const MenuPanelRow> TranslationPanelLayout() {
  static constexpr std::array rows{MenuPanelRow{kHeading, KR_MENU_ITEM_SECTION},
                                   MenuPanelRow{kFooter, KR_MENU_ITEM_FOOTER}};
  return rows;
}
bool TranslationMenuKeepsOpen(int command) {
  return command >= kSourceStart;
}
bool CanShowTranslationPanel(content::WebContents* contents) {
  return Eligible(contents);
}
std::string_view TranslationMenuGlyph(int command) {
  switch (command) {
    case kSettingsMenu:
      return "\uE713";  // Settings
    case kStatus:
      return "\uE946";  // Info
    case kProblem:
      return "\uE783";  // Error
    default:
      return command >= kDownloadStart ? "\uE896" : std::string_view{};
  }
}

std::unique_ptr<ui::MenuModel> CreateTranslationMenu(
    content::WebContents& contents,
    base::RepeatingClosure changed) {
  return Eligible(&contents)
             ? std::make_unique<TranslationMenu>(contents, std::move(changed))
             : nullptr;
}
TranslationActions::TranslationActions(Profile& profile,
                                       base::RepeatingClosure changed)
    : changed_(std::move(changed)) {
  if (auto* installer = local::Installer::Get())
    installer_ = installer->GetWeakPtr();
  if (installer_)
    installer_->AddObserver(this);
  prefs_.Init(profile.GetPrefs());
  prefs_.Add(translate::prefs::kOfferTranslateEnabled, changed_);
  prefs_.Add(language::prefs::kAcceptLanguages, changed_);
}
TranslationActions::~TranslationActions() {
  if (installer_)
    installer_->RemoveObserver(this);
}
void TranslationActions::OnLanguagePackInstalled(local::PackKey) {
  changed_.Run();
}
void TranslationActions::OnLanguagePackInstallationChanged(local::PackKey) {
  changed_.Run();
}
void TranslationActions::OnInstallationChanged() {
  changed_.Run();
}
void TranslationActions::OnLanguagePackProgress(local::PackKey, int) {
  changed_.Run();
}
}  // namespace krypton_ui

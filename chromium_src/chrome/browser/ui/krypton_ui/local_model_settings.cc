// Copyright 2026 The Krypton Authors. GPL-3.0.
#include "chrome/browser/ui/krypton_ui/local_model_settings.h"

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/krypton_translate/model_packs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/krypton_ui/dictation_controller.h"
#include "chrome/browser/ui/krypton_ui/local_model_prefs.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/translate/core/browser/translate_pref_names.h"
#include "components/translate/core/browser/translate_prefs.h"
#include "ui/base/l10n/l10n_util.h"

namespace krypton_ui {
namespace {
namespace translation = krypton::translation;
namespace voice = krypton::speech;

KrStr Borrow(std::string_view text) {
  return {text.data(), text.size()};
}

// The language pages are translated into: the user's latest explicit target,
// otherwise the browser's UI language. Packs pivot through English, so any
// other base language needs its own pack, which Settings then preselects.
std::string TranslationBaseLanguage(PrefService* prefs) {
  std::string base = translate::TranslatePrefs(prefs).GetRecentTargetLanguage();
  if (base.empty())
    base = g_browser_process->GetApplicationLocale();
  return translation::CanonicalLanguage(base);
}

uint32_t FlagsForCommand(uint32_t command) {
  switch (command) {
    case KR_LOCAL_MODEL_DOWNLOAD:
      return KR_LOCAL_MODEL_CAN_DOWNLOAD;
    case KR_LOCAL_MODEL_REMOVE:
      return KR_LOCAL_MODEL_CAN_REMOVE;
    case KR_LOCAL_MODEL_SELECT:
      return KR_LOCAL_MODEL_CAN_SELECT;
    default:
      return 0;
  }
}
}  // namespace

LocalModelSettings::LocalModelSettings(Profile& profile, WindowSession& session)
    : profile_(profile), session_(session) {
  if (auto* installer = translation::Installer::Get()) {
    translation_ = installer->GetWeakPtr();
    translation_->AddObserver(this);
    ++pending_scans_;
  }
  if (auto* installer = voice::Installer::Get()) {
    voice_ = installer->GetWeakPtr();
    voice_->AddObserver(this);
    ++pending_scans_;
  }
  prefs_.Init(profile_->GetPrefs());
  const auto changed = base::BindRepeating(&LocalModelSettings::QueuePublish,
                                           weak_factory_.GetWeakPtr());
  prefs_.Add(kVoicePackPref, changed);
  prefs_.Add(kVoiceLocalNoticePref, changed);
  prefs_.Add(kTranslationLocalNoticePref, changed);
  prefs_.Add(prefs::kAudioCaptureAllowed, changed);
  prefs_.Add(translate::prefs::kOfferTranslateEnabled, changed);
  if (translation_) {
    translation_->ScanInstalled(base::BindOnce(&LocalModelSettings::ScanDone,
                                               weak_factory_.GetWeakPtr()));
  }
  if (voice_) {
    voice_->Scan(base::BindOnce(&LocalModelSettings::ScanDone,
                                weak_factory_.GetWeakPtr()));
  }
  QueuePublish();
}

LocalModelSettings::~LocalModelSettings() {
  if (translation_)
    translation_->RemoveObserver(this);
  if (voice_)
    voice_->RemoveObserver(this);
}

void LocalModelSettings::ScanDone() {
  --pending_scans_;
  QueuePublish();
}

void LocalModelSettings::QueuePublish() {
  if (!publish_pending_) {
    publish_pending_ = session_->Defer(base::BindOnce(
        &LocalModelSettings::Publish, weak_factory_.GetWeakPtr()));
  }
}

uint32_t LocalModelSettings::TranslationFlags(bool installed) const {
  uint32_t flags = installed ? KR_LOCAL_MODEL_INSTALLED : 0u;
  if (!profile_->GetPrefs()->GetBoolean(kTranslationLocalNoticePref))
    flags |= KR_LOCAL_MODEL_SHOW_FIRST_USE;
  if (!pending_scans_ && !translation_->IsDownloading()) {
    if (installed)
      flags |= KR_LOCAL_MODEL_CAN_REMOVE;
    else if (translate::TranslatePrefs(profile_->GetPrefs())
                 .IsTranslateAllowedByPolicy())
      flags |= KR_LOCAL_MODEL_CAN_DOWNLOAD;
  }
  return flags;
}

uint32_t LocalModelSettings::VoiceFlags(
    const krypton_voice::ModelPack& pack) const {
  const bool installed = voice_->Ready(pack.id);
  const bool active = installed && SelectedVoicePack(*profile_) == &pack;
  const bool allowed =
      profile_->GetPrefs()->GetBoolean(prefs::kAudioCaptureAllowed);
  uint32_t flags = installed ? KR_LOCAL_MODEL_INSTALLED : 0u;
  if (active)
    flags |= KR_LOCAL_MODEL_ACTIVE;
  if (!profile_->GetPrefs()->GetBoolean(kVoiceLocalNoticePref))
    flags |= KR_LOCAL_MODEL_SHOW_FIRST_USE;
  if (!pending_scans_ && !voice_->IsDownloading()) {
    if (installed) {
      flags |= KR_LOCAL_MODEL_CAN_REMOVE;
      if (allowed && (!active || (flags & KR_LOCAL_MODEL_SHOW_FIRST_USE)))
        flags |= KR_LOCAL_MODEL_CAN_SELECT;
    } else if (allowed) {
      flags |= KR_LOCAL_MODEL_CAN_DOWNLOAD;
    }
  }
  return flags;
}

void LocalModelSettings::Publish() {
  publish_pending_ = false;
  const auto* api = session_->api();
  if (!api)
    return;
  const bool variants = KR_HAS_SLOT(api, KrShellUI, set_local_models_v2) &&
                        api->set_local_models_v2;
  if (!variants && (!KR_HAS_SLOT(api, KrShellUI, set_local_models) ||
                    !api->set_local_models))
    return;

  std::vector<KrLocalModel> rows;
  std::vector<std::string> names;
  std::vector<std::string> errors;
  const auto packs = translation::ModelPacks();
  const size_t count = packs.size() + std::size(krypton_voice::kPacks);
  rows.reserve(count);
  names.reserve(count);
  errors.reserve(count);
  const auto add = [&](uint32_t id, uint32_t kind, uint32_t flags, int progress,
                       uint64_t bytes, std::string name, std::string error) {
    rows.push_back({id, kind, flags, progress, bytes,
                    Borrow(names.emplace_back(std::move(name))),
                    Borrow(errors.emplace_back(std::move(error)))});
  };
  if (translation_) {
    const auto installed = translation_->InstalledLanguagePacks();
    const auto& locale = g_browser_process->GetApplicationLocale();
    const auto english = l10n_util::GetDisplayNameForLocale("en", locale, true);
    const auto base = TranslationBaseLanguage(profile_->GetPrefs());
    for (const auto& pack : packs) {
      const auto name = l10n_util::GetDisplayNameForLocale(
          std::string(pack.language), locale, true);
      add(static_cast<uint32_t>(pack.key), KR_LOCAL_MODEL_TRANSLATION,
          TranslationFlags(installed.contains(pack.key)) |
              (base != "en" && pack.language == base ? KR_LOCAL_MODEL_SUGGESTED
                                                     : 0u),
          translation_->Progress(pack.key), pack.download_bytes,
          base::UTF16ToUTF8(english + u" ↔ " + name),
          translation_->Error(pack.key));
    }
  }
  if (voice_) {
    // The saved choice, otherwise the pack detected from the user's languages.
    const auto* detected = SelectedVoicePack(*profile_);
    for (uint32_t index = 0; index < std::size(krypton_voice::kPacks);
         ++index) {
      const auto& pack = krypton_voice::kPacks[index];
      add(index, KR_LOCAL_MODEL_VOICE,
          VoiceFlags(pack) |
              (detected == &pack ? KR_LOCAL_MODEL_SUGGESTED : 0u),
          voice_->Progress(pack.id), pack.download_size,
          base::UTF16ToUTF8(VoicePackName(pack)), voice_->Error(pack.id));
    }
  }
  if (variants) {
    std::vector<KrLocalModelWithVoiceVariant> extended;
    std::vector<std::string> languages;
    extended.reserve(rows.size());
    languages.reserve(std::size(krypton_voice::kPacks));
    for (const auto& row : rows) {
      KrLocalModelWithVoiceVariant entry{};
      entry.model = row;
      if (row.kind == KR_LOCAL_MODEL_VOICE) {
        const auto& pack = krypton_voice::kPacks[row.id];
        entry.language_tag = Borrow(pack.language);
        entry.language_name = Borrow(languages.emplace_back(
            base::UTF16ToUTF8(VoiceLanguageName(pack.language))));
        entry.model_family = Borrow("Moonshine v2 Streaming");
        entry.voice_size = pack.id.ends_with("-medium") ? KR_VOICE_MODEL_MEDIUM
                           : pack.lightweight           ? KR_VOICE_MODEL_TINY
                                                        : KR_VOICE_MODEL_SMALL;
      }
      extended.push_back(entry);
    }
    const KrLocalModelsSnapshotV2 snapshot{
        sizeof(KrLocalModelsSnapshotV2), extended.data(), extended.size(),
        sizeof(KrLocalModelWithVoiceVariant), pending_scans_ > 0};
    api->set_local_models_v2(api->self, &snapshot);
    return;
  }
  const KrLocalModelsSnapshot snapshot{
      sizeof(KrLocalModelsSnapshot), rows.data(), rows.size(),
      sizeof(KrLocalModel), pending_scans_ > 0};
  api->set_local_models(api->self, &snapshot);
}

KrResult LocalModelSettings::Command(uint32_t kind,
                                     uint32_t id,
                                     uint32_t command) {
  if (!FlagsForCommand(command) ||
      (kind != KR_LOCAL_MODEL_TRANSLATION && kind != KR_LOCAL_MODEL_VOICE))
    return KR_ERR_INVALID_ARGUMENT;
  return session_->Defer(base::BindOnce(&LocalModelSettings::RunCommand,
                                        weak_factory_.GetWeakPtr(), kind, id,
                                        command))
             ? KR_OK
             : KR_ERR_PLATFORM;
}

void LocalModelSettings::RunCommand(uint32_t kind,
                                    uint32_t id,
                                    uint32_t command) {
  if (!profile_->IsRegularProfile() || profile_->IsOffTheRecord())
    return;
  PrefService& prefs = *profile_->GetPrefs();
  const uint32_t required = FlagsForCommand(command);
  if (kind == KR_LOCAL_MODEL_TRANSLATION && translation_) {
    const auto key = static_cast<translation::PackKey>(id);
    if (!translation::FindPack(key) ||
        !(TranslationFlags(
              translation_->InstalledLanguagePacks().contains(key)) &
          required))
      return;
    if (command == KR_LOCAL_MODEL_DOWNLOAD) {
      prefs.SetBoolean(kTranslationLocalNoticePref, true);
      translation_->RequestDownload(*profile_, key);
    } else {
      translation_->UnInstallLanguagePack(key);
    }
  } else if (kind == KR_LOCAL_MODEL_VOICE && voice_ &&
             id < std::size(krypton_voice::kPacks)) {
    const auto& pack = krypton_voice::kPacks[id];
    if (!(VoiceFlags(pack) & required))
      return;
    if (command == KR_LOCAL_MODEL_DOWNLOAD) {
      prefs.SetBoolean(kVoiceLocalNoticePref, true);
      voice_->RequestDownload(*profile_, std::string(pack.id));
    } else if (command == KR_LOCAL_MODEL_REMOVE) {
      voice_->RemovePack(std::string(pack.id));
    } else {
      prefs.SetBoolean(kVoiceLocalNoticePref, true);
      prefs.SetString(kVoicePackPref, std::string(pack.id));
    }
  }
  QueuePublish();
}

void LocalModelSettings::OnInstallationChanged() {
  QueuePublish();
}
void LocalModelSettings::OnLanguagePackInstalled(translation::PackKey) {
  QueuePublish();
}
void LocalModelSettings::OnLanguagePackInstallationChanged(
    translation::PackKey) {
  QueuePublish();
}
void LocalModelSettings::OnLanguagePackProgress(translation::PackKey, int) {
  QueuePublish();
}
void LocalModelSettings::OnSodaInstalled(speech::LanguageCode) {
  QueuePublish();
}
void LocalModelSettings::OnSodaInstallError(speech::LanguageCode,
                                            speech::SodaInstaller::ErrorCode) {
  QueuePublish();
}
void LocalModelSettings::OnSodaProgress(speech::LanguageCode, int) {
  QueuePublish();
}
}  // namespace krypton_ui

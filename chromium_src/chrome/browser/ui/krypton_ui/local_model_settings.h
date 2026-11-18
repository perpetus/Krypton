// Copyright 2026 The Krypton Authors. GPL-3.0.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_LOCAL_MODEL_SETTINGS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_LOCAL_MODEL_SETTINGS_H_

#include <cstdint>

#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/krypton_speech/installer.h"
#include "chrome/browser/krypton_translate/installer.h"
#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/prefs/pref_change_registrar.h"

class Profile;

namespace krypton_ui {
class WindowSession;

// Observes device-local pack state only while the Languages page is open.
class LocalModelSettings final
    : public on_device_translation::OnDeviceTranslationInstaller::Observer,
      public speech::SodaInstaller::Observer {
 public:
  LocalModelSettings(Profile& profile, WindowSession& session);
  ~LocalModelSettings() override;

  KrResult Command(uint32_t kind, uint32_t id, uint32_t command);

 private:
  void ScanDone();
  void QueuePublish();
  void Publish();
  uint32_t TranslationFlags(bool installed) const;
  uint32_t VoiceFlags(const krypton_voice::ModelPack& pack) const;
  void RunCommand(uint32_t kind, uint32_t id, uint32_t command);
  void OnInstallationChanged() override;
  void OnLanguagePackInstalled(krypton::translation::PackKey) override;
  void OnLanguagePackInstallationChanged(
      krypton::translation::PackKey) override;
  void OnLanguagePackProgress(krypton::translation::PackKey, int) override;
  void OnSodaInstalled(speech::LanguageCode) override;
  void OnSodaInstallError(speech::LanguageCode,
                          speech::SodaInstaller::ErrorCode) override;
  void OnSodaProgress(speech::LanguageCode, int) override;

  const raw_ref<Profile> profile_;
  const raw_ref<WindowSession> session_;
  base::WeakPtr<krypton::translation::Installer> translation_;
  base::WeakPtr<krypton::speech::Installer> voice_;
  PrefChangeRegistrar prefs_;
  int pending_scans_ = 0;
  bool publish_pending_ = false;
  base::WeakPtrFactory<LocalModelSettings> weak_factory_{this};
};
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_LOCAL_MODEL_SETTINGS_H_

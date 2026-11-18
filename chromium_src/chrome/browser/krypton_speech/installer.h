// Copyright 2026 The Krypton Authors. GPL-3.0.
#ifndef CHROME_BROWSER_KRYPTON_SPEECH_INSTALLER_H_
#define CHROME_BROWSER_KRYPTON_SPEECH_INSTALLER_H_
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "base/containers/flat_map.h"
#include "base/files/file.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/krypton_speech/model_manifest.h"
#include "components/soda/soda_installer.h"
class Profile;
namespace krypton::speech {
std::string Locale(std::string_view language);
const krypton_voice::ModelPack* DefaultPack(std::string_view language);
std::string RecognitionLanguage(const krypton_voice::ModelPack& pack);
class Installer final : public ::speech::SodaInstaller {
 public:
  Installer();
  ~Installer() override;
  static Installer* Get();
  base::WeakPtr<Installer> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
  void Init(PrefService*, PrefService*) override;
  base::FilePath GetSodaBinaryPath() const override;
  base::FilePath GetLanguagePath(std::string_view language) const override;
  void InstallSoda(PrefService*) override;
  void InstallLanguage(std::string_view language, PrefService*) override;
  void UninstallLanguage(std::string_view language, PrefService*) override;
  std::vector<std::string> GetAvailableLanguages() const override;
  std::vector<std::string> GetLiveCaptionEnabledLanguages() const override;
  void Prepare(std::string id, base::OnceCallback<void(bool)> reply);
  void Scan(base::OnceClosure reply);
  void RequestDownload(Profile& profile, std::string id);
  void RemovePack(std::string id);
  bool Ready(std::string_view id) const;
  int Progress(std::string_view id) const;
  bool IsDownloading() const { return download_ != nullptr; }
  std::string Error(std::string_view id) const;
  base::flat_map<std::string, base::FilePath> InstalledPaths() const;
  base::flat_map<std::string, std::vector<base::File>> ModelFilesForService()
      const;

 private:
  struct Verified;
  struct Download;
  static Verified Verify(std::set<std::string> ids);
  void Prepared(std::string id,
                base::OnceCallback<void(bool)> reply,
                Verified result);
  void DirectoryReady(uint64_t generation, base::FilePath directory);
  void NextFile();
  void Downloaded(uint64_t generation, base::FilePath path);
  void FileVerified(uint64_t generation, bool valid);
  void Committed(uint64_t generation, bool valid);
  void Failed(std::string message);
  void Notify(const krypton_voice::ModelPack& pack);
  void UninstallSoda(PrefService*) override;
  base::FilePath root_, library_;
  std::map<std::string, std::vector<base::File>, std::less<>> ready_;
  base::flat_map<std::string, std::string> errors_;
  std::unique_ptr<Download> download_;
  uint64_t generation_ = 0;
  scoped_refptr<base::SequencedTaskRunner> worker_;
  base::WeakPtrFactory<Installer> weak_factory_{this};
};
}  // namespace krypton::speech
#endif

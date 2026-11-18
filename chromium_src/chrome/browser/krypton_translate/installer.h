// Copyright 2026 The Krypton Authors. GPL-3.0.
#ifndef CHROME_BROWSER_KRYPTON_TRANSLATE_INSTALLER_H_
#define CHROME_BROWSER_KRYPTON_TRANSLATE_INSTALLER_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/krypton_translate/model_packs.h"
#include "components/on_device_translation/installer.h"

class Profile;
namespace network {
class SimpleURLLoader;
class SharedURLLoaderFactory;
}  // namespace network

namespace krypton::translation {
// The generic InstallLanguagePack entry point intentionally cannot download.
// Only the host's explicit download menu action may call RequestDownload.
class Installer final
    : public on_device_translation::OnDeviceTranslationInstaller {
 public:
  Installer();
  ~Installer() override;
  static Installer* Get();
  base::WeakPtr<Installer> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  bool IsInit() const override;
  std::set<PackKey> RegisteredLanguagePacks() const override;
  std::set<PackKey> InstalledLanguagePacks() const override;
  base::FilePath GetLibraryPath() const override;
  base::FilePath GetLanguagePackPath(PackKey key) const override;
  void Init(base::RepeatingClosure ready) override;
  void InstallLanguagePack(PackKey key) override;
  void UnInstallLanguagePack(PackKey key) override;
  void AddObserver(Observer* observer) override;
  void RemoveObserver(Observer* observer) override;

  void PreparePair(std::string source,
                   std::string target,
                   base::OnceCallback<void(bool)> callback);
  void ScanInstalled(base::OnceClosure callback);
  void RequestDownload(Profile& profile, PackKey key);
  int Progress(PackKey key) const;
  bool IsDownloading() const { return download_ != nullptr; }
  std::string Error(PackKey key) const;

 private:
  struct Verified;
  struct Download;
  static Verified Verify(std::set<PackKey> keys);
  void Initialized(base::RepeatingClosure ready, Verified result);
  void Prepared(base::OnceCallback<void(bool)> callback,
                std::set<PackKey> required,
                Verified result);
  void DownloadDirectory(uint64_t generation,
                         std::pair<base::FilePath, bool> prepared);
  void NextFile();
  void Downloaded(uint64_t generation, base::FilePath path);
  void FileVerified(uint64_t generation);
  void Installed(uint64_t generation, bool valid);
  void DownloadFailed(std::string message);
  void Notify(PackKey key);
  void NotifyProgress(PackKey key);

  bool initialized_ = false;
  base::FilePath root_;
  base::FilePath library_;
  std::set<PackKey> installed_;
  std::map<PackKey, std::string> errors_;
  std::unique_ptr<Download> download_;
  scoped_refptr<base::SequencedTaskRunner> worker_;
  uint64_t generation_ = 0;
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<Installer> weak_factory_{this};
};
}  // namespace krypton::translation
#endif  // CHROME_BROWSER_KRYPTON_TRANSLATE_INSTALLER_H_

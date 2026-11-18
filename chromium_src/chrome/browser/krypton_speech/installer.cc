// Copyright 2026 The Krypton Authors. GPL-3.0.
#include "chrome/browser/krypton_speech/installer.h"

#include <algorithm>
#include <atomic>
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/krypton_models/files.h"
#include "chrome/browser/krypton_speech/engine_manifest.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/common/chrome_paths.h"
#include "content/public/browser/storage_partition.h"
#include "crypto/hash.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"

namespace krypton::speech {
namespace {
Installer* instance = nullptr;
using Pack = krypton_voice::ModelPack;
constexpr uint64_t kMaximumFile = 192 * 1024 * 1024;
const Pack* PackForId(std::string_view id) {
  for (const auto& pack : krypton_voice::kPacks)
    if (pack.id == id)
      return &pack;
  return nullptr;
}
base::FilePath Directory(const base::FilePath& root, const Pack& pack) {
  return root.AppendASCII(std::string(pack.id))
      .AppendASCII(std::string(pack.revision));
}
std::vector<base::File> OpenVerifiedPack(const base::FilePath& path,
                                         const Pack& pack) {
  if (base::IsLink(path) || base::IsLink(path.DirName()) ||
      base::IsLink(path.DirName().DirName()))
    return {};
  std::vector<base::File> files;
  files.reserve(pack.files.size());
  for (const auto& asset : pack.files) {
    const auto asset_path = models::Child(path, asset.name);
    if (asset_path.empty() || !asset.size || asset.size > kMaximumFile ||
        base::IsLink(asset_path))
      return {};
    // Verify and retain the same read-only handle. Deny writers while it is
    // shared with the utility, but allow removal/replacement by the installer.
    base::File file(asset_path, base::File::FLAG_OPEN | base::File::FLAG_READ |
                                    base::File::FLAG_WIN_EXCLUSIVE_WRITE |
                                    base::File::FLAG_WIN_SHARE_DELETE);
    base::MemoryMappedFile mapped;
    if (!file.IsValid() ||
        file.GetLength() != static_cast<int64_t>(asset.size) ||
        !mapped.Initialize(file.Duplicate()) || mapped.length() != asset.size ||
        crypto::hash::Sha256(mapped.bytes()) != asset.sha256)
      return {};
    files.push_back(std::move(file));
  }
  return files;
}
void RemoveTemporary(base::FilePath path) {
  if (!path.empty())
    base::DeletePathRecursively(path);
}
constexpr net::NetworkTrafficAnnotationTag kDownload =
    net::DefineNetworkTrafficAnnotation("krypton_local_speech_model", R"(
      semantics {
        sender: "Krypton local speech"
        description: "Downloads a fixed, hash-pinned speech pack after the user clicks its size-labelled download button. Audio and text are never uploaded."
        trigger: "An explicit Download action in the microphone menu or local-model settings."
        data: "A fixed model URL. No cookies, credentials, audio or transcripts."
        destination: OTHER
        destination_other: "Moonshine AI model distribution"
      }
      policy {
        cookies_allowed: NO
        setting: "Each missing pack requires a separate explicit user download."
        policy_exception_justification: "Browser-owned local inference; no automatic downloads or updates."
      })");
}  // namespace

std::string Locale(std::string_view language) {
  auto lang =
      base::ToLowerASCII(language.substr(0, language.find_first_of("-_")));
  if (lang == "en")
    return "en-US";
  if (lang == "es")
    return "es-ES";
  if (lang == "de")
    return "de-DE";
  if (lang == "ja")
    return "ja-JP";
  if (lang == "ar")
    return "ar-SA";
  if (lang == "zh" || lang == "cmn")
    return "zh-CN";
  if (lang == "vi")
    return "vi-VN";
  if (lang == "tl" || lang == "fil")
    return "tl-PH";
  return {};
}
const Pack* DefaultPack(std::string_view language) {
  const auto locale = Locale(language);
  const auto base = locale.substr(0, locale.find('-'));
  const auto* small = PackForId(base + "-small");
  return small ? small : PackForId(base + "-tiny");
}
std::string RecognitionLanguage(const Pack& pack) {
  return Locale(pack.language) + (pack.id.ends_with("-medium")
                                      ? "-x-krypton-medium"
                                  : pack.lightweight ? "-x-krypton-tiny"
                                                     : "-x-krypton-small");
}
struct Installer::Verified {
  base::FilePath root, library;
  std::map<std::string, std::vector<base::File>> packs;
};
struct Installer::Download : ProfileObserver {
  ~Download() override {
    cancelled->store(true, std::memory_order_relaxed);
    if (profile)
      profile->RemoveObserver(this);
  }
  void OnProfileWillBeDestroyed(Profile* closing) override {
    closing->RemoveObserver(this);
    profile = nullptr;
    if (owner)
      owner->Failed(
          "Speech-pack download cancelled because the profile closed.");
  }
  raw_ptr<Profile> profile = nullptr;
  base::WeakPtr<Installer> owner;
  const Pack* pack = nullptr;
  base::FilePath directory;
  scoped_refptr<network::SharedURLLoaderFactory> factory;
  std::unique_ptr<network::SimpleURLLoader> loader;
  std::shared_ptr<std::atomic_bool> cancelled =
      std::make_shared<std::atomic_bool>(false);
  size_t file = 0;
  uint64_t received = 0;
  int progress = 0;
};
Installer::Installer()
    : worker_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  CHECK(!instance);
  instance = this;
}
Installer::~Installer() {
  if (download_) {
    auto temporary = download_->directory;
    download_.reset();
    worker_->PostTask(FROM_HERE,
                      base::BindOnce(&RemoveTemporary, std::move(temporary)));
  }
  instance = nullptr;
}
Installer* Installer::Get() {
  return instance;
}
void Installer::Init(PrefService*, PrefService*) {
  // No startup disk scan, model prefetch, component registration or timers.
  soda_installer_initialized_ = true;
}
base::FilePath Installer::GetSodaBinaryPath() const {
  return library_;
}
base::FilePath Installer::GetLanguagePath(std::string_view language) const {
  const auto* pack = DefaultPack(language);
  if (pack && !Ready(pack->id))
    pack = PackForId(std::string(pack->language) + "-tiny");
  return pack && Ready(pack->id) ? Directory(root_, *pack) : base::FilePath();
}
Installer::Verified Installer::Verify(std::set<std::string> ids) {
  Verified verified;
  base::FilePath user, module;
  // Installed builds keep the engine beside the versioned browser module.
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user) ||
      !base::PathService::Get(base::DIR_MODULE, &module))
    return verified;
  verified.root = user.AppendASCII("KryptonSpeech");
  const auto library = module.AppendASCII("krypton_speech.dll");
  if (!models::VerifyFile(library, kEngineSize, kEngineSha256, kMaximumFile))
    return verified;
  for (const auto& dependency : kRuntimeDependencies)
    if (!models::VerifyFile(module.AppendASCII(std::string(dependency.name)),
                            dependency.size, dependency.sha256, kMaximumFile))
      return verified;
  verified.library = library;
  for (const auto& id : ids) {
    if (const auto* pack = PackForId(id)) {
      auto files = OpenVerifiedPack(Directory(verified.root, *pack), *pack);
      if (!files.empty())
        verified.packs.emplace(id, std::move(files));
    }
  }
  return verified;
}
void Installer::Prepare(std::string id, base::OnceCallback<void(bool)> reply) {
  if (!PackForId(id)) {
    std::move(reply).Run(false);
    return;
  }
  if (Ready(id)) {
    std::move(reply).Run(true);
    return;
  }
  auto copy = id;
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&Verify, std::set<std::string>{std::move(copy)}),
      base::BindOnce(&Installer::Prepared, weak_factory_.GetWeakPtr(),
                     std::move(id), std::move(reply)));
}
void Installer::Prepared(std::string id,
                         base::OnceCallback<void(bool)> reply,
                         Verified result) {
  root_ = std::move(result.root);
  library_ = std::move(result.library);
  const bool binary_added = !soda_binary_installed_ && !library_.empty();
  soda_binary_installed_ = !library_.empty();
  for (auto& [found, files] : result.packs) {
    const auto* pack = PackForId(found);
    const bool added = ready_.insert_or_assign(found, std::move(files)).second;
    const auto code = ::speech::GetLanguageCode(Locale(pack->language));
    installed_languages_.insert(code);
    if (added)
      NotifyOnSodaInstalled(code);
  }
  if (binary_added)
    NotifyOnSodaInstalled(::speech::LanguageCode::kNone);
  std::move(reply).Run(Ready(id));
}
void Installer::Scan(base::OnceClosure reply) {
  std::set<std::string> ids;
  for (const auto& pack : krypton_voice::kPacks)
    ids.insert(std::string(pack.id));
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&Verify, std::move(ids)),
      base::BindOnce(
          [](base::WeakPtr<Installer> self, base::OnceClosure reply,
             Verified result) {
            if (!self)
              return;
            self->Prepared({},
                           base::BindOnce([](base::OnceClosure reply,
                                             bool) { std::move(reply).Run(); },
                                          std::move(reply)),
                           std::move(result));
          },
          weak_factory_.GetWeakPtr(), std::move(reply)));
}
void Installer::InstallSoda(PrefService*) {
  // Generic calls only verify the shipped binary and an existing pack.
  const auto* pack = DefaultPack(g_browser_process->GetApplicationLocale());
  if (pack)
    Prepare(std::string(pack->id), base::DoNothing());
}
void Installer::InstallLanguage(std::string_view language, PrefService*) {
  const auto* pack = DefaultPack(language);
  if (pack)
    Prepare(std::string(pack->id),
            base::BindOnce(
                [](base::WeakPtr<Installer> self, std::string language,
                   bool ready) {
                  if (!self || ready)
                    return;
                  const auto* tiny = PackForId(language + "-tiny");
                  if (tiny)
                    self->Prepare(std::string(tiny->id), base::DoNothing());
                },
                weak_factory_.GetWeakPtr(), std::string(pack->language)));
}
void Installer::UninstallLanguage(std::string_view language, PrefService*) {
  for (const auto& pack : krypton_voice::kPacks)
    if (Locale(pack.language) == Locale(language))
      RemovePack(std::string(pack.id));
}
void Installer::UninstallSoda(PrefService*) {
  // The shipped engine is never deleted. Pack removal has an explicit UI path.
}
std::vector<std::string> Installer::GetAvailableLanguages() const {
  return {"en-US", "es-ES", "de-DE", "ja-JP",
          "ar-SA", "zh-CN", "vi-VN", "tl-PH"};
}
std::vector<std::string> Installer::GetLiveCaptionEnabledLanguages() const {
  return GetAvailableLanguages();
}
bool Installer::Ready(std::string_view id) const {
  return !library_.empty() && ready_.contains(id);
}
int Installer::Progress(std::string_view id) const {
  return download_ && download_->pack->id == id ? download_->progress : -1;
}
std::string Installer::Error(std::string_view id) const {
  const auto it = errors_.find(id);
  return it == errors_.end() ? std::string() : it->second;
}
base::flat_map<std::string, base::FilePath> Installer::InstalledPaths() const {
  base::flat_map<std::string, base::FilePath> paths;
  for (const auto& language : GetAvailableLanguages()) {
    auto path = GetLanguagePath(language);
    if (!path.empty())
      paths[language] = std::move(path);
  }
  for (const auto& pack : krypton_voice::kPacks)
    if (Ready(pack.id))
      paths[RecognitionLanguage(pack)] = Directory(root_, pack);
  return paths;
}
base::flat_map<std::string, std::vector<base::File>>
Installer::ModelFilesForService() const {
  base::flat_map<std::string, std::vector<base::File>> result;
  for (const auto& [id, files] : ready_) {
    const auto* pack = PackForId(id);
    std::vector<base::File> copies;
    copies.reserve(files.size());
    for (const auto& file : files) {
      auto copy = file.Duplicate();
      if (!copy.IsValid())
        break;
      copies.push_back(std::move(copy));
    }
    if (copies.size() == files.size())
      result.emplace(Directory(root_, *pack).AsUTF8Unsafe(), std::move(copies));
  }
  return result;
}
void Installer::RequestDownload(Profile& profile, std::string id) {
  const auto* pack = PackForId(id);
  if (!pack || download_ || profile.IsOffTheRecord())
    return;
  download_ = std::make_unique<Download>();
  download_->profile = &profile;
  download_->owner = weak_factory_.GetWeakPtr();
  profile.AddObserver(download_.get());
  download_->pack = pack;
  download_->factory = profile.GetDefaultStoragePartition()
                           ->GetURLLoaderFactoryForBrowserProcess();
  errors_.erase(id);
  const auto generation = ++generation_;
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce([] {
        base::FilePath user;
        if (!base::PathService::Get(chrome::DIR_USER_DATA, &user))
          return base::FilePath();
        const auto root = user.AppendASCII("KryptonSpeech");
        const auto temporary = root.AppendASCII(
            "install-" + base::Uuid::GenerateRandomV4().AsLowercaseString());
        return !base::IsLink(root) && base::CreateDirectory(temporary)
                   ? temporary
                   : base::FilePath();
      }),
      base::BindOnce(&Installer::DirectoryReady, weak_factory_.GetWeakPtr(),
                     generation));
  Notify(*pack);
}
void Installer::DirectoryReady(uint64_t generation, base::FilePath directory) {
  if (generation != generation_ || !download_) {
    worker_->PostTask(FROM_HERE,
                      base::BindOnce(&RemoveTemporary, std::move(directory)));
    return;
  }
  if (directory.empty()) {
    Failed("Cannot prepare the speech-pack directory.");
    return;
  }
  download_->directory = std::move(directory);
  NextFile();
}
void Installer::NextFile() {
  CHECK(download_);
  const auto generation = generation_;
  if (download_->file == download_->pack->files.size()) {
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(
            [](base::FilePath temporary, const Pack* pack,
               std::shared_ptr<std::atomic_bool> cancelled) {
              if (cancelled->load(std::memory_order_relaxed) ||
                  OpenVerifiedPack(temporary, *pack).empty())
                return false;
              const auto destination = Directory(temporary.DirName(), *pack);
              if (!base::CreateDirectory(destination.DirName()) ||
                  base::IsLink(destination.DirName()) ||
                  cancelled->load(std::memory_order_relaxed))
                return false;
              if (base::PathExists(destination)) {
                if (!OpenVerifiedPack(destination, *pack).empty())
                  return true;
                const auto old = destination.AddExtensionASCII(
                    "old-" +
                    base::Uuid::GenerateRandomV4().AsLowercaseString());
                if (base::IsLink(destination) || !base::Move(destination, old))
                  return false;
                if (!base::Move(temporary, destination)) {
                  base::Move(old, destination);
                  return false;
                }
                base::DeletePathRecursively(old);
                return true;
              }
              return base::Move(temporary, destination);
            },
            download_->directory, download_->pack, download_->cancelled),
        base::BindOnce(&Installer::Committed, weak_factory_.GetWeakPtr(),
                       generation));
    return;
  }
  const auto& file = download_->pack->files[download_->file];
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(file.url);
  CHECK(request->url.SchemeIs("https") &&
        request->url.host() == "download.moonshine.ai");
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->redirect_mode = network::mojom::RedirectMode::kError;
  request->load_flags = net::LOAD_DISABLE_CACHE;
  download_->loader =
      network::SimpleURLLoader::Create(std::move(request), kDownload);
  download_->loader->SetTimeoutDuration(base::Minutes(10));
  download_->loader->SetOnDownloadProgressCallback(base::BindRepeating(
      [](base::WeakPtr<Installer> self, uint64_t generation, uint64_t bytes) {
        if (!self || generation != self->generation_ || !self->download_)
          return;
        auto& job = *self->download_;
        const int progress = static_cast<int>(std::min<uint64_t>(
            99, (job.received + bytes) * 100 / job.pack->download_size));
        if (progress != job.progress) {
          job.progress = progress;
          self->Notify(*job.pack);
        }
      },
      weak_factory_.GetWeakPtr(), generation));
  download_->loader->DownloadToFile(
      download_->factory.get(),
      base::BindOnce(&Installer::Downloaded, weak_factory_.GetWeakPtr(),
                     generation),
      models::Child(download_->directory, file.name),
      static_cast<int64_t>(file.size));
}
void Installer::Downloaded(uint64_t generation, base::FilePath path) {
  if (generation != generation_ || !download_)
    return;
  if (path.empty()) {
    Failed("Speech-pack download failed. No model was installed.");
    return;
  }
  download_->loader.reset();
  const auto& file = download_->pack->files[download_->file];
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&models::VerifyFile, std::move(path), file.size,
                     base::HexEncode(file.sha256), kMaximumFile),
      base::BindOnce(&Installer::FileVerified, weak_factory_.GetWeakPtr(),
                     generation));
}
void Installer::FileVerified(uint64_t generation, bool valid) {
  if (generation != generation_ || !download_)
    return;
  if (!valid) {
    Failed("Speech-pack SHA-256 verification failed. No model was installed.");
    return;
  }
  download_->received += download_->pack->files[download_->file++].size;
  NextFile();
}
void Installer::Committed(uint64_t generation, bool valid) {
  if (generation != generation_ || !download_)
    return;
  if (!valid) {
    Failed("Cannot finish the speech-pack installation.");
    return;
  }
  const auto* pack = download_->pack;
  const auto temporary = download_->directory;
  root_ = temporary.DirName();
  download_.reset();
  worker_->PostTask(FROM_HERE, base::BindOnce(&RemoveTemporary, temporary));
  errors_.erase(std::string(pack->id));
  Prepare(std::string(pack->id), base::DoNothing());
  Notify(*pack);
}
void Installer::Failed(std::string message) {
  CHECK(download_);
  const auto* pack = download_->pack;
  const auto temporary = download_->directory;
  download_.reset();
  errors_[std::string(pack->id)] = std::move(message);
  worker_->PostTask(FROM_HERE, base::BindOnce(&RemoveTemporary, temporary));
  NotifyOnSodaInstallError(::speech::GetLanguageCode(Locale(pack->language)),
                           ErrorCode::kUnspecifiedError);
  Notify(*pack);
}
void Installer::RemovePack(std::string id) {
  const auto* pack = PackForId(id);
  if (!pack || root_.empty() || download_)
    return;
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath path) {
            return !base::IsLink(path) && base::DeletePathRecursively(path);
          },
          Directory(root_, *pack)),
      base::BindOnce(
          [](base::WeakPtr<Installer> self, const Pack* pack, bool removed) {
            if (!self)
              return;
            if (removed) {
              self->ready_.erase(std::string(pack->id));
              const auto locale = Locale(pack->language);
              if (self->GetLanguagePath(locale).empty())
                self->installed_languages_.erase(
                    ::speech::GetLanguageCode(locale));
              // Refresh the running utility's path snapshot as well as the UI.
              self->NotifyOnSodaInstalled(::speech::LanguageCode::kNone);
            } else
              self->errors_[std::string(pack->id)] =
                  "Stop voice typing and Live Caption, then try removing the "
                  "pack again.";
            self->Notify(*pack);
          },
          weak_factory_.GetWeakPtr(), pack));
}
void Installer::Notify(const Pack& pack) {
  const auto code = ::speech::GetLanguageCode(Locale(pack.language));
  NotifyOnSodaProgress(code, std::max(0, Progress(pack.id)));
}
}  // namespace krypton::speech

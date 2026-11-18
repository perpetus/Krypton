// Copyright 2026 The Krypton Authors. GPL-3.0.
#include "chrome/browser/krypton_translate/installer.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include "base/base_paths.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/files/memory_mapped_file.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/krypton_models/files.h"
#include "chrome/browser/krypton_translate/engine_manifest.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/common/chrome_paths.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/translate/core/browser/translate_pref_names.h"
#include "components/translate/core/browser/translate_prefs.h"
#include "content/public/browser/storage_partition.h"
#include "crypto/hash.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/zstd/src/lib/zstd.h"

namespace krypton::translation {
namespace {
Installer* instance = nullptr;
constexpr uint64_t kMaximumModel = 128 * 1024 * 1024;

base::FilePath LocalFile(const base::FilePath& root,
                         std::string_view relative) {
  const auto path = base::FilePath::FromUTF8Unsafe(relative);
  if (path.IsAbsolute() || path.ReferencesParent() ||
      relative.find(':') != relative.npos)
    return {};
  return root.Append(path);
}
base::FilePath PackDirectory(const base::FilePath& root,
                             const ModelPack& pack) {
  return root.AppendASCII(std::string(pack.language))
      .AppendASCII(std::string(pack.revision));
}
bool HashFile(const base::FilePath& path,
              uint64_t size,
              std::string_view hash) {
  return models::VerifyFile(path, size, hash, kMaximumModel);
}
bool VerifyPack(const base::FilePath& directory, const ModelPack& pack) {
  if (base::IsLink(directory))
    return false;
  for (const auto& file : pack.files) {
    const auto path = LocalFile(directory, file.path);
    if (path.empty() || base::IsLink(path.DirName()) ||
        !HashFile(path, file.decoded_size, file.decoded_sha256))
      return false;
  }
  return true;
}
enum class DecodeResult { kOk, kWrongSizeOrHash, kInvalidFrame, kWriteFailed };

DecodeResult DecodeFile(const base::FilePath& compressed,
                        const base::FilePath& directory,
                        const ModelFile& file) {
  if (!HashFile(compressed, file.size, file.sha256) ||
      file.decoded_size > kMaximumModel)
    return DecodeResult::kWrongSizeOrHash;
  base::MemoryMappedFile map;
  if (!map.Initialize(compressed))
    return DecodeResult::kWrongSizeOrHash;
  const auto frame_size = ZSTD_getFrameContentSize(map.data(), map.length());
  if (frame_size == ZSTD_CONTENTSIZE_ERROR ||
      (frame_size != ZSTD_CONTENTSIZE_UNKNOWN &&
       frame_size != file.decoded_size))
    return DecodeResult::kInvalidFrame;
  std::string decoded(static_cast<size_t>(file.decoded_size), '\0');
  const size_t count =
      ZSTD_decompress(decoded.data(), decoded.size(), map.data(), map.length());
  if (ZSTD_isError(count) || count != decoded.size())
    return DecodeResult::kInvalidFrame;
  const auto actual = crypto::hash::Sha256(base::as_byte_span(decoded));
  if (!base::EqualsCaseInsensitiveASCII(base::HexEncode(actual),
                                        file.decoded_sha256))
    return DecodeResult::kWrongSizeOrHash;
  const auto path = LocalFile(directory, file.path);
  if (path.empty() || !base::CreateDirectory(path.DirName()) ||
      base::IsLink(path.DirName()) ||
      !base::ImportantFileWriter::WriteFileAtomically(path, decoded))
    return DecodeResult::kWriteFailed;
  return DecodeResult::kOk;
}
void RemoveTemporary(base::FilePath path) {
  // Only directories created by this download job may reach this helper.
  if (!path.empty())
    base::DeletePathRecursively(path);
}
constexpr net::NetworkTrafficAnnotationTag kModelDownload =
    net::DefineNetworkTrafficAnnotation("krypton_local_translation_model", R"(
      semantics {
        sender: "Krypton local translation"
        description: "Downloads a pinned local language pack after explicit consent. No page text is sent."
        trigger: "The user chooses Download in the translation menu."
        data: "A fixed model file URL; no cookies, credentials or page content."
        destination: OTHER
        destination_other: "Mozilla's model attachment CDN"
      }
      policy {
        cookies_allowed: NO
        setting: "Downloads occur only from the explicit translation menu."
        chrome_policy { TranslateEnabled { TranslateEnabled: false } }
      })");
}  // namespace

struct Installer::Verified {
  base::FilePath root;
  base::FilePath library;
  std::set<PackKey> packs;
};
struct Installer::Download : public ProfileObserver {
  ~Download() override {
    cancelled->store(true, std::memory_order_relaxed);
    if (profile)
      profile->RemoveObserver(this);
  }
  void OnProfileWillBeDestroyed(Profile* closing) override {
    prefs.RemoveAll();
    closing->RemoveObserver(this);
    profile = nullptr;
    if (owner)
      owner->DownloadFailed(
          "Language-pack download cancelled because its profile closed.");
  }
  raw_ptr<Profile> profile = nullptr;
  base::WeakPtr<Installer> owner;
  PrefChangeRegistrar prefs;
  const ModelPack* pack = nullptr;
  scoped_refptr<network::SharedURLLoaderFactory> factory;
  base::FilePath directory;
  size_t file = 0;
  uint64_t received = 0;
  int progress = 0;
  std::unique_ptr<network::SimpleURLLoader> loader;
  std::shared_ptr<std::atomic_bool> cancelled =
      std::make_shared<std::atomic_bool>(false);
};

Installer::Installer()
    : worker_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  CHECK(!instance);
  instance = this;
}
Installer::~Installer() {
  if (download_) {
    const auto temporary = download_->directory;
    download_.reset();
    worker_->PostTask(FROM_HERE, base::BindOnce(&RemoveTemporary, temporary));
  }
  instance = nullptr;
}
Installer* Installer::Get() {
  return instance;
}
bool Installer::IsInit() const {
  return initialized_;
}
std::set<PackKey> Installer::RegisteredLanguagePacks() const {
  auto registered = installed_;
  if (download_)
    registered.insert(download_->pack->key);
  return registered;
}
std::set<PackKey> Installer::InstalledLanguagePacks() const {
  return installed_;
}
base::FilePath Installer::GetLibraryPath() const {
  return library_;
}
base::FilePath Installer::GetLanguagePackPath(PackKey key) const {
  const auto* pack = FindPack(key);
  return pack && installed_.contains(key) ? PackDirectory(root_, *pack)
                                          : base::FilePath();
}
void Installer::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}
void Installer::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

Installer::Verified Installer::Verify(std::set<PackKey> keys) {
  Verified result;
  base::FilePath user, module;
  // Installed builds keep the engine beside the versioned browser module.
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user) ||
      !base::PathService::Get(base::DIR_MODULE, &module))
    return result;
  result.root = user.AppendASCII("KryptonTranslate");
  const auto library = module.AppendASCII("krypton_translate.dll");
  if (HashFile(library, kEngineSize, kEngineSha256))
    result.library = library;
  for (auto key : keys) {
    if (const auto* pack = FindPack(key);
        pack && VerifyPack(PackDirectory(result.root, *pack), *pack))
      result.packs.insert(key);
  }
  return result;
}
void Installer::Init(base::RepeatingClosure ready) {
  if (initialized_) {
    ready.Run();
    return;
  }
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&Verify, std::set<PackKey>{}),
      base::BindOnce(&Installer::Initialized, weak_factory_.GetWeakPtr(),
                     std::move(ready)));
}
void Installer::Initialized(base::RepeatingClosure ready, Verified result) {
  root_ = std::move(result.root);
  library_ = std::move(result.library);
  initialized_ = !library_.empty();
  for (auto& observer : observers_)
    observer.OnInstallationChanged();
  ready.Run();
}
void Installer::InstallLanguagePack(PackKey) {
  // Automatic controller/API requests never grant network consent.
}
void Installer::ScanInstalled(base::OnceClosure callback) {
  std::set<PackKey> keys;
  for (const auto& pack : ModelPacks())
    keys.insert(pack.key);
  auto required = keys;
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&Verify, std::move(keys)),
      base::BindOnce(&Installer::Prepared, weak_factory_.GetWeakPtr(),
                     base::BindOnce([](base::OnceClosure callback,
                                       bool) { std::move(callback).Run(); },
                                    std::move(callback)),
                     std::move(required)));
}
void Installer::UnInstallLanguagePack(PackKey key) {
  const auto* pack = FindPack(key);
  if (!pack || root_.empty() || download_)
    return;
  const auto path = PackDirectory(root_, *pack);
  installed_.erase(key);
  Notify(key);
  worker_->PostTask(FROM_HERE, base::BindOnce(
                                   [](base::FilePath path) {
                                     base::DeletePathRecursively(path);
                                   },
                                   path));
}
void Installer::PreparePair(std::string source,
                            std::string target,
                            base::OnceCallback<void(bool)> callback) {
  auto required = RequiredPacks(source, target);
  if (!required) {
    std::move(callback).Run(false);
    return;
  }
  if (initialized_ && std::ranges::all_of(*required, [&](PackKey key) {
        return installed_.contains(key);
      })) {
    std::move(callback).Run(true);
    return;
  }
  auto keys = *required;
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&Verify, std::move(keys)),
      base::BindOnce(&Installer::Prepared, weak_factory_.GetWeakPtr(),
                     std::move(callback), std::move(*required)));
}
void Installer::Prepared(base::OnceCallback<void(bool)> callback,
                         std::set<PackKey> required,
                         Verified result) {
  root_ = std::move(result.root);
  library_ = std::move(result.library);
  initialized_ = !library_.empty();
  std::set<PackKey> removed;
  std::set<PackKey> added;
  for (auto key : required) {
    if (!result.packs.contains(key) && installed_.erase(key))
      removed.insert(key);
  }
  for (auto key : result.packs) {
    if (installed_.insert(key).second)
      added.insert(key);
  }
  // A running utility only sees the pack paths passed at launch. Refresh its
  // snapshot after a disk scan changes the set, before starting a translation.
  for (auto& observer : observers_) {
    for (auto key : added)
      observer.OnLanguagePackInstalled(key);
    if (!added.empty() || !removed.empty())
      observer.OnInstallationChanged();
  }
  const bool ready =
      initialized_ && std::ranges::all_of(required, [&](PackKey key) {
        return installed_.contains(key);
      });
  std::move(callback).Run(ready);
}
void Installer::RequestDownload(Profile& profile, PackKey key) {
  if (profile.IsOffTheRecord() || !translate::TranslatePrefs(profile.GetPrefs())
                                       .IsTranslateAllowedByPolicy())
    return;
  const auto* pack = FindPack(key);
  if (!pack || download_)
    return;
  download_ = std::make_unique<Download>();
  download_->profile = &profile;
  download_->owner = weak_factory_.GetWeakPtr();
  profile.AddObserver(download_.get());
  download_->prefs.Init(profile.GetPrefs());
  download_->prefs.Add(
      translate::prefs::kOfferTranslateEnabled,
      base::BindRepeating(
          [](base::WeakPtr<Installer> owner) {
            if (owner && owner->download_ && owner->download_->profile &&
                !translate::TranslatePrefs(
                     owner->download_->profile->GetPrefs())
                     .IsTranslateAllowedByPolicy())
              owner->DownloadFailed(
                  "Language-pack download cancelled by translation policy.");
          },
          weak_factory_.GetWeakPtr()));
  download_->pack = pack;
  download_->factory = profile.GetDefaultStoragePartition()
                           ->GetURLLoaderFactoryForBrowserProcess();
  errors_.erase(key);
  const uint64_t generation = ++generation_;
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](const ModelPack* pack) {
            base::FilePath user;
            if (!base::PathService::Get(chrome::DIR_USER_DATA, &user))
              return std::pair<base::FilePath, bool>{};
            const auto root = user.AppendASCII("KryptonTranslate");
            const auto directory = root.AppendASCII(
                "install-" +
                base::Uuid::GenerateRandomV4().AsLowercaseString());
            if (!base::CreateDirectory(directory) || base::IsLink(root))
              return std::pair<base::FilePath, bool>{};
            return std::make_pair(
                directory, VerifyPack(PackDirectory(root, *pack), *pack));
          },
          pack),
      base::BindOnce(&Installer::DownloadDirectory, weak_factory_.GetWeakPtr(),
                     generation));
  NotifyProgress(key);
}
void Installer::DownloadDirectory(uint64_t generation,
                                  std::pair<base::FilePath, bool> prepared) {
  auto directory = std::move(prepared.first);
  if (generation != generation_ || !download_) {
    worker_->PostTask(FROM_HERE,
                      base::BindOnce(&RemoveTemporary, std::move(directory)));
    return;
  }
  if (directory.empty()) {
    DownloadFailed("Cannot prepare the language-pack directory.");
    return;
  }
  download_->directory = std::move(directory);
  if (prepared.second) {
    Installed(generation, true);
    return;
  }
  NextFile();
}
void Installer::NextFile() {
  CHECK(download_);
  const uint64_t generation = generation_;
  if (download_->file == download_->pack->files.size()) {
    const auto temporary = download_->directory;
    const auto* pack = download_->pack;
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(
            [](base::FilePath temporary, const ModelPack* pack,
               std::shared_ptr<std::atomic_bool> cancelled) {
              if (cancelled->load(std::memory_order_relaxed))
                return false;
              const auto root = temporary.DirName();
              const auto destination = PackDirectory(root, *pack);
              if (!VerifyPack(temporary, *pack) ||
                  !base::CreateDirectory(destination.DirName()) ||
                  base::IsLink(destination.DirName()))
                return false;
              if (cancelled->load(std::memory_order_relaxed))
                return false;
              if (base::PathExists(destination)) {
                if (VerifyPack(destination, *pack))
                  return true;
                // Repair only this owned cache after an explicit download.
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
            temporary, pack, download_->cancelled),
        base::BindOnce(&Installer::Installed, weak_factory_.GetWeakPtr(),
                       generation));
    return;
  }
  const auto& file = download_->pack->files[download_->file];
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(file.url);
  CHECK(request->url.SchemeIs("https") &&
        request->url.host() == "firefox-settings-attachments.cdn.mozilla.net");
  // Identify this explicit model fetch as a Krypton downloader. The
  // distributor serves its published models to standalone download clients.
  request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                             "Krypton-Model-Downloader/1.0");
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->redirect_mode = network::mojom::RedirectMode::kError;
  request->load_flags = net::LOAD_DISABLE_CACHE;
  download_->loader =
      network::SimpleURLLoader::Create(std::move(request), kModelDownload);
  download_->loader->SetTimeoutDuration(base::Minutes(10));
  // These retries are part of the same explicit download. No background retry.
  download_->loader->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE |
             network::SimpleURLLoader::RETRY_ON_5XX);
  download_->loader->SetOnDownloadProgressCallback(base::BindRepeating(
      [](base::WeakPtr<Installer> self, uint64_t generation, uint64_t current) {
        if (!self || generation != self->generation_ || !self->download_)
          return;
        auto& job = *self->download_;
        const auto total = job.pack->download_bytes;
        const int progress = static_cast<int>(
            std::min<uint64_t>(99, (job.received + current) * 100 / total));
        if (progress != job.progress) {
          job.progress = progress;
          self->NotifyProgress(job.pack->key);
        }
      },
      weak_factory_.GetWeakPtr(), generation));
  download_->loader->DownloadToFile(
      download_->factory.get(),
      base::BindOnce(&Installer::Downloaded, weak_factory_.GetWeakPtr(),
                     generation),
      download_->directory.AppendASCII(
          "download-" + base::NumberToString(download_->file) + ".zst"),
      static_cast<int64_t>(file.size));
}
void Installer::Downloaded(uint64_t generation, base::FilePath path) {
  if (generation != generation_ || !download_)
    return;
  if (path.empty()) {
    const auto* response = download_->loader->ResponseInfo();
    const int status =
        response && response->headers ? response->headers->response_code() : 0;
    const std::string code = "HTTP " + base::NumberToString(status);
    DownloadFailed(
        status == 403 || status == 406
            ? "Mozilla's server refused the download (" + code + ")."
        : status >= 400
            ? "Mozilla's server could not provide the file (" + code + ")."
            : "The download failed (" +
                  net::ErrorToShortString(download_->loader->NetError()) +
                  ").");
    return;
  }
  download_->loader.reset();
  const auto file = download_->pack->files[download_->file];
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath compressed, base::FilePath directory,
             ModelFile file) {
            const auto result = DecodeFile(compressed, directory, file);
            if (!base::DeleteFile(compressed) && result == DecodeResult::kOk)
              return DecodeResult::kWriteFailed;
            return result;
          },
          std::move(path), download_->directory, file),
      base::BindOnce(
          [](base::WeakPtr<Installer> self, uint64_t generation,
             DecodeResult result) {
            if (!self || generation != self->generation_ || !self->download_)
              return;
            switch (result) {
              case DecodeResult::kOk:
                self->FileVerified(generation);
                return;
              case DecodeResult::kWrongSizeOrHash:
                self->DownloadFailed(
                    "Language-pack size or SHA-256 did not match.");
                return;
              case DecodeResult::kInvalidFrame:
                self->DownloadFailed(
                    "The language-pack file could not be unpacked.");
                return;
              case DecodeResult::kWriteFailed:
                self->DownloadFailed("Could not write the language-pack file.");
                return;
            }
          },
          weak_factory_.GetWeakPtr(), generation));
}
void Installer::FileVerified(uint64_t generation) {
  if (generation != generation_ || !download_)
    return;
  download_->received += download_->pack->files[download_->file++].size;
  NextFile();
}
void Installer::Installed(uint64_t generation, bool valid) {
  if (generation != generation_ || !download_)
    return;
  if (!valid) {
    DownloadFailed("Cannot finish the language-pack installation.");
    return;
  }
  const auto key = download_->pack->key;
  root_ = download_->directory.DirName();
  const auto temporary = download_->directory;
  download_.reset();
  installed_.insert(key);
  errors_.erase(key);
  worker_->PostTask(FROM_HERE, base::BindOnce(&RemoveTemporary, temporary));
  for (auto& observer : observers_)
    observer.OnLanguagePackInstalled(key);
  Notify(key);
}
void Installer::DownloadFailed(std::string message) {
  CHECK(download_);
  const auto key = download_->pack->key;
  const auto temporary = download_->directory;
  download_.reset();
  errors_[key] = std::move(message);
  worker_->PostTask(FROM_HERE, base::BindOnce(&RemoveTemporary, temporary));
  NotifyProgress(key);
}
void Installer::Notify(PackKey key) {
  for (auto& observer : observers_)
    observer.OnLanguagePackInstallationChanged(key);
}
void Installer::NotifyProgress(PackKey key) {
  const int progress = std::max(0, Progress(key));
  for (auto& observer : observers_)
    observer.OnLanguagePackProgress(key, progress);
}
int Installer::Progress(PackKey key) const {
  return download_ && download_->pack->key == key ? download_->progress : -1;
}
std::string Installer::Error(PackKey key) const {
  const auto it = errors_.find(key);
  return it == errors_.end() ? std::string() : it->second;
}
}  // namespace krypton::translation

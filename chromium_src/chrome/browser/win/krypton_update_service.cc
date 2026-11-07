// Copyright 2026 Krypton Authors
// Use of this source code is governed by the license in the LICENSE file.
#include "chrome/browser/win/krypton_update_service.h"

// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_path_watcher.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequence_bound.h"
#include "base/win/scoped_handle.h"
#include "build/branding_buildflags.h"
#include "chrome/browser/ui/webui/help/version_updater.h"
#include "chrome/browser/win/krypton_update_trust.h"

namespace krypton {
namespace {
std::atomic<bool> g_update_ready{false};
struct Algorithm {
  BCRYPT_ALG_HANDLE handle{};
  ~Algorithm() {
    if (handle)
      BCryptCloseAlgorithmProvider(handle, 0);
  }
};
struct Hash {
  BCRYPT_HASH_HANDLE handle{};
  ~Hash() {
    if (handle)
      BCryptDestroyHash(handle);
  }
};
struct Key {
  BCRYPT_KEY_HANDLE handle{};
  ~Key() {
    if (handle)
      BCryptDestroyKey(handle);
  }
};

bool VerifyHelper(std::string_view data, std::string_view signature) {
  if (signature.size() != 64)
    return false;
  Algorithm ecdsa, sha;
  Hash hash;
  Key key;
  if (BCryptOpenAlgorithmProvider(&ecdsa.handle, BCRYPT_ECDSA_P256_ALGORITHM,
                                  nullptr, 0) < 0 ||
      BCryptOpenAlgorithmProvider(&sha.handle, BCRYPT_SHA256_ALGORITHM, nullptr,
                                  0) < 0 ||
      BCryptCreateHash(sha.handle, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0)
    return false;
  struct Blob {
    BCRYPT_ECCKEY_BLOB header;
    std::array<unsigned char, 64> point;
  } blob{};
  blob.header = {BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
  std::copy(kReleasePublicKey.begin() + 1, kReleasePublicKey.end(),
            blob.point.begin());
  std::array<unsigned char, 32> digest{};
  return BCryptImportKeyPair(ecdsa.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                             &key.handle, reinterpret_cast<PUCHAR>(&blob),
                             sizeof(blob), 0) >= 0 &&
         BCryptHashData(
             hash.handle,
             reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
             static_cast<ULONG>(data.size()), 0) >= 0 &&
         BCryptFinishHash(hash.handle, digest.data(),
                          static_cast<ULONG>(digest.size()), 0) >= 0 &&
         BCryptVerifySignature(
             key.handle, nullptr, digest.data(),
             static_cast<ULONG>(digest.size()),
             reinterpret_cast<PUCHAR>(const_cast<char*>(signature.data())),
             static_cast<ULONG>(signature.size()), 0) >= 0;
}

void StartVerifiedHelper() {
  base::FilePath module;
  if (!base::PathService::Get(base::DIR_MODULE, &module))
    return;
  const auto updater =
      module.Append(L"Installer").Append(L"KryptonUpdater.exe");
  // Hold the exact executable against replacement until CreateProcess opens
  // it. No executable or network I/O is performed on the browser UI thread.
  base::win::ScopedHandle lock(
      CreateFileW(updater.value().c_str(), GENERIC_READ, FILE_SHARE_READ,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!lock.is_valid())
    return;
  std::string binary, signature;
  if (!base::ReadFileToStringWithMaxSize(updater, &binary, 16 * 1024 * 1024) ||
      !base::ReadFileToStringWithMaxSize(updater.AddExtension(L".sig"),
                                         &signature, 64) ||
      !VerifyHelper(binary, signature))
    return;
  base::CommandLine command(updater);
  command.AppendSwitch("browser-start");
  base::LaunchOptions options;
  options.start_hidden = true;
  options.current_directory = updater.DirName();
  base::LaunchProcess(command, options);
}

class UpdateStatusWatcher {
 public:
  using Callback = base::RepeatingCallback<
      void(VersionUpdater::Status, int, const std::u16string&)>;
  explicit UpdateStatusWatcher(Callback callback)
      : callback_(std::move(callback)) {
    base::FilePath directory;
    if (!base::PathService::Get(base::DIR_EXE, &directory) ||
        !base::FilePath::CompareEqualIgnoreCase(directory.BaseName().value(),
                                                L"Application")) {
      callback_.Run(
          VersionUpdater::DISABLED, 0,
          u"Install Krypton with KryptonSetup to enable signed updates.");
      return;
    }
    path_ = directory.DirName().Append(L"Update").Append(L"status.json");
    if (!watcher_.Watch(path_, base::FilePathWatcher::Type::kNonRecursive,
                        base::BindRepeating(&UpdateStatusWatcher::Changed,
                                            base::Unretained(this)))) {
      callback_.Run(VersionUpdater::FAILED, 0,
                    u"Could not watch Krypton's update status.");
      return;
    }
    ReadStatus();
  }

 private:
  void Changed(const base::FilePath&, bool error) {
    if (!error)
      ReadStatus();
  }
  void ReadStatus() {
    std::string text;
    if (!base::ReadFileToStringWithMaxSize(path_, &text, 65536))
      return;
    auto value = base::JSONReader::ReadDict(text, base::JSON_PARSE_RFC);
    if (!value)
      return;
    const auto* state = value->FindString("state");
    if (!state)
      return;
    g_update_ready.store(*state == "ready", std::memory_order_release);
    auto status = VersionUpdater::CHECKING;
    if (*state == "ready")
      status = VersionUpdater::NEARLY_UPDATED;
    else if (*state == "downloading" || *state == "installing" ||
             *state == "available")
      status = VersionUpdater::UPDATING;
    else if (*state == "up_to_date" || *state == "updated")
      status = VersionUpdater::UPDATED;
    else if (*state == "error")
      status = VersionUpdater::FAILED;
    const auto* message = value->FindString("message");
    callback_.Run(status,
                  std::clamp(value->FindInt("progress").value_or(0), 0, 100),
                  message ? base::UTF8ToUTF16(*message) : std::u16string{});
  }
  Callback callback_;
  base::FilePath path_;
  base::FilePathWatcher watcher_;
};

void WatchStartupStatus() {
  static base::NoDestructor<base::SequenceBound<UpdateStatusWatcher>> watcher(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT}),
      base::BindRepeating(
          [](VersionUpdater::Status, int, const std::u16string&) {}));
}

#if !BUILDFLAG(GOOGLE_CHROME_BRANDING)
class KryptonVersionUpdater final : public VersionUpdater {
 public:
  void CheckForUpdate(StatusCallback callback, PromoteCallback) override {
    weak_.InvalidateWeakPtrs();
    callback_ = std::move(callback);
    OnStatus(CHECKING, 0, {});
    watcher_.Reset();
    watcher_ = base::SequenceBound<UpdateStatusWatcher>(
        base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE}),
        base::BindPostTaskToCurrentDefault(base::BindRepeating(
            &KryptonVersionUpdater::OnStatus, weak_.GetWeakPtr())));
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&StartVerifiedHelper));
  }

 private:
  void OnStatus(Status status, int progress, const std::u16string& message) {
    if (!callback_.is_null())
      callback_.Run(status, progress, false, false, std::string{}, 0, message);
  }
  StatusCallback callback_;
  base::SequenceBound<UpdateStatusWatcher> watcher_;
  base::WeakPtrFactory<KryptonVersionUpdater> weak_{this};
};
#endif
}  // namespace

void MaybeStartSignedUpdater() {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          "disable-background-networking"))
    return;
  WatchStartupStatus();
  base::ThreadPool::PostTask(FROM_HERE,
                             {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
                              base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
                             base::BindOnce(&StartVerifiedHelper));
}

bool PrepareSignedUpdateRelaunch() {
  if (!g_update_ready.load(std::memory_order_acquire))
    return false;
  base::win::ScopedHandle event(OpenEventW(
      EVENT_MODIFY_STATE, FALSE, L"Local\\KryptonSignedUpdateRelaunch"));
  return event.is_valid() && SetEvent(event.get());
}
}  // namespace krypton

#if !BUILDFLAG(GOOGLE_CHROME_BRANDING)
std::unique_ptr<VersionUpdater> VersionUpdater::Create(content::WebContents*) {
  return std::make_unique<krypton::KryptonVersionUpdater>();
}
#endif

#include "chrome/browser/krypton_agent/history_maintenance.h"

#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/wall_clock_timer.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/krypton_agent/agent_component.h"
#include "chrome/browser/krypton_agent/history_index.h"
#include "chrome/browser/krypton_agent/history_prefs.h"
#include "chrome/browser/krypton_agent/history_space.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_manager_observer.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"

namespace krypton_agent {
namespace {

class ProfileRetention : public ProfileObserver {
 public:
  ProfileRetention(Profile& profile, HistoryPurge on_purge)
      : profile_(&profile),
        path_(profile.GetPath()),
        on_purge_(std::move(on_purge)) {
    profile_observation_.Observe(&profile);
    prefs_.Init(profile.GetPrefs());
    for (const auto* pref : {kArchiveAfterDays, kDeleteAfterDays}) {
      prefs_.Add(pref, base::BindRepeating(&ProfileRetention::Schedule,
                                           weak_factory_.GetWeakPtr()));
    }
    Schedule();
  }

  void Schedule() {
    if (!profile_ || !IsProductionAgentEnabled()) {
      return;
    }
    const auto& prefs = *profile_->GetPrefs();
    const int archive = prefs.GetInteger(kArchiveAfterDays);
    const int remove = prefs.GetInteger(kDeleteAfterDays);
    timer_.Stop();
    pending_ = true;
    if (running_) {
      return;
    }
    pending_ = false;
    running_ = HistoryTaskRunner()->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&SweepHistory, path_, HistoryRetention{archive, remove}),
        base::BindOnce(&ProfileRetention::OnSweep, weak_factory_.GetWeakPtr()));
  }

  void OnProfileWillBeDestroyed(Profile* profile) override {
    prefs_.Reset();
    profile_observation_.Reset();
    profile_ = nullptr;
    timer_.Stop();
    weak_factory_.InvalidateWeakPtrs();
  }

 private:
  void OnSweep(std::optional<HistorySweep> result) {
    running_ = false;
    if (result && !result->deleted_ids.empty()) {
      on_purge_.Run(path_, std::move(result->deleted_ids));
    }
    if (pending_) {
      Schedule();
    } else if (result && !result->next_deadline.is_max()) {
      timer_.Start(FROM_HERE, result->next_deadline,
                   base::BindOnce(&ProfileRetention::Schedule,
                                  weak_factory_.GetWeakPtr()));
    }
  }

  raw_ptr<Profile> profile_;
  const base::FilePath path_;
  const HistoryPurge on_purge_;
  PrefChangeRegistrar prefs_;
  base::WallClockTimer timer_;
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{this};
  bool pending_ = false;
  bool running_ = false;
  base::WeakPtrFactory<ProfileRetention> weak_factory_{this};
};

class RetentionManager : public ProfileManagerObserver {
 public:
  RetentionManager(ProfileManager& manager,
                   HistoryPurge on_purge,
                   HistoryProfileDeleted on_profile_deleted)
      : on_purge_(std::move(on_purge)),
        on_profile_deleted_(std::move(on_profile_deleted)) {
    observation_.Observe(&manager);
    space_removal_ = ObserveSpaceRemoval(base::BindRepeating(
        &RetentionManager::OnSpaceRemoved, weak_factory_.GetWeakPtr()));
    for (auto* profile : manager.GetLoadedProfiles()) {
      OnProfileAdded(profile);
    }
  }

  void OnProfileAdded(Profile* profile) override {
    if (profile->IsRegularProfile()) {
      profiles_.insert_or_assign(
          profile->GetPath(),
          std::make_unique<ProfileRetention>(*profile, on_purge_));
    }
  }

  void OnProfileMarkedForPermanentDeletion(Profile* profile) override {
    const auto path = profile->GetPath();
    profiles_.erase(path);
    on_profile_deleted_.Run(path);
    HistoryTaskRunner()->PostTask(
        FROM_HERE, base::BindOnce(&DeleteProfileAgentData, path));
  }

  void OnProfileManagerDestroying() override {
    profiles_.clear();
    observation_.Reset();
    space_removal_ = {};
    weak_factory_.InvalidateWeakPtrs();
  }

  void Schedule(const base::FilePath& path) {
    if (auto it = profiles_.find(path); it != profiles_.end()) {
      it->second->Schedule();
    }
  }

 private:
  void OnSpaceRemoved(Profile& catalog, uint64_t space) {
    auto* manager = observation_.GetSource();
    if (!manager || !catalog.IsRegularProfile() ||
        !IsProductionAgentEnabled()) {
      return;
    }
    const auto key =
        HistorySpaceKey(catalog.GetPath().BaseName().AsUTF8Unsafe(), space);
    for (const auto* entry :
         manager->GetProfileAttributesStorage().GetAllProfilesAttributes()) {
      const auto path = entry->GetPath();
      HistoryTaskRunner()->PostTaskAndReplyWithResult(
          FROM_HERE,
          base::BindOnce(&DeleteHistoryRecords, path,
                         std::optional<std::string>(), std::make_optional(key)),
          base::BindOnce(&RetentionManager::OnSpaceRecordsRemoved,
                         weak_factory_.GetWeakPtr(), path));
    }
  }

  void OnSpaceRecordsRemoved(base::FilePath path,
                             std::optional<base::ListValue> deleted) {
    if (deleted && !deleted->empty()) {
      on_purge_.Run(path, std::move(*deleted));
      Schedule(path);
    }
  }

  const HistoryPurge on_purge_;
  const HistoryProfileDeleted on_profile_deleted_;
  std::map<base::FilePath, std::unique_ptr<ProfileRetention>> profiles_;
  base::ScopedObservation<ProfileManager, ProfileManagerObserver> observation_{
      this};
  base::CallbackListSubscription space_removal_;
  base::WeakPtrFactory<RetentionManager> weak_factory_{this};
};

std::unique_ptr<RetentionManager>& Manager() {
  static base::NoDestructor<std::unique_ptr<RetentionManager>> manager;
  return *manager;
}

}  // namespace

void StartHistoryMaintenance(HistoryPurge on_purge,
                             HistoryProfileDeleted on_profile_deleted) {
  auto* manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  if (manager && IsProductionAgentEnabled() && !Manager()) {
    Manager() = std::make_unique<RetentionManager>(
        *manager, std::move(on_purge), std::move(on_profile_deleted));
  }
}

void StopHistoryMaintenance() {
  Manager().reset();
}

void RescheduleHistoryMaintenance(const base::FilePath& profile) {
  if (Manager()) {
    Manager()->Schedule(profile);
  }
}

void EraseAllProfileAgentData(base::OnceClosure after_keys_erased) {
  auto* manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  if (!manager) {
    return;
  }
  std::vector<base::FilePath> paths;
  for (const auto* entry :
       manager->GetProfileAttributesStorage().GetAllProfilesAttributes()) {
    paths.push_back(entry->GetPath());
  }
  HistoryTaskRunner()->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(
          [](const std::vector<base::FilePath>& paths) {
            for (const auto& path : paths) {
              DeleteProfileAgentData(path);
            }
          },
          std::move(paths)),
      std::move(after_keys_erased));
}

}  // namespace krypton_agent

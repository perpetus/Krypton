// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/space_profiles.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/nuke_profile_directory_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profiles_state.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/krypton_ui/folder_prefs.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"

namespace krypton_ui {
namespace {
constexpr size_t kMaxProfileName = 128;

ProfileManager* Manager() {
  return g_browser_process ? g_browser_process->profile_manager() : nullptr;
}

// Persistent, unlocked profiles of this user-data directory. Guest, system,
// ephemeral, omitted and pending-deletion profiles never host Spaces.
bool Eligible(const ProfileManager& manager,
              const ProfileAttributesEntry& entry) {
  const auto path = entry.GetPath();
  return path.DirName() == manager.user_data_dir() &&
         path != ProfileManager::GetGuestProfilePath() &&
         path != ProfileManager::GetSystemProfilePath() && !entry.IsOmitted() &&
         !entry.IsEphemeral() && !entry.IsSigninRequired() &&
         !IsProfileDirectoryMarkedForDeletion(path);
}

// Unmapped Spaces, and always the Default Space, belong to the catalog owner.
bool OwnerSpace(const Workspace& space) {
  return space.profile.empty() || space.id == kDefaultWorkspace;
}

std::vector<base::FilePath>& ProfileIds() {
  static base::NoDestructor<std::vector<base::FilePath>> ids;
  return *ids;
}
}  // namespace

KrProfileId SpaceProfileIdFor(const base::FilePath& path) {
  auto& ids = ProfileIds();
  const auto found = std::ranges::find(ids, path);
  if (found != ids.end()) {
    return static_cast<KrProfileId>(found - ids.begin()) + 1;
  }
  ids.push_back(path);
  return ids.size();
}

std::optional<base::FilePath> SpaceProfilePath(KrProfileId id) {
  const auto& ids = ProfileIds();
  if (!id || id > ids.size()) {
    return std::nullopt;
  }
  return ids[id - 1];
}

std::set<KrSpaceId> SpacesInUse(Profile& catalog_owner) {
  std::set<KrSpaceId> used;
  const auto catalog = catalog_owner.GetBaseName().AsUTF8Unsafe();
  // An existence scan over every window; activation order does not matter.
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        const bool own =
            browser->GetProfile()->GetOriginalProfile() == &catalog_owner;
        for (auto* tab : *browser->GetTabStripModel()) {
          auto* contents = tab->GetContents();
          const auto space = WorkspaceForTab(contents);
          if (space && (own ? WorkspaceCatalogForTab(contents).empty()
                            : WorkspaceCatalogForTab(contents) == catalog)) {
            used.insert(space);
          }
        }
        return true;
      });
  for (const auto& folder : ReadFolders(*catalog_owner.GetPrefs()).items) {
    used.insert(folder.space);
  }
  return used;
}

SpaceProfile ResolveSpaceProfile(const Profile& catalog_owner,
                                 const Workspace& space) {
  if (OwnerSpace(space)) {
    return {SpaceProfileStatus::kOwner, catalog_owner.GetPath()};
  }
  if (!ValidProfileBaseName(space.profile)) {
    return {SpaceProfileStatus::kUnavailable, {}};
  }
  auto* manager = Manager();
  if (!manager) {
    return {SpaceProfileStatus::kMissing, {}};
  }
  const auto path = manager->user_data_dir().Append(
      base::FilePath::FromUTF8Unsafe(space.profile));
  if (path == catalog_owner.GetPath()) {
    return {SpaceProfileStatus::kOwner, path};
  }
  // Off-the-record windows read the original profile's catalog, but never host
  // another persistent profile.
  if (catalog_owner.IsOffTheRecord()) {
    return {SpaceProfileStatus::kUnavailable, {}};
  }
  const auto* entry =
      manager->GetProfileAttributesStorage().GetProfileAttributesWithPath(path);
  if (!entry || IsProfileDirectoryMarkedForDeletion(path)) {
    return {SpaceProfileStatus::kMissing, {}};
  }
  if (!Eligible(*manager, *entry)) {
    return {SpaceProfileStatus::kUnavailable, {}};
  }
  return {SpaceProfileStatus::kAvailable, path};
}

bool SpaceAcceptsProfile(const Profile& catalog_owner,
                         const Workspace& space,
                         const Profile& tab_profile) {
  // Fast path for tab insertion: no lookup or allocation.
  if (OwnerSpace(space)) {
    return &tab_profile == &catalog_owner;
  }
  const auto resolved = ResolveSpaceProfile(catalog_owner, space);
  switch (resolved.status) {
    case SpaceProfileStatus::kOwner:
      return &tab_profile == &catalog_owner;
    case SpaceProfileStatus::kAvailable:
      return !tab_profile.IsOffTheRecord() &&
             tab_profile.GetPath() == resolved.path;
    case SpaceProfileStatus::kMissing:
    case SpaceProfileStatus::kUnavailable:
      return false;
  }
  return false;
}

bool CatalogHostsSpace(const WorkspaceCatalog& catalog,
                       const Profile& window_profile,
                       KrSpaceId id) {
  const auto found = std::ranges::find(catalog.spaces, id, &Workspace::id);
  return found != catalog.spaces.end() &&
         SpaceAcceptsProfile(window_profile, *found, window_profile);
}

std::vector<SpaceProfileChoice> ListSpaceProfiles(
    const Profile& catalog_owner) {
  std::vector<SpaceProfileChoice> choices;
  auto* manager = Manager();
  if (!manager || catalog_owner.IsOffTheRecord()) {
    return choices;
  }
  for (const auto* entry :
       manager->GetProfileAttributesStorage()
           .GetAllProfilesAttributesSortedByNameWithCheck()) {
    if (Eligible(*manager, *entry)) {
      choices.push_back({entry->GetPath(), entry->GetName(),
                         entry->GetPath() == catalog_owner.GetPath()});
    }
  }
  return choices;
}

SpaceProfileAssignment AssignSpaceProfile(Profile& catalog_owner,
                                          KrSpaceId id,
                                          const base::FilePath& target) {
  if (catalog_owner.IsOffTheRecord()) {
    return SpaceProfileAssignment::kOffTheRecord;
  }
  auto& prefs = *catalog_owner.GetPrefs();
  const auto catalog = ReadWorkspaces(prefs);
  const auto found = std::ranges::find(catalog.spaces, id, &Workspace::id);
  if (found == catalog.spaces.end()) {
    return SpaceProfileAssignment::kUnknownSpace;
  }
  if (id == kDefaultWorkspace) {
    return SpaceProfileAssignment::kDefaultSpace;
  }
  std::string profile;
  if (!target.empty() && target != catalog_owner.GetPath()) {
    auto* manager = Manager();
    Workspace candidate = *found;
    candidate.profile = target.BaseName().AsUTF8Unsafe();
    if (!manager || target.DirName() != manager->user_data_dir() ||
        ResolveSpaceProfile(catalog_owner, candidate).status !=
            SpaceProfileStatus::kAvailable) {
      return SpaceProfileAssignment::kProfileUnavailable;
    }
    profile = std::move(candidate.profile);
  }
  if (found->profile == profile) {
    return SpaceProfileAssignment::kAssigned;
  }
  // Never relabel a Space whose tabs or folders would remain in the old
  // profile. Sleeping tabs keep their WebContents in the strip and count too.
  if (SpacesInUse(catalog_owner).contains(id)) {
    return SpaceProfileAssignment::kSpaceInUse;
  }
  return SetWorkspaceProfile(prefs, id, profile)
             ? SpaceProfileAssignment::kAssigned
             : SpaceProfileAssignment::kUnknownSpace;
}

SpaceProfileDirectory::SpaceProfileDirectory(base::RepeatingClosure changed)
    : changed_(std::move(changed)) {
  CHECK(changed_);
  if (auto* manager = Manager()) {
    observation_.Observe(&manager->GetProfileAttributesStorage());
  }
}

SpaceProfileDirectory::~SpaceProfileDirectory() = default;

void SpaceProfileDirectory::Create(std::u16string name, Created done) {
  const auto request = ++request_;
  pending_ = std::move(done);
  base::TrimWhitespace(name, base::TRIM_ALL, &name);
  auto* manager = Manager();
  if (name.empty() || name.size() > kMaxProfileName || !manager ||
      !profiles::IsProfileCreationAllowed()) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&SpaceProfileDirectory::Finish,
                                  weak_factory_.GetWeakPtr(), request,
                                  std::optional<base::FilePath>()));
    return;
  }
  ProfileManager::CreateMultiProfileAsync(
      name,
      manager->GetProfileAttributesStorage()
          .ChooseAvatarIconIndexForNewProfile(),
      /*is_hidden=*/false,
      base::BindOnce(&SpaceProfileDirectory::Initialized,
                     weak_factory_.GetWeakPtr(), request));
}

void SpaceProfileDirectory::Initialized(uint64_t request, Profile* profile) {
  // Keep only the path. The Profile may unload once nothing holds it.
  Finish(request, profile ? std::optional<base::FilePath>(profile->GetPath())
                          : std::nullopt);
}

void SpaceProfileDirectory::Finish(uint64_t request,
                                   std::optional<base::FilePath> path) {
  if (request != request_ || !pending_) {
    return;
  }
  std::move(pending_).Run(std::move(path));
}

void SpaceProfileDirectory::OnProfileAdded(const base::FilePath&) {
  changed_.Run();
}
void SpaceProfileDirectory::OnProfileWillBeRemoved(const base::FilePath&) {
  changed_.Run();
}
void SpaceProfileDirectory::OnProfileWasRemoved(const base::FilePath&,
                                                const std::u16string&) {
  changed_.Run();
}
void SpaceProfileDirectory::OnProfileNameChanged(const base::FilePath&,
                                                 const std::u16string&) {
  changed_.Run();
}
void SpaceProfileDirectory::OnProfileSigninRequiredChanged(
    const base::FilePath&) {
  changed_.Run();
}
void SpaceProfileDirectory::OnProfileIsOmittedChanged(const base::FilePath&) {
  changed_.Run();
}
}  // namespace krypton_ui

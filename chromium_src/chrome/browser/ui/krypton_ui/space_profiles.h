// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SPACE_PROFILES_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SPACE_PROFILES_H_

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_attributes_storage_observer.h"
#include "chrome/browser/ui/krypton_ui/workspace_prefs.h"

class Profile;

namespace krypton_ui {
// Each Space hosts its tabs in one persistent profile; several Spaces may share
// a profile. Identity is the profile directory base name, as in
// ProfileAttributesStorage. Nothing here loads a profile, copies data between
// profiles or substitutes another profile for a missing one.
//
// A window's own browser hosts the Spaces of its own profile. A Space mapped
// to another profile is hosted by a Browser of that profile sharing the
// window's frame (see SpaceHosts); it never falls back to the window's profile.
enum class SpaceProfileStatus {
  kOwner,        // The profile that owns the Space catalog.
  kAvailable,    // Another persistent, eligible profile.
  kMissing,      // Removed, unknown or marked for deletion.
  kUnavailable,  // Damaged mapping, ineligible profile or off-the-record owner.
};
struct SpaceProfile {
  SpaceProfileStatus status = SpaceProfileStatus::kMissing;
  base::FilePath path;  // Set for kOwner and kAvailable.
};

// Reads in-memory profile attributes only.
[[nodiscard]] SpaceProfile ResolveSpaceProfile(const Profile& catalog_owner,
                                               const Workspace& space);
// Whether a tab of `tab_profile` may belong to `space` of `catalog_owner`.
[[nodiscard]] bool SpaceAcceptsProfile(const Profile& catalog_owner,
                                       const Workspace& space,
                                       const Profile& tab_profile);
// Whether the browser of `window_profile`, which owns `catalog`, may hold tabs
// of Space `id`. False for unknown Spaces.
[[nodiscard]] bool CatalogHostsSpace(const WorkspaceCatalog& catalog,
                                     const Profile& window_profile,
                                     KrSpaceId id);

struct SpaceProfileChoice {
  base::FilePath path;
  std::u16string name;
  bool owner = false;
};
// Eligible persistent profiles, in the storage's display order.
[[nodiscard]] std::vector<SpaceProfileChoice> ListSpaceProfiles(
    const Profile& catalog_owner);

// Session-scoped ABI identity of a profile path, stable while the process
// lives. Never persisted; persistence uses the directory base name.
[[nodiscard]] KrProfileId SpaceProfileIdFor(const base::FilePath& path);
[[nodiscard]] std::optional<base::FilePath> SpaceProfilePath(KrProfileId id);

// Spaces of `catalog_owner`'s catalog that still hold tabs (in any window of
// that profile, including sleeping tabs, or hosted for it by another profile)
// or folders. Their profile cannot change.
[[nodiscard]] std::set<KrSpaceId> SpacesInUse(Profile& catalog_owner);

enum class SpaceProfileAssignment {
  kAssigned,
  kUnknownSpace,
  kDefaultSpace,
  kOffTheRecord,
  kProfileUnavailable,
  kSpaceInUse,
};
// Changes which profile an empty Space uses; an empty `target` restores the
// catalog owner. Refused while any window of the owner profile, including its
// off-the-record windows, holds a tab of the Space (discarded tabs included),
// or while folders belong to the Space. Cookies, logins, history and sessions
// are never copied or migrated.
[[nodiscard]] SpaceProfileAssignment AssignSpaceProfile(
    Profile& catalog_owner,
    KrSpaceId id,
    const base::FilePath& target);

// Profile choices for Space assignment. ProfileAttributesStorage pushes
// changes; there is no polling. Not created during ordinary rendering.
class SpaceProfileDirectory final : public ProfileAttributesStorageObserver {
 public:
  using Created = base::OnceCallback<void(std::optional<base::FilePath>)>;

  // `changed` runs when eligible profiles may have changed. Callers coalesce.
  explicit SpaceProfileDirectory(base::RepeatingClosure changed);
  SpaceProfileDirectory(const SpaceProfileDirectory&) = delete;
  SpaceProfileDirectory& operator=(const SpaceProfileDirectory&) = delete;
  ~SpaceProfileDirectory() override;

  // Creates and initializes a persistent profile without opening a window or
  // changing the last-used profile. Completes asynchronously; a newer request
  // or destruction drops an older callback. Reports only the new path.
  void Create(std::u16string name, Created done);

 private:
  void OnProfileAdded(const base::FilePath& path) override;
  void OnProfileWillBeRemoved(const base::FilePath& path) override;
  void OnProfileWasRemoved(const base::FilePath& path,
                           const std::u16string& name) override;
  void OnProfileNameChanged(const base::FilePath& path,
                            const std::u16string& old_name) override;
  void OnProfileSigninRequiredChanged(const base::FilePath& path) override;
  void OnProfileIsOmittedChanged(const base::FilePath& path) override;
  void Initialized(uint64_t request, Profile* profile);
  void Finish(uint64_t request, std::optional<base::FilePath> path);

  base::RepeatingClosure changed_;
  base::ScopedObservation<ProfileAttributesStorage,
                          ProfileAttributesStorageObserver>
      observation_{this};
  Created pending_;
  uint64_t request_ = 0;
  base::WeakPtrFactory<SpaceProfileDirectory> weak_factory_{this};
};
}  // namespace krypton_ui
#endif  // CHROME_BROWSER_UI_KRYPTON_UI_SPACE_PROFILES_H_

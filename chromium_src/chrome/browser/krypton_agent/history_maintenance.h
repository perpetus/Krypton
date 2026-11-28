#ifndef CHROME_BROWSER_KRYPTON_AGENT_HISTORY_MAINTENANCE_H_
#define CHROME_BROWSER_KRYPTON_AGENT_HISTORY_MAINTENANCE_H_

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/values.h"

namespace krypton_agent {

using HistoryPurge = base::RepeatingCallback<void(const base::FilePath& profile,
                                                  base::ListValue deleted_ids)>;
using HistoryProfileDeleted =
    base::RepeatingCallback<void(const base::FilePath& profile)>;

void StartHistoryMaintenance(HistoryPurge on_purge,
                             HistoryProfileDeleted on_profile_deleted);
void StopHistoryMaintenance();
void RescheduleHistoryMaintenance(const base::FilePath& profile);
void EraseAllProfileAgentData(base::OnceClosure after_keys_erased);

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_HISTORY_MAINTENANCE_H_

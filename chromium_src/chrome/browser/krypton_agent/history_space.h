#ifndef CHROME_BROWSER_KRYPTON_AGENT_HISTORY_SPACE_H_
#define CHROME_BROWSER_KRYPTON_AGENT_HISTORY_SPACE_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "base/callback_list.h"

class Profile;

namespace krypton_agent {

using SpaceRemoved = base::RepeatingCallback<void(Profile&, uint64_t)>;
base::CallbackListSubscription ObserveSpaceRemoval(SpaceRemoved callback);
void NotifySpaceRemoved(Profile& catalog, uint64_t space);
std::string HistorySpaceKey(std::string_view catalog, uint64_t space);

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_HISTORY_SPACE_H_

#include "chrome/browser/krypton_agent/history_space.h"

#include <utility>

#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"

namespace krypton_agent {
namespace {

base::RepeatingCallbackList<void(Profile&, uint64_t)>& SpaceRemovals() {
  static base::NoDestructor<
      base::RepeatingCallbackList<void(Profile&, uint64_t)>>
      callbacks;
  return *callbacks;
}

}  // namespace

base::CallbackListSubscription ObserveSpaceRemoval(SpaceRemoved callback) {
  return SpaceRemovals().Add(std::move(callback));
}

void NotifySpaceRemoved(Profile& catalog, uint64_t space) {
  SpaceRemovals().Notify(catalog, space);
}

std::string HistorySpaceKey(std::string_view catalog, uint64_t space) {
  return base::StrCat({catalog, ":", base::NumberToString(space)});
}

}  // namespace krypton_agent

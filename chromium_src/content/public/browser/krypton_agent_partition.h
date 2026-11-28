#ifndef CONTENT_PUBLIC_BROWSER_KRYPTON_AGENT_PARTITION_H_
#define CONTENT_PUBLIC_BROWSER_KRYPTON_AGENT_PARTITION_H_

#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_partition_config.h"

namespace content {

inline constexpr char kKryptonAgentPartition[] = "kryptonagentisolated";

inline bool IsKryptonAgentPartition(const StoragePartition* partition) {
  return partition &&
         partition->GetConfig().partition_domain() == kKryptonAgentPartition;
}

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_KRYPTON_AGENT_PARTITION_H_

#ifndef CHROME_BROWSER_KRYPTON_AGENT_HISTORY_INDEX_H_
#define CHROME_BROWSER_KRYPTON_AGENT_HISTORY_INDEX_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "base/values.h"

namespace base {
class SequencedTaskRunner;
}

namespace os_crypt_async {
class Encryptor;
}

namespace krypton_agent {

struct NewHistorySession {
  std::string id;
  std::string provider;
  std::string model;
  std::string space;
  std::string title;
};

struct HistoryContext {
  std::string profile_token;
  std::string session_key;
  base::DictValue history;
  base::DictValue credentials;
  bool instruction_like;
};

struct HistoryRetention {
  int archive_after_days;
  int delete_after_days;
};

struct TranscriptRequest {
  base::DictValue history;
  base::DictValue params;
};

struct HistorySweep {
  base::Time next_deadline;
  base::ListValue deleted_ids;
};

// Profile index operations run on this sequence, including permanent deletion.
scoped_refptr<base::SequencedTaskRunner> HistoryTaskRunner();

std::optional<HistoryContext> PrepareSettingsContext(
    const base::FilePath& profile_path,
    scoped_refptr<os_crypt_async::Encryptor> encryptor);
std::optional<HistoryContext> CreateHistorySession(
    const base::FilePath& profile_path,
    scoped_refptr<os_crypt_async::Encryptor> encryptor,
    NewHistorySession session);
std::optional<HistoryContext> ResumeHistorySession(
    const base::FilePath& profile_path,
    scoped_refptr<os_crypt_async::Encryptor> encryptor,
    NewHistorySession session);
std::optional<base::ListValue> ListHistory(
    const base::FilePath& profile_path,
    scoped_refptr<os_crypt_async::Encryptor> encryptor);
std::optional<TranscriptRequest> PrepareTranscriptRequest(
    const base::FilePath& profile_path,
    scoped_refptr<os_crypt_async::Encryptor> encryptor,
    const std::string& session_id,
    int cursor);
bool UpdateHistorySession(const base::FilePath& profile_path,
                          const std::string& session_id,
                          bool instruction_like,
                          std::optional<base::DictValue> provider_session);
bool ArchiveHistorySession(const base::FilePath& profile_path,
                           const std::string& session_id,
                           bool archived);
std::optional<base::ListValue> DeleteHistoryRecords(
    const base::FilePath& profile_path,
    const std::optional<std::string>& session_id,
    const std::optional<std::string>& space_id);
std::optional<HistorySweep> SweepHistory(const base::FilePath& profile_path,
                                         HistoryRetention retention);
void DeleteProfileAgentData(const base::FilePath& profile_path);

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_HISTORY_INDEX_H_

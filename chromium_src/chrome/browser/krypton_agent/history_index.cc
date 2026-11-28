#include "chrome/browser/krypton_agent/history_index.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/base_paths_win.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/values_util.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/krypton_agent/launch_authorization.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace krypton_agent {
namespace {

constexpr size_t kMaximumIndexBytes = 4 * 1024 * 1024;
constexpr size_t kMaximumSessions = 4096;
constexpr size_t kKeyBytes = 32;

base::FilePath IndexPath(const base::FilePath& profile_path) {
  return profile_path.AppendASCII("krypton-agent-index.json");
}

bool IsId(std::string_view id) {
  return !id.empty() && id.size() <= 128 &&
         std::ranges::all_of(id, [](char ch) {
           return base::IsAsciiAlpha(ch) || base::IsAsciiDigit(ch) ||
                  ch == '-' || ch == '_';
         });
}

bool IsProfileToken(std::string_view token) {
  return token.size() == 32 && std::ranges::all_of(token, [](char ch) {
           return base::IsHexDigit(ch);
         });
}

std::optional<base::DictValue> ReadIndex(const base::FilePath& path) {
  std::string json;
  if (!base::ReadFileToStringWithMaxSize(path, &json, kMaximumIndexBytes)) {
    return std::nullopt;
  }
  auto index = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!index || index->FindInt("version") != 1) {
    return std::nullopt;
  }
  const auto* token = index->FindString("profile_token");
  const auto* sessions = index->FindDict("sessions");
  if (!token || !IsProfileToken(*token) || !sessions ||
      sessions->size() > kMaximumSessions ||
      !index->FindString("credential_key")) {
    return std::nullopt;
  }
  for (const auto [id, value] : *sessions) {
    const auto* session = value.GetIfDict();
    if (!IsId(id) || !session || !session->FindString("key") ||
        !session->FindString("title") ||
        !base::ValueToTime(session->Find("created")) ||
        !base::ValueToTime(session->Find("activity"))) {
      return std::nullopt;
    }
  }
  return index;
}

bool WriteIndex(const base::FilePath& path, const base::DictValue& index) {
  const auto json = base::WriteJson(index);
  return json && json->size() <= kMaximumIndexBytes &&
         base::ImportantFileWriter::WriteFileAtomically(path, *json);
}

std::optional<std::string> Encrypt(const os_crypt_async::Encryptor& encryptor,
                                   const std::string& plaintext) {
  auto cipher = encryptor.EncryptString(plaintext);
  return cipher ? std::make_optional(base::Base64Encode(*cipher))
                : std::nullopt;
}

std::optional<std::string> Decrypt(const os_crypt_async::Encryptor& encryptor,
                                   const std::string& ciphertext) {
  auto bytes = base::Base64Decode(ciphertext);
  return bytes ? encryptor.DecryptData(*bytes) : std::nullopt;
}

std::optional<base::DictValue> ReadOrCreateIndex(
    const base::FilePath& path,
    const os_crypt_async::Encryptor& encryptor) {
  if (base::PathExists(path)) {
    return ReadIndex(path);
  }
  auto key = Encrypt(encryptor, base::RandBytesAsString(kKeyBytes));
  if (!key) {
    return std::nullopt;
  }
  return base::DictValue()
      .Set("version", 1)
      .Set("profile_token",
           base::ToLowerASCII(base::HexEncode(base::RandBytesAsVector(16))))
      .Set("credential_key", std::move(*key))
      .Set("sessions", base::DictValue());
}

base::TimeDelta Period(int days) {
  constexpr std::array allowed{1, 7, 30, 90};
  return std::ranges::contains(allowed, days) ? base::Days(days)
                                              : base::TimeDelta::Max();
}

base::DictValue RetainedHistory(const base::DictValue& index) {
  base::ListValue ids;
  for (const auto entry : *index.FindDict("sessions")) {
    ids.Append(entry.first);
  }
  return base::DictValue()
      .Set("profile_token", *index.FindString("profile_token"))
      .Set("ids", std::move(ids));
}

HistoryContext MakeContext(const base::DictValue& index,
                           std::string_view session_key,
                           std::string_view credential_key,
                           bool instruction_like) {
  const auto& token = *index.FindString("profile_token");
  return HistoryContext{token, base::Base64Encode(session_key),
                        RetainedHistory(index),
                        base::DictValue()
                            .Set("profile_token", token)
                            .Set("key", base::Base64Encode(credential_key)),
                        instruction_like};
}

}  // namespace

scoped_refptr<base::SequencedTaskRunner> HistoryTaskRunner() {
  static const base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>>
      runner(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN}));
  return *runner;
}

std::optional<HistoryContext> PrepareSettingsContext(
    const base::FilePath& profile_path,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  if (!encryptor || !encryptor->IsEncryptionAvailable()) {
    return std::nullopt;
  }
  const auto path = IndexPath(profile_path);
  const bool exists = base::PathExists(path);
  auto index = ReadOrCreateIndex(path, *encryptor);
  if (!index || (!exists && !WriteIndex(path, *index))) {
    return std::nullopt;
  }
  const auto key = Decrypt(*encryptor, *index->FindString("credential_key"));
  if (!key || key->size() != 32) {
    return std::nullopt;
  }
  return MakeContext(*index, {}, *key, false);
}

std::optional<HistoryContext> CreateHistorySession(
    const base::FilePath& profile_path,
    scoped_refptr<os_crypt_async::Encryptor> encryptor,
    NewHistorySession session) {
  if (!encryptor || !encryptor->IsEncryptionAvailable() || !IsId(session.id)) {
    return std::nullopt;
  }
  const auto path = IndexPath(profile_path);
  auto index = ReadOrCreateIndex(path, *encryptor);
  if (!index) {
    return std::nullopt;
  }
  auto& sessions = *index->FindDict("sessions");
  if (sessions.size() >= kMaximumSessions || sessions.contains(session.id)) {
    return std::nullopt;
  }
  const auto credential_key =
      Decrypt(*encryptor, *index->FindString("credential_key"));
  const auto raw_key = base::RandBytesAsString(kKeyBytes);
  auto encrypted_key = Encrypt(*encryptor, raw_key);
  std::string title_text;
  base::TruncateUTF8ToByteSize(
      base::TrimWhitespaceASCII(session.title, base::TrimPositions::TRIM_ALL),
      128, &title_text);
  auto title =
      Encrypt(*encryptor, title_text.empty() ? "New session" : title_text);
  if (!credential_key || credential_key->size() != kKeyBytes ||
      !encrypted_key || !title) {
    return std::nullopt;
  }
  const auto now = base::Time::Now();
  sessions.Set(session.id, base::DictValue()
                               .Set("provider", std::move(session.provider))
                               .Set("model", std::move(session.model))
                               .Set("space", std::move(session.space))
                               .Set("created", base::TimeToValue(now))
                               .Set("activity", base::TimeToValue(now))
                               .Set("archived", false)
                               .Set("title", std::move(*title))
                               .Set("key", std::move(*encrypted_key)));
  if (!WriteIndex(path, *index)) {
    return std::nullopt;
  }
  return MakeContext(*index, raw_key, *credential_key, false);
}

std::optional<HistoryContext> ResumeHistorySession(
    const base::FilePath& profile_path,
    scoped_refptr<os_crypt_async::Encryptor> encryptor,
    NewHistorySession session) {
  auto index = ReadIndex(IndexPath(profile_path));
  const auto* record =
      index ? index->FindDict("sessions")->FindDict(session.id) : nullptr;
  if (!record || !encryptor || !record->FindString("native_thread_id")) {
    return std::nullopt;
  }
  const auto* provider = record->FindString("provider");
  const auto* model = record->FindString("model");
  if (!provider || *provider != session.provider || !model ||
      *model != session.model) {
    return std::nullopt;
  }
  auto key = Decrypt(*encryptor, *record->FindString("key"));
  auto credential_key =
      Decrypt(*encryptor, *index->FindString("credential_key"));
  if (!key || key->size() != kKeyBytes || !credential_key ||
      credential_key->size() != kKeyBytes) {
    return std::nullopt;
  }
  return MakeContext(*index, *key, *credential_key,
                     record->FindBool("instruction_like").value_or(false));
}

std::optional<base::ListValue> ListHistory(
    const base::FilePath& profile_path,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  const auto path = IndexPath(profile_path);
  if (!base::PathExists(path)) {
    return base::ListValue();
  }
  auto index = ReadIndex(path);
  if (!index || !encryptor) {
    return std::nullopt;
  }
  base::ListValue sessions;
  for (const auto [id, value] : *index->FindDict("sessions")) {
    auto session = value.GetDict().Clone();
    auto title = Decrypt(*encryptor, *session.FindString("title"));
    session.Remove("key");
    if (!title) {
      return std::nullopt;
    }
    session.Set("title", std::move(*title));
    session.Set("session_id", id);
    sessions.Append(std::move(session));
  }
  return sessions;
}

std::optional<TranscriptRequest> PrepareTranscriptRequest(
    const base::FilePath& profile_path,
    scoped_refptr<os_crypt_async::Encryptor> encryptor,
    const std::string& session_id,
    int cursor) {
  auto index = ReadIndex(IndexPath(profile_path));
  const auto* session =
      index ? index->FindDict("sessions")->FindDict(session_id) : nullptr;
  if (!session || !encryptor || cursor < 0) {
    return std::nullopt;
  }
  auto key = Decrypt(*encryptor, *session->FindString("key"));
  if (!key || key->size() != kKeyBytes) {
    return std::nullopt;
  }
  return TranscriptRequest{RetainedHistory(*index),
                           base::DictValue()
                               .Set("session_id", session_id)
                               .Set("cursor", cursor)
                               .Set("key", base::Base64Encode(*key))};
}

bool UpdateHistorySession(const base::FilePath& profile_path,
                          const std::string& session_id,
                          bool instruction_like,
                          std::optional<base::DictValue> provider_session) {
  const auto path = IndexPath(profile_path);
  auto index = ReadIndex(path);
  auto* session =
      index ? index->FindDict("sessions")->FindDict(session_id) : nullptr;
  if (!session) {
    return false;
  }
  session->Set("activity", base::TimeToValue(base::Time::Now()));
  session->Remove("unarchived");
  if (instruction_like) {
    session->Set("instruction_like", true);
  }
  if (provider_session) {
    for (const auto* field : {"provider_session_id", "native_thread_id"}) {
      const auto* value = provider_session->FindString(field);
      if (value && IsId(*value)) {
        session->Set(field, *value);
      }
    }
  }
  return WriteIndex(path, *index);
}

bool ArchiveHistorySession(const base::FilePath& profile_path,
                           const std::string& session_id,
                           bool archived) {
  const auto path = IndexPath(profile_path);
  auto index = ReadIndex(path);
  auto* session =
      index ? index->FindDict("sessions")->FindDict(session_id) : nullptr;
  if (!session) {
    return false;
  }
  session->Set("archived", archived);
  session->Set("unarchived", !archived);
  return WriteIndex(path, *index);
}

std::optional<base::ListValue> DeleteHistoryRecords(
    const base::FilePath& profile_path,
    const std::optional<std::string>& session_id,
    const std::optional<std::string>& space_id) {
  const auto path = IndexPath(profile_path);
  base::ListValue deleted;
  if (!base::PathExists(path)) {
    return deleted;
  }
  auto index = ReadIndex(path);
  if (!index) {
    return std::nullopt;
  }
  auto& sessions = *index->FindDict("sessions");
  for (const auto [id, value] : sessions) {
    const auto* space = value.GetDict().FindString("space");
    if ((!session_id || id == *session_id) &&
        (!space_id || (space && *space == *space_id))) {
      deleted.Append(id);
    }
  }
  for (const auto& id : deleted) {
    sessions.Remove(id.GetString());
  }
  if (!deleted.empty() && !WriteIndex(path, *index)) {
    return std::nullopt;
  }
  return deleted;
}

std::optional<HistorySweep> SweepHistory(const base::FilePath& profile_path,
                                         HistoryRetention retention) {
  HistorySweep sweep{base::Time::Max(), base::ListValue()};
  const auto path = IndexPath(profile_path);
  if (!base::PathExists(path)) {
    return sweep;
  }
  auto index = ReadIndex(path);
  if (!index) {
    return std::nullopt;
  }
  const auto now = base::Time::Now();
  const auto archive = Period(retention.archive_after_days);
  const auto expire = Period(retention.delete_after_days);
  auto& sessions = *index->FindDict("sessions");
  bool dirty = false;
  for (auto [id, value] : sessions) {
    auto& session = value.GetDict();
    const auto activity = *base::ValueToTime(session.Find("activity"));
    const auto deletion =
        expire.is_max() ? base::Time::Max() : activity + expire;
    if (deletion <= now) {
      sweep.deleted_ids.Append(id);
      continue;
    }
    sweep.next_deadline = std::min(sweep.next_deadline, deletion);
    if (archive >= expire || session.FindBool("archived").value_or(false) ||
        session.FindBool("unarchived").value_or(false)) {
      continue;
    }
    const auto archiving = activity + archive;
    if (archiving <= now) {
      session.Set("archived", true);
      dirty = true;
    } else {
      sweep.next_deadline = std::min(sweep.next_deadline, archiving);
    }
  }
  for (const auto& id : sweep.deleted_ids) {
    sessions.Remove(id.GetString());
  }
  if ((dirty || !sweep.deleted_ids.empty()) && !WriteIndex(path, *index)) {
    return std::nullopt;
  }
  return sweep;
}

void DeleteProfileAgentData(const base::FilePath& profile_path) {
  const auto path = IndexPath(profile_path);
  auto index = ReadIndex(path);
  if (!base::DeleteFile(path) || !index) {
    return;
  }
  base::FilePath local;
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local)) {
    return;
  }
  const auto root = local.AppendASCII("Krypton").AppendASCII("Agent");
  const auto& token = *index->FindString("profile_token");
  RemoveProviderSandboxes(token);
  // The sandbox sweep owns work and config deletion and skips live instances.
  for (const auto* folder : {"history", "secrets"}) {
    base::DeletePathRecursively(root.AppendASCII(folder).AppendASCII(token));
  }
}

}  // namespace krypton_agent

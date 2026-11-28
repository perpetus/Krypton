// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_DOCUMENT_STORE_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_DOCUMENT_STORE_H_

#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"

class Profile;

namespace krypton_ui {
enum class DocumentKind { kNote, kEasel };
bool IsDocumentId(std::string_view id);

// Created only on an explicit document/capture request. One serialized blocking
// sequence per profile; tasks retain paths, never a Profile or WebContents.
class DocumentStore : public base::RefCountedThreadSafe<DocumentStore> {
 public:
  using Reply = base::OnceCallback<void(base::DictValue)>;
  static scoped_refptr<DocumentStore> ForProfile(Profile* profile);
  explicit DocumentStore(base::FilePath root);
  void Request(DocumentKind kind, base::DictValue request, Reply reply);
  void CreateCapture(std::string png, Reply reply);
  void TransferCapture(std::string source,
                       std::string destination,
                       std::string revision,
                       Reply reply);
  void Export(DocumentKind kind,
              std::string id,
              std::string format,
              std::string png,
              base::FilePath destination,
              Reply reply);

 private:
  friend class base::RefCountedThreadSafe<DocumentStore>;
  ~DocumentStore();
  base::DictValue Run(DocumentKind kind, base::DictValue request);
  base::DictValue SaveCapture(std::string png);
  base::DictValue MoveCapture(std::string source,
                              std::string destination,
                              std::string revision);
  base::DictValue WriteExport(DocumentKind kind,
                              std::string id,
                              std::string format,
                              std::string png,
                              base::FilePath destination);
  base::FilePath Directory(DocumentKind kind, std::string_view id) const;
  const base::FilePath root_;
  const scoped_refptr<base::SequencedTaskRunner> runner_;
};
}  // namespace krypton_ui
#endif

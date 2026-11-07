// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_DEFERRED_COMMANDS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_DEFERRED_COMMANDS_H_

#include "base/containers/circular_deque.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"

namespace base {
class SequencedTaskRunner;
}

namespace krypton_ui {

// Defers browser mutations out of module callbacks, including callbacks made
// by a command's synchronous observers. Cancel before destroying a shell.
class DeferredCommands {
 public:
  explicit DeferredCommands(scoped_refptr<base::SequencedTaskRunner> runner);
  ~DeferredCommands();
  DeferredCommands(const DeferredCommands&) = delete;
  DeferredCommands& operator=(const DeferredCommands&) = delete;

  [[nodiscard]] bool Post(base::OnceClosure command);
  void Cancel();

 private:
  void Drain();

  const scoped_refptr<base::SequencedTaskRunner> runner_;
  base::circular_deque<base::OnceClosure> commands_;
  bool scheduled_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<DeferredCommands> weak_factory_{this};
};

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_DEFERRED_COMMANDS_H_

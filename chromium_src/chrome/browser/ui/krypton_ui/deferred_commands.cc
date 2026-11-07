// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/deferred_commands.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "base/trace_event/trace_event.h"

namespace krypton_ui {

DeferredCommands::DeferredCommands(
    scoped_refptr<base::SequencedTaskRunner> runner)
    : runner_(std::move(runner)) {
  CHECK(runner_);
}

DeferredCommands::~DeferredCommands() = default;

bool DeferredCommands::Post(base::OnceClosure command) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!command) {
    return false;
  }
  commands_.push_back(std::move(command));
  if (scheduled_) {
    return true;
  }
  scheduled_ = true;
  if (!runner_->PostNonNestableTask(
          FROM_HERE, base::BindOnce(&DeferredCommands::Drain,
                                    weak_factory_.GetWeakPtr()))) {
    Cancel();
    return false;
  }
  return true;
}

void DeferredCommands::Cancel() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  weak_factory_.InvalidateWeakPtrs();
  commands_.clear();
  scheduled_ = false;
}

void DeferredCommands::Drain() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  TRACE_EVENT0("browser", "KryptonUI.RunCommands");
  scheduled_ = false;
  const auto weak = weak_factory_.GetWeakPtr();
  // An observer may enqueue another intent while a command runs. Its command
  // belongs to a later UI task, even though this batch is still draining.
  const size_t count = commands_.size();
  for (size_t i = 0; i < count; ++i) {
    auto command = std::move(commands_.front());
    commands_.pop_front();
    std::move(command).Run();
    // Commands may close the browser and destroy this queue, or cancel the
    // rest of the batch. Do not access a member after either operation.
    if (!weak) {
      return;
    }
  }
}

}  // namespace krypton_ui

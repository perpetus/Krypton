// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_TEST_TASK_RUNNER_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_TEST_TASK_RUNNER_H_

#include <utility>

#include "base/containers/circular_deque.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"

namespace krypton_ui::test {

// A manually advanced FIFO for these synchronous adapter unit tests. Tasks
// never run inline, and no message loop, thread pool or rendering is started.
class TaskRunner final : public base::SequencedTaskRunner {
 public:
  bool PostDelayedTask(const base::Location&,
                       base::OnceClosure task,
                       base::TimeDelta) override {
    ++nestable_posts;
    return Enqueue(std::move(task));
  }
  bool PostNonNestableDelayedTask(const base::Location&,
                                  base::OnceClosure task,
                                  base::TimeDelta) override {
    return Enqueue(std::move(task));
  }
  bool RunsTasksInCurrentSequence() const override { return true; }
  bool RunOne() {
    if (tasks_.empty()) {
      return false;
    }
    auto task = std::move(tasks_.front());
    tasks_.pop_front();
    std::move(task).Run();
    return true;
  }
  size_t pending() const { return tasks_.size(); }
  bool accepting = true;
  size_t nestable_posts = 0;

 private:
  ~TaskRunner() override = default;
  bool Enqueue(base::OnceClosure task) {
    if (!accepting) {
      return false;
    }
    tasks_.push_back(std::move(task));
    return true;
  }
  base::circular_deque<base::OnceClosure> tasks_;
};

}  // namespace krypton_ui::test

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_TEST_TASK_RUNNER_H_

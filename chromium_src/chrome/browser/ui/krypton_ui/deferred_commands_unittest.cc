// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/deferred_commands.h"

#include <functional>
#include <memory>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "chrome/browser/ui/krypton_ui/test_task_runner.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
namespace {

base::OnceClosure Append(std::vector<int>& values, int value) {
  return base::BindOnce(
      [](std::vector<int>& values, int value) { values.push_back(value); },
      std::ref(values), value);
}

base::OnceClosure Add(int& value, int delta) {
  return base::BindOnce([](int& value, int delta) { value += delta; },
                        std::ref(value), delta);
}

TEST(KryptonUiDeferredCommandsTest, CoalescesInOrderWithoutInlineExecution) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  DeferredCommands commands(runner);
  std::vector<int> calls;
  EXPECT_TRUE(commands.Post(Append(calls, 1)));
  EXPECT_TRUE(commands.Post(Append(calls, 2)));
  EXPECT_TRUE(calls.empty());
  EXPECT_EQ(runner->pending(), 1u);
  EXPECT_EQ(runner->nestable_posts, 0u);
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, (std::vector<int>{1, 2}));
  EXPECT_FALSE(runner->RunOne());
}

TEST(KryptonUiDeferredCommandsTest, ObserverCommandsWaitForAnotherTask) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  DeferredCommands commands(runner);
  std::vector<int> calls;
  ASSERT_TRUE(commands.Post(base::BindOnce(
      [](DeferredCommands& commands, std::vector<int>& calls) {
        calls.push_back(1);
        EXPECT_TRUE(commands.Post(Append(calls, 3)));
      },
      std::ref(commands), std::ref(calls))));
  ASSERT_TRUE(commands.Post(Append(calls, 2)));
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, (std::vector<int>{1, 2}));
  EXPECT_EQ(runner->pending(), 1u);
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, (std::vector<int>{1, 2, 3}));
}

TEST(KryptonUiDeferredCommandsTest, CancelInvalidatesAlreadyPostedTasks) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  DeferredCommands commands(runner);
  int calls = 0;
  ASSERT_TRUE(commands.Post(Add(calls, 1)));
  commands.Cancel();
  ASSERT_TRUE(commands.Post(Add(calls, 2)));
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, 2);
}

TEST(KryptonUiDeferredCommandsTest, CancellationDuringDispatchEndsTheBatch) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  DeferredCommands commands(runner);
  int calls = 0;
  ASSERT_TRUE(commands.Post(base::BindOnce(
      [](DeferredCommands& commands, int& calls) {
        ++calls;
        commands.Cancel();
        EXPECT_TRUE(commands.Post(Add(calls, 2)));
      },
      std::ref(commands), std::ref(calls))));
  ASSERT_TRUE(commands.Post(Add(calls, 100)));
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, 3);
}

TEST(KryptonUiDeferredCommandsTest, CommandCanDestroyItsQueue) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  auto commands = std::make_unique<DeferredCommands>(runner);
  int calls = 0;
  ASSERT_TRUE(commands->Post(base::BindOnce(
      [](std::unique_ptr<DeferredCommands>& commands, int& calls) {
        ++calls;
        commands.reset();
      },
      std::ref(commands), std::ref(calls))));
  ASSERT_TRUE(commands->Post(Add(calls, 100)));
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, 1);
}

TEST(KryptonUiDeferredCommandsTest, DestructionInvalidatesOutstandingTasks) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  int calls = 0;
  {
    DeferredCommands commands(runner);
    ASSERT_TRUE(commands.Post(Add(calls, 1)));
  }
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, 0);
}

TEST(KryptonUiDeferredCommandsTest, FailedPostingDiscardsTheCommand) {
  auto runner = base::MakeRefCounted<test::TaskRunner>();
  DeferredCommands commands(runner);
  runner->accepting = false;
  int calls = 0;
  EXPECT_FALSE(commands.Post(Add(calls, 1)));
  runner->accepting = true;
  EXPECT_TRUE(commands.Post(Add(calls, 2)));
  EXPECT_TRUE(runner->RunOne());
  EXPECT_EQ(calls, 2);
  EXPECT_FALSE(commands.Post({}));
}

}  // namespace
}  // namespace krypton_ui

// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_WINDOW_SESSION_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_WINDOW_SESSION_H_

#include <optional>
#include <string>

#include "base/memory/raw_ref.h"
#include "base/types/expected.h"
#include "base/win/windows_types.h"
#include "chrome/browser/ui/krypton_ui/deferred_commands.h"
#include "chrome/browser/ui/krypton_ui/window_geometry.h"

namespace krypton_ui {

class Module;

// One browser window's native UI lifetime. Its browser callback context must
// outlive this object. Close before destroying the parent HWND or observers.
class WindowSession {
 public:
  WindowSession(Module& module,
                scoped_refptr<base::SequencedTaskRunner> task_runner);
  ~WindowSession();
  WindowSession(const WindowSession&) = delete;
  WindowSession& operator=(const WindowSession&) = delete;

  [[nodiscard]] base::expected<void, std::string> Initialize(
      HWND parent,
      const KrBrowser& browser,
      const KrTheme& initial_theme);
  void Close();
  [[nodiscard]] bool Defer(base::OnceClosure command);
  [[nodiscard]] base::expected<bool, std::string> UpdateWindowState(
      uint32_t flags);

  [[nodiscard]] const KrShellUI* api() const {
    return state_ == State::kActive ? &ui_ : nullptr;
  }
  [[nodiscard]] WindowGeometry& geometry() { return geometry_; }
  [[nodiscard]] const WindowGeometry& geometry() const { return geometry_; }
  [[nodiscard]] base::WeakPtr<WindowSession> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  enum class State { kNew, kActive, kClosed };

  const raw_ref<Module> module_;
  DeferredCommands commands_;
  HWND parent_ = nullptr;
  KrShellUI ui_{};
  State state_ = State::kNew;
  WindowGeometry geometry_;
  std::optional<KrWindowState> previous_state_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<WindowSession> weak_factory_{this};
};

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_WINDOW_SESSION_H_

// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/window_session.h"

#include <windows.h>

#include <utility>

#include "base/task/sequenced_task_runner.h"
#include "base/trace_event/trace_event.h"
#include "chrome/browser/ui/krypton_ui/module.h"

namespace krypton_ui {

WindowSession::WindowSession(
    Module& module,
    scoped_refptr<base::SequencedTaskRunner> task_runner)
    : module_(module), commands_(std::move(task_runner)) {}

WindowSession::~WindowSession() {
  Close();
}

base::expected<void, std::string> WindowSession::Initialize(
    HWND parent,
    const KrBrowser& browser,
    const KrTheme& initial_theme) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kNew) {
    return base::unexpected("KryptonUI window session has already been used.");
  }
  // A failed creation is terminal for this window, not a retry or fallback.
  state_ = State::kClosed;
  auto created = module_->Create(parent, browser, initial_theme);
  if (!created.has_value()) {
    return base::unexpected(std::move(created.error()));
  }
  parent_ = parent;
  ui_ = *created;
  state_ = State::kActive;
  return base::ok();
}

void WindowSession::Close() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  state_ = State::kClosed;
  weak_factory_.InvalidateWeakPtrs();
  commands_.Cancel();
  previous_state_.reset();
  parent_ = nullptr;
  module_->Destroy(ui_);
}

bool WindowSession::Defer(base::OnceClosure command) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_ == State::kActive && commands_.Post(std::move(command));
}

base::expected<bool, std::string> WindowSession::UpdateWindowState(
    uint32_t flags) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  TRACE_EVENT0("browser", "KryptonUI.SetWindowState");
  if (state_ != State::kActive) {
    return base::unexpected("KryptonUI window session is not active.");
  }
  RECT client{};
  if (!::GetClientRect(parent_, &client)) {
    return base::unexpected("Cannot read Krypton's native client bounds.");
  }
  const auto state =
      MakeWindowState(client.right - client.left, client.bottom - client.top,
                      ::GetDpiForWindow(parent_), flags);
  if (!state) {
    return base::unexpected("Krypton's native window geometry is unavailable.");
  }
  if (previous_state_ && SameWindowState(*previous_state_, *state)) {
    return false;
  }
  // Publish the cache before calling out. A synchronous minimum-size callback
  // can reenter with newer dimensions, which the outer call must not overwrite.
  previous_state_ = *state;
  ui_.set_window_state(ui_.self, &*state);
  return true;
}

}  // namespace krypton_ui

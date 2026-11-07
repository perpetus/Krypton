// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_CONTENTS_LAYOUT_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_CONTENTS_LAYOUT_H_

#include <cstdint>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout.h"

namespace krypton_ui {

class WindowSession;

// Consumes the module's page rectangle. Only Chromium-owned content services
// are measured here; browser chrome dimensions remain entirely in KryptonUI.
class ContentsLayout final : public BrowserViewLayout {
 public:
  ContentsLayout(std::unique_ptr<BrowserViewLayoutDelegate> delegate,
                 BrowserViewLayoutViews views,
                 base::WeakPtr<WindowSession> session,
                 base::RepeatingCallback<uint32_t()> window_flags,
                 base::RepeatingCallback<void(std::string)> failure);
  ~ContentsLayout() override;

  void Layout(views::View* host) override;
  gfx::Size GetMinimumSize(const views::View* host) const override;

 private:
  gfx::Point GetDialogPosition(const gfx::Size& dialog_size) const override;
  gfx::Size GetMaximumDialogSize() const override;

  const base::WeakPtr<WindowSession> session_;
  const base::RepeatingCallback<uint32_t()> window_flags_;
  const base::RepeatingCallback<void(std::string)> failure_;
  gfx::Rect contents_bounds_;
  bool failed_ = false;
  base::WeakPtrFactory<ContentsLayout> weak_factory_{this};
};

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_CONTENTS_LAYOUT_H_

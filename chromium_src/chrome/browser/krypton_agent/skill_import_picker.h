#ifndef CHROME_BROWSER_KRYPTON_AGENT_SKILL_IMPORT_PICKER_H_
#define CHROME_BROWSER_KRYPTON_AGENT_SKILL_IMPORT_PICKER_H_

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "ui/shell_dialogs/select_file_dialog.h"

namespace content {
class WebContents;
}

namespace krypton_agent {

// Owned by the native Settings caller for the lifetime of the dialog.
class SkillImportPicker final : public ui::SelectFileDialog::Listener {
 public:
  enum class Kind { kFolder, kZip };
  using Selection = base::OnceCallback<void(base::FilePath)>;

  SkillImportPicker();
  ~SkillImportPicker() override;
  SkillImportPicker(const SkillImportPicker&) = delete;
  SkillImportPicker& operator=(const SkillImportPicker&) = delete;

  bool Open(content::WebContents& owner, Kind kind, Selection selection);

 private:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void FileSelectionCanceled() override;
  void Finish(base::FilePath path);

  ui::SelectFileDialog::FileTypeInfo zip_types_;
  scoped_refptr<ui::SelectFileDialog> dialog_;
  Selection selection_;
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_SKILL_IMPORT_PICKER_H_

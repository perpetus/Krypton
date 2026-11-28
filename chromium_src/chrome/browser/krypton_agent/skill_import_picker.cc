#include "chrome/browser/krypton_agent/skill_import_picker.h"

#include <memory>
#include <utility>

#include "chrome/browser/krypton_agent/agent_component.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "content/public/browser/web_contents.h"
#include "ui/shell_dialogs/selected_file_info.h"

namespace krypton_agent {

SkillImportPicker::SkillImportPicker() {
  zip_types_.extensions = {{FILE_PATH_LITERAL("zip")}};
}

SkillImportPicker::~SkillImportPicker() {
  if (dialog_) {
    dialog_->ListenerDestroyed();
  }
}

bool SkillImportPicker::Open(content::WebContents& owner,
                             Kind kind,
                             Selection selection) {
  auto* profile = Profile::FromBrowserContext(owner.GetBrowserContext());
  if (dialog_ || !selection || !profile->IsRegularProfile() ||
      !IsProductionAgentEnabled()) {
    return false;
  }
  dialog_ = ui::SelectFileDialog::Create(
      this, std::make_unique<ChromeSelectFilePolicy>(&owner));
  selection_ = std::move(selection);
  const bool folder = kind == Kind::kFolder;
  dialog_->SelectFile(
      folder ? ui::SelectFileDialog::SELECT_EXISTING_FOLDER
             : ui::SelectFileDialog::SELECT_OPEN_FILE,
      folder ? u"Choose a skill folder" : u"Choose a skill or plugin ZIP",
      base::FilePath(), folder ? nullptr : &zip_types_, folder ? 0 : 1, {},
      owner.GetTopLevelNativeWindow(), nullptr);
  return true;
}

void SkillImportPicker::FileSelected(const ui::SelectedFileInfo& file,
                                     int index) {
  Finish(file.local_path);
}

void SkillImportPicker::FileSelectionCanceled() {
  Finish({});
}

void SkillImportPicker::Finish(base::FilePath path) {
  auto dialog = std::move(dialog_);
  if (dialog) {
    dialog->ListenerDestroyed();
  }
  if (!IsProductionAgentEnabled()) {
    path.clear();
  }
  auto callback = std::move(selection_);
  std::move(callback).Run(std::move(path));
}

}  // namespace krypton_agent

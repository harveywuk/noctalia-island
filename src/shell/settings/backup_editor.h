#pragma once
#include "config/config_backup.h"
#include "shell/settings/settings_content.h"

namespace settings {
  struct BackupEditorState {
    std::string name = "My setup", selected, message, undoId;
    std::vector<std::string> selectedSections{config_backup::sections.begin(), config_backup::sections.end()};
    std::vector<std::string> undoSections;
    std::vector<config_backup::Info> backups;
    std::shared_ptr<config_backup::Plan> plan;
    std::size_t visibleChanges = 30;
    bool loaded = false, displayPreview = false;
  };
  std::unique_ptr<Node> makeBackupEditor(const SettingsContentContext&);
} // namespace settings

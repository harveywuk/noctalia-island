#include "config/config_backup.h"

#include "config/atomic_file.h"
#include "config/config_export.h"
#include "config/config_merge.h"
#include "config/config_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  constexpr std::uintmax_t maxBackupBytes = 16 * 1024 * 1024;
  std::string format(const toml::table& table) {
    std::ostringstream out;
    out << toml::toml_formatter{table};
    return out.str();
  }
  std::string_view section(std::string_view root, std::string_view child = {}) {
    if (root == "shell") {
      if (child == "hyprland_input" || child == "animation" || child == "keyboard_layout")
        return "input";
      if (child == "panel_anchor_bar")
        return "bars";
      if (child == "hyprland_displays")
        return "displays";
      if (child == "hyprland_keybinds")
        return "shortcuts";
      if (child == "hyprland_workspaces"
          || child == "hyprland_tiling"
          || child == "hyprland_window_behaviour"
          || child == "hyprland_placement_rules")
        return "workspaces";
      if (child == "session" || child.starts_with("launch_apps_"))
        return "session";
      if (child == "font_family"
          || child == "corner_radius_scale"
          || child.ends_with("_borders")
          || child == "popup_shadows"
          || child == "hyprland_appearance"
          || child == "hyprland_app_rules"
          || child == "hyprland_profile_switching"
          || child == "hyprland_appearance_profiles"
          || child == "shadow"
          || child == "panel"
          || child == "screen_corners"
          || child == "avatar_path"
          || child.starts_with("app_icon_")
          || child == "settings_window_translucent")
        return "appearance";
      return "other";
    }
    if (root == "theme" || root == "wallpaper" || root == "backdrop" || root == "wallpaper_favorites")
      return "appearance";
    if (root == "bar" || root == "widget" || root == "island")
      return "bars";
    if (root == "keybinds")
      return "shortcuts";
    return "other";
  }
  bool chosen(const std::vector<std::string>& sections, std::string_view root, std::string_view child = {}) {
    return std::ranges::find(sections, section(root, child)) != sections.end();
  }
  void validateSections(const std::vector<std::string>& sections) {
    if (sections.empty())
      throw std::runtime_error("Choose at least one section.");
    for (const auto& id : sections)
      if (std::ranges::none_of(config_backup::sections, [&](auto known) { return id == known; }))
        throw std::runtime_error("Unknown backup section.");
  }
  fs::path backupPath(const std::string& overrides, const std::string& id) {
    if (id.empty() || id.size() > 100 || id.find_first_not_of("0123456789-") != std::string::npos)
      throw std::runtime_error("Invalid backup identifier.");
    return fs::path(overrides).parent_path() / "backups" / (id + ".toml");
  }
  toml::table readBackup(const fs::path& path) {
    if (!fs::is_regular_file(fs::symlink_status(path)) || fs::file_size(path) > maxBackupBytes)
      throw std::runtime_error("Backup is not a regular file or is too large.");
    auto table = toml::parse_file(path.string());
    if (table["format"].value<int>() != 1
        || !table["name"].is_string()
        || !table["created"].is_integer()
        || !table["base"].is_table()
        || !table["overrides"].is_table()
        || !table["effective"].is_table())
      throw std::runtime_error("Unsupported or incomplete backup.");
    return table;
  }
  config_backup::Info info(const toml::table& table, std::string id) {
    return {
        std::move(id), table["name"].value_or(std::string{}), table["created"].value_or(std::int64_t{}),
        table["automatic"].value_or(false)
    };
  }
  std::string value(const toml::node* node) {
    if (!node)
      return "(not set)";
    toml::table wrapper;
    wrapper.insert("value", *node);
    auto text = format(wrapper);
    if (text.starts_with("value = "))
      text.erase(0, 8);
    return text;
  }
  void
  changes(const toml::table& a, const toml::table& b, std::string prefix, std::vector<config_backup::Change>& result) {
    std::set<std::string> keys;
    for (const auto& [key, v] : a)
      keys.emplace(key.str());
    for (const auto& [key, v] : b)
      keys.emplace(key.str());
    for (const auto& key : keys) {
      const auto* av = a.get(key);
      const auto* bv = b.get(key);
      const auto path = prefix.empty() ? key : prefix + "." + key;
      if ((av && av->is_table()) || (bv && bv->is_table())) {
        const toml::table empty;
        changes(
            av && av->is_table() ? *av->as_table() : empty, bv && bv->is_table() ? *bv->as_table() : empty, path, result
        );
      } else if (value(av) != value(bv)) {
        result.push_back({path, value(av), value(bv)});
      }
    }
  }
} // namespace

namespace config_backup {
  toml::table select(const toml::table& table, const std::vector<std::string>& selectedSections) {
    toml::table result;
    for (const auto& [key, node] : table) {
      if (key == "shell" && node.is_table()) {
        toml::table shell;
        for (const auto& [child, v] : *node.as_table())
          if (chosen(selectedSections, "shell", child.str()))
            shell.insert(child, v);
        if (!shell.empty())
          result.insert("shell", std::move(shell));
      } else if (chosen(selectedSections, key.str()))
        result.insert(key, node);
    }
    return result;
  }
  toml::table
  replace(const toml::table& current, const toml::table& saved, const std::vector<std::string>& selectedSections) {
    std::vector<std::string> remaining;
    for (auto id : config_backup::sections)
      if (std::ranges::find(selectedSections, id) == selectedSections.end())
        remaining.emplace_back(id);
    auto result = select(current, remaining);
    ConfigService::deepMerge(result, select(saved, selectedSections));
    return result;
  }
  std::vector<Change> diff(const toml::table& a, const toml::table& b) {
    std::vector<Change> result;
    changes(a, b, {}, result);
    return result;
  }
} // namespace config_backup

toml::table ConfigService::backupBase() const {
  auto merged = noctalia::config::mergeConfigWithIncludes(m_configDir);
  if (!merged.firstError.empty())
    throw std::runtime_error(merged.firstError);
  if (!m_overridesParseError.empty())
    throw std::runtime_error("Fix the current settings file before using backups.");
  const auto disk = std::filesystem::exists(m_overridesPath) ? toml::parse_file(m_overridesPath) : toml::table{};
  if (disk != m_persistedOverridesTable || m_overridesTable != m_persistedOverridesTable)
    throw std::runtime_error("Settings changed on disk. Wait for them to reload, then try again.");
  const auto current = configForOverrides(m_overridesTable);
  if (!current || config_export::serialize(*current) != config_export::serialize(m_config))
    throw std::runtime_error("Configuration changed on disk. Wait for it to reload, then try again.");
  return merged.merged;
}

std::vector<config_backup::Info> ConfigService::listBackups(std::string& error) const {
  error.clear();
  std::vector<config_backup::Info> result;
  try {
    const auto directory = std::filesystem::path(m_overridesPath).parent_path() / "backups";
    if (!std::filesystem::exists(directory))
      return result;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
      if (file.path().extension() != ".toml")
        continue;
      try {
        const auto id = file.path().stem().string();
        result.push_back(info(readBackup(backupPath(m_overridesPath, id)), id));
      } catch (const std::exception&) {
        error = "Some backups could not be read. Their files have been preserved.";
      }
    }
    std::ranges::sort(result, [](const auto& a, const auto& b) { return a.created > b.created; });
  } catch (const std::exception& e) {
    error = e.what();
  }
  return result;
}

std::optional<config_backup::Info> ConfigService::createBackup(std::string name, std::string& error, bool automatic) {
  error.clear();
  try {
    if (name.empty() || name.size() > 120 || name.find_first_of("\r\n\t") != std::string::npos)
      throw std::runtime_error("Use a backup name of 1–120 characters on one line.");
    const auto base = backupBase();
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    const auto id = std::to_string(stamp) + "-" + std::to_string(::getpid());
    const auto path = backupPath(m_overridesPath, id);
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::permissions(path.parent_path(), std::filesystem::perms::owner_all);
    toml::table document{
        {"format", 1},
        {"name", name},
        {"created", stamp},
        {"automatic", automatic},
        {"base", base},
        {"overrides", m_overridesTable},
        {"effective", config_export::serialize(m_config)}
    };
    const auto content = format(document);
    if (content.size() > maxBackupBytes)
      throw std::runtime_error("Backup exceeds the 16 MiB limit.");
    if (!writeTextFileAtomic(path, content, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write))
      throw std::runtime_error("Could not save the backup.");
    return info(document, id);
  } catch (const std::exception& e) {
    error = e.what();
    return std::nullopt;
  }
}

std::optional<config_backup::Plan>
ConfigService::previewBackup(const std::string& id, const std::vector<std::string>& sections, std::string& error) {
  error.clear();
  try {
    validateSections(sections);
    const auto saved = readBackup(backupPath(m_overridesPath, id));
    const auto base = backupBase();
    if (config_backup::select(base, sections) != config_backup::select(*saved["base"].as_table(), sections))
      throw std::runtime_error(
          "Hand-written config changed in a selected section. Restore those files first, or deselect that section."
      );
    auto candidate = config_backup::replace(m_overridesTable, *saved["overrides"].as_table(), sections);
    if (!validateOverrideMutation(candidate))
      throw std::runtime_error(m_lastMutationError);
    const auto parsed = configForOverrides(candidate);
    if (!parsed)
      throw std::runtime_error("The saved settings are no longer compatible.");
    const auto effective = config_export::serialize(*parsed);
    if (config_backup::select(effective, sections) != config_backup::select(*saved["effective"].as_table(), sections))
      throw std::runtime_error("The saved settings cannot be restored exactly with this version of Noctalia.");
    auto diff = config_backup::diff(
        config_backup::select(config_export::serialize(m_config), sections), config_backup::select(effective, sections)
    );
    // Source-only differences (e.g. clearing an explicit default) matter for undo,
    // even when the rendered value is unchanged. Include them in the review.
    for (auto& change : config_backup::diff(
             config_backup::select(m_overridesTable, sections), config_backup::select(candidate, sections)
         )) {
      if (std::ranges::none_of(diff, [&](const auto& row) { return row.path == change.path; })) {
        change.path += " (saved override)";
        diff.push_back(std::move(change));
      }
    }
    const bool displaysChanged = parsed->shell.hyprlandDisplays != m_config.shell.hyprlandDisplays;
    return config_backup::Plan{info(saved, id),      sections, m_overridesTable, base,
                               std::move(candidate), *parsed,  std::move(diff),  displaysChanged};
  } catch (const std::exception& e) {
    error = e.what();
    return std::nullopt;
  }
}

bool ConfigService::restoreBackup(const config_backup::Plan& plan, std::string& undoId, std::string& error) {
  error.clear();
  undoId.clear();
  try {
    if (backupBase() != plan.base || m_overridesTable != plan.baseline)
      throw std::runtime_error("Settings changed since this preview. Preview the backup again.");
    // Re-read the immutable snapshot, so edited/corrupt files cannot evade review.
    const auto fresh = previewBackup(plan.backup.id, plan.sections, error);
    if (!fresh)
      return false;
    if (fresh->candidate != plan.candidate)
      throw std::runtime_error("Backup changed. Preview it again.");
    if (plan.candidate == m_overridesTable)
      return true;
    auto undo = createBackup("Before restoring " + plan.backup.name.substr(0, 95), error, true);
    if (!undo)
      return false;
    undoId = undo->id;
    bool changed = false;
    if (!commitOverrideTable(plan.candidate, &changed))
      throw std::runtime_error(m_lastMutationError.empty() ? "Could not save restored settings." : m_lastMutationError);
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

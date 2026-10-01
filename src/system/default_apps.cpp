#include "system/default_apps.h"

#include "config/atomic_file.h"
#include "config/config_service.h"
#include "core/toml.h"
#include "system/terminal_launch.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
  namespace fs = std::filesystem;
  using KeyFile = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;
  constexpr const char* groups[] = {"Default Applications", "Added Associations", "Removed Associations"};
  struct File {
    fs::path path;
    std::optional<std::string> before;
    std::string after;
  };
  fs::path configHome() { return g_get_user_config_dir(); }
  fs::path journalPath() { return fs::path(g_get_user_state_dir()) / "noctalia/default-apps-undo.toml"; }
  std::optional<std::string> read(const fs::path& path) {
    if (!fs::exists(path))
      return {};
    if (!fs::is_regular_file(path) || fs::file_size(path) > 4 * 1024 * 1024)
      throw std::runtime_error("Application association file is not a regular file or is too large.");
    std::ifstream in(path);
    if (!in)
      throw std::runtime_error("Cannot read " + path.string());
    std::string text{std::istreambuf_iterator<char>(in), {}};
    if (in.bad())
      throw std::runtime_error("Cannot read " + path.string());
    return text;
  }
  KeyFile parse(const std::optional<std::string>& text) {
    KeyFile key(g_key_file_new(), g_key_file_unref);
    GError* error = nullptr;
    if (text
        && !text->empty()
        && !g_key_file_load_from_data(
            key.get(), text->data(), text->size(),
            GKeyFileFlags(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS), &error
        )) {
      std::string message = error->message;
      g_error_free(error);
      throw std::runtime_error("Invalid application associations: " + message);
    }
    return key;
  }
  std::string serialize(GKeyFile* key) {
    gsize length = 0;
    char* data = g_key_file_to_data(key, &length, nullptr);
    std::string result(data, length);
    g_free(data);
    return result;
  }
  std::optional<std::string> value(GKeyFile* key, const char* group, const std::string& type) {
    char* data = g_key_file_get_value(key, group, type.c_str(), nullptr);
    if (!data)
      return {};
    std::string result(data);
    g_free(data);
    return result;
  }
  void assign(GKeyFile* key, const char* group, const std::string& type, const std::optional<std::string>& data) {
    if (data)
      g_key_file_set_value(key, group, type.c_str(), data->c_str());
    else
      g_key_file_remove_key(key, group, type.c_str(), nullptr);
  }
  std::vector<fs::path> desktopFiles() {
    std::vector<fs::path> files;
    const char* desktop = g_getenv("XDG_CURRENT_DESKTOP");
    if (!desktop)
      return files;
    char** parts = g_strsplit(desktop, ":", -1);
    for (char** part = parts; *part; ++part) {
      std::string name(*part);
      if (name.empty()
          || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ")
              != std::string::npos)
        continue;
      char* lower = g_ascii_strdown(name.c_str(), -1);
      auto path = configHome() / (std::string(lower) + "-mimeapps.list");
      g_free(lower);
      if (std::ranges::find(files, path) == files.end())
        files.push_back(std::move(path));
    }
    g_strfreev(parts);
    return files;
  }
  bool write(const fs::path& path, const std::optional<std::string>& text) {
    if (!text) {
      std::error_code ec;
      fs::remove(path, ec);
      return !ec;
    }
    auto mode = fs::exists(path) ? fs::status(path).permissions() : fs::perms::owner_read | fs::perms::owner_write;
    return writeTextFileAtomic(path, *text, mode);
  }
  void apply(const std::vector<File>& files) {
    for (const auto& file : files)
      if (read(file.path) != file.before)
        throw std::runtime_error("Application defaults changed. Refresh and try again.");
    std::size_t applied = 0;
    try {
      for (const auto& file : files) {
        if (read(file.path) != file.before || !write(file.path, file.after))
          throw std::runtime_error("Cannot update " + file.path.string());
        ++applied;
      }
    } catch (...) {
      while (applied > 0) {
        const auto& file = files[--applied];
        if (read(file.path) == file.after)
          (void)write(file.path, file.before);
      }
      throw;
    }
  }
  std::vector<File> loadJournal() {
    const auto content = read(journalPath());
    if (!content)
      return {};
    auto table = toml::parse(*content);
    auto* entries = table["files"].as_array();
    if (table["version"].value<int>() != 1 || !entries || entries->size() > 8)
      throw std::runtime_error("The application-default undo record is invalid.");
    std::vector<File> files;
    for (const auto& node : *entries) {
      if (!node.is_table())
        throw std::runtime_error("The application-default undo record is invalid.");
      const auto& entry = *node.as_table();
      const fs::path path(entry["path"].value_or(std::string{}));
      const auto filename = path.filename().string();
      if (path.parent_path() != configHome()
          || !(filename == "mimeapps.list" || filename.ends_with("-mimeapps.list"))
          || !entry["after"].is_string()
          || !entry["before"].is_string()
          || !entry["existed"].is_boolean())
        throw std::runtime_error("The application-default undo record is invalid.");
      files.push_back(
          {path, entry["existed"].value_or(false) ? entry["before"].value<std::string>() : std::nullopt,
           entry["after"].value_or(std::string{})}
      );
    }
    return files;
  }
  void saveJournal(const std::vector<File>& files) {
    toml::array entries;
    for (const auto& file : files)
      entries.push_back(
          toml::table{
              {"path", file.path.string()},
              {"existed", file.before.has_value()},
              {"before", file.before.value_or("")},
              {"after", file.after}
          }
      );
    std::ostringstream out;
    out << toml::table{{"version", 1}, {"files", std::move(entries)}};
    if (!writeTextFileAtomic(journalPath(), out.str(), fs::perms::owner_read | fs::perms::owner_write))
      throw std::runtime_error("Cannot save the undo record. No defaults were changed.");
  }
  bool supports(GAppInfo* app, const std::string& type) {
    const auto* types = g_app_info_get_supported_types(app);
    if (types)
      for (; *types; ++types)
        if (type == *types)
          return true;
    return false;
  }
} // namespace

namespace default_apps {
  const std::vector<Role>& roles() {
    static const std::vector<Role> result{
        {"browser", {"x-scheme-handler/http", "x-scheme-handler/https", "text/html", "application/xhtml+xml"}},
        {"email", {"x-scheme-handler/mailto"}},
        {"files", {"inode/directory"}},
        {"text", {"text/plain"}},
        {"pdf", {"application/pdf"}}
    };
    return result;
  }
  std::vector<App> choices(const Role& role) {
    std::vector<App> result;
    GList* apps = g_app_info_get_all();
    for (auto* it = apps; it; it = it->next) {
      auto* app = G_APP_INFO(it->data);
      const char* id = g_app_info_get_id(app);
      if (id
          && g_app_info_should_show(app)
          && supports(app, role.types.front())
          && (role.id != "browser" || supports(app, role.types[1])))
        result.push_back({id, g_app_info_get_display_name(app)});
    }
    g_list_free_full(apps, g_object_unref);
    std::ranges::sort(result, [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
  }
  App current(const std::string& type) {
    App result;
    if (auto* app = g_app_info_get_default_for_type(type.c_str(), FALSE)) {
      const char* id = g_app_info_get_id(app);
      result = {id ? id : "", g_app_info_get_display_name(app)};
      g_object_unref(app);
    }
    return result;
  }
  bool set(const std::string& roleId, const std::string& desktopId, std::string& error) {
    error.clear();
    try {
      const auto role = std::ranges::find(roles(), roleId, &Role::id);
      if (role == roles().end())
        throw std::runtime_error("Unknown application category.");
      const auto apps = choices(*role);
      if (std::ranges::find(apps, desktopId, &App::id) == apps.end())
        throw std::runtime_error("This application is unavailable or does not support this category.");
      std::vector<File> files;
      File generic{configHome() / "mimeapps.list", {}, {}};
      generic.before = read(generic.path);
      auto key = parse(generic.before);
      for (const auto& type : role->types) {
        assign(key.get(), groups[0], type, desktopId + ";");
        for (const char* group : {groups[1], groups[2]}) {
          gsize count = 0;
          char** ids = g_key_file_get_string_list(key.get(), group, type.c_str(), &count, nullptr);
          std::string list = group == groups[1] ? desktopId + ";" : "";
          for (gsize i = 0; i < count; ++i)
            if (ids[i] != desktopId)
              list += std::string(ids[i]) + ";";
          g_strfreev(ids);
          if (!list.empty())
            assign(key.get(), group, type, list);
          else
            assign(key.get(), group, type, std::nullopt);
        }
      }
      generic.after = serialize(key.get());
      if (serialize(parse(generic.before).get()) != generic.after)
        files.push_back(std::move(generic));
      // An advanced desktop-specific user file takes precedence over the normal
      // GUI file. Update its defaults too; associations belong only in the latter.
      for (const auto& path : desktopFiles()) {
        auto before = read(path);
        auto desktop = parse(before);
        if (std::ranges::none_of(role->types, [&](const auto& type) {
              return value(desktop.get(), groups[0], type).has_value();
            }))
          continue;
        for (const auto& type : role->types)
          assign(desktop.get(), groups[0], type, desktopId + ";");
        auto after = serialize(desktop.get());
        if (serialize(parse(before).get()) != after)
          files.push_back({path, std::move(before), std::move(after)});
        break;
      }
      if (files.empty())
        return true;
      const auto oldJournal = read(journalPath());
      saveJournal(files);
      try {
        apply(files);
      } catch (...) {
        if (std::ranges::all_of(files, [](const auto& file) { return read(file.path) == file.before; }))
          (void)write(journalPath(), oldJournal);
        throw;
      }
      return true;
    } catch (const std::exception& e) {
      error = e.what();
      return false;
    }
  }
  bool canUndo() {
    // A malformed record still exposes the button so Undo can explain the error.
    std::error_code ec;
    return fs::exists(journalPath(), ec);
  }
  bool undo(std::string& error) {
    error.clear();
    try {
      const auto recorded = loadJournal();
      if (recorded.empty())
        throw std::runtime_error("There is no application-default change to undo.");
      std::vector<File> changes;
      std::vector<fs::path> remove;
      for (const auto& file : recorded) {
        const auto now = read(file.path);
        if (now == file.before)
          continue;
        if (now == file.after) {
          changes.push_back({file.path, now, file.before.value_or("")});
          if (!file.before)
            remove.push_back(file.path);
          continue;
        }
        auto before = parse(file.before), after = parse(file.after), actual = parse(now);
        for (const auto* group : groups) {
          std::set<std::string> types;
          for (auto* source : {before.get(), after.get()}) {
            char** keys = g_key_file_get_keys(source, group, nullptr, nullptr);
            if (keys)
              for (char** k = keys; *k; ++k)
                types.emplace(*k);
            g_strfreev(keys);
          }
          for (const auto& type : types) {
            const auto oldValue = value(before.get(), group, type), newValue = value(after.get(), group, type);
            if (oldValue == newValue)
              continue;
            if (value(actual.get(), group, type) != newValue)
              throw std::runtime_error(
                  "A changed default was edited elsewhere. Refresh and choose the desired application instead of "
                  "undoing."
              );
            assign(actual.get(), group, type, oldValue);
          }
        }
        changes.push_back({file.path, now, serialize(actual.get())});
      }
      apply(changes);
      for (const auto& path : remove)
        if (read(path) == std::string{})
          fs::remove(path);
      fs::remove(journalPath());
      return true;
    } catch (const std::exception& e) {
      error = e.what();
      return false;
    }
  }

  bool canUndoTerminal(const ConfigService& config) {
    return config.stateString("default_apps_terminal", "undo").has_value();
  }
  bool setTerminal(ConfigService& config, const std::string& id, std::string& error) {
    error.clear();
    const std::vector<std::string> path{"shell", "preferred_terminal"};
    if (config.config().shell.preferredTerminal == id)
      return true;
    if (!id.empty()) {
      const auto apps = terminal_launch::availableTerminals();
      if (std::ranges::find(apps, id, &terminal_launch::TerminalApp::id) == apps.end()) {
        error = "This terminal is no longer available.";
        return false;
      }
    }
    const auto oldUndo = config.stateString("default_apps_terminal", "undo");
    std::ostringstream record;
    record << toml::table{
        {"version", 1},
        {"before", config.config().shell.preferredTerminal},
        {"overridden", config.hasOverride(path)},
        {"after", id}
    };
    if (!config.setStateString("default_apps_terminal", "undo", record.str())) {
      error = "Cannot save terminal undo information.";
      return false;
    }
    if (!config.setOverride(path, id)) {
      if (oldUndo)
        (void)config.setStateString("default_apps_terminal", "undo", *oldUndo);
      else
        (void)config.clearStateOwner("default_apps_terminal");
      error = config.lastMutationError();
      return false;
    }
    return true;
  }
  bool undoTerminal(ConfigService& config, std::string& error) {
    error.clear();
    try {
      const auto record = config.stateString("default_apps_terminal", "undo");
      if (!record)
        throw std::runtime_error("No terminal change to undo.");
      const auto table = toml::parse(*record);
      if (table["version"].value<int>() != 1
          || !table["before"].is_string()
          || !table["overridden"].is_boolean()
          || !table["after"].is_string())
        throw std::runtime_error("The terminal undo record is invalid.");
      if (table["after"].value<std::string>() != config.config().shell.preferredTerminal)
        throw std::runtime_error("The terminal preference changed elsewhere. Choose a terminal instead of undoing.");
      bool changed = false;
      const std::vector<std::string> path{"shell", "preferred_terminal"};
      const bool ok = table["overridden"].value_or(false)
          ? config.setOverride(path, table["before"].value_or(std::string{}))
          : config.clearOverrides({path}, &changed);
      if (!ok)
        throw std::runtime_error(config.lastMutationError());
      if (!config.clearStateOwner("default_apps_terminal"))
        throw std::runtime_error("Terminal restored, but the undo record could not be cleared.");
      return true;
    } catch (const std::exception& e) {
      error = e.what();
      return false;
    }
  }
} // namespace default_apps

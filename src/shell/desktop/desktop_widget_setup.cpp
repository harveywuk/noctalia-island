#include "shell/desktop/desktop_widget_setup.h"

#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/desktop/widgets/desktop_remote_source.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace {
  std::string text(const DesktopWidgetState& state, const char* key) {
    const auto it = state.settings.find(key);
    if (it != state.settings.end())
      if (const auto* value = std::get_if<std::string>(&it->second))
        return *value;
    return {};
  }
  std::vector<std::string> list(const DesktopWidgetState& state, const char* key) {
    const auto it = state.settings.find(key);
    if (it != state.settings.end())
      if (const auto* value = std::get_if<std::vector<std::string>>(&it->second))
        return *value;
    return {};
  }
  bool readable(const std::string& path, bool folder) {
    if (path.empty())
      return false;
    std::error_code ec;
    const auto expanded = FileUtils::expandXdgBaseDir(path);
    if (folder) {
      const std::filesystem::directory_iterator it(expanded, ec);
      return !ec;
    }
    return std::filesystem::is_regular_file(expanded, ec) && std::ifstream(expanded).good();
  }
  bool validTokenFile(const std::string& path) {
    if (!readable(path, false))
      return false;
    std::ifstream input(FileUtils::expandXdgBaseDir(path), std::ios::binary);
    std::string token(4097, '\0');
    input.read(token.data(), static_cast<std::streamsize>(token.size()));
    token.resize(static_cast<std::size_t>(input.gcount()));
    if (token.size() > 4096)
      return false;
    token = StringUtils::trim(token);
    return !token.empty() && token.find_first_of("\r\n") == std::string::npos;
  }
} // namespace

bool desktop_setup::guided(std::string_view type) {
  return type == "photos"
      || type == "notes"
      || type == "journal"
      || type == "reminders"
      || type == "contacts"
      || type == "shortcuts"
      || type == "reading_list"
      || type == "video_library"
      || type == "news"
      || type == "podcasts"
      || type == "stocks"
      || type == "home"
      || type == "find_my"
      || type == "stack";
}

std::string desktop_setup::validate(const DesktopWidgetState& draft) {
  const auto& type = draft.type;
  if (type == "photos") {
    const auto image = text(draft, "image_path");
    if (!readable(image.empty() ? text(draft, "folder_path") : image, image.empty()))
      return "desktop-widgets.setup.invalid-photo";
  } else if (type == "video_library") {
    if (!readable(text(draft, "folder_path"), true))
      return "desktop-widgets.setup.invalid-folder";
  } else if (type == "news" || type == "podcasts") {
    if (!desktop_sources::validWebUrl(text(draft, "feed_url")))
      return "desktop-widgets.setup.invalid-url";
  } else if (type == "reminders") {
    auto tasks = list(draft, "items");
    if (tasks.empty() || std::ranges::any_of(tasks, [](const auto& task) { return StringUtils::trim(task).empty(); }))
      return "desktop-widgets.setup.empty-items";
  } else if (type == "contacts" || type == "shortcuts" || type == "reading_list") {
    auto it = draft.settings.find("entries");
    const auto* entries = it == draft.settings.end() ? nullptr : std::get_if<WidgetSettingStringMap>(&it->second);
    if (!entries || entries->empty())
      return "desktop-widgets.setup.empty-items";
    for (const auto& [name, action] : *entries) {
      if (StringUtils::trim(name).empty() || StringUtils::trim(action).empty())
        return "desktop-widgets.setup.empty-items";
      if (type == "reading_list" && !desktop_sources::validWebUrl(action))
        return "desktop-widgets.setup.invalid-url";
      if (type == "contacts"
          && !(action.starts_with("mailto:") && action.size() > 7)
          && !(action.starts_with("tel:") && action.size() > 4))
        return "desktop-widgets.setup.invalid-contact";
    }
  } else if (type == "home" || type == "find_my" || type == "stocks") {
    if (type != "stocks" && !desktop_sources::validWebUrl(text(draft, "server_url")))
      return "desktop-widgets.setup.invalid-url";
    if (!validTokenFile(text(draft, "token_file")))
      return "desktop-widgets.setup.invalid-token";
    const auto values = list(draft, type == "stocks" ? "symbols" : "entities");
    if (values.empty()
        || std::ranges::any_of(values, [](const auto& value) { return StringUtils::trim(value).empty(); }))
      return "desktop-widgets.setup.empty-items";
    if (type == "stocks" && values.size() > 3)
      return "desktop-widgets.setup.too-many-symbols";
    if (type == "find_my" && std::ranges::any_of(values, [](const auto& value) {
          return !value.starts_with("person.") && !value.starts_with("device_tracker.");
        }))
      return "desktop-widgets.setup.invalid-location";
  } else if (type == "stack") {
    const auto values = desktop_stacks::members(draft);
    const std::unordered_set<std::string> unique(values.begin(), values.end());
    if (unique.size() < 2 || values.size() > desktop_stacks::maxMembers || unique.size() != values.size())
      return "desktop-widgets.setup.stack-count";
  }
  return {};
}

std::vector<std::string> desktop_stacks::members(const DesktopWidgetState& stack) { return list(stack, "members"); }

desktop_stacks::Membership desktop_stacks::resolve(const std::vector<DesktopWidgetState>& widgets) {
  Membership result;
  std::unordered_set<std::string> claimed;
  for (const auto& stack : widgets) {
    if (stack.type != "stack")
      continue;
    auto& accepted = result[stack.id];
    for (const auto& id : members(stack)) {
      if (accepted.size() == maxMembers)
        break;
      const auto found = std::ranges::find(widgets, id, &DesktopWidgetState::id);
      if (found == widgets.end()
          || found->type == "stack"
          || !desktop_cards::supportsSizePresets(found->type)
          || !claimed.insert(id).second)
        continue;
      accepted.push_back(id);
    }
  }
  return result;
}

bool desktop_stacks::canJoin(
    const std::vector<DesktopWidgetState>& widgets, const std::string& source, const std::string& target
) {
  const auto from = std::ranges::find(widgets, source, &DesktopWidgetState::id);
  const auto to = std::ranges::find(widgets, target, &DesktopWidgetState::id);
  if (from == widgets.end()
      || to == widgets.end()
      || source == target
      || !from->enabled
      || !to->enabled
      || from->type == "stack"
      || !desktop_cards::supportsSizePresets(from->type)
      || !desktop_cards::supportsSizePresets(to->type))
    return false;
  const auto ownership = resolve(widgets);
  return !contains(ownership, source)
      && !contains(ownership, target)
      && (to->type != "stack" || ownership.at(target).size() < maxMembers);
}

std::string desktop_stacks::join(
    std::vector<DesktopWidgetState>& widgets, const std::string& source, const std::string& target,
    const std::string& newId
) {
  if (!canJoin(widgets, source, target))
    return {};
  auto to = std::ranges::find(widgets, target, &DesktopWidgetState::id);
  if (to->type == "stack") {
    auto ordered = resolve(widgets).at(target);
    ordered.push_back(source);
    to->settings["members"] = ordered;
    return target;
  }
  if (newId.empty() || std::ranges::find(widgets, newId, &DesktopWidgetState::id) != widgets.end())
    return {};
  auto stack = *to;
  stack.id = newId;
  stack.type = "stack";
  auto size = text(*to, "card_size");
  if (size.empty() || size == "classic")
    size = "medium";
  stack.settings = desktop_settings::newDesktopWidgetSettings("stack", size);
  for (const auto& [key, value] : to->settings)
    if (key.starts_with("background") || key == "font_family")
      stack.settings[key] = value;
  stack.settings["members"] = std::vector<std::string>{target, source};
  widgets.push_back(std::move(stack));
  return newId;
}

bool desktop_stacks::reorder(
    std::vector<DesktopWidgetState>& widgets, const std::string& stack, const std::string& member, std::size_t to
) {
  const auto ownership = resolve(widgets);
  const auto group = ownership.find(stack);
  if (group == ownership.end())
    return false;
  auto ordered = group->second;
  auto from = std::ranges::find(ordered, member);
  if (from == ordered.end() || to >= ordered.size())
    return false;
  ordered.erase(from);
  ordered.insert(ordered.begin() + static_cast<std::ptrdiff_t>(to), member);
  std::ranges::find(widgets, stack, &DesktopWidgetState::id)->settings["members"] = ordered;
  return true;
}

bool desktop_stacks::detach(
    std::vector<DesktopWidgetState>& widgets, const std::string& stack, const std::string& member,
    const std::string& output, float x, float y
) {
  const auto ownership = resolve(widgets);
  auto group = ownership.find(stack);
  if (group == ownership.end() || std::ranges::find(group->second, member) == group->second.end())
    return false;
  auto ordered = group->second;
  std::erase(ordered, member);
  auto source = std::ranges::find(widgets, member, &DesktopWidgetState::id);
  auto container = std::ranges::find(widgets, stack, &DesktopWidgetState::id);
  source->cx = x;
  source->cy = y;
  source->outputName = output;
  if (ordered.size() >= 2) {
    container->settings["members"] = ordered;
  } else {
    if (!ordered.empty()) {
      auto remaining = std::ranges::find(widgets, ordered.front(), &DesktopWidgetState::id);
      remaining->cx = container->cx;
      remaining->cy = container->cy;
      remaining->outputName = container->outputName;
    }
    std::erase_if(widgets, [&](const auto& state) { return state.id == stack; });
  }
  return true;
}

desktop_stacks::RotationSchedule::RotationSchedule(int seconds, Clock::time_point now)
    : m_interval(std::clamp(seconds, 5, 3600)), m_next(now + m_interval) {}

void desktop_stacks::RotationSchedule::reset(Clock::time_point now) { m_next = now + m_interval; }

bool desktop_stacks::RotationSchedule::due(bool paused, Clock::time_point now) {
  if (paused) {
    reset(now);
    return false;
  }
  if (now < m_next)
    return false;
  reset(now);
  return true;
}

bool desktop_stacks::contains(const Membership& membership, const std::string& id) {
  return std::ranges::any_of(membership, [&](const auto& entry) {
    return std::ranges::find(entry.second, id) != entry.second.end();
  });
}

std::vector<DesktopWidgetState>
desktop_stacks::cards(const std::vector<DesktopWidgetState>& widgets, const std::string& stackId) {
  const auto ownership = resolve(widgets);
  std::vector<DesktopWidgetState> result;
  if (auto it = ownership.find(stackId); it != ownership.end())
    for (const auto& id : it->second) {
      const auto found = std::ranges::find(widgets, id, &DesktopWidgetState::id);
      if (found != widgets.end())
        result.push_back(*found);
    }
  return result;
}

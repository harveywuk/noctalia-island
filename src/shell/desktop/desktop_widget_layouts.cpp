#include "shell/desktop/desktop_widget_layouts.h"

#include "shell/desktop/desktop_widget_setup.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

namespace desktop_layouts {
  namespace {
    using Json = nlohmann::json;
    constexpr std::size_t maxBytes = 8 * 1024 * 1024;

    Json encodeWidget(const DesktopWidgetState& widget) {
      Json settings = Json::object();
      for (const auto& [key, value] : widget.settings)
        std::visit([&](const auto& item) { settings[key] = item; }, value);
      return {
          {"id", widget.id},
          {"type", widget.type},
          {"output", widget.outputName},
          {"cx", widget.cx},
          {"cy", widget.cy},
          {"placement_width", widget.placementWidth},
          {"placement_height", widget.placementHeight},
          {"box_width", widget.boxWidth},
          {"box_height", widget.boxHeight},
          {"rotation", widget.rotationRad},
          {"flip_x", widget.flipX},
          {"flip_y", widget.flipY},
          {"enabled", widget.enabled},
          {"settings", std::move(settings)}
      };
    }

    float coordinate(const Json& value, bool nonnegative = false) {
      if (!value.is_number())
        throw std::runtime_error("Invalid layout coordinate");
      const auto result = value.get<float>();
      if (!std::isfinite(result) || std::abs(result) > 10000000 || (nonnegative && result < 0))
        throw std::runtime_error("Invalid layout coordinate");
      return result;
    }

    DesktopWidgetState decodeWidget(const Json& value) {
      DesktopWidgetState widget{
          .id = value.at("id").get<std::string>(),
          .type = value.at("type").get<std::string>(),
          .outputName = value.at("output").get<std::string>(),
          .cx = coordinate(value.at("cx")),
          .cy = coordinate(value.at("cy")),
          .placementWidth = coordinate(value.at("placement_width"), true),
          .placementHeight = coordinate(value.at("placement_height"), true),
          .boxWidth = coordinate(value.at("box_width"), true),
          .boxHeight = coordinate(value.at("box_height"), true),
          .rotationRad = coordinate(value.at("rotation")),
          .flipX = value.at("flip_x").get<bool>(),
          .flipY = value.at("flip_y").get<bool>(),
          .enabled = value.at("enabled").get<bool>(),
      };
      if (widget.id.empty() || widget.type.empty() || widget.id.size() > 256 || widget.type.size() > 256)
        throw std::runtime_error("Invalid layout widget");
      const auto& settings = value.at("settings");
      if (!settings.is_object())
        throw std::runtime_error("Invalid layout settings");
      for (const auto& [key, item] : settings.items()) {
        if (item.is_boolean())
          widget.settings[key] = item.get<bool>();
        else if (item.is_number_integer()) {
          if (item.is_number_unsigned() && item.get<std::uint64_t>() > std::numeric_limits<std::int64_t>::max())
            throw std::runtime_error("Invalid layout integer");
          widget.settings[key] = item.get<std::int64_t>();
        } else if (item.is_number_float() && std::isfinite(item.get<double>()))
          widget.settings[key] = item.get<double>();
        else if (item.is_string())
          widget.settings[key] = item.get<std::string>();
        else if (item.is_array())
          widget.settings[key] = item.get<std::vector<std::string>>();
        else if (item.is_object())
          widget.settings[key] = item.get<WidgetSettingStringMap>();
        else
          throw std::runtime_error("Invalid layout setting");
      }
      return widget;
    }

    std::unordered_set<std::string> monitorMembers(
        const DesktopWidgetsConfig& snapshot, const std::string& output, const OutputResolver& resolveOutput
    ) {
      const auto membership = desktop_stacks::resolve(snapshot.widgets);
      std::unordered_set<std::string> ids;
      for (const auto& widget : snapshot.widgets) {
        if (desktop_stacks::contains(membership, widget.id) || resolveOutput(widget) != output)
          continue;
        ids.insert(widget.id);
        if (const auto it = membership.find(widget.id); it != membership.end())
          ids.insert(it->second.begin(), it->second.end());
      }
      return ids;
    }
  } // namespace

  std::string cleanName(std::string name) {
    const auto start = name.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
      return {};
    name = name.substr(start, name.find_last_not_of(" \t\r\n") - start + 1);
    if (std::ranges::count_if(name, [](unsigned char c) { return (c & 0xc0) != 0x80; }) > 80
        || std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; }))
      return {};
    return name;
  }

  std::string encode(const std::vector<Layout>& layouts) {
    Json entries = Json::array();
    for (const auto& layout : layouts) {
      Json widgets = Json::array();
      for (const auto& widget : layout.snapshot.widgets)
        widgets.push_back(encodeWidget(widget));
      const auto& state = layout.snapshot;
      entries.push_back(
          {{"name", layout.name},
           {"output", layout.output},
           {"enabled", state.enabled},
           {"always_full_color", state.alwaysFullColor},
           {"schema_version", state.schemaVersion},
           {"grid",
            {{"visible", state.grid.visible},
             {"cell_size", state.grid.cellSize},
             {"major_interval", state.grid.majorInterval}}},
           {"widgets", std::move(widgets)}}
      );
    }
    return Json{{"version", 1}, {"layouts", std::move(entries)}}.dump();
  }

  std::optional<std::vector<Layout>> decode(const std::string& data) {
    if (data.size() > maxBytes)
      return std::nullopt;
    try {
      const auto root = Json::parse(data);
      if (root.at("version") != 1 || !root.at("layouts").is_array() || root.at("layouts").size() > maxLayouts)
        return std::nullopt;
      std::vector<Layout> layouts;
      std::unordered_set<std::string> names;
      for (const auto& value : root.at("layouts")) {
        Layout layout{.name = value.at("name").get<std::string>(), .output = value.at("output").get<std::string>()};
        if (cleanName(layout.name).empty()
            || layout.name != cleanName(layout.name)
            || !names.insert(layout.name).second)
          return std::nullopt;
        auto& state = layout.snapshot;
        state.enabled = value.at("enabled").get<bool>();
        state.alwaysFullColor = value.at("always_full_color").get<bool>();
        if (value.at("schema_version") != 2)
          return std::nullopt;
        const auto& grid = value.at("grid");
        if (!grid.at("cell_size").is_number_integer()
            || grid.at("cell_size") < 1
            || grid.at("cell_size") > 1024
            || !grid.at("major_interval").is_number_integer()
            || grid.at("major_interval") < 1
            || grid.at("major_interval") > 1024)
          return std::nullopt;
        state.grid = {
            .visible = grid.at("visible").get<bool>(),
            .cellSize = grid.at("cell_size").get<std::int32_t>(),
            .majorInterval = grid.at("major_interval").get<std::int32_t>()
        };
        if (!value.at("widgets").is_array() || value.at("widgets").size() > 1024)
          return std::nullopt;
        std::unordered_set<std::string> ids;
        for (const auto& entry : value.at("widgets")) {
          auto widget = decodeWidget(entry);
          if (!ids.insert(widget.id).second)
            return std::nullopt;
          state.widgets.push_back(std::move(widget));
        }
        layouts.push_back(std::move(layout));
      }
      return layouts;
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

  Layout capture(
      const DesktopWidgetsConfig& snapshot, std::string name, const std::string& output,
      const OutputResolver& resolveOutput
  ) {
    Layout layout{.name = cleanName(std::move(name)), .output = output, .snapshot = snapshot};
    if (!output.empty()) {
      const auto ids = monitorMembers(snapshot, output, resolveOutput);
      std::erase_if(layout.snapshot.widgets, [&](const auto& widget) { return !ids.contains(widget.id); });
    }
    return layout;
  }

  DesktopWidgetsConfig apply(
      const DesktopWidgetsConfig& current, const Layout& layout, const std::string& targetOutput,
      const OutputResolver& resolveOutput
  ) {
    if (layout.output.empty())
      return layout.snapshot;
    if (targetOutput.empty())
      return current;
    auto result = current;
    const auto replaced = monitorMembers(current, targetOutput, resolveOutput);
    std::erase_if(result.widgets, [&](const auto& widget) { return replaced.contains(widget.id); });
    std::unordered_set<std::string> occupied;
    for (const auto& widget : result.widgets)
      occupied.insert(widget.id);
    // Reserve all imported IDs before allocating replacements, so a generated ID
    // cannot collide with a later imported widget.
    for (const auto& widget : layout.snapshot.widgets)
      occupied.insert(widget.id);
    std::unordered_map<std::string, std::string> ids;
    for (const auto& widget : layout.snapshot.widgets) {
      auto id = widget.id;
      if (std::ranges::any_of(result.widgets, [&](const auto& existing) { return existing.id == id; })) {
        std::size_t suffix = 1;
        do {
          id = widget.id + "_layout_" + std::to_string(suffix++);
        } while (occupied.contains(id));
      }
      occupied.insert(id);
      ids.emplace(widget.id, id);
    }
    for (auto widget : layout.snapshot.widgets) {
      widget.id = ids.at(widget.id);
      widget.outputName = targetOutput;
      if (widget.type == "stack") {
        std::vector<std::string> members;
        for (const auto& member : desktop_stacks::members(widget))
          if (const auto it = ids.find(member); it != ids.end())
            members.push_back(it->second);
        widget.settings["members"] = std::move(members);
      }
      result.widgets.push_back(std::move(widget));
    }
    return result;
  }
} // namespace desktop_layouts

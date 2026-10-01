#include "compositors/hyprland/hyprland_displays.h"

#include "compositors/hyprland/hyprland_runtime.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <nlohmann/json.hpp>

namespace compositors::hyprland {
  std::vector<DisplayInfo> parseDisplays(std::string_view payload) {
    std::vector<DisplayInfo> result;
    const auto json = nlohmann::json::parse(payload, nullptr, false);
    if (!json.is_array())
      return result;
    for (const auto& item : json) {
      try {
        if (item.value("disabled", false))
          continue;
        DisplayInfo info;
        info.output = item.at("name").get<std::string>();
        info.description = item.value("description", info.output);
        if (info.description.empty())
          info.description = info.output;
        info.mode = std::format(
            "{}x{}@{:.2f}", item.at("width").get<int>(), item.at("height").get<int>(),
            item.at("refreshRate").get<double>()
        );
        info.x = item.value("x", 0);
        info.y = item.value("y", 0);
        info.scale = item.value("scale", 1.0F);
        info.transform = item.value("transform", 0);
        info.colorMode = item.value("colorManagementPreset", std::string("srgb"));
        info.vrrActive = item.value("vrr", false);
        info.sdrBrightness = item.value("sdrBrightness", 1.0F);
        info.sdrSaturation = item.value("sdrSaturation", 1.0F);
        info.bitDepth = item.value("currentFormat", std::string{}).find("2101010") != std::string::npos ? 10 : 8;
        for (auto mode : item.value("availableModes", std::vector<std::string>{})) {
          if (mode.ends_with("Hz"))
            mode.resize(mode.size() - 2);
          if (std::ranges::find(info.modes, mode) == info.modes.end())
            info.modes.push_back(std::move(mode));
        }
        if (std::ranges::find(info.modes, info.mode) == info.modes.end())
          info.modes.push_back(info.mode);
        if (!info.output.empty())
          result.push_back(std::move(info));
      } catch (const nlohmann::json::exception&) {
      }
    }
    return result;
  }

  std::string displayProblem(const HyprlandDisplayConfig& c, const DisplayInfo& info) {
    if (c.output.empty() || c.output != info.output)
      return "Display is no longer connected.";
    if (!c.managed)
      return {};
    if (!c.mode.empty()
        && c.mode != "preferred"
        && c.mode != "highres"
        && c.mode != "highrr"
        && std::ranges::find(info.modes, c.mode) == info.modes.end())
      return "This mode is not reported by the display.";
    if (!std::isfinite(c.scale)
        || (c.scale != 0 && (c.scale < .5F || c.scale > 4))
        || c.x < -32768
        || c.x > 32768
        || c.y < -32768
        || c.y > 32768)
      return "Invalid display scale or position.";
    if (c.transform < -1 || c.transform > 7 || c.vrr < -2 || c.vrr > 3)
      return "Invalid display setting.";
    const std::vector<std::string> colors{"",      "auto", "srgb", "dcip3", "dp3",
                                          "adobe", "wide", "edid", "hdr",   "hdredid"};
    if (std::ranges::find(colors, c.colorMode) == colors.end())
      return "Unknown color management preset.";
    if (c.bitDepth != 0 && c.bitDepth != 8 && c.bitDepth != 10)
      return "Bit depth must be 8 or 10.";
    if (!std::isfinite(c.sdrBrightness)
        || !std::isfinite(c.sdrSaturation)
        || c.sdrBrightness < 0
        || c.sdrBrightness > 2
        || c.sdrSaturation < 0
        || c.sdrSaturation > 2)
      return "Invalid SDR brightness or saturation.";
    return {};
  }

  std::string displayCommand(const HyprlandDisplayConfig& c, const DisplayInfo& info) {
    if (!c.managed || !displayProblem(c, info).empty())
      return {};
    // Explicit position/scale keep a newly created connector rule from changing the layout.
    std::string command = std::format(
        "; hl.monitor({{output={},position={},scale={}", luaStringLiteral(c.output),
        luaStringLiteral(std::format("{}x{}", c.positionManaged ? c.x : info.x, c.positionManaged ? c.y : info.y)),
        c.scale > 0 ? c.scale : info.scale
    );
    if (!c.mode.empty())
      command += ",mode=" + luaStringLiteral(c.mode);
    else
      command += ",mode=" + luaStringLiteral(info.mode);
    command += std::format(",transform={}", c.transform >= 0 ? c.transform : info.transform);
    if (c.vrr >= -1)
      command += std::format(",vrr={}", c.vrr);
    if (!c.colorMode.empty())
      command += ",cm=" + luaStringLiteral(c.colorMode);
    if (c.bitDepth)
      command += std::format(",bitdepth={}", c.bitDepth);
    if (c.sdrBrightness > 0)
      command += std::format(",sdrbrightness={}", c.sdrBrightness);
    if (c.sdrSaturation > 0)
      command += std::format(",sdrsaturation={}", c.sdrSaturation);
    return command + "})";
  }

  HyprlandDisplays::HyprlandDisplays(HyprlandRuntime& runtime) : HyprlandEventHandler(runtime) {}
  HyprlandDisplays::~HyprlandDisplays() {
    changed = {};
    if (m_preview)
      revert();
  }
  std::vector<DisplayInfo> HyprlandDisplays::displays() const {
    const auto reply = m_runtime.request("j/monitors");
    return reply ? parseDisplays(*reply) : std::vector<DisplayInfo>{};
  }
  void HyprlandDisplays::sync(const std::vector<HyprlandDisplayConfig>& saved) {
    if (saved == m_saved)
      return;
    const bool removed = std::ranges::any_of(m_saved, [&](const auto& old) {
      return old.managed
          && std::ranges::none_of(saved, [&](const auto& row) { return row.output == old.output && row.managed; });
    });
    m_saved = saved;
    if (removed)
      restore();
    else
      apply();
  }
  bool HyprlandDisplays::apply() {
    if (!m_runtime.available() || !m_runtime.configIsLua())
      return false;
    const auto outputs = displays();
    std::string command = "repl do end";
    for (const auto& output : outputs) {
      const auto saved = std::ranges::find(m_saved, output.output, &HyprlandDisplayConfig::output);
      const auto* preview = m_preview ? &*m_preview : nullptr;
      const auto candidate = preview ? std::ranges::find(*preview, output.output, &HyprlandDisplayConfig::output)
                                     : std::vector<HyprlandDisplayConfig>::const_iterator{};
      if (preview && candidate != preview->end())
        command += displayCommand(*candidate, output);
      else if (saved != m_saved.end())
        command += displayCommand(*saved, output);
    }
    if (command == "repl do end")
      return true;
    const auto reply = m_runtime.request(command);
    return reply && reply->find("error") == std::string::npos && reply->find("Error") == std::string::npos;
  }
  void HyprlandDisplays::restore() {
    (void)m_runtime.request("reload config-only");
    notifyChanged();
  }
  bool HyprlandDisplays::preview(std::vector<HyprlandDisplayConfig> candidates) {
    if (m_preview)
      revert();
    m_error.clear();
    const auto outputs = displays();
    if (candidates.empty())
      return false;
    for (const auto& candidate : candidates) {
      const auto output = std::ranges::find(outputs, candidate.output, &DisplayInfo::output);
      m_error = output == outputs.end() ? "Display is no longer connected." : displayProblem(candidate, *output);
      if (!m_error.empty())
        return false;
    }
    m_preview = std::move(candidates);
    if (std::ranges::any_of(*m_preview, [](const auto& c) { return !c.managed; }))
      restore();
    else if (!apply()) {
      m_preview.reset();
      restore();
      m_error = "Hyprland rejected the display settings.";
      return false;
    }
    m_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    m_confirmation.startRepeating(std::chrono::seconds(1), [this] {
      const auto connected = displays();
      if (secondsLeft() <= 0
          || (m_preview && std::ranges::any_of(*m_preview, [&](const auto& c) {
                return std::ranges::none_of(connected, [&](const auto& o) { return o.output == c.output; });
              })))
        revert();
    });
    if (changed)
      changed();
    return true;
  }
  int HyprlandDisplays::secondsLeft() const {
    return m_preview
        ? static_cast<int>(
              std::chrono::ceil<std::chrono::seconds>(m_deadline - std::chrono::steady_clock::now()).count()
          )
        : 0;
  }
  bool HyprlandDisplays::keep(const std::function<bool(const std::vector<HyprlandDisplayConfig>&)>& save) {
    if (!m_preview || secondsLeft() <= 0)
      return false;
    const auto outputs = displays();
    if (std::ranges::any_of(*m_preview, [&](const auto& c) {
          return std::ranges::none_of(outputs, [&](const auto& output) { return output.output == c.output; });
        })) {
      revert();
      m_error = "Display is no longer connected.";
      return false;
    }
    if (!save(*m_preview)) {
      m_error = "Could not save display settings.";
      return false;
    }
    m_preview.reset();
    m_confirmation.stop();
    if (changed)
      changed();
    return true;
  }
  void HyprlandDisplays::revert() {
    if (!m_preview)
      return;
    m_preview.reset();
    m_confirmation.stop();
    restore();
    if (changed)
      changed();
  }
  void HyprlandDisplays::handleEvent(std::string_view event, std::string_view) {
    if (event == "configreloaded" || event.starts_with("monitoradded") || event.starts_with("monitorremoved"))
      notifyChanged();
  }
  void HyprlandDisplays::notifyChanged() {
    m_applyTimer.start(std::chrono::milliseconds(100), [this] { apply(); });
  }
  void HyprlandDisplays::notifyCleanup() {
    m_confirmation.stop();
    m_applyTimer.stop();
    m_preview.reset();
  }
} // namespace compositors::hyprland

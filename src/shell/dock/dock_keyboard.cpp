#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "render/scene/input_area.h"
#include "shell/dock/dock.h"
#include "shell/dock/dock_context_menu.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/dock_instance.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <xkbcommon/xkbcommon-keysyms.h>

bool Dock::focusKeyboard() {
  if (!m_config || !m_config->config().dock.enabled || m_overlayDisplaySuppressed || m_instances.empty())
    return false;
  if (m_keyboardInstance) {
    leaveKeyboard();
    return true;
  }
  closeItemMenu();
  const auto output = m_platform->preferredInteractiveOutput();
  auto found = std::ranges::find_if(m_instances, [output](const auto& item) { return item->output == output; });
  auto* instance = (found == m_instances.end() ? m_instances.front() : *found).get();
  if (!instance->surface || !instance->sceneRoot)
    return false;
  m_keyboardInstance = instance;
  m_keyboardIndex = 0;
  instance->hideTimer.stop();
  clearHoverZoomPointer(*instance);
  shell::dock::revealAutoHideDock(*instance, *m_config);
  instance->surface->setKeyboardInteractivity(LayerShellKeyboard::Exclusive);
  updateKeyboardFocus();
  instance->surface->requestFrameTick();
  return true;
}

void Dock::leaveKeyboard() {
  auto* instance = m_keyboardInstance;
  m_keyboardInstance = nullptr;
  if (!instance)
    return;
  TooltipManager::instance().forceDestroy();
  if (instance->keyboardOutline)
    instance->keyboardOutline->setVisible(false);
  if (instance->surface) {
    instance->surface->setKeyboardInteractivity(LayerShellKeyboard::None);
    instance->surface->requestRedraw();
  }
  if (!instance->pointerInside)
    scheduleHide(*instance);
}

void Dock::updateKeyboardFocus() {
  auto* instance = m_keyboardInstance;
  if (!instance || !instance->sceneRoot || !instance->surface || m_itemMenu)
    return;
  const auto& cfg = m_config->config().dock;
  std::vector<InputArea*> areas;
  if (cfg.launcherPosition == DockLauncherPosition::Start && instance->launcherArea)
    areas.push_back(instance->launcherArea);
  for (auto& item : instance->items)
    areas.push_back(item.area);
  if (cfg.launcherPosition == DockLauncherPosition::End && instance->launcherArea)
    areas.push_back(instance->launcherArea);
  if (areas.empty()) {
    leaveKeyboard();
    return;
  }
  m_keyboardIndex = std::min(m_keyboardIndex, areas.size() - 1);
  auto* area = areas[m_keyboardIndex];
  float x = 0, y = 0;
  Node::absolutePosition(area, x, y);
  if (!instance->keyboardOutline) {
    auto outline = ui::box({});
    outline->setParticipatesInLayout(false);
    outline->setFill(clearColorSpec());
    outline->setBorder(colorSpecFromRole(ColorRole::OnSurface, 0.55F), Style::focusRingWidth);
    outline->setRadius(static_cast<float>(cfg.iconSize) * .25F);
    outline->setZIndex(300);
    instance->keyboardOutline = static_cast<Box*>(instance->sceneRoot->addChild(std::move(outline)));
  }
  instance->keyboardOutline->setPosition(x + 3, y + 3);
  instance->keyboardOutline->setSize(static_cast<float>(cfg.iconSize) + 6, static_cast<float>(cfg.iconSize) + 6);
  instance->keyboardOutline->setVisible(true);
  TooltipManager::instance().onHoverChange(area, instance->surface->layerSurface(), instance->output);
  TooltipManager::instance().syncAnchor(area);
  if (instance->sceneRoot->paintDirty())
    instance->surface->requestRedraw();
}

bool Dock::onKeyboardEvent(const KeyboardEvent& event) {
  for (const auto& instance : m_instances) {
    if (instance->drag.active || instance->drag.armed) {
      if (event.pressed && event.sym == XKB_KEY_Escape)
        endDrag(*instance, false);
      return true;
    }
  }
  if (m_itemMenu) {
    if (!event.pressed)
      return true;
    auto* control = m_itemMenu->control;
    if (event.sym == XKB_KEY_Escape) {
      closeItemMenu();
      updateKeyboardFocus();
    } else if (control) {
      if (event.sym == XKB_KEY_Up)
        (void)control->moveHighlight(-1);
      else if (event.sym == XKB_KEY_Down)
        (void)control->moveHighlight(1);
      else if (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter || event.sym == XKB_KEY_space)
        (void)control->activateHighlighted();
      else if (event.sym == XKB_KEY_Home)
        control->setHighlightedIndex(0);
    }
    return true;
  }
  auto* instance = m_keyboardInstance;
  if (!instance)
    return false;
  if (m_platform->lastKeyboardSurface() != instance->surface->wlSurface()) {
    leaveKeyboard();
    return false;
  }
  if (!event.pressed)
    return true;
  const auto& cfg = m_config->config().dock;
  const auto count = instance->items.size() + shell::dock::dockLauncherButtonCount(cfg);
  if (event.sym == XKB_KEY_Escape || count == 0) {
    leaveKeyboard();
    return true;
  }
  const bool vertical = shell::dock::isVerticalEdge(cfg.position);
  const bool backwards = event.sym == (vertical ? XKB_KEY_Up : XKB_KEY_Left)
      || event.sym == XKB_KEY_ISO_Left_Tab
      || (event.sym == XKB_KEY_Tab && (event.modifiers & KeyMod::Shift));
  const bool forwards = event.sym == (vertical ? XKB_KEY_Down : XKB_KEY_Right) || event.sym == XKB_KEY_Tab;
  if (backwards || forwards || event.sym == XKB_KEY_Home || event.sym == XKB_KEY_End) {
    if (event.sym == XKB_KEY_Home)
      m_keyboardIndex = 0;
    else if (event.sym == XKB_KEY_End)
      m_keyboardIndex = count - 1;
    else
      m_keyboardIndex = (m_keyboardIndex + count + (backwards ? -1 : 1)) % count;
    updateKeyboardFocus();
    return true;
  }
  const bool launcher = (cfg.launcherPosition == DockLauncherPosition::Start && m_keyboardIndex == 0)
      || (cfg.launcherPosition == DockLauncherPosition::End && m_keyboardIndex == count - 1);
  const bool activate = event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter || event.sym == XKB_KEY_space;
  if (launcher) {
    if (activate) {
      leaveKeyboard();
      PanelManager::instance().togglePanel("launcher", PanelOpenRequest{.output = instance->output});
    }
    return true;
  }
  const auto index = m_keyboardIndex - (cfg.launcherPosition == DockLauncherPosition::Start ? 1 : 0);
  if (index >= instance->snapshot.items.size())
    return true;
  const auto& model = instance->snapshot.items[index];
  const shell::dock::DockItemAction action{
      .entry = model.entry,
      .idLower = model.idLower,
      .startupWmClassLower = model.startupWmClassLower,
      .windowLookupIdLower = model.windowLookupIdLower,
      .windowLookupWmClassLower = model.windowLookupWmClassLower
  };
  if (event.sym == XKB_KEY_Menu || (event.sym == XKB_KEY_F10 && (event.modifiers & KeyMod::Shift))) {
    openItemMenu(*instance, action);
  } else if (activate) {
    leaveKeyboard();
    activateOrLaunchItem(*instance, action);
  }
  return true;
}

#include "core/deferred_call.h"
#include "render/render_context.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_manager.h"
#include "tests/test_check.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/popup_chrome.h"
#include "wayland/wayland_connection.h"

#include <memory>

class ContextMenuPopupTestAccess {
public:
  static void activate(ContextMenuPopup& popup, ContextMenuControlEntry entry) {
    popup.deferActivation(std::move(entry));
  }

  static void dismiss(ContextMenuPopup& popup) { popup.deferClose(); }
  static std::uint64_t generation(const ContextMenuPopup& popup) { return popup.m_generation; }
};

namespace {

  class PopupOwningPanel final : public Panel {
  public:
    PopupOwningPanel(WaylandConnection& wayland, RenderContext& renderContext, int& activations)
        : m_popup(wayland, renderContext) {
      m_popup.setOnActivate([&activations](const ContextMenuControlEntry&) { ++activations; });
    }

    void create() override {}
    [[nodiscard]] float preferredWidth() const override { return 1.0F; }
    [[nodiscard]] float preferredHeight() const override { return 1.0F; }

    void deferActivation() {
      ContextMenuPopupTestAccess::activate(m_popup, ContextMenuControlEntry{.id = 2, .label = "Delete"});
    }

  protected:
    void doLayout(Renderer&, float, float) override {}

  private:
    ContextMenuPopup m_popup;
  };

  void drainDeferredCalls() {
    for (auto& callback : DeferredCall::takePending()) {
      callback();
    }
  }

} // namespace

int main() {
  // Long menus at larger UI scales must fit a logical output, including shadows.
  const auto large = popup_chrome::computeGeometry(900, 1600, ShellConfig::ShadowConfig{});
  const auto fitted = popup_chrome::constrainGeometry(large, 800, 600, 12);
  TEST_CHECK(fitted.surfaceWidth <= 776);
  TEST_CHECK(fitted.surfaceHeight <= 576);
  TEST_CHECK(fitted.contentWidth > 0 && fitted.contentHeight > 0);
  TEST_CHECK(fitted.bleed.left == large.bleed.left && fitted.bleed.down == large.bleed.down);
  const auto small = popup_chrome::computeGeometry(200, 100, ShellConfig::ShadowConfig{});
  const auto unchanged = popup_chrome::constrainGeometry(small, 1280, 720, 8);
  TEST_CHECK(unchanged.surfaceWidth == small.surfaceWidth && unchanged.surfaceHeight == small.surfaceHeight);
  const auto unknown = popup_chrome::constrainGeometry(large, 0, 0, 8);
  TEST_CHECK(unknown.surfaceWidth == large.surfaceWidth && unknown.surfaceHeight == large.surfaceHeight);

  WaylandConnection wayland;
  RenderContext renderContext;

  int activations = 0;
  {
    ContextMenuPopup popup(wayland, renderContext);
    popup.setOnActivate([&activations](const ContextMenuControlEntry&) { ++activations; });
    ContextMenuPopupTestAccess::activate(popup, ContextMenuControlEntry{.id = 1, .label = "Copy"});
    drainDeferredCalls();
  }
  TEST_CHECK(activations == 1);

  {
    ContextMenuPopup popup(wayland, renderContext);
    ContextMenuPopupTestAccess::dismiss(popup);
    popup.close();
    const auto generation = ContextMenuPopupTestAccess::generation(popup);
    drainDeferredCalls();
    TEST_CHECK(ContextMenuPopupTestAccess::generation(popup) == generation);
  }

  // Plugin unregistration destroys its panel-owned popup immediately. A queued
  // activation must then become a no-op instead of dereferencing the destroyed
  // ContextMenuPopup on the next main-loop iteration.
  {
    PanelManager panels;
    auto panel = std::make_unique<PopupOwningPanel>(wayland, renderContext, activations);
    panel->deferActivation();
    panels.registerPanel("test/plugin:panel", std::move(panel));
    panels.unregisterPanel("test/plugin:panel");
  }
  drainDeferredCalls();
  TEST_CHECK(activations == 1);

  return 0;
}

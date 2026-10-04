#pragma once

#include "app/poll_source.h"
#include "core/inotify/inotify.h"
#include "launcher/snippet_matcher.h"

#include <functional>
#include <string>
#include <vector>

class ClipboardService;
class ConfigService;
class SnippetStore;
class VirtualKeyboardService;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

// Raycast's snippet expansion: type a snippet's keyword in any app and it is replaced by the
// snippet's text. Wayland has no protocol for watching the keyboard, so, like espanso, this
// reads the keyboards under /dev/input (the user must be in the `input` group) and decodes keys
// with the compositor's keymap. When a keyword lands, the keyword is erased with Backspace through
// the virtual keyboard and the expanded text pasted. Off unless shell.launcher.snippet_expansion.
class SnippetExpander : public PollSource {
public:
  SnippetExpander(
      ConfigService* config, SnippetStore* store, VirtualKeyboardService* virtualKeyboard, ClipboardService* clipboard
  );
  ~SnippetExpander() override;

  SnippetExpander(const SnippetExpander&) = delete;
  SnippetExpander& operator=(const SnippetExpander&) = delete;

  // The compositor's keymap text (wl_keyboard.keymap), so keys decode with the user's layout.
  void setKeymapSource(std::function<std::string()> keymap) { m_keymapSource = std::move(keymap); }
  // True while one of the shell's own surfaces has the keyboard; typing there never expands.
  void setSuspendedSource(std::function<bool()> suspended) { m_suspended = std::move(suspended); }

  // Re-reads the config: starts watching the keyboards when enabled, stops when not. Call after
  // a config reload and when snippets change.
  void apply();
  void refreshKeywords();

  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  // True when expansion is on but no keyboard could be opened (usually a missing `input` group).
  [[nodiscard]] bool unavailable() const noexcept { return m_enabled && m_devices.empty(); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct Device {
    int fd = -1;
    std::string path;
  };

  void start();
  void stop();
  void scanDevices();
  void openDevice(const std::string& path);
  void closeDevices();
  bool ensureKeymap();
  void releaseKeymap();
  void handleKey(std::uint32_t code, std::int32_t value);
  void expand(const SnippetMatcher::Keyword& keyword);

  ConfigService* m_config = nullptr;
  SnippetStore* m_store = nullptr;
  VirtualKeyboardService* m_virtualKeyboard = nullptr;
  ClipboardService* m_clipboard = nullptr;
  std::function<std::string()> m_keymapSource;
  std::function<bool()> m_suspended;
  bool m_enabled = false;
  bool m_warnedNoAccess = false;
  std::vector<Device> m_devices;
  Inotify m_inotify;
  SnippetMatcher m_matcher;
  xkb_context* m_xkbContext = nullptr;
  xkb_keymap* m_xkbKeymap = nullptr;
  xkb_state* m_xkbState = nullptr;
  std::string m_keymapText;
};

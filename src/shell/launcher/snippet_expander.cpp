#include "shell/launcher/snippet_expander.h"

#include "config/config_service.h"
#include "core/log.h"
#include "launcher/snippet_provider.h"
#include "launcher/snippet_store.h"
#include "shell/clipboard/clipboard_paste.h"
#include "wayland/clipboard_service.h"
#include "wayland/virtual_keyboard_service.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

namespace {

  constexpr Logger kLog("snippet-expander");
  constexpr std::string_view kInputDir = "/dev/input";
  constexpr std::uint32_t kEvdevOffset = 8; // evdev keycode → XKB keycode

  [[nodiscard]] bool testBit(const unsigned char* bits, unsigned bit) {
    return (bits[bit / 8] & (1U << (bit % 8))) != 0;
  }

  // A device that reports letter keys and Space: a keyboard rather than a mouse or a power button.
  [[nodiscard]] bool looksLikeKeyboard(int fd) {
    unsigned char types[EV_MAX / 8 + 1] = {};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(types)), types) < 0 || !testBit(types, EV_KEY)) {
      return false;
    }
    unsigned char keys[KEY_MAX / 8 + 1] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) {
      return false;
    }
    return testBit(keys, KEY_A) && testBit(keys, KEY_Z) && testBit(keys, KEY_SPACE) && testBit(keys, KEY_BACKSPACE);
  }

  [[nodiscard]] bool endsWord(xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_Escape:
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
    case XKB_KEY_Left:
    case XKB_KEY_Right:
    case XKB_KEY_Up:
    case XKB_KEY_Down:
    case XKB_KEY_Home:
    case XKB_KEY_End:
    case XKB_KEY_Page_Up:
    case XKB_KEY_Page_Down:
    case XKB_KEY_Delete:
    case XKB_KEY_Insert:
      return true;
    default:
      return false;
    }
  }

} // namespace

SnippetExpander::SnippetExpander(
    ConfigService* config, SnippetStore* store, VirtualKeyboardService* virtualKeyboard, ClipboardService* clipboard
)
    : m_config(config), m_store(store), m_virtualKeyboard(virtualKeyboard), m_clipboard(clipboard) {}

SnippetExpander::~SnippetExpander() {
  stop();
  releaseKeymap();
  if (m_xkbContext != nullptr) {
    xkb_context_unref(m_xkbContext);
    m_xkbContext = nullptr;
  }
}

void SnippetExpander::apply() {
  const bool wanted = m_config != nullptr && m_config->config().shell.launcher.snippetExpansion;
  if (wanted && !m_enabled) {
    start();
  } else if (!wanted && m_enabled) {
    stop();
  }
  if (m_enabled) {
    refreshKeywords();
  }
}

void SnippetExpander::refreshKeywords() {
  std::vector<SnippetMatcher::Keyword> keywords;
  for (const auto& entry : SnippetProvider::collect(m_config, m_store)) {
    if (!entry.keyword.empty()) {
      keywords.push_back({.keyword = entry.keyword, .snippetId = entry.id});
    }
  }
  m_matcher.setKeywords(std::move(keywords));
}

void SnippetExpander::start() {
  m_enabled = true;
  m_warnedNoAccess = false;
  scanDevices();
  if (m_inotify.fd() >= 0) {
    (void)m_inotify.watch(std::filesystem::path(kInputDir), IN_CREATE | IN_ATTRIB | IN_DELETE);
  }
  if (m_devices.empty()) {
    kLog.warn(
        "snippet expansion is on but no keyboard under {} could be read; add your user to the `input` group", kInputDir
    );
    m_warnedNoAccess = true;
  } else {
    kLog.info("snippet expansion watching {} keyboard(s)", m_devices.size());
  }
}

void SnippetExpander::stop() {
  m_enabled = false;
  closeDevices();
  m_matcher.reset();
}

void SnippetExpander::scanDevices() {
  std::error_code ec;
  std::vector<std::string> paths;
  for (const auto& entry : std::filesystem::directory_iterator(kInputDir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.starts_with("event")) {
      paths.push_back(entry.path().string());
    }
  }
  std::ranges::sort(paths);
  for (const auto& path : paths) {
    openDevice(path);
  }
}

void SnippetExpander::openDevice(const std::string& path) {
  if (std::ranges::any_of(m_devices, [&](const Device& device) { return device.path == path; })) {
    return;
  }
  const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return; // EACCES without the input group, or a device that vanished
  }
  if (!looksLikeKeyboard(fd)) {
    close(fd);
    return;
  }
  m_devices.push_back({.fd = fd, .path = path});
}

void SnippetExpander::closeDevices() {
  for (auto& device : m_devices) {
    if (device.fd >= 0) {
      close(device.fd);
    }
  }
  m_devices.clear();
}

void SnippetExpander::releaseKeymap() {
  if (m_xkbState != nullptr) {
    xkb_state_unref(m_xkbState);
    m_xkbState = nullptr;
  }
  if (m_xkbKeymap != nullptr) {
    xkb_keymap_unref(m_xkbKeymap);
    m_xkbKeymap = nullptr;
  }
  m_keymapText.clear();
}

bool SnippetExpander::ensureKeymap() {
  const std::string text = m_keymapSource ? m_keymapSource() : std::string();
  if (m_xkbState != nullptr && text == m_keymapText) {
    return true;
  }
  releaseKeymap();
  if (m_xkbContext == nullptr) {
    m_xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  }
  if (m_xkbContext == nullptr) {
    return false;
  }
  if (!text.empty()) {
    m_xkbKeymap =
        xkb_keymap_new_from_string(m_xkbContext, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  }
  if (m_xkbKeymap == nullptr) {
    // No compositor keymap yet: the session default layout.
    const xkb_rule_names names = {
        .rules = "evdev", .model = "pc105", .layout = nullptr, .variant = nullptr, .options = nullptr
    };
    m_xkbKeymap = xkb_keymap_new_from_names(m_xkbContext, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  }
  if (m_xkbKeymap == nullptr) {
    return false;
  }
  m_xkbState = xkb_state_new(m_xkbKeymap);
  m_keymapText = text;
  return m_xkbState != nullptr;
}

void SnippetExpander::doAddPollFds(std::vector<pollfd>& fds) {
  if (!m_enabled) {
    return;
  }
  for (const auto& device : m_devices) {
    fds.push_back({.fd = device.fd, .events = POLLIN, .revents = 0});
  }
  if (m_inotify.fd() >= 0) {
    fds.push_back({.fd = m_inotify.fd(), .events = POLLIN, .revents = 0});
  }
}

void SnippetExpander::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (!m_enabled) {
    return;
  }
  std::size_t index = startIdx;
  std::vector<std::string> gone;
  for (const auto& device : m_devices) {
    if (index >= fds.size()) {
      break;
    }
    const pollfd& pfd = fds[index++];
    if (pfd.fd != device.fd) {
      continue;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      gone.push_back(device.path);
      continue;
    }
    if ((pfd.revents & POLLIN) == 0) {
      continue;
    }
    input_event events[32];
    for (;;) {
      const ssize_t read = ::read(device.fd, events, sizeof(events));
      if (read <= 0) {
        if (read < 0 && errno != EAGAIN && errno != EINTR) {
          gone.push_back(device.path);
        }
        break;
      }
      const std::size_t count = static_cast<std::size_t>(read) / sizeof(input_event);
      for (std::size_t i = 0; i < count; ++i) {
        if (events[i].type == EV_KEY) {
          handleKey(events[i].code, events[i].value);
        }
      }
    }
  }
  if (index < fds.size() && m_inotify.fd() >= 0 && fds[index].fd == m_inotify.fd()) {
    if ((fds[index].revents & POLLIN) != 0) {
      m_inotify.drain([this](const inotify_event* event) {
        if (event->len == 0) {
          return;
        }
        const std::string name(event->name);
        if (!name.starts_with("event")) {
          return;
        }
        const std::string path = std::string(kInputDir) + "/" + name;
        if ((event->mask & IN_DELETE) != 0) {
          std::erase_if(m_devices, [&](Device& device) {
            if (device.path != path) {
              return false;
            }
            close(device.fd);
            return true;
          });
        } else {
          openDevice(path);
        }
      });
    }
  }
  for (const auto& path : gone) {
    std::erase_if(m_devices, [&](Device& device) {
      if (device.path != path) {
        return false;
      }
      close(device.fd);
      return true;
    });
  }
  if (m_enabled && m_devices.empty() && !m_warnedNoAccess) {
    kLog.warn("snippet expansion lost its last keyboard; waiting for one under {}", kInputDir);
    m_warnedNoAccess = true;
  }
}

void SnippetExpander::handleKey(std::uint32_t code, std::int32_t value) {
  if (!ensureKeymap()) {
    return;
  }
  const xkb_keycode_t keycode = code + kEvdevOffset;
  if (value == 0) {
    xkb_state_update_key(m_xkbState, keycode, XKB_KEY_UP);
    return;
  }
  if (value == 1) {
    xkb_state_update_key(m_xkbState, keycode, XKB_KEY_DOWN);
  }
  // value 2 is the kernel's autorepeat: another press of the same key.
  if (m_matcher.empty() || (m_suspended && m_suspended())) {
    m_matcher.reset();
    return;
  }
  const bool chord = xkb_state_mod_name_is_active(m_xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0
      || xkb_state_mod_name_is_active(m_xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0
      || xkb_state_mod_name_is_active(m_xkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0;
  const xkb_keysym_t sym = xkb_state_key_get_one_sym(m_xkbState, keycode);
  if (sym == XKB_KEY_BackSpace) {
    if (chord) {
      m_matcher.reset();
    } else {
      m_matcher.backspace();
    }
    return;
  }
  if (chord || endsWord(sym)) {
    m_matcher.reset();
    return;
  }
  char utf8[8] = {};
  const int written = xkb_state_key_get_utf8(m_xkbState, keycode, utf8, sizeof(utf8));
  if (written <= 0) {
    return; // a modifier or dead key on its own
  }
  if (static_cast<unsigned char>(utf8[0]) < 0x20 || utf8[0] == 0x7F) {
    m_matcher.reset(); // a control character
    return;
  }
  if (const auto match = m_matcher.feed(std::string_view(utf8, static_cast<std::size_t>(written))); match.has_value()) {
    expand(*match);
  }
}

void SnippetExpander::expand(const SnippetMatcher::Keyword& keyword) {
  if (m_virtualKeyboard == nullptr || m_clipboard == nullptr || !m_virtualKeyboard->isAvailable()) {
    return;
  }
  std::string text;
  for (const auto& entry : SnippetProvider::collect(m_config, m_store)) {
    if (entry.id == keyword.snippetId) {
      text = entry.text;
      break;
    }
  }
  if (text.empty()) {
    return;
  }
  const std::string clipboardText = m_clipboard->clipboardText().value_or(std::string());
  text = SnippetProvider::expand(text, std::chrono::system_clock::now(), clipboardText);
  if (!m_virtualKeyboard->typeBackspaces(SnippetMatcher::codePoints(keyword.keyword))) {
    return;
  }
  if (!m_clipboard->copyText(text)) {
    return;
  }
  const auto mode = m_config != nullptr ? m_config->config().shell.launcher.autoPaste : ClipboardAutoPasteMode::Auto;
  (void)clipboard_paste::pasteEntry(
      false, mode == ClipboardAutoPasteMode::Off ? ClipboardAutoPasteMode::Auto : mode, *m_virtualKeyboard
  );
}

#include "shell/dock/pinned_apps.h"
#include "system/internal_app_metadata.h"
#include "tests/test_check.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace internal_apps {

  std::optional<AppMetadata> metadataForAppId(std::string_view /*appId*/) { return std::nullopt; }

  void applyMetadataToDesktopEntry(DesktopEntry& /*entry*/) {}

} // namespace internal_apps

namespace {

  DesktopEntry sampleEntry() {
    DesktopEntry entry;
    entry.id = "sample-chat.desktop";
    entry.name = "Sample Chat";
    entry.nameLower = "sample chat";
    entry.startupWmClass = "SampleChat";
    entry.startupWmClassLower = "samplechat";
    entry.exec = "sample-chat";
    entry.icon = "sample-chat";
    return entry;
  }

  DesktopEntry pathStyleEntry() {
    DesktopEntry entry;
    entry.id = "/usr/share/applications/org.example.Mail.desktop";
    entry.name = "Example Mail";
    entry.nameLower = "example mail";
    entry.startupWmClass = "ExampleMail";
    entry.startupWmClassLower = "examplemail";
    return entry;
  }

} // namespace

int main() {
  const DesktopEntry chat = sampleEntry();

  TEST_CHECK(shell::dock::pinned_apps::matchesEntry(chat, "sample-chat.desktop"));
  TEST_CHECK(shell::dock::pinned_apps::matchesEntry(chat, "sample-chat")); // the id with or without .desktop
  DesktopEntry foot;
  foot.id = "org.codeberg.dnkl.foot";
  foot.path = "/usr/share/applications/org.codeberg.dnkl.foot.desktop";
  TEST_CHECK(shell::dock::pinned_apps::matchesEntry(foot, "org.codeberg.dnkl.foot.desktop"));
  TEST_CHECK(shell::dock::pinned_apps::matchesEntry(foot, "/usr/share/applications/org.codeberg.dnkl.foot.desktop"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(foot, "foot"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "SampleChat"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "sample chat"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "sample_chat_desktop"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, ""));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "calendar"));

  const DesktopEntry mail = pathStyleEntry();
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(mail, "org.example.Mail"));
  TEST_CHECK(shell::dock::pinned_apps::matchesEntry(mail, "/usr/share/applications/org.example.Mail.desktop"));

  std::vector<std::string> pinned = {"calendar", "samplechat", "sample-chat.desktop"};
  TEST_CHECK(shell::dock::pinned_apps::containsEntry(pinned, chat));

  shell::dock::pinned_apps::removeEntry(pinned, chat);
  TEST_CHECK((pinned == std::vector<std::string>{"calendar", "samplechat"}));

  // Moving an existing pin preserves its configured alias and never duplicates it.
  pinned = {"org.codeberg.dnkl.foot.desktop", "sample-chat", "calendar"};
  shell::dock::pinned_apps::placeEntry(pinned, chat, &foot);
  TEST_CHECK((pinned == std::vector<std::string>{"sample-chat", "org.codeberg.dnkl.foot.desktop", "calendar"}));
  shell::dock::pinned_apps::placeEntry(pinned, chat, &chat);
  TEST_CHECK(pinned.front() == "sample-chat");
  shell::dock::pinned_apps::placeEntry(pinned, foot, nullptr);
  TEST_CHECK((pinned == std::vector<std::string>{"sample-chat", "calendar", "org.codeberg.dnkl.foot.desktop"}));
  // A running app can enter an empty pinned group or be inserted before an existing app.
  pinned.clear();
  shell::dock::pinned_apps::placeEntry(pinned, foot, nullptr);
  shell::dock::pinned_apps::placeEntry(pinned, chat, &foot);
  TEST_CHECK((pinned == std::vector<std::string>{chat.id, foot.id}));
  shell::dock::pinned_apps::removeEntry(pinned, chat);
  TEST_CHECK((pinned == std::vector<std::string>{foot.id}));
  return 0;
}

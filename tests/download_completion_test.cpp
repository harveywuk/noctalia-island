#include "dbus/downloads/launcher_progress.h"
#include "shell/island/island_state.h"

#include <cassert>

int main() {
  const LauncherProgress pending{0.42, true};
  const LauncherProgress done{1.0, true};
  const LauncherProgress hiddenDone{1.0, false};
  assert(pending.active() && !done.active());
  assert(pending.completedBy(done));
  assert(pending.completedBy(hiddenDone)); // Success and hide can arrive together.
  assert(!done.completedBy(done));         // Repeated updates never extend the completion notice.
  assert(!done.completedBy(hiddenDone));
  assert(!pending.completedBy({0.42, false})); // Cancellation is not success.
  assert(!pending.completedBy({0.0, false}));
  assert(!LauncherProgress{}.completedBy(done));            // No historical success on shell startup.
  assert(!hiddenDone.completedBy(done));                    // Nor when an old entry becomes visible again.
  assert(!LauncherProgress(0.99, false).completedBy(done)); // Ignore late updates after hiding.
  assert(LauncherProgress(0.0, true).completedBy(done));    // Tiny or unknown-size transfer.

  using island::Activity;
  using island::View;
  const auto view = [](bool notification, bool osd, bool hovered, bool complete) {
    return island::view(
        notification, osd, hovered, true, true, true, false, true, true, Activity::Media, Activity::Media, complete
    );
  };
  assert(view(false, false, false, true) == View::Completion);
  assert(view(false, false, true, true) == View::Completion);
  assert(view(true, false, false, true) == View::Notification);
  assert(view(false, true, false, true) == View::Osd);
  assert(view(false, false, false, false) == View::Activity);
  assert(view(false, false, true, false) == View::Media);
  assert(!island::showsStatusIcons(View::Completion));
}

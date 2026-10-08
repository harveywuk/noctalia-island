#include "dbus/downloads/download_progress.h"
#include "dbus/downloads/launcher_progress.h"
#include "shell/island/island_state.h"
#include "shell/island/island_transfer.h"

#include <array>
#include <cassert>
#include <cmath>

int main() {
  const std::array entries{
      DesktopEntry{.id = "org.example.Transfer", .name = "Shared name", .startupWmClass = "TransferWindow"},
      DesktopEntry{.id = "other.desktop", .name = "Shared name", .startupWmClass = "OtherWindow"}
  };
  const auto source = downloadSource("org.example.Transfer.desktop", entries);
  assert(source.desktopId == "org.example.Transfer" && source.wmClass == "TransferWindow");
  assert(source.name == "Shared name");
  const auto other = downloadSource("other.desktop", entries);
  assert(other.desktopId == "other" && other.wmClass == "OtherWindow");
  // The display label cannot select another application with the same name.
  const auto unknown = downloadSource("unknown.desktop", entries);
  assert(unknown.desktopId == "unknown" && unknown.name == "unknown" && unknown.wmClass.empty());

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
  assert(view(false, false, false, true) == View::TransferNotice);
  assert(view(false, false, true, true) == View::TransferNotice);
  assert(view(true, false, false, true) == View::Notification);
  assert(view(false, true, false, true) == View::Osd);
  assert(view(false, false, false, false) == View::Activity);
  assert(view(false, false, true, false) == View::Media);
  // Expiry resolves against current work, including another download, rather
  // than restoring a snapshot of the job that just finished.
  assert(
      island::view(
          false, false, false, true, false, true, false, true, true, Activity::None, Activity::Downloads, false
      )
      == View::DownloadActivity
  );
  assert(
      island::view(false, false, false, false, false, false, false, true, true, Activity::None, Activity::None, false)
      == View::Rest
  );
  assert(!island::showsStatusIcons(View::TransferNotice));

  // Unknown-size paused transfers stop spinning without inventing a percentage.
  const DownloadProgress paused{.progress = .4, .phase = "paused"};
  const DownloadProgress unknownPaused{.determinate = false, .phase = "paused"};
  const DownloadProgress running{.progress = .8, .phase = "working"};
  const DownloadProgress unknownRunning{.determinate = false, .phase = "downloading"};
  const std::array pausedGroup{paused, unknownPaused};
  assert(downloadsPaused(pausedGroup));
  assert(downloadFraction(pausedGroup) == 0.0F);
  assert(downloadFraction(std::array{paused}) == .4F);
  const std::array knownMixed{paused, running};
  assert(!downloadsPaused(knownMixed));
  assert(std::abs(*downloadFraction(knownMixed) - .6F) < .001F);
  assert(!downloadFraction(std::array{unknownPaused, running}));
  assert(!downloadFraction(std::array{paused, unknownRunning}));
  assert(!downloadFraction({}) && !downloadsPaused({}));

  using island::TransferStatus;
  assert(island::transferStatus("running") == TransferStatus::Running);
  assert(island::transferStatus("paused") == TransferStatus::Paused);
  assert(island::transferStatus("failed") == TransferStatus::Failed);
  assert(!island::transferStatus("") && !island::transferStatus("stalled"));
  assert(!island::transferStatus("Paused"));
}

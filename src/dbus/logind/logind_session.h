#pragma once

#include <optional>
#include <sdbus-c++/Types.h>

namespace sdbus {
  class IConnection;
}

namespace logind {

  // Resolves the logind session this shell belongs to, trying in order:
  //   1. XDG_SESSION_ID, when the launcher passed it through;
  //   2. the session containing this process;
  //   3. the user's graphical display session. Services started by the systemd user
  //      manager (e.g. a graphical-session.target unit) belong to no session, so
  //      without this lock-before-sleep and brightness control would be disabled.
  [[nodiscard]] std::optional<sdbus::ObjectPath> resolveSessionPath(sdbus::IConnection& connection);

} // namespace logind

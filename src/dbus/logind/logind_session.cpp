#include "dbus/logind/logind_session.h"

#include "core/log.h"

#include <cstdint>
#include <cstdlib>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <string>
#include <unistd.h>
#include <utility>

namespace logind {

  namespace {
    constexpr Logger kLog("logind");

    const sdbus::ServiceName kBusName{"org.freedesktop.login1"};
    const sdbus::ObjectPath kManagerPath{"/org/freedesktop/login1"};
    constexpr auto kManagerInterface = "org.freedesktop.login1.Manager";
    constexpr auto kUserInterface = "org.freedesktop.login1.User";
  } // namespace

  std::optional<sdbus::ObjectPath> resolveSessionPath(sdbus::IConnection& connection) {
    std::unique_ptr<sdbus::IProxy> manager;
    try {
      manager = sdbus::createProxy(connection, kBusName, kManagerPath);
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to resolve logind session: {}", e.what());
      return std::nullopt;
    }

    if (const char* sessionId = std::getenv("XDG_SESSION_ID"); sessionId != nullptr && sessionId[0] != '\0') {
      try {
        sdbus::ObjectPath path;
        manager->callMethod("GetSession")
            .onInterface(kManagerInterface)
            .withArguments(std::string(sessionId))
            .storeResultsTo(path);
        return path;
      } catch (const sdbus::Error& e) {
        kLog.debug("failed to resolve logind session via XDG_SESSION_ID={}: {}", sessionId, e.what());
      }
    }

    try {
      sdbus::ObjectPath path;
      manager->callMethod("GetSessionByPID")
          .onInterface(kManagerInterface)
          .withArguments(static_cast<std::uint32_t>(::getpid()))
          .storeResultsTo(path);
      return path;
    } catch (const sdbus::Error& e) {
      kLog.debug("process is not in a logind session ({}); trying the user's display session", e.what());
    }

    try {
      sdbus::ObjectPath userPath;
      manager->callMethod("GetUser")
          .onInterface(kManagerInterface)
          .withArguments(static_cast<std::uint32_t>(::getuid()))
          .storeResultsTo(userPath);
      auto user = sdbus::createProxy(connection, kBusName, userPath);
      const auto display =
          user->getProperty("Display").onInterface(kUserInterface).get<sdbus::Struct<std::string, sdbus::ObjectPath>>();
      const sdbus::ObjectPath& path = std::get<1>(display);
      if (!path.empty() && path != "/") {
        static bool logged = false;
        if (!std::exchange(logged, true)) {
          kLog.info("using the user's display session {} (shell runs outside a session)", std::get<0>(display));
        }
        return path;
      }
      kLog.warn("failed to resolve logind session: the user has no graphical display session");
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to resolve logind session: {}", e.what());
    }
    return std::nullopt;
  }

} // namespace logind

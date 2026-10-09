#pragma once

#include "dbus/network/network_types.h"
#include "i18n/i18n.h"
#include "shell/island/island_preview_target.h"

#include <chrono>
#include <optional>
#include <string>

namespace island {
  struct NetworkLink {
    NetworkConnectivity kind = NetworkConnectivity::Unknown;
    std::string ssid;

    bool matches(const NetworkLink& other) const {
      return kind == other.kind
          && (kind != NetworkConnectivity::Wireless || ssid == other.ssid || ssid.empty() || other.ssid.empty());
    }
  };

  struct NetworkNotice {
    using Clock = std::chrono::steady_clock;
    enum class Kind { Connected, Lost, Restored };
    Kind kind;
    NetworkLink link;
    Clock::time_point started;
    std::uint64_t serial;
    PreviewTarget target;

    std::string actionKey() const { return "network:" + std::to_string(serial); }
    std::string title() const {
      if (kind == Kind::Lost)
        return i18n::tr("island.network.lost");
      if (kind == Kind::Restored)
        return i18n::tr("island.network.restored");
      if (link.kind == NetworkConnectivity::Wired)
        return i18n::tr("island.network.ethernet-connected");
      if (link.kind == NetworkConnectivity::Cellular)
        return i18n::tr("island.network.cellular-connected");
      return link.ssid.empty() ? i18n::tr("island.network.wifi-connected") : i18n::tr("island.network.connected-to");
    }
    std::string detail() const {
      if (link.kind == NetworkConnectivity::Wireless && !link.ssid.empty())
        return link.ssid;
      if (kind == Kind::Connected)
        return {};
      return i18n::tr(
          link.kind == NetworkConnectivity::Wired          ? "island.network.ethernet"
              : link.kind == NetworkConnectivity::Cellular ? "island.network.cellular"
                                                           : "island.network.wifi"
      );
    }
    std::string icon() const {
      if (link.kind == NetworkConnectivity::Wired)
        return kind == Kind::Lost ? "ethernet-off" : "ethernet";
      if (link.kind == NetworkConnectivity::Cellular)
        return kind == Kind::Lost ? "cell-signal-off" : "cell-signal-5";
      return kind == Kind::Lost ? "wifi-off" : "wifi";
    }
  };

  // Observe the shared network snapshot. Signal, IP, scan and VPN metadata do
  // not define a new physical connection, and startup establishes a quiet baseline.
  class NetworkActivity {
  public:
    using Clock = NetworkNotice::Clock;
    static constexpr auto kConnectDelay = std::chrono::milliseconds(750);
    static constexpr auto kLossDelay = std::chrono::milliseconds(2500);

    void update(const NetworkState& state, Clock::time_point now, std::string_view focusedOutput = {}) {
      // An overlay or incomplete connected snapshot cannot establish a physical link.
      if (state.connected && state.kind == NetworkConnectivity::Unknown) {
        m_pending.reset();
        return;
      }
      advance(now);
      std::optional<NetworkLink> link;
      if (state.connected && !state.resolving)
        link = NetworkLink{state.kind, state.kind == NetworkConnectivity::Wireless ? state.ssid : ""};
      if (!m_initialized) {
        m_initialized = true;
        m_stable = link;
        return;
      }
      if (matches(link, m_stable)) {
        enrich(m_stable, link);
        if (m_notice && link && m_notice->link.matches(*link) && !link->ssid.empty())
          m_notice->link.ssid = link->ssid;
        m_pending.reset();
        return;
      }
      // Hide obsolete feedback immediately, even while the new state is settling.
      m_notice.reset();
      if (m_pending && matches(m_pending->link, link)) {
        enrich(m_pending->link, link);
        return;
      }
      m_pending = Pending{link, now + (link ? kConnectDelay : kLossDelay), PreviewTarget{std::string(focusedOutput)}};
    }

    void advance(Clock::time_point now) {
      if (!m_pending || now < m_pending->deadline)
        return;
      const auto pending = *m_pending;
      if (pending.link) {
        const auto kind =
            m_lost && m_lost->matches(*pending.link) ? NetworkNotice::Kind::Restored : NetworkNotice::Kind::Connected;
        m_notice = NetworkNotice{kind, *pending.link, pending.deadline, ++m_serial, pending.target};
        m_lost.reset();
      } else if (m_stable) {
        m_lost = m_stable;
        m_notice = NetworkNotice{NetworkNotice::Kind::Lost, *m_stable, pending.deadline, ++m_serial, pending.target};
      }
      m_stable = pending.link;
      m_pending.reset();
    }

    std::optional<Clock::time_point> nextChange() const {
      return m_pending ? std::optional{m_pending->deadline} : std::nullopt;
    }
    std::optional<Clock::time_point> nextExpiry(Clock::time_point now, int seconds) const {
      if (m_notice && now < m_notice->started + std::chrono::seconds(seconds))
        return m_notice->started + std::chrono::seconds(seconds);
      return std::nullopt;
    }
    std::optional<NetworkNotice>
    preview(Clock::time_point now, int seconds, std::string_view setting = "all", std::string_view output = {}) const {
      return nextExpiry(now, seconds) && m_notice->target.matches(setting, output) ? m_notice : std::nullopt;
    }
    void dismiss(std::uint64_t serial) {
      if (m_notice && m_notice->serial == serial)
        m_notice.reset();
    }
    void reconcileOutputs(const std::vector<std::string>& available, std::string_view focused) {
      if (m_pending)
        m_pending->target.reconcile(available, focused);
      if (m_notice)
        m_notice->target.reconcile(available, focused);
    }

  private:
    static bool matches(const std::optional<NetworkLink>& a, const std::optional<NetworkLink>& b) {
      return a && b ? a->matches(*b) : !a && !b;
    }
    static void enrich(std::optional<NetworkLink>& target, const std::optional<NetworkLink>& value) {
      if (target && value && !value->ssid.empty())
        target->ssid = value->ssid;
    }
    struct Pending {
      std::optional<NetworkLink> link;
      Clock::time_point deadline;
      PreviewTarget target;
    };
    bool m_initialized = false;
    std::optional<NetworkLink> m_stable, m_lost;
    std::optional<Pending> m_pending;
    std::optional<NetworkNotice> m_notice;
    std::uint64_t m_serial = 0;
  };
} // namespace island

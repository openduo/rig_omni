#pragma once

#include <atomic>

namespace ambient {

/*
 * Arm microlink_rebind only after a real Wi-Fi loss, not on every GOT_IP.
 *
 * DHCP renew also emits IP_EVENT_STA_GOT_IP. Rebinding then would tear down
 * healthy sockets and stall the VPN for several seconds. A reconnect is
 * DISCONNECTED then GOT_IP; a renew is GOT_IP alone.
 */
struct WifiRebindGate {
    void OnDisconnected() { lost_.store(true, std::memory_order_release); }

    bool OnGotIp() {
        if (!lost_.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }
        pending_.store(true, std::memory_order_release);
        return true;
    }

    bool Pending() const { return pending_.load(std::memory_order_acquire); }

    bool Consume() { return pending_.exchange(false, std::memory_order_acq_rel); }

private:
    std::atomic<bool> lost_{false};
    std::atomic<bool> pending_{false};
};

}  // namespace ambient

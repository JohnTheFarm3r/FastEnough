// Entry points used when the server runs inside the Android companion app.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct AfmHostHooks {
    // Checks a CMD_HELLO request. Returns false to reject the client. On success
    // `reply` holds the JSON answer and `useRoot` asks for a root helper handoff.
    std::function<bool(const std::string& request, std::string& reply, bool& useRoot)> authorize;
    // Runs a companion-only command such as listing or installing apps.
    std::function<bool(uint32_t cmd, const std::string& payload, std::string& reply)> appCommand;
    // Returns a connected socket to the root helper, or -1 when root is unavailable.
    std::function<int()> rootHelperSocket;
    // Reports the number of connected clients.
    std::function<void(int count)> clientsChanged;
    // Reports whether the listening socket was opened.
    std::function<void(bool listening, int error)> listening;
};

// Listens on `port` and serves clients until afm_host_stop() is called.
int afm_host_run(int port, const AfmHostHooks& hooks);
void afm_host_stop();

// Sends a client socket to the root helper over a unix socket.
bool afm_send_fd(int channel, int fd);

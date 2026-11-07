#include "companion.h"
#include "protocol.h"
#include "activity_log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#pragma comment(lib, "iphlpapi.lib")

namespace companion {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kOnlineWindow = std::chrono::seconds(12);
constexpr const char* kSerialPrefix = "fe-app-";
constexpr DWORD kHelloTimeoutMs = 100000;

struct Address {
    std::string kind;
    Clock::time_point lastSeen{};
};

struct Device {
    std::string id;
    std::string name;
    std::string model;
    std::string token;
    std::string staging;
    std::string rootError;
    std::string lastError;
    int port = AFM_PORT;
    bool storage = true;
    bool saved = false;
    bool wantRoot = false;
    Clock::time_point lastSeen{};
    std::map<std::string, Address> addresses;
    std::vector<std::string> savedAddresses;
};

struct Link {
    std::string adbIdentity;
    bool useIp = false;
};

struct Adapter {
    uint32_t ip = 0;   // host byte order
    uint32_t mask = 0; // host byte order
};

std::mutex g_mutex;
std::map<std::string, Device> g_devices;
std::map<std::string, std::string> g_endpoints; // "host:port" -> device id
std::vector<std::string> g_manualAddresses;
std::map<std::string, Link> g_links; // companion device id -> ADB link
std::string g_clientId;
std::string g_clientName;
std::atomic<uint64_t> g_generation{1};
std::atomic<bool> g_running{false};
std::thread g_thread;
std::mutex g_wakeMutex;
std::condition_variable g_wake;
bool g_loaded = false;

std::filesystem::path storePath() {
    PWSTR folder = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &folder))) {
        path = std::filesystem::path(folder) / L"FastEnough";
        CoTaskMemFree(folder);
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path / L"companion.json";
}

std::string randomHex(size_t bytes) {
    std::random_device device;
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned value = device() & 0xff;
        out += digits[value >> 4];
        out += digits[value & 15];
    }
    return out;
}

std::string computerName() {
    wchar_t name[256];
    DWORD size = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &size)) return "Windows PC";
    int length = WideCharToMultiByte(CP_UTF8, 0, name, (int)size, nullptr, 0, nullptr, nullptr);
    std::string out(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, (int)size, out.data(), length, nullptr, nullptr);
    return out;
}

// Callers hold g_mutex.
void saveLocked() {
    nlohmann::json root;
    root["client"] = g_clientId;
    root["manual"] = g_manualAddresses;
    auto& list = root["devices"] = nlohmann::json::array();
    for (const auto& [id, device] : g_devices) {
        if (!device.saved) continue;
        list.push_back({{"id", id}, {"name", device.name}, {"model", device.model}, {"token", device.token},
                        {"port", device.port},
                        {"addresses", device.savedAddresses}});
    }
    auto& links = root["links"] = nlohmann::json::array();
    for (const auto& [id, link] : g_links)
        links.push_back({{"id", id}, {"adb", link.adbIdentity}, {"ip", link.useIp}});
    try {
        std::ofstream file(storePath(), std::ios::binary | std::ios::trunc);
        file << root.dump(2);
    } catch (...) {
        LOG_WARN("Companion", "Could not save companion devices");
    }
}

void loadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    g_clientName = computerName();
    try {
        std::ifstream file(storePath(), std::ios::binary);
        if (file) {
            nlohmann::json root = nlohmann::json::parse(file, nullptr, false);
            if (root.is_object()) {
                g_clientId = root.value("client", "");
                if (root.contains("manual") && root["manual"].is_array())
                    for (const auto& value : root["manual"]) if (value.is_string()) g_manualAddresses.push_back(value);
                if (root.contains("devices") && root["devices"].is_array()) {
                    for (const auto& entry : root["devices"]) {
                        Device device;
                        device.id = entry.value("id", "");
                        if (device.id.empty()) continue;
                        device.name = entry.value("name", "");
                        device.model = entry.value("model", "");
                        device.token = entry.value("token", "");
                        device.port = entry.value("port", AFM_PORT);
                        device.saved = true;
                        if (entry.contains("addresses") && entry["addresses"].is_array())
                            for (const auto& value : entry["addresses"])
                                if (value.is_string()) device.savedAddresses.push_back(value);
                        g_devices[device.id] = std::move(device);
                    }
                }
                if (root.contains("links") && root["links"].is_array()) {
                    for (const auto& entry : root["links"]) {
                        if (!entry.is_object()) continue;
                        std::string id = entry.value("id", "");
                        Link link;
                        link.adbIdentity = entry.value("adb", "");
                        link.useIp = entry.value("ip", false);
                        if (!id.empty() && !link.adbIdentity.empty()) g_links[id] = std::move(link);
                    }
                }
            }
        }
    } catch (...) {
        LOG_WARN("Companion", "Could not read saved companion devices");
    }
    if (g_clientId.empty()) {
        g_clientId = randomHex(16);
        saveLocked();
    }
}

bool onlineLocked(const Device& device, Clock::time_point now) {
    return now - device.lastSeen < kOnlineWindow;
}

std::vector<CompanionEndpoint> endpointsLocked(const Device& device, Clock::time_point now) {
    std::vector<CompanionEndpoint> result;
    auto rank = [](const std::string& kind) {
        if (kind == "usb") return 0;
        if (kind == "ethernet") return 1;
        return 2;
    };
    for (const auto& [ip, address] : device.addresses)
        if (now - address.lastSeen < kOnlineWindow) result.push_back({ip, device.port, address.kind});
    std::stable_sort(result.begin(), result.end(),
        [&](const CompanionEndpoint& a, const CompanionEndpoint& b) { return rank(a.kind) < rank(b.kind); });
    return result;
}

void registerEndpointLocked(const std::string& host, int port, const std::string& id) {
    g_endpoints[host + ":" + std::to_string(port)] = id;
}

std::vector<Adapter> localAdapters() {
    std::vector<Adapter> result;
    ULONG size = 16 * 1024;
    std::vector<char> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr, (PIP_ADAPTER_ADDRESSES)buffer.data(), &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_INET, flags, nullptr, (PIP_ADAPTER_ADDRESSES)buffer.data(), &size);
    }
    if (status != NO_ERROR) return result;
    for (auto* adapter = (PIP_ADAPTER_ADDRESSES)buffer.data(); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            auto* addr = (sockaddr_in*)unicast->Address.lpSockaddr;
            if (addr->sin_family != AF_INET) continue;
            uint32_t ip = ntohl(addr->sin_addr.s_addr);
            if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) continue; // loopback, link-local
            int prefix = unicast->OnLinkPrefixLength;
            if (prefix <= 0 || prefix > 32) continue;
            uint32_t mask = prefix == 32 ? 0xffffffffu : ~((1u << (32 - prefix)) - 1);
            result.push_back({ip, mask});
        }
    }
    return result;
}

bool onLocalSubnet(const std::vector<Adapter>& adapters, uint32_t ip) {
    for (const auto& adapter : adapters)
        if ((adapter.ip & adapter.mask) == (ip & adapter.mask) && adapter.ip != ip) return true;
    return false;
}

std::string ipText(uint32_t hostOrder) {
    in_addr addr{};
    addr.s_addr = htonl(hostOrder);
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr, text, sizeof(text));
    return text;
}

bool parseIp(const std::string& text, uint32_t& out) {
    in_addr addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1) return false;
    out = ntohl(addr.s_addr);
    return true;
}

void handleReply(const std::string& text, const std::string& source, const std::vector<Adapter>& adapters) {
    nlohmann::json reply = nlohmann::json::parse(text, nullptr, false);
    if (!reply.is_object() || reply.value("type", "") != "afm-device") return;
    const std::string id = reply.value("id", "");
    if (id.empty()) return;
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& device = g_devices[id];
    const bool wasOnline = !device.id.empty() && onlineLocked(device, now);
    const auto before = endpointsLocked(device, now).size();
    device.id = id;
    device.name = reply.value("name", device.name);
    device.model = reply.value("model", device.model);
    device.port = reply.value("port", AFM_PORT);
    device.storage = reply.value("storage", true);
    device.lastSeen = now;
    std::string sourceKind = "wifi";
    if (reply.contains("addresses") && reply["addresses"].is_array()) {
        for (const auto& entry : reply["addresses"]) {
            const std::string ip = entry.value("ip", "");
            const std::string kind = entry.value("kind", "wifi");
            uint32_t value = 0;
            if (ip == source) sourceKind = kind;
            if (ip.empty() || !parseIp(ip, value) || !onLocalSubnet(adapters, value)) continue;
            device.addresses[ip] = {kind, now};
            registerEndpointLocked(ip, device.port, id);
        }
    }
    device.addresses[source] = {sourceKind, now};
    registerEndpointLocked(source, device.port, id);
    if (device.saved) {
        auto& saved = device.savedAddresses;
        if (std::find(saved.begin(), saved.end(), source) == saved.end()) {
            saved.insert(saved.begin(), source);
            if (saved.size() > 4) saved.resize(4);
            saveLocked();
        }
    }
    if (!wasOnline || before != endpointsLocked(device, now).size()) ++g_generation;
}

void discoveryLoop() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;
    BOOL enable = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&enable, sizeof(enable));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    bind(sock, (sockaddr*)&local, sizeof(local));
    DWORD timeout = 250;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    const std::string probe = nlohmann::json({{"type", "afm-discover"}, {"version", 1}}).dump();
    auto sendTo = [&](uint32_t ip) {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(AFM_DISCOVERY_PORT);
        target.sin_addr.s_addr = htonl(ip);
        sendto(sock, probe.data(), (int)probe.size(), 0, (sockaddr*)&target, sizeof(target));
    };

    const auto started = Clock::now();
    Clock::time_point lastProbe{}, lastSweep{}, lastAdapters{}, lastStatus{};
    std::vector<Adapter> adapters;
    char buffer[4096];
    while (g_running) {
        const auto now = Clock::now();
        if (now - lastAdapters > std::chrono::seconds(4)) {
            adapters = localAdapters();
            lastAdapters = now;
        }
        if (now - lastProbe > std::chrono::seconds(2)) {
            lastProbe = now;
            for (const auto& adapter : adapters) sendTo(adapter.ip | ~adapter.mask);
            std::vector<std::string> known;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                known = g_manualAddresses;
                for (const auto& [id, device] : g_devices) {
                    for (const auto& ip : device.savedAddresses) known.push_back(ip);
                    for (const auto& [ip, address] : device.addresses) known.push_back(ip);
                }
            }
            for (const auto& host : known) {
                uint32_t ip = 0;
                if (parseIp(host, ip)) sendTo(ip);
            }
        }
        // Some phones ignore broadcasts while idle, so also probe each address on small subnets.
        const auto sweepInterval = now - started < std::chrono::seconds(60) ? std::chrono::seconds(6) : std::chrono::seconds(20);
        if (now - lastSweep > sweepInterval) {
            lastSweep = now;
            for (const auto& adapter : adapters) {
                if (~adapter.mask > 1023) continue;
                const uint32_t network = adapter.ip & adapter.mask;
                for (uint32_t host = 1; host < ~adapter.mask; ++host)
                    if ((network | host) != adapter.ip) sendTo(network | host);
            }
        }
        sockaddr_in from{};
        int fromLength = sizeof(from);
        int received = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&from, &fromLength);
        if (received > 0) {
            char source[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, source, sizeof(source));
            handleReply(std::string(buffer, received), source, adapters);
        }
        if (now - lastStatus > std::chrono::seconds(2)) {
            lastStatus = now;
            // Devices that stop answering go offline after a short grace period.
            std::lock_guard<std::mutex> lock(g_mutex);
            static std::set<std::string> lastOnline;
            std::set<std::string> online;
            for (const auto& [id, device] : g_devices)
                if (onlineLocked(device, now)) online.insert(id);
            if (online != lastOnline) {
                lastOnline = online;
                ++g_generation;
            }
        }
    }
    closesocket(sock);
}

bool sendMessage(SOCKET sock, uint32_t cmd, const std::string& payload) {
    MsgHeader header{cmd, (uint32_t)payload.size()};
    std::string data((const char*)&header, sizeof(header));
    data += payload;
    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
        int sent = send(sock, p, (int)left, 0);
        if (sent <= 0) return false;
        p += sent;
        left -= (size_t)sent;
    }
    return true;
}

bool recvExact(SOCKET sock, char* out, size_t length) {
    while (length > 0) {
        int got = recv(sock, out, (int)length, 0);
        if (got <= 0) return false;
        out += got;
        length -= (size_t)got;
    }
    return true;
}

bool recvMessage(SOCKET sock, MsgHeader& header, std::string& payload) {
    if (!recvExact(sock, (char*)&header, sizeof(header))) return false;
    if (header.length > 64u * 1024 * 1024) return false;
    payload.resize(header.length);
    return header.length == 0 || recvExact(sock, payload.data(), header.length);
}

SOCKET openSocket(const std::string& host, int port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return sock;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(sock); return INVALID_SOCKET; }
        fd_set writable, failed;
        FD_ZERO(&writable); FD_SET(sock, &writable);
        FD_ZERO(&failed); FD_SET(sock, &failed);
        timeval tv{4, 0};
        if (select(0, nullptr, &writable, &failed, &tv) <= 0 || FD_ISSET(sock, &failed)) {
            closesocket(sock);
            return INVALID_SOCKET;
        }
    }
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
    BOOL noDelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
    return sock;
}

// Sends CMD_HELLO and records the reply. Returns false when the phone refused.
bool hello(SOCKET sock, const std::string& deviceId, bool root, bool allowPrompt, std::string& error) {
    nlohmann::json request;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        loadLocked();
        auto found = g_devices.find(deviceId);
        request = {{"client", g_clientId}, {"name", g_clientName}, {"version", 1}, {"root", root},
                   {"token", found != g_devices.end() ? found->second.token : ""},
                   {"noPrompt", !allowPrompt}};
    }
    DWORD timeout = kHelloTimeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    MsgHeader header{};
    std::string payload;
    bool ok = sendMessage(sock, CMD_HELLO, request.dump()) && recvMessage(sock, header, payload);
    timeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    if (!ok) {
        error = "The phone did not answer. Check the companion app on the phone.";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& device = g_devices[deviceId];
    device.id = deviceId;
    if (header.cmd != RSP_OK) {
        error = payload.empty() ? "The phone refused the connection" : payload;
        device.lastError = error;
        if (error.find("not paired") != std::string::npos && !device.token.empty()) {
            device.token.clear();
            saveLocked();
            ++g_generation;
        }
        return false;
    }
    nlohmann::json reply = nlohmann::json::parse(payload, nullptr, false);
    if (!reply.is_object()) {
        error = "The phone sent an invalid reply";
        return false;
    }
    const std::string token = reply.value("token", "");
    const bool changed = token != device.token || !device.saved;
    device.token = token;
    device.name = reply.value("name", device.name);
    device.model = reply.value("model", device.model);
    device.storage = reply.value("storage", true);
    device.staging = reply.value("staging", "");
    device.rootError = root && !reply.value("root", false) ? reply.value("rootError", "Root access was not granted") : "";
    device.lastError.clear();
    if (changed) {
        device.saved = true;
        for (const auto& [ip, address] : device.addresses)
            if (std::find(device.savedAddresses.begin(), device.savedAddresses.end(), ip) == device.savedAddresses.end())
                device.savedAddresses.push_back(ip);
        if (device.savedAddresses.size() > 4) device.savedAddresses.resize(4);
        saveLocked();
        ++g_generation;
    }
    return true;
}

} // namespace

bool isSerial(const std::string& serial) {
    return serial.rfind(kSerialPrefix, 0) == 0;
}

std::string serialFor(const std::string& deviceId) {
    return kSerialPrefix + deviceId;
}

std::string idFromSerial(const std::string& serial) {
    return isSerial(serial) ? serial.substr(strlen(kSerialPrefix)) : std::string();
}

void start() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        loadLocked();
    }
    if (g_running.exchange(true)) return;
    g_thread = std::thread(discoveryLoop);
}

void stop() {
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
}

uint64_t generation() {
    return g_generation.load();
}

std::vector<CompanionDeviceView> devices() {
    std::vector<CompanionDeviceView> result;
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    for (const auto& [id, device] : g_devices) {
        if (device.id.empty()) continue;
        CompanionDeviceView view;
        view.id = id;
        view.serial = serialFor(id);
        view.name = device.name.empty() ? device.model : device.name;
        view.model = device.model;
        view.paired = !device.token.empty();
        view.online = onlineLocked(device, now);
        view.storageGranted = device.storage;
        view.saved = device.saved;
        view.endpoints = endpointsLocked(device, now);
        view.lastError = device.lastError;
        if (!view.online && !view.saved) continue;
        result.push_back(std::move(view));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.online != b.online) return a.online;
        return a.name < b.name;
    });
    return result;
}

bool find(const std::string& deviceId, CompanionDeviceView& out) {
    for (auto& view : devices())
        if (view.id == deviceId) { out = std::move(view); return true; }
    return false;
}

std::vector<DeviceInfo> deviceInfos() {
    std::vector<DeviceInfo> result;
    for (const auto& view : devices()) {
        if (!view.online) continue;
        DeviceInfo info;
        info.serial = view.serial;
        info.model = view.name;
        info.state = view.paired ? "device" : "unpaired";
        info.transportType = AdbTransportType::Network;
        for (const auto& endpoint : view.endpoints)
            if (endpoint.isUsb()) { info.transportType = AdbTransportType::Usb; break; }
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<CompanionEndpoint> endpointsFor(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_devices.find(deviceId);
    if (found == g_devices.end()) return {};
    return endpointsLocked(found->second, Clock::now());
}

bool pair(const std::string& deviceId, std::string& error) {
    auto endpoints = endpointsFor(deviceId);
    if (endpoints.empty()) {
        error = "The phone is not reachable. Open the companion app and check the WiFi or USB connection.";
        return false;
    }
    for (const auto& endpoint : endpoints) {
        SOCKET sock = openSocket(endpoint.host, endpoint.port);
        if (sock == INVALID_SOCKET) continue;
        bool ok = hello(sock, deviceId, false, true, error);
        closesocket(sock);
        if (ok) {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++g_generation;
            LOG_INFO("Companion", "Paired with " + deviceId + " via " + endpoint.host);
            return true;
        }
        if (!error.empty() && error.find("did not answer") == std::string::npos) return false;
    }
    if (error.empty()) error = "Could not reach the phone";
    return false;
}

void forget(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_devices.find(deviceId);
    if (found == g_devices.end()) return;
    LOG_INFO("Companion", "Forgot " + deviceId);
    found->second.token.clear();
    found->second.saved = false;
    found->second.savedAddresses.clear();
    auto link = g_links.find(deviceId);
    if (link != g_links.end()) link->second.useIp = false;
    saveLocked();
    ++g_generation;
}

void addAddress(const std::string& host) {
    uint32_t ip = 0;
    if (!parseIp(host, ip)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (std::find(g_manualAddresses.begin(), g_manualAddresses.end(), host) == g_manualAddresses.end()) {
        g_manualAddresses.push_back(host);
        saveLocked();
    }
}

void setWantRoot(const std::string& deviceId, bool root) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices[deviceId].wantRoot = root;
}

std::string rootError(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_devices.find(deviceId);
    return found == g_devices.end() ? std::string() : found->second.rootError;
}

bool isEndpoint(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_endpoints.count(host + ":" + std::to_string(port)) != 0;
}

bool authorizeSocket(uintptr_t socket, const std::string& host, int port, std::string& error) {
    std::string deviceId;
    bool root = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto found = g_endpoints.find(host + ":" + std::to_string(port));
        if (found == g_endpoints.end()) return true;
        deviceId = found->second;
        root = g_devices[deviceId].wantRoot;
    }
    return hello((SOCKET)socket, deviceId, root, false, error);
}

bool request(const std::string& deviceId, uint32_t command, const std::string& payload,
             std::string& reply, uint32_t timeoutMs) {
    for (const auto& endpoint : endpointsFor(deviceId)) {
        SOCKET sock = openSocket(endpoint.host, endpoint.port);
        if (sock == INVALID_SOCKET) continue;
        std::string error;
        if (!hello(sock, deviceId, false, false, error)) {
            closesocket(sock);
            reply = error;
            return false;
        }
        DWORD timeout = timeoutMs;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        MsgHeader header{};
        bool ok = sendMessage(sock, command, payload) && recvMessage(sock, header, reply);
        closesocket(sock);
        if (!ok) {
            reply = "The phone did not answer in time";
            return false;
        }
        return header.cmd == RSP_OK;
    }
    reply = "The phone is not reachable";
    return false;
}

std::string stagingDir(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_devices.find(deviceId);
    return found == g_devices.end() ? std::string() : found->second.staging;
}

void linkAdb(const std::string& deviceId, const std::string& adbIdentity) {
    if (deviceId.empty() || adbIdentity.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    auto found = g_links.find(deviceId);
    if (found != g_links.end() && found->second.adbIdentity == adbIdentity) return;
    bool useIp = found != g_links.end() && found->second.useIp;
    // The companion app gets a new id when it is reinstalled.
    for (auto it = g_links.begin(); it != g_links.end();) {
        if (it->first != deviceId && it->second.adbIdentity == adbIdentity) {
            useIp = useIp || it->second.useIp;
            it = g_links.erase(it);
        } else {
            ++it;
        }
    }
    g_links[deviceId] = {adbIdentity, useIp};
    saveLocked();
    ++g_generation;
    LOG_INFO("Companion", "Linked " + deviceId + " to ADB device " + adbIdentity);
}

std::string linkedAdb(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    auto found = g_links.find(deviceId);
    return found == g_links.end() ? std::string() : found->second.adbIdentity;
}

std::string deviceForAdb(const std::string& adbIdentity) {
    if (adbIdentity.empty()) return {};
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    for (const auto& [id, link] : g_links)
        if (link.adbIdentity == adbIdentity) return id;
    return {};
}

std::vector<std::pair<std::string, std::string>> links() {
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& [id, link] : g_links) result.emplace_back(id, link.adbIdentity);
    return result;
}

bool usesIp(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    auto found = g_links.find(deviceId);
    return found != g_links.end() && found->second.useIp;
}

void setUsesIp(const std::string& deviceId, bool useIp) {
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    auto found = g_links.find(deviceId);
    if (found == g_links.end() || found->second.useIp == useIp) return;
    found->second.useIp = useIp;
    saveLocked();
    ++g_generation;
}

void touch() {
    ++g_generation;
}

} // namespace companion

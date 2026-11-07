// Devices that run the Fast Enough companion app instead of USB debugging.
// They are found over WiFi and USB tethering through UDP discovery, and appear
// to the rest of the app as devices with a "fe-app-" serial.
#pragma once

#include "adb_discovery.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct CompanionEndpoint {
    std::string host;
    int port = 0;
    std::string kind; // "usb", "wifi" or "ethernet"
    bool isUsb() const { return kind == "usb"; }
};

struct CompanionDeviceView {
    std::string id;
    std::string serial;
    std::string name;
    std::string model;
    bool paired = false;
    bool online = false;
    bool storageGranted = true;
    bool saved = false;
    std::vector<CompanionEndpoint> endpoints;
    std::string lastError;
};

namespace companion {

bool isSerial(const std::string& serial);
std::string serialFor(const std::string& deviceId);
std::string idFromSerial(const std::string& serial);

// Starts discovery. Safe to call more than once.
void start();
void stop();

// Increments whenever the set of reachable devices changes.
uint64_t generation();

// Reachable devices as entries for the main device list. Paired devices are
// "device", unpaired ones are "unpaired".
std::vector<DeviceInfo> deviceInfos();
std::vector<CompanionDeviceView> devices();
bool find(const std::string& deviceId, CompanionDeviceView& out);

// Best endpoints first: USB tethering, then WiFi.
std::vector<CompanionEndpoint> endpointsFor(const std::string& deviceId);

// Asks the phone to allow this computer. Blocks until the user answers.
bool pair(const std::string& deviceId, std::string& error);
void forget(const std::string& deviceId);
// Adds a phone by address, for networks that block discovery.
void addAddress(const std::string& host);

// Selects whether new connections to this device ask for root.
void setWantRoot(const std::string& deviceId, bool root);
std::string rootError(const std::string& deviceId);

// Called by DeviceClient for every new socket. Sends CMD_HELLO when the
// endpoint belongs to a companion device. Returns false if it was rejected.
bool authorizeSocket(uintptr_t socket, const std::string& host, int port, std::string& error);
bool isEndpoint(const std::string& host, int port);

// Runs one companion request (app list, install, uninstall) on a fresh,
// non-root connection and returns the reply text.
bool request(const std::string& deviceId, uint32_t command, const std::string& payload,
             std::string& reply, uint32_t timeoutMs = 600000);
std::string stagingDir(const std::string& deviceId);

// A phone that has USB debugging on and runs the companion app shows up twice.
// The ADB identity of that phone is linked to its companion device id, and
// the user chooses which of the two connections is used.
void linkAdb(const std::string& deviceId, const std::string& adbIdentity);
std::string linkedAdb(const std::string& deviceId);
std::string deviceForAdb(const std::string& adbIdentity);
// Pairs of companion device id and ADB identity.
std::vector<std::pair<std::string, std::string>> links();
bool usesIp(const std::string& deviceId);
void setUsesIp(const std::string& deviceId, bool useIp);

// Makes the device list refresh.
void touch();

} // namespace companion

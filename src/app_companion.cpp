#include "app.h"
#include "companion.h"
#include "ui_panels.h"
#include "device_dokan.h"


namespace {

std::string companionStatus(const CompanionDeviceView& device) {
    if (!device.online) return "Offline. Open the companion app on the phone.";
    if (!companion::linkedAdb(device.id).empty() && !companion::usesIp(device.id))
        return "Also connected over USB debugging. Using ADB.";
    std::vector<std::string> links;
    for (const auto& endpoint : device.endpoints) {
        std::string name = endpoint.kind == "usb" ? "USB tethering" :
            endpoint.kind == "ethernet" ? "Ethernet" : "WiFi " + endpoint.host;
        if (std::find(links.begin(), links.end(), name) == links.end()) links.push_back(name);
    }
    std::string joined;
    for (const auto& link : links) joined += (joined.empty() ? "" : ", ") + link;
    if (!device.paired) return "Not paired. " + joined;
    if (!device.storageGranted) return "Allow All files access in the companion app. " + joined;
    return "Ready over " + joined;
}

// Reads the companion device id from "Row: 0 id=<value>".
std::string companionIdFromQuery(const std::string& output) {
    auto start = output.find("id=");
    if (start == std::string::npos) return {};
    start += 3;
    auto end = output.find_first_of(" ,\t\r\n", start);
    std::string id = output.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (id.empty() || id == "NULL") return {};
    for (char ch : id)
        if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_') return {};
    return id;
}

} // namespace

void App::linkCompanionDevices(const std::vector<DeviceInfo>& adbDevices) {
    std::set<std::string> unlinked;
    for (const auto& view : companion::devices())
        if (view.online && companion::linkedAdb(view.id).empty()) unlinked.insert(view.id);
    if (unlinked != m_unlinkedCompanions) {
        // A phone with the companion app just appeared, so ask every ADB device again.
        m_unlinkedCompanions = unlinked;
        m_companionLinkAttempts.clear();
    }
    if (!unlinked.empty()) probeCompanionLinks(adbDevices);

    // One session per phone, even when it was opened over both connections
    // before they were linked. The primary session wins if either is primary.
    bool changed = false;
    for (const auto& [id, identity] : companion::links()) {
        const std::string appSerial = companion::serialFor(id);
        const int appSlot = m_deviceSessions.find(appSerial);
        if (appSlot < 0) continue;
        const int adbSlot = m_deviceSessions.find(identity);
        if (adbSlot >= 0) {
            const int retired = appSlot == 0 ? adbSlot : appSlot;
            auto& session = deviceSession(retired);
            DeviceMountManager::instance(retired).unmount();
            session.client.stopServer();
            session.connected = false;
            session.serial.clear();
            m_deviceSessions.release(retired == appSlot ? appSerial : identity);
            LOG_INFO("Companion", "Merged the sessions of " + identity + " and " + appSerial);
            changed = true;
        }
        changed = m_deviceSessions.rename(appSerial, identity) || changed;
    }
    if (changed) {
        UiMessage message;
        message.saveDeviceSessions = true;
        postUiMessage(std::move(message));
    }
}

void App::probeCompanionLinks(const std::vector<DeviceInfo>& adbDevices) {
    struct Probe { std::string serial, identity, id; };
    std::vector<Probe> pending;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& device : adbDevices) {
        if (device.state != "device" || companion::isSerial(device.serial)) continue;
        std::string identity = deviceIdentity(device.serial);
        if (isWirelessAdbSerial(device.serial) && identity == device.serial) continue;
        if (!companion::deviceForAdb(identity).empty()) continue;
        auto& last = m_companionLinkAttempts[device.serial];
        if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::seconds(60)) continue;
        last = now;
        pending.push_back({device.serial, std::move(identity), {}});
    }
    const std::string adbPath = m_device.getAdbPath();
    runDeviceTasks(pending.size(), [&](size_t index) {
        if (m_shutdownPoll) return;
        DeviceClient probe;
        probe.setOwnsServer(false);
        probe.setAdbPath(adbPath);
        auto result = probe.runAdbCommandResult("-s " + pending[index].serial +
            " shell content query --uri content://com.fastenough.companion.info/id", 4000,
            [this]() { return m_shutdownPoll.load(); });
        if (result.succeeded()) pending[index].id = companionIdFromQuery(result.standardOutput);
    });
    for (const auto& probe : pending)
        if (!probe.id.empty()) companion::linkAdb(probe.id, probe.identity);
}

std::vector<DeviceInfo> App::applyConnectionModes(const std::vector<DeviceInfo>& adbDevices) {
    std::vector<DeviceInfo> result;
    std::set<std::string> onlineAdb;
    for (const auto& device : adbDevices) {
        const std::string identity = deviceIdentity(device.serial);
        const std::string id = companion::deviceForAdb(identity);
        if (!id.empty() && companion::usesIp(id)) continue;
        if (device.state == "device") onlineAdb.insert(identity);
        result.push_back(device);
    }
    for (auto& device : companion::deviceInfos()) {
        const std::string id = companion::idFromSerial(device.serial);
        const std::string adbIdentity = companion::linkedAdb(id);
        if (!adbIdentity.empty() && !companion::usesIp(id) && onlineAdb.count(adbIdentity)) continue;
        result.push_back(std::move(device));
    }
    return result;
}

void App::setCompanionConnectionMode(const std::string& deviceId, bool useIp) {
    if (companion::usesIp(deviceId) == useIp) return;
    {
        std::lock_guard<std::mutex> lock(m_batchMutex);
        for (const auto& batch : m_batchQueue) {
            auto state = batch->state.load();
            if (batch->isLocalCopy) continue;
            if (state == BatchState::Preparing || state == BatchState::Running || state == BatchState::Paused ||
                state == BatchState::Verifying || state == BatchState::WaitingConflict) {
                m_statusMessage = "Wait for the current transfer to finish before switching the connection.";
                m_statusTime = std::chrono::steady_clock::now();
                return;
            }
        }
    }
    if (m_connectionModeSwitching.exchange(true)) return;
    const std::string identity = companion::linkedAdb(deviceId);
    postAsync(useIp ? "Switching to the companion app..." : "Switching to ADB...", [this, deviceId, identity, useIp]() {
        struct Done {
            std::atomic<bool>& flag;
            ~Done() { flag = false; }
        } done{m_connectionModeSwitching};
        // The async worker already holds m_deviceWorkMutex, so the poller waits for this.
        for (int slot = 0; slot < deviceSessionCount(); ++slot) {
            auto& session = deviceSession(slot);
            if (session.serial.empty() || deviceIdentity(session.serial) != identity) continue;
            if (companion::isSerial(session.serial) == useIp) continue;
            DeviceMountManager::instance(slot).unmount();
            if (slot == 0) {
                m_extraChannels.clear();
                m_nicChannels.clear();
                m_secondaryChannel.disconnectTcp();
                m_dualChannelAvailable = false;
                m_secondaryChannelType.clear();
                m_activeChannelCount = 1;
            }
            session.client.stopServer();
            session.connected = false;
            LOG_INFO("Companion", "Stopped " + session.serial + " to use " + (useIp ? "the companion app" : "ADB"));
        }
        companion::setUsesIp(deviceId, useIp);
        m_statusMessage = useIp ? "Using the companion app for this phone. ADB is no longer used for it." :
            "Using ADB for this phone.";
        m_statusTime = std::chrono::steady_clock::now();
    });
}

bool App::renderConnectionModeSwitch(const std::string& deviceId, ImVec2 pos, ImVec2 size) {
    const bool useIp = companion::usesIp(deviceId);
    const bool busy = m_connectionModeSwitching;
    const float half = size.x * 0.5f;
    const float rounding = std::min(size.y * 0.5f, 8.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ImGuiCol_Border), rounding);
    bool changed = false;
    ImGui::PushID(deviceId.c_str());
    ImGui::BeginDisabled(busy);
    const char* labels[] = { "ADB", "IP" };
    const char* tips[] = {
        "Use USB debugging (ADB) for this phone",
        "Use the companion app over USB tethering or WiFi. ADB is not used for this phone." };
    for (int option = 0; option < 2; ++option) {
        const bool selected = (option == 1) == useIp;
        ImVec2 a(pos.x + option * half, pos.y);
        ImVec2 b(a.x + half, pos.y + size.y);
        ImGui::SetCursorScreenPos(a);
        const bool clicked = ImGui::InvisibleButton(option ? "##UseIp" : "##UseAdb", ImVec2(half, size.y));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetTooltip("%s", tips[option]);
        if (selected)
            draw->AddRectFilled(ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, b.y - 2),
                ImGui::GetColorU32(ImGuiCol_ButtonActive), std::max(0.0f, rounding - 2));
        else if (hovered)
            draw->AddRectFilled(ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, b.y - 2),
                ImGui::GetColorU32(ImGuiCol_ButtonHovered), std::max(0.0f, rounding - 2));
        ImVec2 text = ImGui::CalcTextSize(labels[option]);
        draw->AddText(ImVec2(a.x + (half - text.x) * 0.5f, a.y + (size.y - text.y) * 0.5f),
            ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled), labels[option]);
        if (clicked && !selected) {
            setCompanionConnectionMode(deviceId, option == 1);
            changed = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(size);
    return changed;
}

void App::pairCompanionDevice(const std::string& serial) {
    if (m_connectionPendingSerial == serial) return;
    m_connectionPendingSerial = serial;
    m_connectionFeedback.clear();
    postAsync("Waiting for approval on the phone...", [this, serial]() {
        UiMessage message;
        message.connectionRequestFinished = true;
        message.connectionSerial = serial;
        message.hasStatus = true;
        std::string error;
        const std::string id = companion::idFromSerial(serial);
        CompanionDeviceView view;
        const std::string name = companion::find(id, view) ? view.name : "the phone";
        try {
            if (companion::pair(id, error)) {
                connectDeviceBySerialNow(serial);
                for (int slot = 0; slot < deviceSessionCount(); ++slot) {
                    auto& session = deviceSession(slot);
                    if (session.connected && session.client.isServerRunning() && session.serial == serial) {
                        message.connectionSlot = slot;
                        break;
                    }
                }
                message.status = message.connectionSlot >= 0 ? "Paired with " + name + "." :
                    "Paired with " + name + ". Connecting...";
            } else {
                message.status = "Could not pair with " + name + ". " + error;
            }
        } catch (const std::exception& failure) {
            message.status = "Pairing failed: " + std::string(failure.what());
        }
        postUiMessage(std::move(message));
    });
}

float App::renderCompanionSection(FilePanel& panel, ImVec2 origin, float width, float y,
                                  const std::function<void(const DeviceInfo&)>& openDevice) {
    ui::PanelStyle theme(m_resolvedLightTheme);
    const float s = theme.s;
    const float padding = 18 * s;
    const float innerWidth = std::max(1.0f, width - 2 * padding);
    const bool wideHeader = innerWidth >= 900 * s;
    const float header = (wideHeader ? 76 : 122) * s;
    const float rowHeight = 56 * s;
    const float rowGap = 8 * s;

    auto devices = companion::devices();
    const int deviceRows = std::max(1, (int)devices.size());
    const float height = header + deviceRows * (rowHeight + rowGap) + padding;
    theme.card(ImVec2(origin.x, y), ImVec2(width, height), ui::rgb(167, 139, 250));
    theme.heading(ImVec2(origin.x + padding, y + padding), wideHeader ? innerWidth - 430 * s : innerWidth,
        ui::Icon::Link, "Companion app", "Connect without USB debugging over WiFi or USB tethering", ui::rgb(167, 139, 250));

    // Manual address for networks that block discovery.
    const float controlsWidth = wideHeader ? 420 * s : innerWidth;
    const float controlsX = origin.x + width - padding - controlsWidth;
    const float controlsY = y + (wideHeader ? 20 : 72) * s;
    theme.search("##CompanionAddress", "Phone IP address", m_companionAddress, sizeof(m_companionAddress),
        ImVec2(controlsX, controlsY), controlsWidth - 112 * s);
    ui::tooltip("Add a phone by its WiFi address if it does not appear automatically");
    if (theme.button("##AddCompanionAddress", "Add", ui::Icon::Plus,
        ImVec2(controlsX + controlsWidth - 100 * s, controlsY), ImVec2(100 * s, 36 * s), theme.blue)) {
        std::string address = m_companionAddress;
        address.erase(0, address.find_first_not_of(" \t"));
        address.erase(address.find_last_not_of(" \t") + 1);
        if (!address.empty()) {
            companion::addAddress(address);
            m_connectionFeedback = "Looking for the companion app at " + address + "...";
            m_companionAddress[0] = '\0';
        }
    }

    float rowY = y + header;
    if (devices.empty()) {
        theme.label(ImVec2(origin.x + padding + 12 * s, rowY + 6 * s), "No phones with the companion app yet",
            innerWidth - 24 * s, theme.text, 14, true);
        theme.label(ImVec2(origin.x + padding + 12 * s, rowY + 30 * s),
            "Install Fast Enough Companion on the phone and keep it on the same WiFi, or turn on USB tethering.",
            innerWidth - 24 * s, theme.muted, 13);
        rowY += rowHeight + rowGap;
    }
    std::string forgetId;
    for (const auto& device : devices) {
        ImGui::PushID(device.id.c_str());
        ImVec2 p(origin.x + padding, rowY);
        theme.box(p, ImVec2(innerWidth, rowHeight), theme.raised, theme.surface, theme.border, 9);
        theme.tile(ui::Icon::Phone, ImVec2(p.x + 12 * s, p.y + (rowHeight - 40 * theme.s) * 0.5f), 40, ui::rgb(167, 139, 250));
        const bool linked = !companion::linkedAdb(device.id).empty();
        const float switchWidth = linked ? 112 * s : 0.0f;
        const float actionsWidth = 236 * s + (linked ? switchWidth + 10 * s : 0.0f);
        const float textWidth = innerWidth - 76 * s - actionsWidth;
        theme.label(ImVec2(p.x + 64 * s, p.y + 9 * s), device.name.empty() ? "Android phone" : device.name,
            textWidth, theme.text, 14, true);
        const bool attention = device.online && (!device.paired || !device.storageGranted);
        theme.label(ImVec2(p.x + 64 * s, p.y + 31 * s), companionStatus(device), textWidth,
            attention ? theme.gold : device.online ? theme.green : theme.muted, 12);

        float buttonX = p.x + innerWidth - actionsWidth;
        if (linked) {
            renderConnectionModeSwitch(device.id, ImVec2(buttonX, p.y + 10 * s), ImVec2(switchWidth, 36 * s));
            buttonX += switchWidth + 10 * s;
        }
        const bool pending = m_connectionPendingSerial == device.serial;
        ImGui::BeginDisabled(!device.online || pending);
        if (!device.paired) {
            if (theme.button("##PairCompanion", pending ? "Waiting..." : "Pair", ui::Icon::Link,
                ImVec2(buttonX, p.y + 10 * s), ImVec2(118 * s, 36 * s), ui::rgb(255, 255, 255), 1))
                openDevice(DeviceInfo{device.serial, device.name, "unpaired"});
            ui::tooltip("Asks the phone to allow this computer");
        } else {
            if (theme.button("##OpenCompanion", pending ? "Connecting" : "Open", ui::Icon::Folder,
                ImVec2(buttonX, p.y + 10 * s), ImVec2(118 * s, 36 * s), ui::rgb(91, 222, 255), 4))
                openDevice(DeviceInfo{device.serial, device.name, "device"});
        }
        ImGui::EndDisabled();
        if (device.saved && theme.button("##ForgetCompanion", "Forget", ui::Icon::Trash,
            ImVec2(buttonX + 126 * s, p.y + 10 * s), ImVec2(104 * s, 36 * s), ui::rgb(255, 143, 151), 3))
            forgetId = device.id;
        ui::tooltip("Removes this phone. You will need to pair it again.");
        ImGui::PopID();
        rowY += rowHeight + rowGap;
    }
    if (!forgetId.empty()) {
        companion::forget(forgetId);
        m_connectionFeedback = "Removed the companion phone. Remove this computer in the phone app as well.";
    }

    (void)panel;
    return y + height + 18 * s;
}

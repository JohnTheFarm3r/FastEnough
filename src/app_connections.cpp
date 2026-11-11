#include "app.h"
#include "ui_panels.h"
#include "companion.h"
#include <algorithm>
#include <cctype>
#include <map>

namespace {
ImVec4 rgb(int r, int g, int b) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

std::string wifiAddress(const SavedWifiDevice& device) {
    return device.wifiIp + ":" + std::to_string(device.port);
}

std::string savedName(const SavedWifiDevice& device) {
    if (!device.model.empty()) return device.model;
    if (!device.serial.empty() && device.serial != wifiAddress(device)) return device.serial;
    return device.wifiIp;
}




}

std::map<std::string, App::AdbTransports> App::onlineAdbTransports(const std::vector<DeviceInfo>& devices) const {
    std::map<std::string, AdbTransports> transports;
    for (const auto& device : devices) {
        if (device.state != "device" || companion::isSerial(device.serial)) continue;
        auto& entry = transports[deviceIdentity(device.serial)];
        (isWirelessAdbSerial(device.serial) ? entry.wifiSerial : entry.usbSerial) = device.serial;
    }
    return transports;
}

void App::connectFromManager(const std::string& serial, bool savedWifi) {
    if (m_connectionPendingSerial == serial) return;
    m_connectionPendingSerial = serial;
    m_connectionFeedback.clear();
    postAsync("Connecting to " + serial + "...", [this, serial, savedWifi]() {
        UiMessage message;
        message.connectionRequestFinished = true;
        message.connectionSerial = serial;
        message.hasStatus = true;
        try {
            std::string connectSerial = serial;
            const std::string identity = deviceIdentity(serial);
            auto readySlot = [&]() {
                for (int slot = 0; slot < deviceSessionCount(); ++slot)
                    if (deviceSession(slot).connected && deviceSession(slot).client.isServerRunning() &&
                        (deviceSession(slot).serial == connectSerial || deviceIdentity(deviceSession(slot).serial) == identity)) return slot;
                return -1;
            };
            bool connected = true;
            message.connectionSlot = readySlot();
            if (savedWifi && message.connectionSlot < 0) {
                for (const auto& saved : m_prefs.savedWifiDevices) {
                    if (saved.serial != identity || !saved.wirelessDebugging || saved.pairingGuid.empty()) continue;
                    auto discovery = m_device.runAdbCommandResult("mdns services", 5000);
                    for (const auto& service : parseAdbMdnsServices(discovery.standardOutput)) {
                        if (service.type == "_adb-tls-connect._tcp" &&
                            (service.name == saved.pairingGuid || service.name == "adb-" + saved.pairingGuid)) {
                            connectSerial = service.address;
                            break;
                        }
                    }
                    break;
                }
                std::string result = m_device.runAdbCommand("connect " + connectSerial, 8000);
                connected = result.find("connected") != std::string::npos && result.find("failed") == std::string::npos;
                if (!connected) message.status = "Connection failed: " + result;
            }
            if (connected) {
                if (message.connectionSlot < 0) connectDeviceBySerialNow(connectSerial);
                message.connectionSlot = readySlot();
                bool ready = message.connectionSlot >= 0;
                message.status = ready ? "Connected to " + serial : "Could not connect to " + serial + ". Check the device and try again.";
                if (savedWifi && ready) {
                    message.connectedWifiAddress = connectSerial;
                    message.connectedWifiName = queryDeviceDisplayName(connectSerial);
                }
            }
        } catch (const std::exception& error) {
            message.status = "Connection failed: " + std::string(error.what());
        } catch (...) {
            message.status = "Connection failed. Please try again.";
        }
        postUiMessage(std::move(message));
    });
}

void App::renderWifiWizard() {
    if (!m_showWifiWizard) {
        if (m_wizardProbeBusy) {
            ++m_wizardProbeGeneration;
            m_wizardProbeBusy = false;
        }
        return;
    }
    ui::PanelStyle theme(m_resolvedLightTheme);
    const float s = theme.s;
    auto* viewport = ImGui::GetMainViewport();
    ImGui::OpenPopup("WiFi Setup Wizard");
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(600 * s, viewport->WorkSize.x - 32 * s),
        std::min(650 * s, viewport->WorkSize.y - 40 * s)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s, 24 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 10 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme.surface);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.muted);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
    if (ImGui::BeginPopupModal("WiFi Setup Wizard", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) {
        const int step = m_wizardStep;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        auto close = [&]() {
            ++m_wizardProbeGeneration;
            m_wizardProbeBusy = false;
            m_showWifiWizard = false;
            if (m_wizardSucceeded) m_wifiBannerDismissed = true;
            ImGui::CloseCurrentPopup();
        };
        theme.tile(ui::Icon::Wifi, origin, 46, theme.green);
        theme.label(ImVec2(origin.x + 64 * s, origin.y - s), "WiFi Setup Wizard", width - 104 * s, theme.text, 24, true);
        theme.label(ImVec2(origin.x + 64 * s, origin.y + 30 * s), "Connect your phone wirelessly", width - 104 * s, theme.muted, 13);
        if (theme.button("##CloseWifiWizard", "", ui::Icon::Close,
            ImVec2(origin.x + width - 32 * s, origin.y), ImVec2(32 * s, 32 * s), theme.muted)) close();
        ui::tooltip(step == 2 ? "Close this window. Setup will continue in the background." : "Close setup wizard");
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close();

        const char* steps[] = { "Choose device", "Check WiFi", "Connect" };
        float stepWidth = (width - 16 * s) / 3;
        for (int i = 0; i < 3; ++i) {
            bool complete = step > i && (i < 2 || m_wizardSucceeded);
            bool active = std::min(step, 2) == i;
            ImVec2 p(origin.x + i * (stepWidth + 8 * s), origin.y + 70 * s);
            const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            ImVec4 activeFill = accent; activeFill.w = 0.12f;
            ImVec4 activeEdge = accent; activeEdge.w = 0.65f;
            theme.box(p, ImVec2(stepWidth, 42 * s), active ? activeFill : theme.background,
                active ? activeFill : theme.background, active ? activeEdge : theme.border, 9);
            auto* stepDraw = ImGui::GetWindowDrawList();
            ImFont* stepFont = ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
            const float labelWidth = stepFont->CalcTextSizeA(13 * s, FLT_MAX, 0, steps[i]).x;
            const float dot = 22 * s;
            const float groupWidth = dot + 10 * s + labelWidth;
            const float startX = p.x + std::max(8 * s, (stepWidth - groupWidth) * 0.5f);
            const ImVec2 dotCenter(startX + dot * 0.5f, p.y + 21 * s);
            ImVec4 dotFill = complete ? theme.green : active ? accent : theme.raised;
            if (complete) dotFill.w = 0.20f;
            stepDraw->AddCircleFilled(dotCenter, dot * 0.5f, theme.color(dotFill));
            if (complete) {
                ui::icon(ui::Icon::Check, ImVec2(dotCenter.x - 7 * s, dotCenter.y - 7 * s), 14 * s, theme.color(theme.green));
            } else {
                const std::string number = std::to_string(i + 1);
                const ImVec2 numberSize = stepFont->CalcTextSizeA(12 * s, FLT_MAX, 0, number.c_str());
                stepDraw->AddText(stepFont, 12 * s, ImVec2(dotCenter.x - numberSize.x * 0.5f, dotCenter.y - numberSize.y * 0.5f),
                    active ? (ui::brightness(accent) > 0.55f ? IM_COL32(10, 15, 24, 255) : IM_COL32(255, 255, 255, 255)) : theme.color(theme.muted),
                    number.c_str());
            }
            theme.label(ImVec2(startX + dot + 10 * s, p.y + (42 * s - 13 * s) * 0.5f - 1 * s), steps[i],
                stepWidth - (startX - p.x) - dot - 14 * s, active ? theme.text : theme.muted, 13, active);
        }

        std::vector<DeviceInfo> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_deviceMutex);
            snapshot = m_devices;
        }
        std::vector<DeviceInfo> devices;
        std::map<std::string, size_t> identities;
        const auto transports = onlineAdbTransports(snapshot);
        for (const auto& device : snapshot) {
            if (device.state != "device") continue;
            auto [it, inserted] = identities.emplace(deviceIdentity(device.serial), devices.size());
            if (inserted) devices.push_back(device);
            else if (isWirelessAdbSerial(devices[it->second].serial) && !isWirelessAdbSerial(device.serial)) devices[it->second] = device;
        }
        auto displayName = [&](const std::string& serial) {
            auto cached = m_deviceDisplayNames.find(serial);
            if (cached != m_deviceDisplayNames.end() && !cached->second.empty()) return cached->second;
            for (const auto& saved : m_prefs.savedWifiDevices)
                if ((serial == saved.serial || serial == wifiAddress(saved)) && !saved.model.empty()) return saved.model;
            for (const auto& device : snapshot)
                if (device.serial == serial && !device.model.empty()) return device.model;
            return serial;
        };
        bool selectionAvailable = false;
        for (const auto& device : devices) {
            if (deviceIdentity(device.serial) == deviceIdentity(m_wizardSerial)) {
                selectionAvailable = true;
                if (step == 0) m_wizardSerial = device.serial;
                break;
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 132 * s));
        float bodyHeight = std::max(80 * s, ImGui::GetContentRegionAvail().y - 66 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##WifiWizardBody", ImVec2(0, bodyHeight));
        ImGui::PopStyleVar();
        float bodyWidth = ImGui::GetContentRegionAvail().x;
        auto paragraph = [&](const std::string& text, ImVec4 color) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::TextColored(color, "%s", text.c_str());
            ImGui::PopTextWrapPos();
        };
        auto heading = [&](const char* title, const char* subtitle) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            theme.label(p, title, bodyWidth, theme.text, 18, true);
            ImGui::Dummy(ImVec2(bodyWidth, 20 * s));
            paragraph(subtitle, theme.muted);
            ImGui::Spacing();
        };
        auto deviceCard = [&](const std::string& serial, bool interactive) {
            ImGui::PushID(serial.c_str());
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool selected = serial == m_wizardSerial;
            if (interactive) {
                if (theme.button("##WizardDevice", "", ui::Icon::None, p, ImVec2(bodyWidth, 66 * s), theme.blue)) {
                    m_wizardSerial = serial;
                    selected = selectionAvailable = true;
                }
                ui::tooltip((displayName(serial) + "\n" + serial).c_str());
                if (selected) theme.box(p, ImVec2(bodyWidth, 66 * s), theme.raised, theme.background, theme.blue, 7);
            } else theme.box(p, ImVec2(bodyWidth, 66 * s), theme.raised, theme.background, theme.border, 7);
            theme.tile(ui::Icon::Phone, ImVec2(p.x + 12 * s, p.y + 13 * s), 40, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            theme.label(ImVec2(p.x + 66 * s, p.y + 13 * s), displayName(serial), bodyWidth - 172 * s, theme.text, 15, true);
            theme.label(ImVec2(p.x + 66 * s, p.y + 39 * s), serial, bodyWidth - 172 * s, theme.muted, 13);
            auto transport = transports.find(deviceIdentity(serial));
            const bool both = transport != transports.end() && transport->second.both();
            bool wifi = both || isWirelessAdbSerial(serial);
            auto ink = wifi ? theme.green : theme.blue;
            const char* badgeLabel = both ? "USB + WiFi" : wifi ? "WiFi" : "USB";
            const ImVec2 badgeSize = theme.pillSize(badgeLabel);
            ImVec2 badge(p.x + bodyWidth - (interactive ? 44 * s : 16 * s) - badgeSize.x, p.y + (66 * s - badgeSize.y) * 0.5f);
            theme.pill(badge, badgeSize, wifi ? ui::Icon::Wifi : ui::Icon::Usb, badgeLabel, ink);
            if (interactive) {
                ImVec2 center(p.x + bodyWidth - 22 * s, p.y + 33 * s);
                const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                ImGui::GetWindowDrawList()->AddCircle(center, 9 * s, theme.color(selected ? accent : theme.border), 24, 1.5f * s);
                if (selected) ImGui::GetWindowDrawList()->AddCircleFilled(center, 5 * s, theme.color(accent));
            }
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(ImVec2(bodyWidth, 66 * s));
            ImGui::PopID();
        };
        auto statusCard = [&](ui::Icon icon, const char* title, const std::string& detail, ImVec4 ink, bool animated) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float detailWidth = std::max(60 * s, bodyWidth - 80 * s);
            float detailHeight = ImGui::CalcTextSize(detail.c_str(), nullptr, false, detailWidth).y;
            float height = std::max(92 * s, detailHeight + 56 * s);
            theme.box(p, ImVec2(bodyWidth, height), theme.raised, theme.background, theme.border, 9);
            if (animated) {
                float angle = (float)ImGui::GetTime() * 4;
                auto* draw = ImGui::GetWindowDrawList();
                draw->PathArcTo(ImVec2(p.x + 32 * s, p.y + 33 * s), 12 * s, angle, angle + 4.6f, 24);
                draw->PathStroke(theme.color(ink), 0, 2 * s);
            } else ui::icon(icon, ImVec2(p.x + 18 * s, p.y + 19 * s), 28 * s, theme.color(ink));
            theme.label(ImVec2(p.x + 64 * s, p.y + 18 * s), title, detailWidth, ink, 16, true);
            ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 64 * s, p.y + 46 * s),
                theme.color(theme.muted), detail.c_str(), nullptr, detailWidth);
            ImGui::Dummy(ImVec2(bodyWidth, height));
        };

        if (step == 0) {
            heading("Choose your device", "Select the phone you want to connect over WiFi.");
            if (devices.empty()) {
                statusCard(ui::Icon::Usb, "Connect a phone to get started", "Available devices will appear here automatically.", theme.blue, false);
                paragraph("1. Connect your phone with a USB data cable.", theme.text);
                paragraph("2. Enable USB debugging in Settings > Developer Options.", theme.text);
                paragraph("3. Unlock your phone and tap Allow when prompted.", theme.text);
                paragraph("To enable Developer Options, tap Build number 7 times in Settings > About phone.", theme.muted);
            } else {
                for (const auto& device : devices) deviceCard(device.serial, true);
                ImGui::Spacing();
                paragraph("Keep USB connected until wireless setup is complete.", theme.muted);
            }
        } else if (step == 1) {
            heading("Check your WiFi connection", "Your phone and computer need to be on the same network.");
            deviceCard(m_wizardSerial, false);
            ImGui::Spacing();
            auto transport = transports.find(deviceIdentity(m_wizardSerial));
            const bool wifiActive = transport != transports.end() && !transport->second.wifiSerial.empty();
            if (m_wizardProbeBusy) statusCard(ui::Icon::Wifi, "Checking your phone...", "Looking for an active WiFi connection.", theme.blue, true);
            else if (!m_wizardWifiIp.empty()) statusCard(ui::Icon::Wifi, wifiActive ? "WiFi is already connected" : "WiFi is ready",
                "Phone address: " + m_wizardWifiIp, theme.green, false);
            else statusCard(ui::Icon::Info, "WiFi needs attention", m_wizardProbeError.empty() ?
                "Connect your phone to WiFi, then choose Check again." : m_wizardProbeError, theme.gold, false);
            ImGui::Spacing();
            paragraph(wifiActive ? "This phone is already reachable over WiFi. The next step saves it for automatic reconnection." :
                "The next step enables wireless access and saves this phone for automatic reconnection.", theme.muted);
        } else if (step == 2) {
            heading("Connecting your phone", "Keep your phone connected while setup finishes.");
            deviceCard(m_wizardSerial, false);
            ImGui::Spacing();
            statusCard(ui::Icon::Wifi, "Setting up wireless access", m_wizardStatus, theme.blue, true);
            ImGui::Spacing();
            paragraph("Setup will continue in the background if you close this window.", theme.muted);
        } else {
            heading(m_wizardSucceeded ? "Your wireless connection is ready" : "Setup could not be completed",
                m_wizardSucceeded ? "Your phone is ready for wireless file transfers." : "Check your connection, then try again.");
            deviceCard(m_wizardSerial, false);
            ImGui::Spacing();
            statusCard(m_wizardSucceeded ? ui::Icon::Check : ui::Icon::Info,
                m_wizardSucceeded ? "Connected over WiFi" : "Connection unsuccessful", m_wizardStatus,
                m_wizardSucceeded ? theme.green : theme.red, false);
            ImGui::Spacing();
            paragraph(m_wizardSucceeded ? "Manage this phone in Saved WiFi devices on the Connections page." :
                "Keep the USB cable connected and check that both devices are on the same WiFi network.", theme.muted);
        }
        ImGui::EndChild();

        ImVec2 footer(origin.x, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - 64 * s);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(footer.x, footer.y - 16 * s), ImVec2(footer.x + width, footer.y - 16 * s), theme.color(theme.border), s);
        bool back = step == 1 || (step == 3 && !m_wizardSucceeded);
        ImGui::SetCursorScreenPos(footer);
        if (ui::button(back ? "Back##WizardBack" : step == 0 ? "Cancel##WizardBack" : "Close##WizardBack",
                back ? ui::Icon::Back : ui::Icon::None, ui::Appearance::Secondary, ImVec2(110 * s, 40 * s))) {
            if (back) { ++m_wizardProbeGeneration; m_wizardProbeBusy = false; m_wizardStep = 0; }
            else close();
        }
        const char* action = step == 0 ? "Next" : step == 1 ? (m_wizardProbeBusy ? "Checking..." : m_wizardWifiIp.empty() ? "Check again" : "Set up WiFi") :
            step == 2 ? "Setting up..." : m_wizardSucceeded ? "Done" : "Try again";
        bool disabled = (step == 0 && !selectionAvailable) || (step == 1 && m_wizardProbeBusy) || step == 2;
        ImGui::BeginDisabled(disabled);
        ImGui::SetCursorScreenPos(ImVec2(footer.x + width - 160 * s, footer.y));
        if (ui::button((std::string(action) + "##WizardNext").c_str(),
                step == 0 ? ui::Icon::Forward : step == 3 && m_wizardSucceeded ? ui::Icon::Check : ui::Icon::Wifi,
                ui::Appearance::Primary, ImVec2(160 * s, 40 * s))) {
            if (step == 0 || (step == 3 && !m_wizardSucceeded)) { m_wizardStep = 1; requestWizardWifiProbe(); }
            else if (step == 1) { if (m_wizardWifiIp.empty()) requestWizardWifiProbe(); else startWizardWifiSetup(); }
            else if (step == 3) close();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(4);
}

void App::renderConnectionManager(FilePanel& panel) {
    ui::PanelStyle theme(m_resolvedLightTheme);
    const float s = theme.s;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.background);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.muted);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme.raised);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme.surface);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme.raised);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * s, 18 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 10 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, s);
    ImGui::BeginChild("##ConnectionManager", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);

    ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float padding = 18 * s;
    const float innerWidth = std::max(1.0f, width - 2 * padding);
    float y = origin.y;
    theme.tile(ui::Icon::Link, origin, 56, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    theme.label(ImVec2(origin.x + 78 * s, y - 1 * s), "Connections", width - 172 * s, theme.text, 29, true);
    theme.label(ImVec2(origin.x + 78 * s, y + 36 * s), "USB and wireless connections", width - 80 * s, theme.muted, 16);
    if (theme.button("##ConnectionHelp", "Help", ui::Icon::Help,
        ImVec2(origin.x + width - 84 * s, y + 15 * s), ImVec2(84 * s, 36 * s), theme.text))
        ImGui::OpenPopup("##ConnectionHelpPopup");
    if (ImGui::BeginPopup("##ConnectionHelpPopup")) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(390 * s, width - 24 * s));
        ImGui::TextUnformatted("Connect your Android device");
        ImGui::Separator();
        ImGui::TextWrapped("USB: connect a data cable, enable USB debugging in Developer Options, then tap Allow on your phone.");
        ImGui::Spacing();
        ImGui::TextWrapped("WiFi: keep your phone and computer on the same network. Run Setup Wizard with USB connected, or use WiFi ADB Pairing on Android 11 or newer.");
        ImGui::Spacing();
        ImGui::TextWrapped("Without USB debugging: install Fast Enough Companion on the phone. It appears here over WiFi or USB tethering.");
        ImGui::Spacing();
        ImGui::TextWrapped("Select an available device to connect or open its files. Auto reconnects a saved device when the app starts. Delete removes its saved connection.");
        ImGui::Spacing();
        ImGui::TextWrapped("If a device is missing, unlock it, check the cable or WiFi network, and refresh the device list.");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    y += 84 * s;

    std::vector<DeviceInfo> devices;
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        devices = m_devices;
    }
    std::vector<DeviceInfo> physicalDevices;
    std::map<std::string, size_t> identities;
    for (const auto& device : devices) {
        std::string identity = deviceIdentity(device.serial);
        auto [it, inserted] = identities.emplace(identity, physicalDevices.size());
        if (inserted) physicalDevices.push_back(device);
        else {
            auto& existing = physicalDevices[it->second];
            if ((existing.state != "device" && device.state == "device") ||
                (existing.state == device.state && isWirelessAdbSerial(existing.serial) && !isWirelessAdbSerial(device.serial)))
                existing = device;
        }
    }
    int available = (int)std::count_if(physicalDevices.begin(), physicalDevices.end(),
        [](const DeviceInfo& device) { return device.state == "device" || device.state == "unpaired"; });
    std::string countLabel = std::to_string(available) + (available == 1 ? " device available" : " devices available");
    bool busy = !m_connectionPendingSerial.empty() || deviceFor(panel).isConnecting() ||
        m_primaryReconnectActive || m_wifiTransitionActive;
    if (busy || !m_connectionFeedback.empty()) {
        theme.box(ImVec2(origin.x, y), ImVec2(width, 48 * s), theme.raised, theme.surface, theme.border);
        theme.status(ImVec2(origin.x + padding, y + 15 * s), busy ? "Connecting..." : "Connection status", busy, 150 * s);
        std::string pending = m_connectionPendingSerial;
        if (companion::isSerial(pending)) {
            CompanionDeviceView view;
            const bool known = companion::find(companion::idFromSerial(pending), view);
            pending = (known && !view.paired ? "Tap Allow on " : "Connecting to ") + (known ? view.name : std::string("the phone"));
        }
        std::string detail = busy ? (!pending.empty() ? pending : deviceFor(panel).statusText()) : m_connectionFeedback;
        theme.label(ImVec2(origin.x + 166 * s, y + 15 * s), detail, width - 210 * s, theme.muted, 13);
        if (!busy && theme.button("##DismissConnectionStatus", "", ui::Icon::Close,
            ImVec2(origin.x + width - 35 * s, y + 9 * s), ImVec2(28 * s, 28 * s), theme.muted)) m_connectionFeedback.clear();
        y += 60 * s;
    }

    auto connectedSlot = [&](const std::string& serial) {
        for (int slot = 0; slot < deviceSessionCount(); ++slot) {
            if (deviceSession(slot).connected && deviceSession(slot).client.isServerRunning() &&
                deviceIdentity(deviceSession(slot).serial) == deviceIdentity(serial)) return slot;
        }
        return -1;
    };
    auto openDevice = [&](const DeviceInfo& device) {
        if (device.state != "device" && device.state != "unpaired") {
            m_connectionFeedback = device.state == "unauthorized" ? "Unlock your phone and tap Allow on the USB debugging prompt." : "This device is offline. Check its cable or WiFi connection.";
            return;
        }
        int slot = connectedSlot(device.serial);
        FilePanel& target = &panel == &m_leftPanel ? m_leftPanel : m_rightPanel;
        if (slot >= 0) openDeviceSession(target, slot);
        else {
            rememberDeviceView(target);
            ++target.listingGeneration;
            target.refreshInProgress = false;
            target.isAndroid = true;
            target.isApps = target.isConnections = false;
            target.androidEntries.clear();
            target.selectedIndices.clear();
            target.currentPath = "/sdcard";
            strcpy_s(target.pathInput, target.currentPath.c_str());
            target.needsRefresh = true;
        }
        target.pendingDeviceSerial = slot < 0 ? device.serial : "";
        target.pendingDeviceName = slot < 0 ? (device.model.empty() ? device.serial : device.model) : "";
        target.deviceOpenError.clear();
        m_showConnectionsWorkspace = m_showAppsWorkspace = m_showBackupManager = false;
        m_lastFocusedPanel = &target;
        if (slot < 0) {
            if (device.state == "unpaired") pairCompanionDevice(device.serial);
            else connectFromManager(device.serial, false);
        }
    };

    bool wideHeader = innerWidth >= 1030 * s;
    bool tinyHeader = innerWidth < 500 * s;
    float availableHeader = (wideHeader ? 76 : tinyHeader ? 158 : 122) * s;
    int columns = std::clamp((int)((innerWidth + 12 * s) / (272 * s)), 1, 4);
    float cardWidth = (innerWidth - (columns - 1) * 12 * s) / columns;
    std::vector<DeviceInfo> visibleDevices;
    std::string query = lower(m_connectionSearch);
    auto isWifiDevice = [](const DeviceInfo& device) {
        return companion::isSerial(device.serial) ? device.transportType == AdbTransportType::Network
                                                  : isWirelessAdbSerial(device.serial);
    };
    const auto transports = onlineAdbTransports(devices);
    // A phone connected by cable and WiFi at once is listed under its USB serial.
    auto pairedWifiSerial = [&](const DeviceInfo& device) {
        if (device.state != "device" || companion::isSerial(device.serial)) return std::string();
        auto found = transports.find(deviceIdentity(device.serial));
        return found != transports.end() && found->second.both() ? found->second.wifiSerial : std::string();
    };
    for (const auto& device : physicalDevices) {
        bool wifi = isWifiDevice(device);
        const bool both = !pairedWifiSerial(device).empty();
        if ((m_connectionFilter == 1 && wifi && !both) || (m_connectionFilter == 2 && !wifi && !both)) continue;
        if (lower(device.model + " " + device.serial).find(query) != std::string::npos) visibleDevices.push_back(device);
    }
    int rows = std::max(1, ((int)visibleDevices.size() + columns - 1) / columns);
    float availableHeight = availableHeader + rows * 78 * s + (rows - 1) * 12 * s + padding;
    theme.card(ImVec2(origin.x, y), ImVec2(width, availableHeight), theme.blue);
    theme.heading(ImVec2(origin.x + padding, y + padding), wideHeader ? 440 * s : innerWidth,
        ui::Icon::Phone, "Available devices", "Devices detected on your network or via USB", theme.blue);

    float controlsY = y + (wideHeader ? 20 : 72) * s;
    float controlsX = wideHeader ? origin.x + width - padding - 566 * s : origin.x + padding;
    float controlsWidth = wideHeader ? 566 * s : innerWidth;
    float searchWidth = tinyHeader ? controlsWidth : controlsWidth - 322 * s;
    theme.search("##AvailableDeviceSearch", "Search devices...", m_connectionSearch, sizeof(m_connectionSearch),
        ImVec2(controlsX, controlsY), searchWidth);
    float filterX = tinyHeader ? controlsX : controlsX + searchWidth + 12 * s;
    float filterY = controlsY + (tinyHeader ? 46 * s : 0);
    ImGui::SetCursorScreenPos(ImVec2(filterX, filterY));
    ImGui::SetNextItemWidth(112 * s);
    const char* filters[] = { "All devices", "USB devices", "WiFi devices" };
    if (ui::beginCombo("##ConnectionTransport", filters[m_connectionFilter], ImGuiComboFlags_NoArrowButton)) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(filters[i], m_connectionFilter == i)) m_connectionFilter = i;
        ImGui::EndCombo();
    }
    ui::icon(ui::Icon::Down, ImVec2(filterX + 91 * s, filterY + 12 * s), 13 * s, theme.color(theme.muted));
    ImGui::BeginDisabled(m_connectionsRefreshing);
    if (theme.button("##RefreshConnections", "", ui::Icon::Refresh, ImVec2(filterX + 124 * s, filterY),
        ImVec2(36 * s, 36 * s), theme.blue)) {
        m_connectionsRefreshing = true;
        postAsync("Refreshing devices...", [this]() {
            UiMessage message;
            message.connectionRefreshFinished = true;
            try { message.connectionDevices = m_device.getDevices(); }
            catch (const std::exception& error) { message.hasStatus = true; message.status = error.what(); }
            catch (...) { message.hasStatus = true; message.status = "Could not refresh devices. Please try again."; }
            postUiMessage(std::move(message));
        });
    }
    ui::tooltip("Refresh available USB and wireless devices");
    ImGui::EndDisabled();
    theme.status(ImVec2(filterX + 180 * s, filterY + 9 * s), m_connectionsRefreshing ? "Refreshing..." : countLabel,
        available > 0, controlsX + controlsWidth - filterX - 180 * s);

    float cardsY = y + availableHeader;
    if (visibleDevices.empty()) {
        theme.label(ImVec2(origin.x + padding + 12 * s, cardsY + 12 * s),
            physicalDevices.empty() ? "Waiting for a device" : "No matching devices", innerWidth - 24 * s, theme.text, 16, true);
        theme.label(ImVec2(origin.x + padding + 12 * s, cardsY + 40 * s),
            physicalDevices.empty() ? "Connect via USB or use wireless setup below." : "Try another name, address, or connection filter.",
            innerWidth - 24 * s, theme.muted, 13);
    }
    for (int i = 0; i < (int)visibleDevices.size(); ++i) {
        const auto& device = visibleDevices[i];
        ImVec2 p(origin.x + padding + (i % columns) * (cardWidth + 12 * s), cardsY + (i / columns) * 90 * s);
        ImGui::PushID(device.serial.c_str());
        bool pressed = theme.button("##DeviceCard", "", ui::Icon::None, p, ImVec2(cardWidth, 78 * s), theme.blue);
        bool hovered = ImGui::IsItemHovered();
        const bool ready = connectedSlot(device.serial) >= 0;
        const std::string readiness = ready ? "Files ready. Click to open instantly." :
            device.state == "device" ? "Preparing file access in the background..." :
            device.state == "unpaired" ? "Click to pair. Approve this computer on the phone." : device.state;
        const std::string pairedWifi = pairedWifiSerial(device);
        const bool both = !pairedWifi.empty();
        ui::tooltip((device.model + "\n" + device.serial + (both ? "\nWiFi: " + pairedWifi : "") + "\n" + readiness).c_str());
        bool wifi = both || isWifiDevice(device);
        theme.tile(ui::Icon::Phone, ImVec2(p.x + 16 * s, p.y + (78 * s - 42 * s) * 0.5f), 42 * s / theme.s, wifi ? theme.green : theme.blue);
        const bool app = companion::isSerial(device.serial);
        const char* badge = app ? (wifi ? "WiFi App" : "USB App") : both ? "USB + WiFi" : wifi ? "WiFi" : "USB";
        ImVec4 badgeColor = wifi ? theme.green : theme.blue;
        if (device.state == "unpaired") { badge = "Pair"; badgeColor = theme.gold; }
        else if (device.state != "device") { badge = device.state == "unauthorized" ? "Allow USB" : "Offline"; badgeColor = theme.gold; }
        // Status line and transport badge share one right-aligned column, centered on each other.
        const char* statusLabel = ready ? "Ready" : device.state == "device" ? "Preparing..." : device.state == "unpaired" ? "Not paired" : "";
        const ImVec2 badgeSize = theme.pillSize(badge);
        const float statusWidth = ImGui::CalcTextSize(statusLabel).x + (*statusLabel ? 13 * s : 0);
        const float columnWidth = std::max(badgeSize.x, statusWidth);
        const float columnX = p.x + cardWidth - 16 * s - columnWidth;
        const float groupHeight = ImGui::GetTextLineHeight() + 6 * s + badgeSize.y;
        const float groupY = p.y + (78 * s - groupHeight) * 0.5f;
        if (*statusLabel) {
            const ImVec4 statusInk = ready ? theme.green : theme.gold;
            const float statusX = columnX + (columnWidth - statusWidth) * 0.5f;
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(statusX + 3 * s, groupY + ImGui::GetTextLineHeight() * 0.5f), 3 * s, theme.color(statusInk));
            ImGui::GetWindowDrawList()->AddText(ImVec2(statusX + 13 * s, groupY), theme.color(statusInk), statusLabel);
        }
        ImVec2 badgePos(columnX + (columnWidth - badgeSize.x) * 0.5f, groupY + ImGui::GetTextLineHeight() + 6 * s);
        theme.pill(badgePos, badgeSize, wifi ? ui::Icon::Wifi : ui::Icon::Usb, badge, badgeColor);
        const float nameX = p.x + 72 * s;
        const float nameWidth = columnX - nameX - 10 * s;
        theme.label(ImVec2(nameX, p.y + 20 * s), device.model.empty() ? device.serial : device.model, nameWidth, theme.text, 14, true);
        theme.label(ImVec2(nameX, p.y + 44 * s), app ? "Companion app" : device.serial, nameWidth, theme.muted, 12);
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (pressed) openDevice(device);
        ImGui::PopID();
    }
    y += availableHeight + 18 * s;

    bool wideSavedHeader = innerWidth >= 900 * s;
    bool compactRows = innerWidth < 790 * s;
    bool narrowRows = innerWidth < 330 * s;
    float savedHeader = (wideSavedHeader ? 76 : 122) * s;
    float rowHeight = (compactRows ? (narrowRows ? 134 : 100) : 50) * s;
    float rowGap = 8 * s;
    std::vector<int> savedIndices;
    query = lower(m_savedConnectionSearch);
    for (int i = 0; i < (int)m_prefs.savedWifiDevices.size(); ++i) {
        const auto& saved = m_prefs.savedWifiDevices[i];
        if (!saved.wifiIp.empty() && lower(savedName(saved) + " " + wifiAddress(saved) + " " + saved.serial).find(query) != std::string::npos)
            savedIndices.push_back(i);
    }
    std::stable_sort(savedIndices.begin(), savedIndices.end(), [&](int a, int b) {
        const auto& left = m_prefs.savedWifiDevices[a];
        const auto& right = m_prefs.savedWifiDevices[b];
        if (m_savedConnectionSort == 1) return lower(wifiAddress(left)) < lower(wifiAddress(right));
        if (m_savedConnectionSort == 2 && left.autoConnect != right.autoConnect) return left.autoConnect > right.autoConnect;
        return lower(savedName(left)) < lower(savedName(right));
    });
    float savedHeight = savedHeader + std::max(1, (int)savedIndices.size()) * rowHeight +
        std::max(0, (int)savedIndices.size() - 1) * rowGap + padding;
    theme.card(ImVec2(origin.x, y), ImVec2(width, savedHeight), theme.green);
    theme.heading(ImVec2(origin.x + padding, y + padding), wideSavedHeader ? innerWidth - 430 * s : innerWidth,
        ui::Icon::Wifi, "Saved WiFi devices", "Manage your saved wireless devices", theme.green);
    float savedControlsWidth = wideSavedHeader ? 420 * s : innerWidth;
    float savedControlsX = origin.x + width - padding - savedControlsWidth;
    float savedControlsY = y + (wideSavedHeader ? 20 : 72) * s;
    theme.search("##SavedDeviceSearch", "Search saved devices...", m_savedConnectionSearch, sizeof(m_savedConnectionSearch),
        ImVec2(savedControlsX, savedControlsY), savedControlsWidth - 158 * s);
    const char* sorts[] = { "Sort by name", "Sort by address", "Auto first" };
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - padding - 146 * s, savedControlsY));
    ImGui::SetNextItemWidth(146 * s);
    if (ui::beginCombo("##SavedDeviceSort", sorts[m_savedConnectionSort], ImGuiComboFlags_NoArrowButton)) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(sorts[i], m_savedConnectionSort == i)) m_savedConnectionSort = i;
        ImGui::EndCombo();
    }
    ui::icon(ui::Icon::Down, ImVec2(origin.x + width - padding - 23 * s, savedControlsY + 12 * s), 13 * s, theme.color(theme.muted));
    if (savedIndices.empty()) {
        theme.label(ImVec2(origin.x + padding + 12 * s, y + savedHeader + 5 * s),
            m_prefs.savedWifiDevices.empty() ? "No saved WiFi devices yet" : "No matching saved devices", innerWidth - 24 * s, theme.text, 14, true);
        theme.label(ImVec2(origin.x + padding + 12 * s, y + savedHeader + 29 * s),
            m_prefs.savedWifiDevices.empty() ? "Use the setup wizard or WiFi pairing to add a device." : "Try a different name or address.",
            innerWidth - 24 * s, theme.muted, 13);
    }
    int deleteIndex = -1;
    for (int row = 0; row < (int)savedIndices.size(); ++row) {
        int index = savedIndices[row];
        auto& saved = m_prefs.savedWifiDevices[index];
        std::string address = wifiAddress(saved);
        ImVec2 p(origin.x + padding, y + savedHeader + row * (rowHeight + rowGap));
        theme.box(p, ImVec2(innerWidth, rowHeight), theme.raised, theme.surface, theme.border, 9);
        theme.tile(ui::Icon::Phone, ImVec2(p.x + 16 * s, p.y + (compactRows ? 10 * s : (rowHeight - 32 * s) * 0.5f)), 32 * s / theme.s, theme.green);
        float actionsX = compactRows ? p.x + 16 * s : p.x + innerWidth - 430 * s;
        float actionsY = p.y + (compactRows ? 56 : 9) * s;
        float textWidth = compactRows ? innerWidth - 76 * s : actionsX - p.x - 82 * s;
        float nameWidth = compactRows ? textWidth : textWidth * .53f;
        const float nameY = compactRows ? p.y + 10 * s : p.y + (rowHeight - 14 * theme.s) * 0.5f - 1 * s;
        theme.label(ImVec2(p.x + 76 * s, nameY), savedName(saved), nameWidth, theme.text, 14, true);
        theme.label(ImVec2(compactRows ? p.x + 76 * s : p.x + 76 * s + nameWidth + 12 * s,
            compactRows ? p.y + 31 * s : p.y + (rowHeight - 13 * theme.s) * 0.5f - 1 * s), address,
            compactRows ? textWidth : textWidth - nameWidth - 12 * s, theme.muted, 13);
        ImGui::PushID(address.c_str());
        if (!compactRows) actionsY = p.y + (rowHeight - 34 * s) * 0.5f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4 * s, 4 * s));
        ImGui::SetCursorScreenPos(ImVec2(actionsX, actionsY + (34 * s - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        if (ui::checkbox("Auto##Reconnect", &saved.autoConnect)) m_prefs.save();
        ui::tooltip("Automatically reconnect to this device when the app starts");
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        float buttonWidth = compactRows ? std::max(30 * s, std::min(114 * s, (innerWidth - (narrowRows ? 70 : 146) * s) * .5f)) : 114 * s;
        float connectX = narrowRows ? actionsX : compactRows ? actionsX + 78 * s : actionsX + 126 * s;
        float buttonY = actionsY + (narrowRows ? 34 * s : 0);
        ImGui::BeginDisabled(!m_connectionPendingSerial.empty());
        if (theme.button("##ConnectSavedDevice", m_connectionPendingSerial == address ? "Connecting" : "Connect", ui::Icon::Wifi,
            ImVec2(connectX, buttonY), ImVec2(buttonWidth, 34 * s), rgb(91, 222, 255), 4)) {
            if (connectedSlot(address) >= 0) openDevice(DeviceInfo{address, savedName(saved), "device"});
            else connectFromManager(address, true);
        }
        ImGui::EndDisabled();
        if (theme.button("##DeleteSavedDevice", "Delete", ui::Icon::Trash,
            ImVec2(connectX + buttonWidth + 16 * s, buttonY), ImVec2(buttonWidth - 10 * s, 34 * s), rgb(255, 143, 151), 3)) deleteIndex = index;
        if (theme.button("##SavedDeviceMore", "", ui::Icon::MoreVertical,
            ImVec2(p.x + innerWidth - 36 * s, buttonY), ImVec2(26 * s, 34 * s), theme.muted)) ImGui::OpenPopup("##SavedDeviceMenu");
        ui::tooltip("More device options");
        if (ImGui::BeginPopup("##SavedDeviceMenu")) {
            ImGui::TextUnformatted(savedName(saved).c_str());
            ImGui::TextDisabled("%s", address.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(address.c_str());
            if (!saved.serial.empty() && ImGui::MenuItem("Copy device serial")) ImGui::SetClipboardText(saved.serial.c_str());
            if (ImGui::MenuItem("Auto-connect on launch", nullptr, &saved.autoConnect)) m_prefs.save();
            ImGui::Separator();
            if (ImGui::MenuItem("Remove saved device")) deleteIndex = index;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (deleteIndex >= 0) {
        std::string name = savedName(m_prefs.savedWifiDevices[deleteIndex]);
        m_prefs.savedWifiDevices.erase(m_prefs.savedWifiDevices.begin() + deleteIndex);
        m_prefs.save();
        m_statusMessage = "Removed saved device: " + name;
        m_statusTime = std::chrono::steady_clock::now();
    }
    y += savedHeight + 18 * s;

    y = renderCompanionSection(panel, origin, width, y, openDevice);

    bool wideSetup = innerWidth >= 1020 * s;
    bool horizontalSteps = innerWidth >= 720 * s;
    float stepsWidth = wideSetup ? innerWidth - 364 * s : innerWidth;
    float stepWidth = horizontalSteps ? (stepsWidth - 24 * s) / 3 : stepsWidth;
    const char* stepTitles[] = { "Connect via USB", "Enable Developer Options", "Authorize this computer" };
    const char* stepDescriptions[] = {
        "Connect your phone via USB cable and enable USB debugging.",
        "Go to Settings > About phone and tap Build number 7 times.",
        "When prompted on your device, tap Allow to authorize this computer."
    };
    float stepHeight = 0;
    for (auto description : stepDescriptions)
        stepHeight = std::max(stepHeight, ImGui::GetFont()->CalcTextSizeA(13 * s, FLT_MAX,
            std::max(40 * s, stepWidth - 62 * s), description).y + 36 * s);
    stepHeight = std::max(66 * s, stepHeight);
    float stepsHeight = horizontalSteps ? stepHeight : 3 * (stepHeight + 10 * s) - 10 * s;
    float setupHeight = 76 * s + stepsHeight + padding + (wideSetup ? 0 : 142 * s);
    theme.card(ImVec2(origin.x, y), ImVec2(width, setupHeight), ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    theme.heading(ImVec2(origin.x + padding, y + padding), stepsWidth, ui::Icon::Gear,
        "Setup & Instructions", "Follow these steps to connect your devices", m_theme.gradientSecondary);
    for (int i = 0; i < 3; ++i) {
        ImVec2 p(origin.x + padding + (horizontalSteps ? i * (stepWidth + 12 * s) : 0),
            y + 76 * s + (horizontalSteps ? 0 : i * (stepHeight + 10 * s)));
        const ImVec2 center(p.x + 21 * s, p.y + 21 * s);
        {
            auto* stepDraw = ImGui::GetWindowDrawList();
            ImVec4 glow = m_theme.gradientPrimary; glow.w = 0.18f;
            stepDraw->AddCircleFilled(ImVec2(center.x, center.y + 2 * s), 23 * s, theme.color(glow));
            const int first = stepDraw->VtxBuffer.Size;
            stepDraw->AddCircleFilled(center, 21 * s, theme.color(m_theme.gradientPrimary), 40);
            for (int v = first; v < stepDraw->VtxBuffer.Size; ++v) {
                auto& vertex = stepDraw->VtxBuffer[v];
                const float t = std::clamp(((vertex.pos.x - center.x) + (vertex.pos.y - center.y)) / (84 * s) + 0.5f, 0.0f, 1.0f);
                const ImVec4 a = m_theme.gradientPrimary, b = m_theme.gradientSecondary;
                const ImU32 alpha = vertex.col & IM_COL32_A_MASK;
                vertex.col = (theme.color(ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1)) & ~IM_COL32_A_MASK) | alpha;
            }
        }
        ImFont* numberFont = ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
        const char number[] = { static_cast<char>('1' + i), '\0' };
        if (const auto* glyph = numberFont->FindGlyph(number[0])) {
            const float fontSize = 21 * s;
            const float glyphScale = fontSize / numberFont->FontSize;
            // Center the visible numeral bounds, including the font's bearings.
            const ImVec2 textPosition(
                std::round(center.x - (glyph->X0 + glyph->X1) * glyphScale * 0.5f),
                std::round(center.y - (glyph->Y0 + glyph->Y1) * glyphScale * 0.5f));
            const ImVec4 mid((m_theme.gradientPrimary.x + m_theme.gradientSecondary.x) * 0.5f,
                (m_theme.gradientPrimary.y + m_theme.gradientSecondary.y) * 0.5f,
                (m_theme.gradientPrimary.z + m_theme.gradientSecondary.z) * 0.5f, 1);
            ImGui::GetWindowDrawList()->AddText(numberFont, fontSize, textPosition,
                ui::brightness(mid) > 0.5f ? IM_COL32(10, 15, 24, 255) : IM_COL32(255, 255, 255, 255), number);
        }
        theme.label(ImVec2(p.x + 64 * s, p.y + 5 * s), stepTitles[i], stepWidth - 64 * s, theme.text, 13, true);
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), 13 * s, ImVec2(p.x + 64 * s, p.y + 29 * s),
            theme.color(theme.muted), stepDescriptions[i], nullptr, std::max(40 * s, stepWidth - 64 * s));
    }
    float setupActionsWidth = wideSetup ? 328 * s : std::min(innerWidth, 440 * s);
    float setupActionsX = wideSetup ? origin.x + width - padding - setupActionsWidth : origin.x + (width - setupActionsWidth) * .5f;
    float setupActionsY = wideSetup ? y + 24 * s : y + 76 * s + stepsHeight + 18 * s;
    if (wideSetup) ImGui::GetWindowDrawList()->AddLine(ImVec2(setupActionsX - 28 * s, y + 20 * s),
        ImVec2(setupActionsX - 28 * s, y + setupHeight - 20 * s), theme.color(theme.border), s);
    ImGui::SetCursorScreenPos(ImVec2(setupActionsX, setupActionsY));
    if (ui::button("Run Setup Wizard##RunConnectionSetup", ui::Icon::Wand, ui::Appearance::Primary, ImVec2(setupActionsWidth, 40 * s))) {
        openWifiWizard();
    }
    ImGui::SetCursorScreenPos(ImVec2(setupActionsX, setupActionsY + 50 * s));
    if (ui::button("Pair with QR code (Android 11+)##PairConnectionWifi", ui::Icon::Link, ui::Appearance::Secondary, ImVec2(setupActionsWidth, 40 * s))) {
        openWifiPairing();
    }
    float statusWidth = ImGui::CalcTextSize(countLabel.c_str()).x + 20 * s;
    theme.status(ImVec2(setupActionsX + std::max(0.0f, (setupActionsWidth - statusWidth) * .5f), setupActionsY + 106 * s),
        countLabel, available > 0, statusWidth + 10 * s);
    y += setupHeight + 4 * s;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    ImGui::Dummy(ImVec2(width, s));
    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(7);
}

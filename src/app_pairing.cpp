#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "app.h"
#include "ui_panels.h"
#include "qrcodegen/qrcodegen.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#pragma comment(lib, "bcrypt.lib")

namespace {
std::string pairingToken(size_t length) {
    constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string token;
    while (token.size() < length) {
        std::array<unsigned char, 32> bytes{};
        if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) throw std::runtime_error("Could not create a pairing code.");
        for (auto byte : bytes) {
            if (byte >= 248) continue;
            token += alphabet[byte % 62];
            if (token.size() == length) break;
        }
    }
    return token;
}

std::string trimmed(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    auto first = value.find_first_not_of(" \r\n\t");
    return first == std::string::npos ? std::string{} : value.substr(first);
}
}

void App::openWifiPairing() {
    m_showWifiPairing = true;
    m_pairingUseQr = true;
    m_pairingIp[0] = m_pairingCode[0] = m_connectIp[0] = '\0';
    startWifiQrSession();
}

void App::startWifiQrSession() {
    ++m_pairingGeneration;
    m_pairingDone = false;
    m_pairingAddress.clear();
    m_pairingGuid.clear();
    m_pairingReadyAddress.clear();
    m_pairingError.clear();
    m_pairingStatus = "Waiting for your phone to scan the code";
    m_pairingNextPoll = 0;
    m_pairingExpiresAt = ImGui::GetTime() + 120;
    m_pairingQrModules.clear();
    m_pairingQrSize = 0;
    try {
        m_pairingService = "studio-" + pairingToken(10);
        m_pairingPassword = pairingToken(20);
        const std::string payload = "WIFI:T:ADB;S:" + m_pairingService + ";P:" + m_pairingPassword + ";;";
        auto qr = qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        m_pairingQrSize = qr.getSize();
        for (int y = 0; y < m_pairingQrSize; ++y)
            for (int x = 0; x < m_pairingQrSize; ++x)
                m_pairingQrModules.push_back(qr.getModule(x, y) ? 1 : 0);
    } catch (const std::exception& error) {
        m_pairingError = error.what();
    }
}

void App::pollWifiPairing(int action) {
    if (m_pairingWorkerBusy.load()) return;
    if (m_pairingThread.joinable()) m_pairingThread.join();
    const auto generation = m_pairingGeneration.load();
    const std::string serviceName = m_pairingService;
    const std::string password = action == 1 ? std::string(m_pairingCode) : m_pairingPassword;
    const std::string manualAddress = action == 1 ? std::string(m_pairingIp) : std::string(m_connectIp);
    const std::string adbPath = m_device.getAdbPath();
    UiMessage result;
    result.hasWifiPairing = true;
    result.wifiPairingGeneration = generation;
    result.wifiPaired = m_pairingDone;
    result.wifiPairingAddress = m_pairingAddress;
    result.wifiPairingGuid = m_pairingGuid;
    m_pairingError.clear();
    if (action == 1) m_pairingStatus = "Pairing with your phone...";
    if (action == 2) m_pairingStatus = "Connecting to your phone...";
    m_pairingNextPoll = ImGui::GetTime() + 1.2;
    m_pairingWorkerBusy = true;
    m_pairingThread = std::thread([this, generation, serviceName, password, manualAddress, adbPath,
        action, result = std::move(result)]() mutable {
        auto cancelled = [this, generation]() {
            return m_shutdownPoll.load() || m_pairingGeneration.load() != generation;
        };
        try {
            DeviceClient probe;
            probe.setOwnsServer(false);
            probe.setAdbPath(adbPath);
            std::vector<AdbMdnsService> services;
            if (!cancelled() && action != 2) {
                auto discovery = probe.runAdbCommandResult("mdns services", 5000, cancelled);
                if (discovery.succeeded()) services = parseAdbMdnsServices(discovery.standardOutput);
                else if (action == 0 && !cancelled())
                    result.wifiPairingError = "Network discovery is unavailable. Check that ADB is up to date, or use a pairing code.";
            }
            std::string pairingAddress;
            if (action == 1) pairingAddress = manualAddress;
            else if (!result.wifiPaired) {
                for (const auto& service : services) {
                    if (service.type == "_adb-tls-pairing._tcp" && service.name == serviceName) {
                        pairingAddress = service.address;
                        break;
                    }
                }
            }
            if (!pairingAddress.empty() && !cancelled()) {
                auto progress = result;
                progress.wifiPairingStatus = "Phone found. Pairing securely...";
                postUiMessage(std::move(progress));
                auto paired = probe.runAdbCommandResult("pair " + pairingAddress + " " + password, 15000, cancelled);
                if (paired.succeeded() && paired.standardOutput.find("Successfully paired") != std::string::npos) {
                    result.wifiPaired = true;
                    result.wifiPairingAddress = pairingAddress;
                    result.wifiPairingStatus = "Paired successfully. Finding your phone's connection...";
                    auto begin = paired.standardOutput.find("[guid=");
                    if (begin != std::string::npos) {
                        begin += 6;
                        auto end = paired.standardOutput.find(']', begin);
                        if (end != std::string::npos) result.wifiPairingGuid = paired.standardOutput.substr(begin, end - begin);
                    }
                } else if (!cancelled()) {
                    result.wifiPairingError = action == 1
                        ? "Pairing failed. Check the address and the current six-digit code on your phone."
                        : "Pairing did not finish. Generate a new QR code and scan it again.";
                }
            }
            std::string connectAddress;
            if (action == 2) connectAddress = manualAddress;
            else if (result.wifiPaired)
                connectAddress = findAdbConnectAddress(services, result.wifiPairingAddress, result.wifiPairingGuid);
            if (!connectAddress.empty() && !cancelled()) {
                auto connected = probe.runAdbCommandResult("connect " + connectAddress, 8000, cancelled);
                if (connected.succeeded() && (connected.standardOutput.starts_with("connected to ") ||
                    connected.standardOutput.starts_with("already connected to "))) {
                    result.wifiPairingConnectedAddress = connectAddress;
                    result.wifiPairingStatus = "Your phone is paired and connected over WiFi.";
                    auto model = probe.runAdbCommandResult("-s " + connectAddress + " shell getprop ro.product.model", 4000, cancelled);
                    if (model.succeeded()) result.wifiPairingModel = trimmed(model.standardOutput);
                    auto serial = probe.runAdbCommandResult("-s " + connectAddress + " shell getprop ro.serialno", 4000, cancelled);
                    if (serial.succeeded()) result.wifiPairingSerial = trimmed(serial.standardOutput);
                } else if (!cancelled()) {
                    result.wifiPairingError = "Your phone is paired, but the connection failed. Check Wireless debugging and try its current IP address and port.";
                }
            }
        } catch (const std::exception&) {
            result.wifiPairingError = "Pairing could not finish. Try again or use the pairing code option.";
        }
        if (!cancelled()) postUiMessage(std::move(result));
        m_pairingWorkerBusy = false;
    });
}

void App::applyWifiPairingResult(const UiMessage& message) {
    if (!m_showWifiPairing || message.wifiPairingGeneration != m_pairingGeneration.load()) return;
    if (!message.wifiPairingStatus.empty()) m_pairingStatus = message.wifiPairingStatus;
    if (!message.wifiPairingError.empty()) m_pairingError = message.wifiPairingError;
    if (message.wifiPaired && !m_pairingDone) m_pairingExpiresAt = ImGui::GetTime() + 30;
    m_pairingDone = message.wifiPaired;
    m_pairingAddress = message.wifiPairingAddress;
    m_pairingGuid = message.wifiPairingGuid;
    if (message.wifiPairingConnectedAddress.empty() || !m_pairingReadyAddress.empty()) return;
    m_pairingReadyAddress = message.wifiPairingConnectedAddress;
    auto endpoint = parseAdbEndpoint(m_pairingReadyAddress);
    if (!endpoint) return;
    SavedWifiDevice saved;
    saved.serial = message.wifiPairingSerial.empty() ? m_pairingReadyAddress : message.wifiPairingSerial;
    saved.wifiIp = endpoint->host;
    saved.port = endpoint->port;
    saved.model = message.wifiPairingModel;
    saved.autoConnect = true;
    saved.wirelessDebugging = true;
    saved.pairingGuid = m_pairingGuid;
    auto existing = std::find_if(m_prefs.savedWifiDevices.begin(), m_prefs.savedWifiDevices.end(), [&](const SavedWifiDevice& device) {
        return device.serial == saved.serial || (device.wifiIp == saved.wifiIp && device.port == saved.port);
    });
    if (existing == m_prefs.savedWifiDevices.end()) m_prefs.savedWifiDevices.push_back(saved);
    else {
        if (!existing->model.empty()) saved.model = existing->model;
        *existing = saved;
    }
    m_prefs.wifiAutoConnect = true;
    m_prefs.save();
    m_connectionFeedback = m_pairingStatus;
}

void App::renderWifiPairingDialog() {
    if (!m_showWifiPairing) return;
    const double now = ImGui::GetTime();
    const bool expired = now >= m_pairingExpiresAt;
    if (!m_pairingWorkerBusy && m_pairingReadyAddress.empty() && m_pairingError.empty() &&
        !expired && now >= m_pairingNextPoll && (m_pairingUseQr || m_pairingDone)) pollWifiPairing();
    ui::PanelStyle theme(m_resolvedLightTheme);
    const float s = theme.s;
    auto* viewport = ImGui::GetMainViewport();
    ImGui::OpenPopup("WiFi ADB Pairing");
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(710 * s, viewport->WorkSize.x - 28 * s),
        std::min(568 * s, viewport->WorkSize.y - 32 * s)), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s, 24 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * s, 10 * s));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme.surface);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.muted);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
    if (ImGui::BeginPopupModal("WiFi ADB Pairing", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) {
        auto close = [&]() {
            ++m_pairingGeneration;
            m_showWifiPairing = false;
            m_pairingPassword.clear();
            m_pairingQrModules.clear();
            ImGui::CloseCurrentPopup();
        };
        ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float footerY = origin.y + ImGui::GetContentRegionAvail().y - 36 * s;
        theme.tile(ui::Icon::Link, origin, 48, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        theme.label(ImVec2(origin.x + 66 * s, origin.y), "Pair over WiFi", width - 110 * s, theme.text, 24, true);
        theme.label(ImVec2(origin.x + 66 * s, origin.y + 32 * s), "Android 11 and later. No USB cable needed.", width - 110 * s, theme.muted, 13);
        if (theme.button("##ClosePairing", "", ui::Icon::Close,
            ImVec2(origin.x + width - 32 * s, origin.y), ImVec2(32 * s, 32 * s), theme.muted)) close();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close();

        const bool ready = !m_pairingReadyAddress.empty();
        {
            const ImVec2 track(origin.x, origin.y + 70 * s);
            const ImVec2 trackSize(360 * s, 40 * s);
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(track, ImVec2(track.x + trackSize.x, track.y + trackSize.y), theme.color(theme.background), 10 * s);
            draw->AddRect(track, ImVec2(track.x + trackSize.x, track.y + trackSize.y), theme.color(theme.border), 10 * s);
            for (int tab = 0; tab < 2; ++tab) {
                bool active = m_pairingUseQr == (tab == 0);
                ImGui::BeginDisabled(m_pairingDone || m_pairingWorkerBusy);
                ImGui::SetCursorScreenPos(ImVec2(track.x + 4 * s + tab * (trackSize.x - 8 * s) * 0.5f, track.y + 4 * s));
                if (ui::button(tab == 0 ? "Scan QR code##PairQr" : "Use pairing code##PairCode",
                        tab == 0 ? ui::Icon::Link : ui::Icon::Settings,
                        active ? ui::Appearance::Selected : ui::Appearance::Quiet,
                        ImVec2((trackSize.x - 8 * s) * 0.5f, trackSize.y - 8 * s)) && !active) {
                    m_pairingUseQr = tab == 0;
                    startWifiQrSession();
                    if (!m_pairingUseQr) m_pairingStatus = "Enter the details shown on your phone";
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 124 * s));
        ImGui::BeginChild("##PairingBody", ImVec2(width, std::max(50 * s, footerY - origin.y - 140 * s)), 0);
        if (ready) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            theme.tile(ui::Icon::Check, p, 56);
            ImGui::SetCursorScreenPos(ImVec2(p.x + 76 * s, p.y + 2 * s));
            ImGui::TextColored(theme.green, "Connected successfully");
            ImGui::SetCursorScreenPos(ImVec2(p.x + 76 * s, p.y + 31 * s));
            ImGui::TextUnformatted(m_pairingReadyAddress.c_str());
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 84 * s));
            ImGui::TextWrapped("Your phone is saved and ready to open from Connections. Keep Wireless debugging enabled while you use WiFi.");
        } else if (m_pairingDone) {
            ImGui::TextColored(theme.green, "Phone paired");
            ImGui::TextWrapped("%s", m_pairingStatus.c_str());
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::TextWrapped("If your phone does not connect automatically, enter the IP address and port on the main Wireless debugging screen.");
            ImGui::Spacing();
            ImGui::TextDisabled("Connection address");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##PairConnectAddress", "192.168.1.100:42135", m_connectIp, sizeof(m_connectIp));
            ImGui::TextDisabled("Use the connection port, which is different from the pairing port.");
        } else if (m_pairingUseQr) {
            const float qrArea = std::min(244 * s, width * .42f);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool showQr = !expired && m_pairingQrSize > 0 && m_pairingError.empty();
            if (showQr) {
                const float pixel = std::max(1.0f, std::floor(qrArea / (m_pairingQrSize + 8)));
                const float extent = pixel * (m_pairingQrSize + 8);
                ImVec2 base(std::floor(p.x + (qrArea - extent) * .5f), std::floor(p.y));
                auto* draw = ImGui::GetWindowDrawList();
                ImVec4 glow = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark); glow.w = 0.16f;
                draw->AddRectFilled(ImVec2(base.x - 6 * s, base.y - 4 * s), ImVec2(base.x + extent + 6 * s, base.y + extent + 10 * s), theme.color(glow), 16 * s);
                draw->AddRectFilled(base, ImVec2(base.x + extent, base.y + extent), IM_COL32_WHITE, 12 * s);
                for (int y = 0; y < m_pairingQrSize; ++y) {
                    for (int x = 0; x < m_pairingQrSize; ++x) {
                        if (!m_pairingQrModules[y * m_pairingQrSize + x]) continue;
                        ImVec2 cell(base.x + (x + 4) * pixel, base.y + (y + 4) * pixel);
                        draw->AddRectFilled(cell, ImVec2(cell.x + pixel, cell.y + pixel), IM_COL32(0, 0, 0, 255));
                    }
                }
                const std::string expires = "Expires in " + std::to_string(std::max(0, static_cast<int>(m_pairingExpiresAt - now))) + " seconds";
                const float expiresWidth = ImGui::GetFont()->CalcTextSizeA(12 * s, FLT_MAX, 0, expires.c_str()).x;
                theme.label(ImVec2(p.x + std::max(0.0f, (qrArea - expiresWidth) * 0.5f), p.y + qrArea + 14 * s),
                    expires, qrArea, theme.muted, 12);
            } else {
                theme.box(p, ImVec2(qrArea, qrArea), theme.raised, theme.surface, theme.border);
                theme.label(ImVec2(p.x + 18 * s, p.y + qrArea * .38f), expired ? "QR code expired" : "QR code unavailable", qrArea - 36 * s, theme.text, 16, true);
                if (theme.button("##RenewQrCard", "New QR code", ui::Icon::Refresh,
                    ImVec2(p.x + 18 * s, p.y + qrArea * .57f), ImVec2(qrArea - 36 * s, 36 * s), theme.blue)) startWifiQrSession();
            }
            const float textX = p.x + qrArea + 28 * s;
            const char* titles[] = {"Open Wireless debugging", "Tap Pair device with QR code", "Scan this code"};
            const char* details[] = {"Settings > Developer options", "Use the scanner inside Android settings.", "Keep your phone and PC on the same WiFi network."};
            for (int i = 0; i < 3; ++i) {
                float y = p.y + i * 80 * s;
                auto* stepDraw = ImGui::GetWindowDrawList();
                const ImVec2 center(textX + 14 * s, y + 13 * s);
                ui::gradientRect(stepDraw, ImVec2(center.x - 14 * s, center.y - 14 * s), ImVec2(center.x + 14 * s, center.y + 14 * s),
                    m_theme.gradientPrimary, m_theme.gradientSecondary, 14 * s);
                ImFont* numberFont = ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
                const std::string number = std::to_string(i + 1);
                const ImVec2 numberSize = numberFont->CalcTextSizeA(14 * s, FLT_MAX, 0, number.c_str());
                const ImVec4 mid((m_theme.gradientPrimary.x + m_theme.gradientSecondary.x) * 0.5f,
                    (m_theme.gradientPrimary.y + m_theme.gradientSecondary.y) * 0.5f, (m_theme.gradientPrimary.z + m_theme.gradientSecondary.z) * 0.5f, 1);
                stepDraw->AddText(numberFont, 14 * s, ImVec2(center.x - numberSize.x * 0.5f, center.y - numberSize.y * 0.5f),
                    ui::brightness(mid) > 0.5f ? IM_COL32(10, 15, 24, 255) : IM_COL32(255, 255, 255, 255), number.c_str());
                theme.label(ImVec2(textX + 42 * s, y + 3 * s), titles[i], width - qrArea - 70 * s, theme.text, 14, true);
                ImGui::SetCursorScreenPos(ImVec2(textX + 42 * s, y + 27 * s));
                ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
                ImGui::TextWrapped("%s", details[i]);
                ImGui::PopStyleColor();
            }
            if (!expired && m_pairingError.empty()) {
                const ImVec2 status(textX, p.y + 3 * 80 * s);
                auto* draw = ImGui::GetWindowDrawList();
                const float angle = (float)ImGui::GetTime() * 4.0f;
                draw->PathArcTo(ImVec2(status.x + 10 * s, status.y + 10 * s), 7 * s, angle, angle + 4.4f, 20);
                draw->PathStroke(theme.color(theme.blue), 0, 2 * s);
                theme.label(ImVec2(status.x + 28 * s, status.y + 2 * s), m_pairingStatus, width - qrArea - 60 * s, theme.blue, 13, true);
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + qrArea + 42 * s));
            } else {
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + qrArea + 42 * s));
            }
        } else {
            ImGui::TextWrapped("On your phone, open Settings > Developer options > Wireless debugging, then tap Pair device with pairing code.");
            ImGui::Spacing();
            ImGui::TextDisabled("Pairing address");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##PairAddress", "192.168.1.100:37000", m_pairingIp, sizeof(m_pairingIp));
            ImGui::Spacing();
            ImGui::TextDisabled("Six-digit pairing code");
            ImGui::SetNextItemWidth(210 * s);
            ImGui::InputTextWithHint("##PairCodeInput", "123456", m_pairingCode, 7, ImGuiInputTextFlags_CharsDecimal);
            ImGui::Spacing();
            ImGui::TextWrapped("%s", m_pairingStatus.c_str());
        }
        if (!m_pairingError.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.red);
            ImGui::TextWrapped("%s", m_pairingError.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::SetCursorScreenPos(ImVec2(origin.x, footerY));
        if (ui::button(ready ? "Done##CancelPairing" : "Cancel##CancelPairing", ui::Icon::None, ui::Appearance::Secondary,
            ImVec2(110 * s, 38 * s))) close();
        if (ready) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 160 * s, footerY));
            if (ui::button("Open files##OpenPairedPhone", ui::Icon::Folder, ui::Appearance::Primary, ImVec2(160 * s, 38 * s))) {
                std::string address = m_pairingReadyAddress;
                FilePanel& panel = m_rightPanel;
                if (!panel.isAndroid || panel.isApps || panel.isConnections) switchPanelMode(panel, true);
                panel.androidEntries.clear();
                panel.selectedIndices.clear();
                panel.focusedIndex = -1;
                panel.insideMcraw = false;
                panel.mcrawFilePath.clear();
                panel.navHistory.clear();
                panel.navHistoryPos = -1;
                panel.searchFilter[0] = '\0';
                panel.pendingDeviceSerial = address;
                panel.pendingDeviceName = address;
                panel.deviceOpenError.clear();
                m_lastFocusedPanel = &panel;
                m_showConnectionsWorkspace = false;
                m_showBackupManager = false;
                m_showAppsWorkspace = false;
                close();
                connectFromManager(address, false);
            }
        } else if (m_pairingDone || !m_pairingUseQr) {
            bool valid = m_pairingDone ? parseAdbEndpoint(m_connectIp).has_value() :
                parseAdbEndpoint(m_pairingIp).has_value() && std::string(m_pairingCode).size() == 6 &&
                    std::all_of(m_pairingCode, m_pairingCode + 6, [](unsigned char c) { return c >= '0' && c <= '9'; });
            ImGui::BeginDisabled(!valid || m_pairingWorkerBusy);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 160 * s, footerY));
            if (ui::button(m_pairingDone ? "Connect##SubmitPairing" : "Pair device##SubmitPairing", ui::Icon::Wifi,
                    ui::Appearance::Primary, ImVec2(160 * s, 38 * s))) {
                m_pairingExpiresAt = now + 30;
                pollWifiPairing(m_pairingDone ? 2 : 1);
            }
            ImGui::EndDisabled();
        } else {
            ImGui::BeginDisabled(m_pairingWorkerBusy);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 160 * s, footerY));
            if (ui::button("New QR code##NewQr", ui::Icon::Refresh, ui::Appearance::Secondary, ImVec2(160 * s, 38 * s))) startWifiQrSession();
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
}

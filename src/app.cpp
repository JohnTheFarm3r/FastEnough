#include "app.h"
#include "companion.h"
#include "ui_widgets.h"
#include "ui_panels.h"
#include "local_copy.h"
#include "local_directory.h"
#include "backup_archive.h"
#include "imgui_internal.h"
#include "mcraw_local.h"
#include "mcraw_projfs.h"
#include "device_dokan.h"
#include "transfer_coverage.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <set>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <nlohmann/json.hpp>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")

// Safe UTF-8 path-to-string helper (avoids ANSI code page issues with .string())
static std::string pathToUtf8(const std::filesystem::path& p) {
    auto ws = p.wstring();
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), utf8.data(), len, nullptr, nullptr);
    return utf8;
}

// UTF-8 to wide string helper for Unicode-safe Win32 API calls
static std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), len);
    return wide;
}

// UTF-8 string to std::filesystem::path (via wide string to avoid ANSI codepage issues)
static std::filesystem::path toFsPath(const std::string& utf8) {
    return std::filesystem::path(toWide(utf8));
}

static constexpr const char* kBackupArchiveReferenceSeparator = "|fezip|";

struct TransferRateSample {
    double bytesPerSecond = 0.0;
    double etaSeconds = -1.0;
};

class TransferRateEstimator {
public:
    TransferRateSample update(uint64_t transferred, uint64_t total) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto now = std::chrono::steady_clock::now();
        const double interval = std::chrono::duration<double>(now - m_lastSampleTime).count();
        const double elapsed = std::chrono::duration<double>(now - m_started).count();
        if (interval > 2.0) {
            m_lastSampleTime = now;
            m_lastBytes = transferred;
            m_smoothedBytesPerSecond = 0.0;
            m_stableSamples = 0;
            return {};
        }
        if (interval >= 0.35 && transferred >= m_lastBytes) {
            const double instantaneous = (double)(transferred - m_lastBytes) / interval;
            if (m_smoothedBytesPerSecond <= 0.0) {
                m_smoothedBytesPerSecond = instantaneous;
                m_stableSamples = 0;
            } else {
                const double difference = std::abs(instantaneous - m_smoothedBytesPerSecond) /
                    std::max(1.0, m_smoothedBytesPerSecond);
                m_stableSamples = difference <= 0.25 ? m_stableSamples + 1 : 0;
                m_smoothedBytesPerSecond = m_smoothedBytesPerSecond * 0.70 + instantaneous * 0.30;
            }
            m_lastSampleTime = now;
            m_lastBytes = transferred;
        }

        TransferRateSample sample;
        sample.bytesPerSecond = m_smoothedBytesPerSecond;
        if (elapsed >= 2.0 && m_stableSamples >= 3 && m_smoothedBytesPerSecond > 0.0 && transferred < total)
            sample.etaSeconds = (double)(total - transferred) / m_smoothedBytesPerSecond;
        return sample;
    }

private:
    std::mutex m_mutex;
    std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point m_lastSampleTime = m_started;
    uint64_t m_lastBytes = 0;
    double m_smoothedBytesPerSecond = 0.0;
    int m_stableSamples = 0;
};

static std::string makeBackupArchiveReference(const std::string& archivePath, const std::string& prefix) {
    return archivePath + kBackupArchiveReferenceSeparator + prefix;
}

static bool splitBackupArchiveReference(const std::string& value, std::string& archivePath, std::string& prefix) {
    const auto position = value.find(kBackupArchiveReferenceSeparator);
    if (position == std::string::npos) return false;
    archivePath = value.substr(0, position);
    prefix = value.substr(position + strlen(kBackupArchiveReferenceSeparator));
    return !archivePath.empty() && !prefix.empty();
}

static bool readBackupMetadata(const std::string& reference, nlohmann::json& metadata, std::string& error) {
    std::string archivePath;
    std::string prefix;
    if (splitBackupArchiveReference(reference, archivePath, prefix)) {
        BackupArchive archive;
        if (!archive.open(toFsPath(archivePath), error)) return false;
        std::string data;
        if (!archive.readEntry(prefix + "/backup.json", data, error)) return false;
        metadata = nlohmann::json::parse(data, nullptr, false);
    } else {
        std::ifstream file(toFsPath(reference) / "backup.json", std::ios::binary);
        if (!file) {
            error = "Backup metadata is missing";
            return false;
        }
        metadata = nlohmann::json::parse(file, nullptr, false);
    }
    if (metadata.is_discarded()) {
        error = "Backup metadata is not valid JSON";
        return false;
    }
    return true;
}

static void appendPathSeparator(std::string& path, char sep) {
    if (path.empty()) {
        path.push_back(sep);
    } else if (path.back() != sep) {
        path.push_back(sep);
    }
}

static char lastCharOr(const std::string& value, char fallback = '\0') {
    return value.empty() ? fallback : value.back();
}

static void alignTextToCompactFrame() {
    ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset =
        ImGui::GetStyle().FramePadding.y * 0.62f;
}

static std::string sanitizeDeviceDisplayValue(std::string value) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && isSpace((unsigned char)value.front())) value.erase(value.begin());
    while (!value.empty() && isSpace((unsigned char)value.back())) value.pop_back();
    if (value.empty() || value.size() > 200 || value.find('|') != std::string::npos)
        return "";

    for (unsigned char c : value)
        if (c < 0x20) return "";

    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    const char* adbDiagnostics[] = {
        "adb.exe:",
        "adb:",
        "error:",
        "device '",
        "device \"",
        "not found",
        "offline",
        "unauthorized",
        "no devices/emulators",
        "more than one device/emulator",
        "failed to",
        "cannot connect",
        "* daemon",
    };
    for (const char* diagnostic : adbDiagnostics)
        if (lower.find(diagnostic) != std::string::npos) return "";

    return value;
}

static std::string normalizeCompareName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return name;
}

static bool fileNameMatchesFilter(const std::string& name, const std::string& filter) {
    if (filter.empty()) return true;
    if (filter.find_first_of("*?") != std::string::npos)
        return PathMatchSpecW(toWide(name).c_str(), toWide(filter).c_str()) == TRUE;

    std::string normalizedName = normalizeCompareName(name);
    std::string normalizedFilter = normalizeCompareName(filter);
    return normalizedName.find(normalizedFilter) != std::string::npos;
}

static bool panelContainsName(const FilePanel& panel, const std::string& name, int excludedIndex) {
    for (int i = 0; i < panel.entryCount(); ++i) {
        if (i == excludedIndex) continue;
        const std::string existingName = panel.entryName(i);
        if (panel.isAndroid) {
            if (existingName == name) return true;
        } else if (_stricmp(existingName.c_str(), name.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

static std::string makeUniquePanelName(
    const FilePanel& panel, const std::string& requestedName, int sourceIndex) {
    if (!panelContainsName(panel, requestedName, sourceIndex)) return requestedName;

    std::string base = requestedName;
    std::string extension;
    if (!panel.entryIsDir(sourceIndex)) {
        const size_t dot = requestedName.find_last_of('.');
        if (dot != std::string::npos && dot > 0) {
            base = requestedName.substr(0, dot);
            extension = requestedName.substr(dot);
        }
    }

    for (int suffix = 1; ; ++suffix) {
        std::string candidate = base + " (" + std::to_string(suffix) + ")" + extension;
        if (!panelContainsName(panel, candidate, sourceIndex)) return candidate;
    }
}

static std::string bytesToHex(const BYTE* data, DWORD len) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve((size_t)len * 2);
    for (DWORD i = 0; i < len; i++) {
        BYTE b = data[i];
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0f]);
    }
    return out;
}

static bool computeCompareLocalSha256(const std::string& path, std::string& outSha256, std::string& detail) {
    HANDLE hFile = CreateFileW(toWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        detail = "Could not read local file";
        return false;
    }

    FILE_IO_PRIORITY_HINT_INFO ph = { (PRIORITY_HINT)1 };
    SetFileInformationByHandle(hFile, FileIoPriorityHintInfo, &ph, sizeof(ph));

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        if (hash) CryptDestroyHash(hash);
        if (prov) CryptReleaseContext(prov, 0);
        CloseHandle(hFile);
        detail = "Could not start SHA-256";
        return false;
    }

    const DWORD bufSize = 4 * 1024 * 1024;
    std::vector<char> buf(bufSize);
    DWORD bytesRead = 0;
    BOOL readOk = TRUE;
    while ((readOk = ReadFile(hFile, buf.data(), bufSize, &bytesRead, nullptr)) && bytesRead > 0) {
        if (!CryptHashData(hash, (const BYTE*)buf.data(), bytesRead, 0)) {
            CryptDestroyHash(hash);
            CryptReleaseContext(prov, 0);
            CloseHandle(hFile);
            detail = "Could not update SHA-256";
            return false;
        }
    }
    CloseHandle(hFile);
    if (!readOk) {
        CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);
        detail = "Could not finish local hash";
        return false;
    }

    BYTE digest[32];
    DWORD digestLen = sizeof(digest);
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digestLen, 0) || digestLen != 32) {
        CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);
        detail = "Could not finalize SHA-256";
        return false;
    }

    outSha256 = bytesToHex(digest, digestLen);
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return true;
}

static uint64_t directorySizeBytes(const std::filesystem::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

static std::string backupJobStamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return ss.str();
}

static std::string formatBackupCreated(const std::string& created) {
    if (created.size() == 15 && created[8] == '-' &&
        std::all_of(created.begin(), created.begin() + 8, ::isdigit) &&
        std::all_of(created.begin() + 9, created.end(), ::isdigit)) {
        return created.substr(0, 4) + "-" + created.substr(4, 2) + "-" + created.substr(6, 2) +
               " " + created.substr(9, 2) + ":" + created.substr(11, 2) + ":" + created.substr(13, 2);
    }
    return created;
}

static std::vector<DetectedNic> enumerateNics() {
    std::vector<DetectedNic> result;
    ULONG bufLen = 16384;
    std::vector<BYTE> buf(bufLen);
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                      (PIP_ADAPTER_ADDRESSES)buf.data(), &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    (PIP_ADAPTER_ADDRESSES)buf.data(), &bufLen);
    }
    if (ret != NO_ERROR) return result;

    for (auto* adapter = (PIP_ADAPTER_ADDRESSES)buf.data(); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD &&
            adapter->IfType != IF_TYPE_IEEE80211) continue;

        for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            sockaddr_in* sa = (sockaddr_in*)ua->Address.lpSockaddr;
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));
            std::string ip(ipStr);
            if (ip == "127.0.0.1") continue;

            DetectedNic nic;
            int wlen = (int)wcslen(adapter->FriendlyName);
            int mbLen = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, wlen, nullptr, 0, nullptr, nullptr);
            nic.adapterName.resize(mbLen);
            WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, wlen, nic.adapterName.data(), mbLen, nullptr, nullptr);
            nic.ipAddress = ip;
            nic.speed = adapter->TransmitLinkSpeed;
            wlen = (int)wcslen(adapter->Description);
            mbLen = WideCharToMultiByte(CP_UTF8, 0, adapter->Description, wlen, nullptr, 0, nullptr, nullptr);
            nic.description.resize(mbLen);
            WideCharToMultiByte(CP_UTF8, 0, adapter->Description, wlen, nic.description.data(), mbLen, nullptr, nullptr);
            result.push_back(std::move(nic));
        }
    }
    return result;
}

class SimpleDropSource : public IDropSource {
    LONG m_ref = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        if (fEscapePressed) return DRAGDROP_S_CANCEL;
        if (!(grfKeyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

class SimpleDataObject : public IDataObject {
    LONG m_ref = 1;
    struct Entry { FORMATETC fmt; STGMEDIUM stg; };
    std::vector<Entry> m_entries;

    static HGLOBAL dupGlobal(HGLOBAL src) {
        if (!src) return nullptr;
        SIZE_T sz = GlobalSize(src);
        HGLOBAL dst = GlobalAlloc(GHND, sz);
        if (!dst) return nullptr;
        void* s = GlobalLock(src); void* d = GlobalLock(dst);
        memcpy(d, s, sz);
        GlobalUnlock(dst); GlobalUnlock(src);
        return dst;
    }
    static bool fmtMatch(const FORMATETC& a, const FORMATETC& b) {
        return a.cfFormat == b.cfFormat && a.dwAspect == b.dwAspect && a.lindex == b.lindex;
    }
public:
    ~SimpleDataObject() {
        for (auto& e : m_entries) ReleaseStgMedium(&e.stg);
    }

    void setHDrop(const std::vector<std::wstring>& files) {
        size_t totalChars = 0;
        for (auto& f : files) totalChars += f.size() + 1;
        totalChars += 1;
        size_t bufSize = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);

        HGLOBAL hGlobal = GlobalAlloc(GHND, bufSize);
        if (!hGlobal) return;
        auto* df = (DROPFILES*)GlobalLock(hGlobal);
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        wchar_t* dst = (wchar_t*)((char*)df + sizeof(DROPFILES));
        for (auto& f : files) {
            memcpy(dst, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
            dst += f.size() + 1;
        }
        *dst = L'\0';
        GlobalUnlock(hGlobal);

        Entry e{};
        e.fmt.cfFormat = CF_HDROP;
        e.fmt.dwAspect = DVASPECT_CONTENT;
        e.fmt.lindex = -1;
        e.fmt.tymed = TYMED_HGLOBAL;
        e.stg.tymed = TYMED_HGLOBAL;
        e.stg.hGlobal = hGlobal;
        m_entries.push_back(e);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fmt, STGMEDIUM* stg) override {
        for (auto& e : m_entries) {
            if (fmtMatch(e.fmt, *fmt) && (e.fmt.tymed & fmt->tymed)) {
                stg->tymed = e.stg.tymed;
                stg->pUnkForRelease = nullptr;
                if (e.stg.tymed == TYMED_HGLOBAL) {
                    stg->hGlobal = dupGlobal(e.stg.hGlobal);
                    if (!stg->hGlobal) return E_OUTOFMEMORY;
                    return S_OK;
                } else if (e.stg.tymed == TYMED_ISTREAM) {
                    stg->pstm = e.stg.pstm;
                    if (stg->pstm) stg->pstm->AddRef();
                    return S_OK;
                } else if (e.stg.tymed == TYMED_GDI) {
                    stg->hBitmap = e.stg.hBitmap;
                    return S_OK;
                }
                return DV_E_TYMED;
            }
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fmt) override {
        for (auto& e : m_entries) {
            if (fmtMatch(e.fmt, *fmt) && (e.fmt.tymed & fmt->tymed)) return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        out->ptd = nullptr; return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* fmt, STGMEDIUM* stg, BOOL fRelease) override {
        if (!fmt || !stg) return E_INVALIDARG;
        // Replace existing entry of same format
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (fmtMatch(it->fmt, *fmt)) {
                ReleaseStgMedium(&it->stg);
                m_entries.erase(it);
                break;
            }
        }
        Entry e{};
        e.fmt = *fmt;
        if (fRelease) {
            e.stg = *stg;
        } else {
            // Copy the medium
            e.stg.tymed = stg->tymed;
            e.stg.pUnkForRelease = nullptr;
            if (stg->tymed == TYMED_HGLOBAL) {
                e.stg.hGlobal = dupGlobal(stg->hGlobal);
                if (!e.stg.hGlobal) return E_OUTOFMEMORY;
            } else if (stg->tymed == TYMED_ISTREAM) {
                e.stg.pstm = stg->pstm;
                if (e.stg.pstm) e.stg.pstm->AddRef();
            } else {
                e.stg = *stg;
            }
        }
        m_entries.push_back(e);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** ppEnum) override {
        if (dir != DATADIR_GET) { *ppEnum = nullptr; return E_NOTIMPL; }
        std::vector<FORMATETC> fmts;
        for (auto& e : m_entries) fmts.push_back(e.fmt);
        return SHCreateStdEnumFmtEtc((UINT)fmts.size(), fmts.empty() ? nullptr : fmts.data(), ppEnum);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

// Shared state for AdbPullStream — held via shared_ptr so the producer thread
// can outlive the IStream object on cancel without us blocking IStream::Release.
struct AdbStreamState {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::vector<char>> queue;
    size_t queueBytes = 0;
    size_t frontConsumed = 0;
    uint64_t pos = 0;
    bool eof = false;
    bool aborted = false;
};

// IStream that streams a remote file via a producer thread running
// DeviceClient::readRangeStreaming. State is shared with the producer so
// destruction is non-blocking — abort flag is set, the producer is detached,
// and it cleans up when the device finishes draining.
class AdbPullStream : public IStream {
    LONG m_ref = 1;
    std::string m_remotePath;
    DeviceClient* m_dev = nullptr;
    uint64_t m_size = 0;
    std::wstring m_name;
    std::shared_ptr<AdbStreamState> m_state;
    bool m_producerStarted = false;
    std::atomic<int>* m_inflightCounter = nullptr;

    static constexpr size_t kMaxQueue = 128 * 1024 * 1024; // 128 MiB read-ahead cap

    // Caller holds m_state->mtx. Aborts any prior producer (detached) and starts
    // a new one at startOffset. The new producer captures a fresh shared_ptr to
    // its own state — restarting effectively replaces m_state.
    void restartProducerLocked(std::unique_lock<std::mutex>& lk, uint64_t startOffset) {
        // Signal the current producer to abort and detach it; it will finish
        // draining on its own and then drop its shared_ptr<State>.
        m_state->aborted = true;
        m_state->cv.notify_all();
        lk.unlock();

        // New, independent state for the new producer / consumer pair.
        auto fresh = std::make_shared<AdbStreamState>();
        fresh->pos = startOffset;
        m_state = fresh;
        lk = std::unique_lock<std::mutex>(m_state->mtx);
        m_producerStarted = true;
        if (startOffset >= m_size) { m_state->eof = true; return; }

        std::shared_ptr<AdbStreamState> stateRef = m_state;
        DeviceClient* dev = m_dev;
        std::string remote = m_remotePath;
        uint64_t length = m_size - startOffset;
        std::atomic<int>* inflight = m_inflightCounter;
        std::thread([stateRef, dev, remote, startOffset, length, inflight]() {
            if (inflight) inflight->fetch_add(1);
            dev->readRangeStreaming(remote, startOffset, length,
                [stateRef](const void* data, uint32_t len, uint64_t /*offset*/) -> bool {
                    // Apply backpressure first (mutex briefly held).
                    {
                        std::unique_lock<std::mutex> lk(stateRef->mtx);
                        stateRef->cv.wait(lk, [&] {
                            return stateRef->aborted || stateRef->queueBytes < kMaxQueue;
                        });
                        if (stateRef->aborted) return false;
                    }
                    // Copy out of the device's reusable buffer outside the lock so
                    // the consumer is not blocked during the per-chunk memcpy.
                    std::vector<char> chunk((const char*)data, (const char*)data + len);
                    {
                        std::lock_guard<std::mutex> lk(stateRef->mtx);
                        if (stateRef->aborted) return false;
                        stateRef->queue.push_back(std::move(chunk));
                        stateRef->queueBytes += len;
                        stateRef->cv.notify_all();
                    }
                    return true;
                });
            {
                std::lock_guard<std::mutex> lk(stateRef->mtx);
                stateRef->eof = true;
                stateRef->cv.notify_all();
            }
            if (inflight) inflight->fetch_sub(1);
        }).detach();
    }

public:
    AdbPullStream(std::string remotePath, DeviceClient* dev, uint64_t size, std::wstring name,
                  std::atomic<int>* inflight)
        : m_remotePath(std::move(remotePath)), m_dev(dev), m_size(size), m_name(std::move(name)),
          m_state(std::make_shared<AdbStreamState>()), m_inflightCounter(inflight) {}
    ~AdbPullStream() {
        // Tell the (possibly running) producer to abort; do NOT join — we want
        // IStream::Release to return immediately so Explorer's cancel is fast.
        std::lock_guard<std::mutex> lk(m_state->mtx);
        m_state->aborted = true;
        m_state->cv.notify_all();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (pcbRead) *pcbRead = 0;
        std::unique_lock<std::mutex> lk(m_state->mtx);
        if (!m_producerStarted) restartProducerLocked(lk, m_state->pos);
        auto* st = m_state.get();
        if (st->pos >= m_size) return S_FALSE;
        ULONG written = 0;
        while (written < cb && st->pos < m_size) {
            if (st->queue.empty()) {
                if (st->eof) break;
                st->cv.wait(lk, [&] { return !st->queue.empty() || st->eof || st->aborted; });
                if (st->aborted) return E_FAIL;
                continue;
            }
            auto& front = st->queue.front();
            size_t avail = front.size() - st->frontConsumed;
            size_t want = cb - written;
            size_t take = (avail < want) ? avail : want;
            memcpy((char*)pv + written, front.data() + st->frontConsumed, take);
            written += (ULONG)take;
            st->frontConsumed += take;
            st->pos += take;
            st->queueBytes -= take;
            if (st->frontConsumed == front.size()) {
                st->queue.pop_front();
                st->frontConsumed = 0;
            }
            st->cv.notify_all();
        }
        if (pcbRead) *pcbRead = written;
        if (written == 0) return S_FALSE;
        return (written < cb) ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER off, DWORD origin, ULARGE_INTEGER* newPos) override {
        std::unique_lock<std::mutex> lk(m_state->mtx);
        auto* st = m_state.get();
        int64_t base = 0;
        if (origin == STREAM_SEEK_SET) base = 0;
        else if (origin == STREAM_SEEK_CUR) base = (int64_t)st->pos;
        else if (origin == STREAM_SEEK_END) base = (int64_t)m_size;
        else return STG_E_INVALIDFUNCTION;
        int64_t np = base + off.QuadPart;
        if (np < 0) return STG_E_INVALIDFUNCTION;
        uint64_t want = (uint64_t)np;
        if (want == st->pos) {
            if (newPos) newPos->QuadPart = st->pos;
            return S_OK;
        }
        if (want > st->pos && (want - st->pos) <= st->queueBytes) {
            uint64_t skip = want - st->pos;
            while (skip > 0 && !st->queue.empty()) {
                auto& front = st->queue.front();
                size_t avail = front.size() - st->frontConsumed;
                size_t take = (size_t)((skip < avail) ? skip : avail);
                st->frontConsumed += take;
                st->queueBytes -= take;
                skip -= take;
                st->pos += take;
                if (st->frontConsumed == front.size()) {
                    st->queue.pop_front();
                    st->frontConsumed = 0;
                }
            }
            st->cv.notify_all();
            if (newPos) newPos->QuadPart = st->pos;
            return S_OK;
        }
        // Restart producer at new offset (replaces m_state).
        restartProducerLocked(lk, want);
        if (newPos) newPos->QuadPart = m_state->pos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* dst, ULARGE_INTEGER cb,
                                     ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
        ULARGE_INTEGER tot{}; ULARGE_INTEGER w{};
        std::vector<char> buf(1 << 20); // 1 MiB
        while (cb.QuadPart > 0) {
            ULONG to = (ULONG)((cb.QuadPart < buf.size()) ? cb.QuadPart : buf.size());
            ULONG got = 0;
            HRESULT hr = Read(buf.data(), to, &got);
            if (FAILED(hr)) return hr;
            if (got == 0) break;
            tot.QuadPart += got;
            ULONG ww = 0;
            if (dst) dst->Write(buf.data(), got, &ww);
            w.QuadPart += ww;
            cb.QuadPart -= got;
            if (got < to) break;
        }
        if (read) *read = tot;
        if (written) *written = w;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* p, DWORD flags) override {
        if (!p) return E_POINTER;
        memset(p, 0, sizeof(*p));
        if (!(flags & STATFLAG_NONAME)) {
            size_t nb = (m_name.size() + 1) * sizeof(wchar_t);
            p->pwcsName = (LPOLESTR)CoTaskMemAlloc(nb);
            if (p->pwcsName) memcpy(p->pwcsName, m_name.c_str(), nb);
        }
        p->type = STGTY_STREAM;
        p->cbSize.QuadPart = m_size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
};

// Register virtual files (FILEGROUPDESCRIPTORW + per-index FILECONTENTS streams)
// on the data object. Streams ownership is transferred (refcount stays at the
// caller's, we AddRef when we store).
static void setVirtualFilesOnDataObject(SimpleDataObject* obj,
                                        const std::vector<std::wstring>& names,
                                        const std::vector<uint64_t>& sizes,
                                        const std::vector<IStream*>& streams) {
    UINT cfDesc = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    UINT cfCont = RegisterClipboardFormatW(CFSTR_FILECONTENTS);

    // Build FILEGROUPDESCRIPTORW
    size_t n = names.size();
    size_t bufSize = sizeof(FILEGROUPDESCRIPTORW) + (n - 1) * sizeof(FILEDESCRIPTORW);
    HGLOBAL hg = GlobalAlloc(GHND, bufSize);
    if (hg) {
        auto* fgd = (FILEGROUPDESCRIPTORW*)GlobalLock(hg);
        fgd->cItems = (UINT)n;
        for (size_t i = 0; i < n; i++) {
            FILEDESCRIPTORW& fd = fgd->fgd[i];
            fd.dwFlags = FD_FILESIZE | FD_PROGRESSUI;
            fd.nFileSizeLow = (DWORD)(sizes[i] & 0xFFFFFFFF);
            fd.nFileSizeHigh = (DWORD)(sizes[i] >> 32);
            const std::wstring& nm = names[i];
            size_t copy = nm.size() < (MAX_PATH - 1) ? nm.size() : (MAX_PATH - 1);
            memcpy(fd.cFileName, nm.c_str(), copy * sizeof(wchar_t));
            fd.cFileName[copy] = L'\0';
        }
        GlobalUnlock(hg);

        FORMATETC fmt{};
        fmt.cfFormat = (CLIPFORMAT)cfDesc;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = -1;
        fmt.tymed = TYMED_HGLOBAL;
        STGMEDIUM stg{};
        stg.tymed = TYMED_HGLOBAL;
        stg.hGlobal = hg;
        obj->SetData(&fmt, &stg, TRUE);
    }

    // Per-index FILECONTENTS
    for (size_t i = 0; i < n; i++) {
        FORMATETC fmt{};
        fmt.cfFormat = (CLIPFORMAT)cfCont;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = (LONG)i;
        fmt.tymed = TYMED_ISTREAM;
        STGMEDIUM stg{};
        stg.tymed = TYMED_ISTREAM;
        stg.pstm = streams[i];
        if (stg.pstm) stg.pstm->AddRef();
        obj->SetData(&fmt, &stg, TRUE);
    }
}

// Build a small RGB bitmap with drag label text. Caller owns the returned HBITMAP.
static HBITMAP createDragLabelBitmap(const std::wstring& label, COLORREF& outColorKey) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    LOGFONTW lf{};
    GetObjectW(font, sizeof(lf), &lf);
    lf.lfHeight = -16;
    HFONT bigFont = CreateFontIndirectW(&lf);

    HFONT oldFont = (HFONT)SelectObject(mem, bigFont);
    RECT measure{0,0,0,0};
    DrawTextW(mem, label.c_str(), -1, &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int w = measure.right + 16;
    int h = measure.bottom + 8;
    if (w < 64) w = 64;
    if (h < 24) h = 24;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

    // Magenta color key background
    outColorKey = RGB(255, 0, 255);
    HBRUSH bgBrush = CreateSolidBrush(outColorKey);
    RECT full{0,0,w,h};
    FillRect(mem, &full, bgBrush);
    DeleteObject(bgBrush);

    // Rounded panel
    HBRUSH panelBrush = CreateSolidBrush(RGB(30, 50, 80));
    HPEN panelPen = CreatePen(PS_SOLID, 1, RGB(80, 130, 200));
    HBRUSH oldBrush = (HBRUSH)SelectObject(mem, panelBrush);
    HPEN oldPen = (HPEN)SelectObject(mem, panelPen);
    RoundRect(mem, 0, 0, w, h, 8, 8);
    SelectObject(mem, oldBrush);
    SelectObject(mem, oldPen);
    DeleteObject(panelBrush);
    DeleteObject(panelPen);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(220, 235, 255));
    RECT textRect{8, 4, w - 8, h - 4};
    DrawTextW(mem, label.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bigFont);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return bmp;
}

// Attach a label drag image to the data object via IDragSourceHelper2.
static void attachDragImage(IDataObject* dataObj, const std::wstring& label) {
    IDragSourceHelper2* dsh2 = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dsh2)))) return;
    SHDRAGIMAGE img{};
    COLORREF key = 0;
    img.hbmpDragImage = createDragLabelBitmap(label, key);
    if (img.hbmpDragImage) {
        BITMAP bm{};
        GetObject(img.hbmpDragImage, sizeof(bm), &bm);
        img.sizeDragImage.cx = bm.bmWidth;
        img.sizeDragImage.cy = bm.bmHeight;
        img.ptOffset.x = bm.bmWidth / 2;
        img.ptOffset.y = bm.bmHeight / 2;
        img.crColorKey = key;
        if (FAILED(dsh2->InitializeFromBitmap(&img, dataObj))) {
            DeleteObject(img.hbmpDragImage);
        }
    }
    dsh2->Release();
}

static std::wstring buildDragLabel(const std::vector<std::wstring>& items) {
    if (items.size() == 1) {
        const std::wstring& p = items[0];
        size_t slash = p.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? p : p.substr(slash + 1);
    }
    return std::to_wstring(items.size()) + L" files";
}

void App::performOleDragDrop(const std::vector<std::string>& filePaths) {
    std::vector<std::wstring> wPaths;
    for (auto& p : filePaths) {
        int len = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, nullptr, 0);
        std::wstring wp(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, wp.data(), len);
        wPaths.push_back(std::move(wp));
    }
    auto* dataObj = new SimpleDataObject();
    dataObj->setHDrop(wPaths);
    auto* dropSrc = new SimpleDropSource();

    attachDragImage(dataObj, buildDragLabel(wPaths));

    DWORD effect = 0;
    DoDragDrop(dataObj, dropSrc, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);

    // If Explorer performed a move, delete the source files
    if (effect == DROPEFFECT_MOVE) {
        for (auto& p : filePaths) {
            try { std::filesystem::remove(toFsPath(p)); } catch (...) {}
        }
    }

    // Clear selection on the source panel so it doesn't look stuck after the drop.
    if (m_dragSourcePanel) {
        m_dragSourcePanel->selectedIndices.clear();
        m_dragSourcePanel->focusedIndex = -1;
    }
    m_dragSourcePanel = nullptr;

    // Refresh panels after any drag-out (file may have been moved/copied)
    m_leftPanel.needsRefresh = true;
    m_rightPanel.needsRefresh = true;

    dropSrc->Release();
    dataObj->Release();
}

void App::performAndroidDragOut(FilePanel* src) {
    if (!src || !src->isAndroid) return;
    DeviceClient* dev = &deviceFor(*src);
    if (!dev->isServerRunning()) return;

    std::vector<std::string> remotePaths;
    std::vector<std::wstring> wNames;
    std::vector<uint64_t> sizes;
    for (int idx : src->selectedIndices) {
        if (!src->validIndex(idx)) continue;
        if (src->entryIsDir(idx)) continue;
        std::string name = src->entryName(idx);
        std::string remotePath = src->currentPath;
        appendPathSeparator(remotePath, '/');
        remotePath += name;
        remotePaths.push_back(remotePath);
        wNames.push_back(toWide(name));
        sizes.push_back(src->entrySize(idx));
    }
    if (remotePaths.empty()) return;

    std::string serial = dev->connectedSerial();
    FilePanel* srcPanel = src;
    DWORD uiThreadId = GetCurrentThreadId();

    // Run DoDragDrop on a dedicated STA thread so Explorer's IStream::Read calls
    // (which may pull large files via ADB) don't block the UI thread.
    std::thread worker([this, dev, serial, remotePaths, wNames, sizes, srcPanel, uiThreadId]() {
        OleInitialize(nullptr);
        // Share input state with the UI thread so DoDragDrop sees the live mouse
        // button state and can render the drag image as the user moves the cursor.
        AttachThreadInput(GetCurrentThreadId(), uiThreadId, TRUE);

        auto* dataObj = new SimpleDataObject();
        auto* dropSrc = new SimpleDropSource();

        std::vector<IStream*> streams;
        streams.reserve(remotePaths.size());
        for (size_t i = 0; i < remotePaths.size(); i++) {
            streams.push_back(new AdbPullStream(remotePaths[i], dev, sizes[i], wNames[i],
                                                &m_androidDragInflight));
        }
        setVirtualFilesOnDataObject(dataObj, wNames, sizes, streams);
        for (auto* s : streams) s->Release();

        attachDragImage(dataObj, buildDragLabel(wNames));

        DWORD effect = 0;
        DoDragDrop(dataObj, dropSrc, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);

        dropSrc->Release();
        dataObj->Release();

        if (srcPanel) {
            srcPanel->selectedIndices.clear();
            srcPanel->focusedIndex = -1;
        }
        m_leftPanel.needsRefresh = true;
        m_rightPanel.needsRefresh = true;
        m_dragSourcePanel = nullptr;

        AttachThreadInput(GetCurrentThreadId(), uiThreadId, FALSE);
        OleUninitialize();
    });
    worker.detach();
}

static bool probeTcpEndpoint(const std::string& ip, int port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) return false;
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET) return false;
    unsigned long nonblocking = 1;
    bool connected = false;
    if (ioctlsocket(probe, FIONBIO, &nonblocking) == 0) {
        int result = connect(probe, (sockaddr*)&address, sizeof(address));
        if (result == 0) connected = true;
        else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable, failed;
            FD_ZERO(&writable); FD_SET(probe, &writable);
            FD_ZERO(&failed); FD_SET(probe, &failed);
            timeval timeout{1, 0};
            int error = 0, size = sizeof(error);
            connected = select(0, nullptr, &writable, &failed, &timeout) > 0 &&
                FD_ISSET(probe, &writable) &&
                getsockopt(probe, SOL_SOCKET, SO_ERROR, (char*)&error, &size) == 0 && error == 0;
        }
    }
    closesocket(probe);
    return connected;
}

static bool isWifiSerial(const std::string& serial) {
    return isWirelessAdbSerial(serial);
}

static std::string companionChannelName(const std::string& kind) {
    if (kind == "usb") return "USB (Tethering)";
    if (kind == "ethernet") return "Ethernet (App)";
    return "WiFi (App)";
}

static std::string companionPrimaryName(const DeviceClient& base) {
    for (const auto& endpoint : companion::endpointsFor(companion::idFromSerial(base.connectedSerial())))
        if (endpoint.host == base.connectedHost()) return companionChannelName(endpoint.kind);
    return base.isDirectConnection() ? "WiFi (App)" : "USB (App)";
}

// Extra endpoints for parallel transfers to a companion device. USB links fill
// the USB pipe count and network links fill the WiFi pipe count; the primary
// connection already counts as one of its kind.
static std::vector<CompanionEndpoint> companionChannelTargets(const DeviceClient& base, int usbPipes,
                                                              int wifiPipes, int maxTotal) {
    std::vector<CompanionEndpoint> usb, network, out;
    for (const auto& endpoint : companion::endpointsFor(companion::idFromSerial(base.connectedSerial())))
        (endpoint.isUsb() ? usb : network).push_back(endpoint);
    const bool primaryUsb = !base.isDirectConnection();
    int usbCount = primaryUsb ? 1 : 0;
    int networkCount = primaryUsb ? 0 : 1;
    for (size_t i = 0; !usb.empty() && usbCount < usbPipes && (int)out.size() + 1 < maxTotal; ++i, ++usbCount)
        out.push_back(usb[i % usb.size()]);
    for (size_t i = 0; !network.empty() && networkCount < wifiPipes && (int)out.size() + 1 < maxTotal; ++i, ++networkCount)
        out.push_back(network[i % network.size()]);
    return out;
}

static bool endsWithCI(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return _stricmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

static std::string getPrefsPath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::string dir = std::string(path) + "\\FastEnough";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\preferences.ini";
    }
    return "";
}

void AppPreferences::save() {
    std::string path = getPrefsPath();
    if (path.empty()) return;
    std::ofstream f(path);
    if (!f) return;
    f << "[Preferences]\n";
    f << "paneSplit=" << paneSplit << "\n";
    f << "compactRows=" << compactRows << "\n";
    f << "transferDockExpanded=" << transferDockExpanded << "\n";
    f << "restartAdbOnLaunch=" << (restartAdbOnLaunch ? 1 : 0) << "\n";
    f << "enableCrcVerification=" << (enableCrcVerification ? 1 : 0) << "\n";
    f << "autoDismissTransfer=" << (autoDismissTransfer ? 1 : 0) << "\n";
    f << "wifiAutoConnect=" << (wifiAutoConnect ? 1 : 0) << "\n";
    f << "confirmOnClose=" << (confirmOnClose ? 1 : 0) << "\n";
    f << "killAdbOnClose=" << (killAdbOnClose ? 1 : 0) << "\n";
    for (auto& w : savedWifiDevices) {
        f << "wifiDevice=" << w.serial << "|" << w.wifiIp << "|" << w.port << "|" << (w.autoConnect ? 1 : 0) << "|" << w.model << "\n";
        if (w.wirelessDebugging) f << "wifiPairing=" << w.serial << "|" << w.pairingGuid << "\n";
    }
    f << "enableMultiNic=" << (enableMultiNic ? 1 : 0) << "\n";
    f << "pipeSettingsV2=1\n";
    f << "usbPipeCount=" << usbPipeCount << "\n";
    f << "wifiPipeCount=" << wifiPipeCount << "\n";
    for (const auto& pipes : devicePipePreferences) {
        if (!pipes.serial.empty())
            f << "devicePipes=" << pipes.serial << "|" << pipes.usbPipeCount << "|" << pipes.wifiPipeCount << "\n";
    }
    for (auto& r : rootDevices) {
        if (!r.serial.empty())
            f << "rootDevice=" << r.serial << "|" << (r.enabled ? 1 : 0) << "\n";
    }
    for (const auto& identity : deviceSessionIdentities)
        if (!identity.empty()) f << "deviceSession=" << identity << "\n";
    for (auto& fav : favoritePaths) {
        f << "favoritePath=" << (fav.isAndroid ? 1 : 0) << "|" << fav.deviceSlot << "|"
          << fav.label << "|" << fav.path << "\n";
    }
    for (auto& nb : multiNicBindings) {
        f << "nicBinding=" << nb.adapterName << "|" << nb.localIp << "|" << (nb.enabled ? 1 : 0) << "\n";
    }
}

bool AppPreferences::rootEnabledForSerial(const std::string& serial) const {
    if (serial.empty()) return false;
    for (const auto& r : rootDevices)
        if (r.serial == serial) return r.enabled;
    return false;
}

void AppPreferences::setRootEnabledForSerial(const std::string& serial, bool enabled) {
    if (serial.empty()) return;
    for (auto& r : rootDevices) {
        if (r.serial == serial) {
            r.enabled = enabled;
            return;
        }
    }
    rootDevices.push_back({serial, enabled});
}

DevicePipePreference& AppPreferences::pipePreferencesFor(const std::string& serial) {
    std::string key = serial;
    for (const auto& saved : savedWifiDevices) {
        std::string wifiSerial = saved.wifiIp.empty() ? "" : saved.wifiIp + ":" + std::to_string(saved.port);
        if (!saved.serial.empty() && (saved.serial == serial || wifiSerial == serial)) {
            key = saved.serial;
            break;
        }
    }
    auto it = std::find_if(devicePipePreferences.begin(), devicePipePreferences.end(),
        [&](const DevicePipePreference& pipes) { return pipes.serial == key; });
    if (it != devicePipePreferences.end()) return *it;
    devicePipePreferences.push_back({key, usbPipeCount, wifiPipeCount});
    return devicePipePreferences.back();
}

DevicePipePreference AppPreferences::pipePreferencesFor(const std::string& serial) const {
    std::string key = serial;
    for (const auto& saved : savedWifiDevices) {
        std::string wifiSerial = saved.wifiIp.empty() ? "" : saved.wifiIp + ":" + std::to_string(saved.port);
        if (!saved.serial.empty() && (saved.serial == serial || wifiSerial == serial)) {
            key = saved.serial;
            break;
        }
    }
    auto it = std::find_if(devicePipePreferences.begin(), devicePipePreferences.end(),
        [&](const DevicePipePreference& pipes) { return pipes.serial == key; });
    return it != devicePipePreferences.end() ? *it : DevicePipePreference{key, usbPipeCount, wifiPipeCount};
}

void AppPreferences::load() {
    std::string path = getPrefsPath();
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    bool pipeSettingsV2 = false;
    bool repairedSavedWifiName = false;
    while (std::getline(f, line)) {
        if (line.find("paneSplit=") == 0) {
            try { paneSplit = std::clamp(std::stof(line.substr(10)), 0.3f, 0.7f); } catch (...) { paneSplit = 0.5f; }
            if (!std::isfinite(paneSplit)) paneSplit = 0.5f;
        } else if (line.find("compactRows=") == 0)
            compactRows = lastCharOr(line) == '1';
        else if (line.find("transferDockExpanded=") == 0)
            transferDockExpanded = lastCharOr(line) == '1';
        else if (line.find("restartAdbOnLaunch=") == 0)
            restartAdbOnLaunch = (lastCharOr(line) == '1');
        else if (line.find("enableCrcVerification=") == 0)
            enableCrcVerification = (lastCharOr(line) == '1');
        else if (line.find("autoDismissTransfer=") == 0)
            autoDismissTransfer = (lastCharOr(line) == '1');
        else if (line.find("wifiAutoConnect=") == 0)
            wifiAutoConnect = (lastCharOr(line) == '1');
        else if (line.find("confirmOnClose=") == 0)
            confirmOnClose = (lastCharOr(line) == '1');
        else if (line.find("killAdbOnClose=") == 0)
            killAdbOnClose = (lastCharOr(line) == '1');
        else if (line.find("wifiDevice=") == 0) {
            std::string val = line.substr(11);
            SavedWifiDevice w;
            auto p1 = val.find('|'); if (p1 == std::string::npos) continue;
            auto p2 = val.find('|', p1+1); if (p2 == std::string::npos) continue;
            auto p3 = val.find('|', p2+1); if (p3 == std::string::npos) continue;
            w.serial = val.substr(0, p1);
            w.wifiIp = val.substr(p1+1, p2-p1-1);
            w.port = std::stoi(val.substr(p2+1, p3-p2-1));
            auto p4 = val.find('|', p3+1);
            if (p4 != std::string::npos) {
                w.autoConnect = (val[p3+1] == '1');
                w.model = val.substr(p4+1);
                std::string sanitizedModel = sanitizeDeviceDisplayValue(w.model);
                if (sanitizedModel != w.model) {
                    w.model = std::move(sanitizedModel);
                    repairedSavedWifiName = true;
                }
            } else {
                w.autoConnect = (lastCharOr(val) == '1');
            }
            savedWifiDevices.push_back(std::move(w));
        }
        else if (line.find("wifiPairing=") == 0) {
            const auto split = line.find('|', 12);
            if (split == std::string::npos) continue;
            const auto serial = line.substr(12, split - 12);
            for (auto& saved : savedWifiDevices) {
                if (saved.serial != serial) continue;
                saved.wirelessDebugging = true;
                saved.pairingGuid = line.substr(split + 1);
            }
        }
        else if (line.find("enableMultiNic=") == 0)
            enableMultiNic = (lastCharOr(line) == '1');
        else if (line.find("pipeSettingsV2=") == 0)
            pipeSettingsV2 = (lastCharOr(line) == '1');
        else if (line.find("usbPipeCount=") == 0)
            usbPipeCount = std::clamp(std::stoi(line.substr(13)), 1, 4);
        else if (line.find("wifiPipeCount=") == 0)
            wifiPipeCount = std::clamp(std::stoi(line.substr(14)), 1, 4);
        else if (line.find("devicePipes=") == 0) {
            std::string value = line.substr(12);
            size_t first = value.find('|'), second = first == std::string::npos ? std::string::npos : value.find('|', first + 1);
            if (first != std::string::npos && second != std::string::npos) {
                try {
                    devicePipePreferences.push_back({value.substr(0, first),
                        std::clamp(std::stoi(value.substr(first + 1, second - first - 1)), 1, 4),
                        std::clamp(std::stoi(value.substr(second + 1)), 1, 4)});
                } catch (...) {}
            }
        }
        else if (line.find("useRoot=") == 0) {
            // Legacy global root setting intentionally ignored. Root is now per device.
        }
        else if (line.find("rootDevice=") == 0) {
            std::string val = line.substr(11);
            auto p1 = val.find('|'); if (p1 == std::string::npos) continue;
            RootDevicePreference r;
            r.serial = val.substr(0, p1);
            r.enabled = (val.substr(p1 + 1) == "1");
            if (!r.serial.empty()) rootDevices.push_back(std::move(r));
        }
        else if (line.find("deviceSession=") == 0) {
            if (line.size() > 14) deviceSessionIdentities.push_back(line.substr(14));
        }
        else if (line.find("favoritePath=") == 0) {
            std::string val = line.substr(13);
            FavoritePath fav;
            auto p1 = val.find('|'); if (p1 == std::string::npos) continue;
            auto p2 = val.find('|', p1 + 1); if (p2 == std::string::npos) continue;
            auto p3 = val.find('|', p2 + 1); if (p3 == std::string::npos) continue;
            fav.isAndroid = (val.substr(0, p1) == "1");
            try { fav.deviceSlot = std::max(0, std::stoi(val.substr(p1 + 1, p2 - p1 - 1))); }
            catch (...) { fav.deviceSlot = 0; }
            fav.label = val.substr(p2 + 1, p3 - p2 - 1);
            fav.path = val.substr(p3 + 1);
            if (!fav.path.empty()) favoritePaths.push_back(std::move(fav));
        }
        else if (line.find("nicBinding=") == 0) {
            std::string val = line.substr(11);
            NicBinding nb;
            auto p1 = val.find('|'); if (p1 == std::string::npos) continue;
            auto p2 = val.find('|', p1+1); if (p2 == std::string::npos) continue;
            nb.adapterName = val.substr(0, p1);
            nb.localIp = val.substr(p1+1, p2-p1-1);
            nb.enabled = (val[p2+1] == '1');
            multiNicBindings.push_back(std::move(nb));
        }
    }

    // Older builds only allowed up to 2 pipes. Treat the old maximum as
    // "high speed" and migrate it to the new maximum once.
    if (!pipeSettingsV2) {
        if (usbPipeCount == 2) usbPipeCount = 4;
        if (wifiPipeCount == 2) wifiPipeCount = 4;
    }
    if (repairedSavedWifiName) {
        f.close();
        save();
    }
}

std::string AppTheme::getThemePath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::string dir = std::string(path) + "\\FastEnough";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\theme.ini";
    }
    return "";
}

void AppTheme::save() {
    std::string path = getThemePath();
    if (path.empty()) return;
    std::ofstream f(path);
    if (!f) return;
    f << "[Theme]\n";
    f << "userScale=" << userScale << "\n";
    f << "fontScale=" << fontScale << "\n";
    f << "mode=" << mode << "\n";
    f << "gradientPrimary=" << gradientPrimary.x << "," << gradientPrimary.y << ","
      << gradientPrimary.z << "," << gradientPrimary.w << "\n";
    f << "gradientSecondary=" << gradientSecondary.x << "," << gradientSecondary.y << ","
      << gradientSecondary.z << "," << gradientSecondary.w << "\n";
    f << "gradientStrength=" << gradientStrength << "\n";
    f << "customColors=" << (customColors ? 1 : 0) << "\n";
    if (customColors) {
        for (int i = 0; i < ImGuiCol_COUNT; i++) {
            f << "color" << i << "="
              << colors[i].x << "," << colors[i].y << ","
              << colors[i].z << "," << colors[i].w << "\n";
        }
    }
}

void AppTheme::load() {
    std::string path = getThemePath();
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    bool hasAdaptiveThemeSettings = false;
    while (std::getline(f, line)) {
        if (line.find("userScale=") == 0) {
            userScale = std::stof(line.substr(10));
            if (userScale < 0.5f) userScale = 0.5f;
            if (userScale > 3.0f) userScale = 3.0f;
        } else if (line.find("fontScale=") == 0) {
            float value = 1.0f;
            if (sscanf(line.c_str() + 10, "%f", &value) == 1 && std::isfinite(value))
                fontScale = std::clamp(value, 0.75f, 2.0f);
        } else if (line.find("mode=") == 0) {
            mode = std::clamp(std::stoi(line.substr(5)), 0, 2);
            hasAdaptiveThemeSettings = true;
        } else if (line.find("gradientPrimary=") == 0) {
            sscanf(line.c_str() + 16, "%f,%f,%f,%f", &gradientPrimary.x, &gradientPrimary.y,
                &gradientPrimary.z, &gradientPrimary.w);
            hasAdaptiveThemeSettings = true;
        } else if (line.find("gradientSecondary=") == 0) {
            sscanf(line.c_str() + 18, "%f,%f,%f,%f", &gradientSecondary.x, &gradientSecondary.y,
                &gradientSecondary.z, &gradientSecondary.w);
        } else if (line.find("gradientStrength=") == 0) {
            gradientStrength = std::clamp(std::stof(line.substr(17)), 0.0f, 1.0f);
        } else if (line.find("customColors=") == 0) {
            customColors = (lastCharOr(line) == '1');
        } else if (line.find("color") == 0) {
            // Parse "colorN=r,g,b,a"
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            int idx = std::stoi(line.substr(5, eq - 5));
            if (idx < 0 || idx >= ImGuiCol_COUNT) continue;
            std::string vals = line.substr(eq + 1);
            float r, g, b, a;
            if (sscanf(vals.c_str(), "%f,%f,%f,%f", &r, &g, &b, &a) == 4) {
                colors[idx] = ImVec4(r, g, b, a);
            }
        }
    }
    if (!hasAdaptiveThemeSettings)
        customColors = false;
}

// Defined in main.cpp — for real quit (bypassing minimize-to-tray)
extern bool g_reallyQuit;
extern HWND g_mainHwnd;

// IFileOperation-based delete: recycle bin or permanent
static bool deleteWindowsFiles(const std::vector<std::string>& paths, bool permanent, std::string& error) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool needUninit = SUCCEEDED(hr);

    IFileOperation* pfo = nullptr;
    hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pfo));
    if (FAILED(hr)) {
        error = "Failed to create IFileOperation";
        if (needUninit) CoUninitialize();
        return false;
    }

    DWORD flags = FOF_NOCONFIRMATION | FOF_SILENT;
    if (!permanent) flags |= FOFX_RECYCLEONDELETE;
    // FOF_ALLOWUNDO also enables recycle for older behavior
    if (!permanent) flags |= FOF_ALLOWUNDO;

    pfo->SetOperationFlags(flags);

    for (const auto& path : paths) {
        // Convert to wide string
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wpath(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

        IShellItem* pItem = nullptr;
        hr = SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&pItem));
        if (SUCCEEDED(hr)) {
            pfo->DeleteItem(pItem, nullptr);
            pItem->Release();
        }
    }

    hr = pfo->PerformOperations();
    pfo->Release();
    if (needUninit) CoUninitialize();

    if (FAILED(hr)) {
        error = "Delete operation failed (HRESULT: " + std::to_string(hr) + ")";
        return false;
    }
    return true;
}

App::App() {
    m_prefs.load();
    m_theme.load();
    for (const auto& identity : m_prefs.deviceSessionIdentities) {
        int slot = m_deviceSessions.ensure(identity);
        deviceSession(slot).serial = identity;
    }


    // Second device uses a different local port for ADB forward
    deviceSession(1).client.setLocalPort(AFM_PORT + 1);
    // Secondary channel (dual-channel WiFi) uses a different port to avoid ADB forward collision
    m_secondaryChannel.setLocalPort(AFM_PORT + 2);

    m_leftPanel.isAndroid = false;
    m_leftPanel.currentPath = "C:\\";
    strcpy_s(m_leftPanel.pathInput, m_leftPanel.currentPath.c_str());

    m_rightPanel.isAndroid = true;
    m_rightPanel.currentPath = "/";  // will be updated once device is detected
    strcpy_s(m_rightPanel.pathInput, m_rightPanel.currentPath.c_str());

    m_shutdownTransfer = false;
    m_batchThread = std::thread([this]() { processBatchQueue(); });

    companion::start();

    m_shutdownPoll = false;
    m_pollThread = std::thread([this]() { devicePollLoop(); });

    m_asyncThread = std::thread([this]() { asyncWorkerLoop(); });

    // setupStyle sets base sizes + colors, then we scale for DPI + user preference
    // m_systemDpiScale is set by main.cpp AFTER construction, so initial scaling
    // is handled by applyScale() called from the first render frame.
    // For now, set base style — main.cpp will set m_systemDpiScale and the first
    // render handles the rest.
    setupStyle();
}

App::~App() {
    m_shutdownTransfer = true;
    m_shutdownPoll = true;
    {
        std::lock_guard<std::mutex> lock(m_batchMutex);
        for (const auto& batch : m_batchQueue) batch->requestStop();
    }
    ++m_pairingGeneration;
    if (m_pairingThread.joinable()) m_pairingThread.join();
    m_openFile.cancelRequested = true;
    if (m_openFileThread.joinable()) m_openFileThread.join();
    for (int slot = 0; slot < deviceSessionCount(); ++slot) deviceForSlot(slot).cancelCommands();
    m_secondaryChannel.cancelCommands();
    m_batchCV.notify_all();
    m_asyncCV.notify_all();
    m_listingWork.stop();
    m_manifestWork.stop();
    if (m_batchThread.joinable()) m_batchThread.join();
    if (m_pollThread.joinable()) m_pollThread.join();
    if (m_asyncThread.joinable()) m_asyncThread.join();
    companion::stop();

    // Release wakelock process if still running
    if (m_wakeLockProcess) {
        TerminateProcess(m_wakeLockProcess, 0);
        CloseHandle(m_wakeLockProcess);
        m_wakeLockProcess = nullptr;
    }

    // Unmount all mounts
    for (int slot = 0; slot < deviceSessionCount(); ++slot) DeviceMountManager::instance(slot).unmount();
    McrawMountManager::instance().unmountAll();

    // Finish every client before the optional shared ADB shutdown.
    m_extraChannels.clear();
    m_nicChannels.clear();
    m_secondaryChannel.disconnectTcp();
    m_secondaryChannel.setOwnsServer(false);
    for (int slot = 0; slot < deviceSessionCount(); ++slot) {
        auto& device = deviceForSlot(slot);
        device.resetCommandCancellation();
        device.stopServer();
    }
    if (m_prefs.killAdbOnClose) {
        m_device.runAdbCommand("kill-server");
    }
}

void App::asyncWorkerLoop() {
    while (!m_shutdownPoll) {
        std::function<void()> action;
        {
            std::unique_lock<std::mutex> lk(m_asyncMutex);
            m_asyncCV.wait(lk, [&]() { return m_shutdownPoll || !m_asyncQueue.empty(); });
            if (m_shutdownPoll) return;
            action = std::move(m_asyncQueue.front());
            m_asyncQueue.pop_front();
        }
        m_asyncBusy = true;
        try {
            std::lock_guard<std::mutex> deviceWork(m_deviceWorkMutex);
            if (m_shutdownPoll) return;
            action();
        } catch (const std::exception& e) {
            LOG_ERROR("Async", std::string("Background task failed: ") + e.what());
            UiMessage message;
            message.hasNotification = true;
            message.notificationTitle = "Operation Failed";
            message.notificationMessage = e.what();
            message.notificationIsError = true;
            postUiMessage(std::move(message));
        } catch (...) {
            LOG_ERROR("Async", "Background task failed with an unknown exception");
            UiMessage message;
            message.hasNotification = true;
            message.notificationTitle = "Operation Failed";
            message.notificationMessage = "Unexpected background task failure.";
            message.notificationIsError = true;
            postUiMessage(std::move(message));
        }
        m_asyncBusy = false;
        m_asyncStatus.clear();
    }
}

void App::postAsync(const std::string& statusMsg, std::function<void()> action) {
    {
        std::lock_guard<std::mutex> lk(m_asyncMutex);
        m_asyncStatus = statusMsg;
        m_asyncQueue.push_back([statusMsg,action=std::move(action)]() {
            if (!statusMsg.empty()) LOG_INFO("Activity", "Started: "+statusMsg);
            try {
                action();
                if (!statusMsg.empty()) LOG_DEBUG("Activity", "Task returned: "+statusMsg);
            } catch (const std::exception& error) {
                LOG_ERROR("Activity", statusMsg+": "+error.what());
                throw;
            } catch (...) {
                LOG_ERROR("Activity", statusMsg+": unexpected exception");
                throw;
            }
        });
    }
    m_asyncCV.notify_one();
}

void App::postUiMessage(UiMessage message) {
    std::lock_guard<std::mutex> lk(m_uiMessageMutex);
    m_uiMessages.push_back(std::move(message));
}

void App::postListing(FilePanel* panel, std::function<void()> action) {
    const auto generation = panel->listingGeneration;
    m_listingWork.post(reinterpret_cast<uintptr_t>(panel), [this, panel, generation, action = std::move(action)] {
        if (m_shutdownPoll) return;
        try {
            action();
        } catch (const std::exception& error) {
            UiMessage message;
            message.listingPanel = panel;
            message.listingGeneration = generation;
            message.listingError = error.what();
            postUiMessage(std::move(message));
        } catch (...) {
            UiMessage message;
            message.listingPanel = panel;
            message.listingGeneration = generation;
            message.listingError = "Unexpected folder loading failure";
            postUiMessage(std::move(message));
        }
    });
}

void App::drainUiMessages() {
    std::deque<UiMessage> messages;
    {
        std::lock_guard<std::mutex> lk(m_uiMessageMutex);
        messages.swap(m_uiMessages);
    }

    for (auto& message : messages) {
        if (message.preparedBatch) {
            auto& batch = message.preparedBatch;
            if (batch->stopRequested) batch->state = BatchState::Stopped;
            else if (!message.preparationError.empty()) {
                batch->errorMessage = std::move(message.preparationError);
                batch->state = BatchState::Failed;
                batch->logEvent(LogLevel::Error, "Source scan failed: " + batch->errorMessage);
            } else {
                batch->files = std::move(message.preparedFiles);
                batch->totalBytes = message.preparedBytes;
                batch->logQueued();
                batch->state = BatchState::Queued;
                m_batchCV.notify_one();
            }
        }
        if (message.mountedPanel && message.mountedPanel->listingGeneration == message.mountedGeneration &&
            !message.mountedPanel->isAndroid && !message.mountedPath.empty()) {
            message.mountedPanel->insideMcraw = true;
            message.mountedPanel->mcrawFilePath = message.mountedSource;
            navigateToDirectory(*message.mountedPanel, message.mountedPath);
        }
        if (message.listingPanel) {
            FilePanel& panel = *message.listingPanel;
            if (panel.listingGeneration == message.listingGeneration) {
                panel.refreshInProgress = false;
                if (!message.listingError.empty()) {
                    m_statusMessage = "Cannot load folder: " + message.listingError;
                    m_statusTime = std::chrono::steady_clock::now();
                    LOG_WARN("Files", m_statusMessage);
                    continue;
                }
                if (panel.pendingDeviceSerial.empty() && panel.deviceSlot == message.listingSlot &&
                    (message.listingApps ? panel.isApps : !panel.isApps && !panel.isConnections &&
                        panel.isAndroid != message.listingWindows && panel.currentPath == message.listingPath)) {
                    if (message.listingApps) panel.appEntries = std::move(message.listingAppEntries);
                    else if (message.listingWindows) panel.windowsEntries = std::move(message.listingWindowsEntries);
                    else panel.androidEntries = std::move(message.listingEntries);
                    ++panel.entriesRevision;
                    panel.selectedIndices.clear();
                    strcpy_s(panel.pathInput, message.listingApps ? "Installed apps" : panel.currentPath.c_str());
                    if (panel.navigationTransitionPending) {
                        panel.navigationTransitionPending = false;
                        panel.navigationTransitionReady = true;
                    }
                    m_compareDirty = true;
                }
            }
        }
        if (message.saveDeviceSessions) {
            m_prefs.deviceSessionIdentities = m_deviceSessions.identities();
            m_prefs.save();
        }
        if (message.deviceSessionReady) {
            int slot = message.readyDeviceSlot;
            for (FilePanel* panel : {&m_leftPanel, &m_rightPanel}) {
                if (!panel->pendingDeviceSerial.empty() &&
                    deviceIdentity(panel->pendingDeviceSerial) == deviceIdentity(deviceSession(slot).serial))
                    openDeviceSession(*panel, slot);
                else if (panel->isAndroid && panel->deviceSlot == slot && panel->pendingDeviceSerial.empty()) {
                    auto& session = deviceSession(slot);
                    if (panel->currentPath.empty() || panel->currentPath == "/") {
                        ++panel->listingGeneration;
                        panel->refreshInProgress = false;
                        panel->currentPath = session.storageRoot;
                        strcpy_s(panel->pathInput, panel->currentPath.c_str());
                        std::lock_guard<std::mutex> lock(session.cacheMutex);
                        panel->androidEntries = session.rootEntries;
                    }
                    panel->needsRefresh = true;
                }
            }
        }
        if (message.hasWifiPairing) applyWifiPairingResult(message);
        if (message.rootRequestFinished) {
            auto& session = deviceSession(message.rootSlot);
            if (session.serial == message.rootSerial) {
                m_prefs.setRootEnabledForSerial(deviceIdentity(message.rootSerial), message.rootEnabled);
                m_prefs.save();
                if (message.rootModeChanged) {
                    for (auto& view : session.views) view.reset();
                    for (FilePanel* panel : {&m_leftPanel, &m_rightPanel}) {
                        if (!panel->isAndroid || panel->deviceSlot != message.rootSlot || !panel->pendingDeviceSerial.empty()) continue;
                        bool openRoot = message.rootEnabled && panel == message.rootPanel &&
                            panel->listingGeneration == message.rootPanelGeneration;
                        ++panel->listingGeneration;
                        panel->refreshInProgress = false;
                        panel->selectedIndices.clear();
                        panel->needsRefresh = true;
                        if (openRoot || !message.rootEnabled) {
                            panel->insideMcraw = false;
                            panel->mcrawFilePath.clear();
                            navigateToDirectory(*panel, openRoot ? "/" : session.storageRoot);
                        }
                    }
                }
            }
            session.rootRequestPending = false;
        }
        if (message.connectionRequestFinished) {
            if (m_connectionPendingSerial == message.connectionSerial) m_connectionPendingSerial.clear();
            m_connectionFeedback = message.status;
            for (FilePanel* panel : {&m_leftPanel, &m_rightPanel}) {
                if (panel->pendingDeviceSerial != message.connectionSerial) continue;
                if (message.connectionSlot >= 0) {
                    openDeviceSession(*panel, message.connectionSlot);
                } else panel->deviceOpenError = message.status;
            }
            if (!message.connectedWifiAddress.empty()) {
                m_prefs.wifiAutoConnect = true;
                for (auto& saved : m_prefs.savedWifiDevices) {
                    if (saved.wifiIp + ":" + std::to_string(saved.port) != message.connectionSerial) continue;
                    if (saved.model.empty()) saved.model = message.connectedWifiName;
                    if (auto endpoint = parseAdbEndpoint(message.connectedWifiAddress)) {
                        saved.wifiIp = endpoint->host;
                        saved.port = endpoint->port;
                    }
                }
                m_prefs.save();
            }
        }
        if (message.connectionRefreshFinished) {
            m_connectionsRefreshing = false;
            if (!message.hasStatus) {
                std::lock_guard<std::mutex> lock(m_deviceMutex);
                std::string selectedSerial;
                if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size()) selectedSerial = m_devices[m_selectedDevice].serial;
                m_devices = std::move(message.connectionDevices);
                m_selectedDevice = -1;
                for (int i = 0; i < (int)m_devices.size(); ++i)
                    if (m_devices[i].serial == selectedSerial) m_selectedDevice = i;
            } else m_connectionFeedback = message.status;
        }
        if (message.hasWizardProbe && m_showWifiWizard && m_wizardStep == 1 &&
            message.wizardProbeGeneration == m_wizardProbeGeneration.load() &&
            message.wizardProbeSerial == m_wizardSerial) {
            m_wizardProbeBusy = false;
            m_wizardWifiIp = std::move(message.wizardProbeIp);
            m_wizardProbeError = std::move(message.wizardProbeError);
        }
        if (message.hasWizardSetupProgress && message.wizardSetupGeneration == m_wizardSetupGeneration &&
            message.wizardSetupSerial == m_wizardSerial) {
            m_wizardStatus = std::move(message.wizardSetupStatus);
            if (message.wizardSetupFinished) {
                m_wizardBusy = false;
                m_wizardSucceeded = message.wizardSetupSucceeded;
                m_wizardStep = 3;
            }
        }
        if (message.setBackupApps) {
            m_backupManagerApps = std::move(message.backupApps);
            m_backupManagerAppSelectionAnchor = -1;
        }
        if (message.markBackupAppsFresh) {
            m_backupManagerNeedsAppRefresh = false;
        }
        if (message.refreshBackupList) {
            m_backupManagerNeedsBackupRefresh = true;
        }
        if (message.refreshAppList) {
            m_backupManagerNeedsAppRefresh = true;
        }
        if (message.refreshFilePanel) {
            message.refreshFilePanel->needsRefresh = true;
        }
        if (message.hasAppUninstallProgress) {
            m_appUninstallActive = message.appUninstallActive;
            m_appUninstallUseRoot = message.appUninstallUseRoot;
            m_appUninstallCurrent = message.appUninstallCurrent;
            m_appUninstallTotal = message.appUninstallTotal;
            m_appUninstallSucceeded = message.appUninstallSucceeded;
            m_appUninstallFailed = message.appUninstallFailed;
            m_appUninstallPackage = std::move(message.appUninstallPackage);
        }
        if (message.backupRootProbeComplete && message.backupRootSlot == (m_backupManagerSlot) &&
            message.backupRootSerial == deviceSession(m_backupManagerSlot).serial) {
            m_backupRootAccessSerial = message.backupRootSerial;
            m_backupRootAccess = message.backupRootAvailable ? BackupRootAccess::Available : BackupRootAccess::Unavailable;
            if (message.backupRootAvailable) {
                m_showBackupRootChoice = false;
                m_backupIncludeData = true;
                m_backupIncludeDeData = true;
            } else {
                m_showBackupRootChoice = true;
            }
        }
        if (message.hasBackupProgress) {
            m_backupProgressActive = message.backupProgressActive;
            m_backupProgressIsRestore = message.backupProgressIsRestore;
            if (message.backupProgressActive && !message.backupProgressIsRestore)
                m_backupProgressCurrent = std::max(m_backupProgressCurrent, message.backupProgressCurrent);
            else
                m_backupProgressCurrent = message.backupProgressCurrent;
            m_backupProgressTotal = message.backupProgressTotal;
            m_backupProgressStage = message.backupProgressStage;
            m_backupProgressStageTotal = message.backupProgressStageTotal;
            m_backupProgressBytes = message.backupProgressBytes;
            m_backupProgressTotalBytes = message.backupProgressTotalBytes;
            m_backupProgressBytesPerSecond = message.backupProgressBytesPerSecond;
            m_backupProgressEtaSeconds = message.backupProgressEtaSeconds;
            if (message.backupProgressBytesPerSecond > 0.0)
                m_backupProgressLastTransferUpdate = std::chrono::steady_clock::now();
            m_backupProgressPackage = std::move(message.backupProgressPackage);
            m_backupProgressStageLabel = std::move(message.backupProgressStageLabel);
            m_backupProgressUsbPipes = message.backupProgressUsbPipes;
            m_backupProgressWifiPipes = message.backupProgressWifiPipes;
            if (message.backupProgressAppIndex >= 0 &&
                message.backupProgressAppIndex < (int)m_backupAppProgress.size()) {
                int index = message.backupProgressAppIndex;
                m_backupProgressActiveAppIndex = index;
                m_backupAppProgress[index] = std::max(m_backupAppProgress[index],
                    std::clamp(message.backupProgressAppFraction, 0.0f, 1.0f));
                m_backupAppActive[index] = true;
            }
        }
        if (message.hasStatus) {
            m_statusMessage = std::move(message.status);
            m_statusTime = std::chrono::steady_clock::now();
        }
        if (message.hasNotification) {
            showNotification(message.notificationTitle, message.notificationMessage, message.notificationIsError);
        }
    }
}


void App::connectDeviceBySerialNow(const std::string& serial) {
    if (serial.empty() || m_shutdownPoll) return;
    discoverDeviceIdentities({DeviceInfo{serial, {}, "device"}});
    int slot = m_deviceSessions.ensure(deviceIdentity(serial));
    if (deviceSession(slot).connected && deviceForSlot(slot).isServerRunning()) return;
    prepareDeviceSession(slot, serial);
}

void App::addKnownPrimarySerials(std::set<std::string>& serials, const std::string& serial) const {
    if (serial.empty()) return;
    serials.insert(serial);
    for (const auto& w : m_prefs.savedWifiDevices) {
        if (w.wifiIp.empty()) continue;
        std::string wifiSerial = w.wifiIp + ":" + std::to_string(w.port);
        if (w.serial == serial || wifiSerial == serial) {
            if (!w.serial.empty()) serials.insert(w.serial);
            serials.insert(wifiSerial);
        }
    }
}

static bool windowsUsesLightAppTheme() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    LSTATUS status = RegGetValueA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        "AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value != 0;
}

void App::setupStyle() {
    ImGui::GetStyle() = ImGuiStyle();
    m_resolvedLightTheme = m_theme.mode == 2 || (m_theme.mode == 0 && windowsUsesLightAppTheme());
    BOOL darkTitle = !m_resolvedLightTheme;
    if (g_mainHwnd) DwmSetWindowAttribute(g_mainHwnd, 20, &darkTitle, sizeof(darkTitle));
    if (m_resolvedLightTheme) ImGui::StyleColorsLight(); else ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10;
    style.ChildRounding = 10;
    style.FrameRounding = 7;
    style.PopupRounding = 10;
    style.GrabRounding = 6;
    style.ScrollbarRounding = 9;
    style.TabRounding = 7;
    style.WindowBorderSize = style.ChildBorderSize = style.PopupBorderSize = 1;
    style.FrameBorderSize = 1;
    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(8, 4);
    style.CellPadding = ImVec2(10, 5);
    style.ScrollbarSize = 10;
    style.GrabMinSize = 12;
    style.SeparatorTextBorderSize = 1;
    style.DisabledAlpha = 0.45f;
    style.Alpha = 1;
    style.AntiAliasedLines = style.AntiAliasedFill = true;
    auto color = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r/255.0f,g/255.0f,b/255.0f,a); };
    auto blend = [](ImVec4 a, ImVec4 b, float t) {
        return ImVec4(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t);
    };
    bool light = m_resolvedLightTheme;
    ImVec4 accent = m_theme.gradientPrimary;
    ImVec4 canvas = light ? color(238,242,249) : color(6,10,17);
    ImVec4 surface = light ? color(255,255,255) : color(12,18,29);
    ImVec4 raised = light ? color(233,239,248) : color(20,30,46);
    ImVec4 hover = light ? color(221,231,245) : color(28,42,63);
    ImVec4 selected = light ? color(207,226,252) : color(29,55,92);
    ImVec4 border = light ? color(200,212,229) : color(32,46,67);
    ImVec4 inset = light ? color(246,248,252) : color(8,13,22);
    auto* c = style.Colors;
    c[ImGuiCol_Text] = light ? color(26,38,58) : color(234,239,247);
    c[ImGuiCol_TextDisabled] = light ? color(84,101,126) : color(141,160,188);
    c[ImGuiCol_WindowBg] = canvas;
    c[ImGuiCol_ChildBg] = surface;
    c[ImGuiCol_PopupBg] = light ? surface : color(16,24,38);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = color(0,0,0,0);
    c[ImGuiCol_FrameBg] = inset;
    c[ImGuiCol_FrameBgHovered] = blend(inset, hover, 0.7f);
    c[ImGuiCol_FrameBgActive] = blend(inset, selected, 0.5f);
    c[ImGuiCol_TitleBg] = surface;
    c[ImGuiCol_TitleBgActive] = raised;
    c[ImGuiCol_TitleBgCollapsed] = surface;
    c[ImGuiCol_MenuBarBg] = canvas;
    c[ImGuiCol_Button] = raised;
    c[ImGuiCol_ButtonHovered] = hover;
    c[ImGuiCol_ButtonActive] = selected;
    c[ImGuiCol_Header] = selected;
    c[ImGuiCol_HeaderHovered] = blend(surface, hover, light ? 0.9f : 0.85f);
    c[ImGuiCol_HeaderActive] = selected;
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = color(0,0,0,0);
    c[ImGuiCol_ResizeGripHovered] = blend(border, accent, 0.5f);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_TableHeaderBg] = light ? color(236,241,249) : color(17,26,40);
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = color(0,0,0,0);
    c[ImGuiCol_TableRowBg] = color(0,0,0,0);
    c[ImGuiCol_TableRowBgAlt] = light ? color(233,239,248,0.45f) : color(22,33,50,0.32f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = blend(accent, color(255,255,255), 0.2f);
    c[ImGuiCol_ScrollbarBg] = color(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab] = light ? color(190,203,222) : color(43,59,83);
    c[ImGuiCol_ScrollbarGrabHovered] = light ? color(160,176,200) : color(63,84,114);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_Tab] = raised;
    c[ImGuiCol_TabSelected] = selected;
    c[ImGuiCol_TabHovered] = hover;
    c[ImGuiCol_TextSelectedBg] = blend(selected, accent, 0.25f);
    c[ImGuiCol_NavHighlight] = accent;
    c[ImGuiCol_DragDropTarget] = accent;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_ModalWindowDimBg] = color(0,0,0,light?0.25f:0.55f);
}

bool App::modernButton(const char* label, const ImVec2& requestedSize, ButtonTone tone) {
    return ui::actionButton(label, requestedSize, static_cast<ui::Tone>(tone));
}

bool App::modernSmallButton(const char* label) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6*ui::scale(), 3*ui::scale()));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return pressed;
}

void App::render() {
    ImGuiIO& io = ImGui::GetIO();
    drainUiMessages();
    updateTransferPresentation();

    if (m_theme.mode == 0) {
        auto themeNow = std::chrono::steady_clock::now();
        if (m_lastSystemThemeCheck.time_since_epoch().count() == 0 ||
            themeNow - m_lastSystemThemeCheck >= std::chrono::seconds(2)) {
            m_lastSystemThemeCheck = themeNow;
            bool light = windowsUsesLightAppTheme();
            if (light != m_resolvedLightTheme && m_pendingScale < 0.0f)
                m_pendingScale = m_theme.userScale;
        }
    }

    // Reset drag state when mouse button released
    if (m_isDragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        m_isDragging = false;

    // Global keyboard shortcuts
    bool anyModalOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) || m_workspaceDrawer != 0 || m_showBackupManager || m_showAppsWorkspace || m_showConnectionsWorkspace;
    ImGuiContext* imguiContext = ImGui::GetCurrentContext();
    bool textInputActive = imguiContext && imguiContext->ActiveId != 0 &&
        imguiContext->InputTextState.ID == imguiContext->ActiveId;

    if (ImGui::IsKeyPressed(ImGuiKey_F12)) m_showDebugWindow = !m_showDebugWindow;
    if (ImGui::IsKeyPressed(ImGuiKey_F5) && !anyModalOpen) {
        m_leftPanel.needsRefresh = true;
        m_rightPanel.needsRefresh = true;
        m_lastNavTimeLeft = std::chrono::steady_clock::now();
        m_lastNavTimeRight = std::chrono::steady_clock::now();
    }
    // F6 — copy selected files to Android (push)
    if (ImGui::IsKeyPressed(ImGuiKey_F6) && !anyModalOpen) startTransfer(false);
    // F7 — copy selected files to Windows (pull)
    if (ImGui::IsKeyPressed(ImGuiKey_F7) && !anyModalOpen) startTransfer(true);

    if (ImGui::IsKeyPressed(ImGuiKey_F2) && !anyModalOpen && !textInputActive && m_lastFocusedPanel && panelAvailable(*m_lastFocusedPanel) && m_lastFocusedPanel->selectedIndices.size()==1) {
        m_contextPanel=m_lastFocusedPanel;m_contextIndex=*m_contextPanel->selectedIndices.begin();
        if(m_contextPanel->validIndex(m_contextIndex)){strcpy_s(m_renameBuf,m_contextPanel->entryName(m_contextIndex).c_str());m_showRenamePopup=true;}
    }

    // Del / Shift+Del — delete selected files in the focused panel
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !anyModalOpen && !textInputActive) {
        FilePanel* focused = m_lastFocusedPanel;
        if (focused && !focused->isApps && !focused->isConnections && !focused->selectedIndices.empty()) {
            m_contextPanel = focused;
            m_contextIndex = *focused->selectedIndices.begin();
            m_deletePermanent = ImGui::GetIO().KeyShift;
            m_showDeleteConfirm = true;
        }
    }

    // Backspace — navigate up
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !anyModalOpen && !textInputActive) {
        if (m_lastFocusedPanel && !m_lastFocusedPanel->isApps && !m_lastFocusedPanel->isConnections)
            navigateUp(*m_lastFocusedPanel);
    }

    // Enter — open/navigate the focused item (same as double-click)
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !anyModalOpen && !textInputActive) {
        FilePanel* focused = m_lastFocusedPanel;
        if (focused && !focused->isApps && !focused->isConnections &&
            focused->focusedIndex >= 0 && focused->validIndex(focused->focusedIndex)) {
            std::string name = focused->entryName(focused->focusedIndex);
            bool isDir = focused->entryIsDir(focused->focusedIndex);
            if (isDir) {
                std::string sep = focused->isAndroid ? "/" : "\\";
                std::string np = focused->currentPath;
                appendPathSeparator(np, sep[0]);
                np += name;
                navigateToDirectory(*focused, np);
            } else if (endsWithCI(name, ".mcraw") && !focused->isAndroid) {
                std::string mcrawFullPath = focused->currentPath;
                appendPathSeparator(mcrawFullPath, '\\');
                mcrawFullPath += name;
                requestMcrawMount(*focused, mcrawFullPath);
            } else if (endsWithCI(name, ".mcraw") && focused->isAndroid) {
                std::string np = focused->currentPath;
                appendPathSeparator(np, '/');
                np += name;
                navigateToDirectory(*focused, np);
            }
        }
    }

    // Space — pause/resume active transfer
    if (ImGui::IsKeyPressed(ImGuiKey_Space) && !anyModalOpen && !textInputActive && !ImGui::IsAnyItemFocused()) {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        for (auto& b : m_batchQueue) {
            auto s = b->state.load();
            if (s == BatchState::Running) { b->pauseRequested = true; b->logEvent(LogLevel::Info,"Pause requested"); break; }
            if (s == BatchState::Paused)  { b->pauseRequested = false; b->logEvent(LogLevel::Info,"Resume requested"); m_batchCV.notify_all(); break; }
        }
    }

    // Ctrl+C — copy selected files to clipboard
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !anyModalOpen && !textInputActive) {
        FilePanel* focused = m_lastFocusedPanel;
        if (focused && !focused->isApps && !focused->isConnections && !focused->selectedIndices.empty()) {
            m_clipboardPaths.clear();
            std::string sep = focused->isAndroid ? "/" : "\\";
            for (int idx : focused->selectedIndices) {
                if (!focused->validIndex(idx)) continue;
                std::string p = focused->currentPath;
                appendPathSeparator(p, sep[0]);
                p += focused->entryName(idx);
                m_clipboardPaths.push_back(p);
            }
            m_clipboardIsAndroid = focused->isAndroid;
            m_clipboardDeviceSlot = focused->deviceSlot;
            m_clipboardCut = false;
        }
    }

    // Ctrl+X — cut selected files to clipboard
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X) && !anyModalOpen && !textInputActive) {
        FilePanel* focused = m_lastFocusedPanel;
        if (focused && !focused->isApps && !focused->isConnections && !focused->selectedIndices.empty()) {
            m_clipboardPaths.clear();
            std::string sep = focused->isAndroid ? "/" : "\\";
            for (int idx : focused->selectedIndices) {
                if (!focused->validIndex(idx)) continue;
                std::string p = focused->currentPath;
                appendPathSeparator(p, sep[0]);
                p += focused->entryName(idx);
                m_clipboardPaths.push_back(p);
            }
            m_clipboardIsAndroid = focused->isAndroid;
            m_clipboardDeviceSlot = focused->deviceSlot;
            m_clipboardCut = true;
        }
    }

    // Ctrl+V — paste clipboard files into focused panel
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && !anyModalOpen && !textInputActive) {
        FilePanel* focused = m_lastFocusedPanel;
        if (focused && !focused->isApps && !focused->isConnections && !m_clipboardPaths.empty()) {
            performClipboardPaste(*focused);
        }
    }

    if (m_showBackupManager && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A) && !anyModalOpen && !ImGui::IsAnyItemActive()) {
        setBackupManagerAppSelection(true, true);
    }

    // Escape — cancel confirm dialogs
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if(m_workspaceDrawer) m_workspaceDrawer=0;
        else if(!anyModalOpen && !textInputActive && m_lastFocusedPanel) m_lastFocusedPanel->selectedIndices.clear();
        if (m_confirmStopTransfer) m_confirmStopTransfer = false;
        else if (m_showDeleteConfirm) m_showDeleteConfirm = false;
        else if (m_showNewFolderPopup) m_showNewFolderPopup = false;
        else if (m_showRenamePopup) m_showRenamePopup = false;
    }

    // Device polling happens in background thread - nothing blocking here

    // Refresh panels — each can be Windows or Android independently
    bool panelsRefreshed = false;
    auto refreshPanel = [&](FilePanel& panel) {
        if (!panel.needsRefresh) return;
        if ((panel.isAndroid || panel.isApps) && deviceSession(panel.deviceSlot).rootRequestPending) return;
        if (!panel.pendingDeviceSerial.empty()) return;
        if (panel.isConnections) {
            panel.needsRefresh = false;
            return;
        }
        if (panel.isApps) {
            if (panel.refreshInProgress) return;
            panel.refreshInProgress = true;
            panel.needsRefresh = false;
            const auto generation = ++panel.listingGeneration;
            const int col = panel.sortColumn;
            const bool desc = panel.sortDescending;
            FilePanel* targetPanel = &panel;
            int slot = panel.deviceSlot;
            std::string expectedSerial = (slot >= 0 && slot < deviceSessionCount()) ? deviceSession(slot).serial : "";
            postListing(targetPanel, [this, targetPanel, slot, expectedSerial, generation, col, desc]() {
                std::vector<InstalledAppEntry> apps;
                DeviceClient dev;
                dev.setAdbPath(deviceForSlot(slot).getAdbPath());
                if (!expectedSerial.empty()) {
                    apps = dev.listInstalledApps(expectedSerial, true);
                    std::sort(apps.begin(), apps.end(),
                        [col, desc](const InstalledAppEntry& a, const InstalledAppEntry& b) {
                            int cmp = 0;
                            if (col == 1) cmp = _stricmp(a.packageName.c_str(), b.packageName.c_str());
                            else if (col == 2) cmp = (int)a.isSystem - (int)b.isSystem;
                            else if (col == 3) cmp = _stricmp(a.apkPath.c_str(), b.apkPath.c_str());
                            else {
                                const std::string& aName = a.appName.empty() ? a.packageName : a.appName;
                                const std::string& bName = b.appName.empty() ? b.packageName : b.appName;
                                cmp = _stricmp(aName.c_str(), bName.c_str());
                            }
                            return desc ? cmp > 0 : cmp < 0;
                        });
                }
                UiMessage message;
                message.listingPanel = targetPanel;
                message.listingGeneration = generation;
                message.listingSlot = slot;
                message.listingApps = true;
                message.listingAppEntries = std::move(apps);
                postUiMessage(std::move(message));
            });
        } else if (panel.isAndroid) {
            if (!deviceSession(panel.deviceSlot).connected ||
                !deviceForSlot(panel.deviceSlot).isServerRunning()) return;
            if (panel.refreshInProgress) return;
            panel.refreshInProgress = true;
            panel.needsRefresh = false;
            const auto generation = ++panel.listingGeneration;
            const int col = panel.sortColumn;
            const bool desc = panel.sortDescending;
            FilePanel* targetPanel = &panel;
            int slot = panel.deviceSlot;
            std::string path = panel.currentPath;
            bool insideMcraw = panel.insideMcraw;
            std::string mcrawPath = panel.mcrawFilePath;
            postListing(targetPanel, [this, targetPanel, slot, path, insideMcraw, mcrawPath, generation, col, desc]() {
                std::vector<DeviceFileEntry> entries;
                DeviceClient dev;
                if (dev.connectParallelFrom(deviceForSlot(slot))) {
                    if (insideMcraw)
                        entries = dev.listMcraw(mcrawPath);
                    else {
                        bool listed = false;
                        entries = dev.listDirectory(path, &listed);
                        if (!listed) throw std::runtime_error(dev.lastError());
                    }
                    if (!dev.lastError().empty()) throw std::runtime_error(dev.lastError());
                    std::sort(entries.begin(), entries.end(),
                        [col, desc](const DeviceFileEntry& a, const DeviceFileEntry& b) {
                            if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
                            int cmp;
                            if (col == 1) cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
                            else if (col == 2) cmp = (a.mtime < b.mtime) ? -1 : (a.mtime > b.mtime) ? 1 : 0;
                            else cmp = _stricmp(a.name.c_str(), b.name.c_str());
                            return desc ? cmp > 0 : cmp < 0;
                        });
                } else throw std::runtime_error("Device is unavailable");
                UiMessage message;
                message.listingPanel = targetPanel;
                message.listingGeneration = generation;
                message.listingSlot = slot;
                message.listingPath = path;
                message.listingEntries = std::move(entries);
                postUiMessage(std::move(message));
            });
        } else {
            refreshWindowsPanel(panel);
        }
    };
    refreshPanel(m_leftPanel);
    refreshPanel(m_rightPanel);
    if(m_showAppsWorkspace) refreshPanel(m_appsWorkspacePanel);
    if (m_compareEnabled && (m_compareDirty || panelsRefreshed)) {
        updateCompareHighlights();
        m_compareDirty = false;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = ui::scale();
    m_shellEntranceAnimation = 1.0f;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags mainFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0,0));
    ImGui::Begin("##MainWindow", nullptr, mainFlags);
    ImGui::PopStyleVar(4);
    {
        ImDrawList* background = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetWindowPos();
        const ImVec2 b(a.x + ImGui::GetWindowWidth(), a.y + ImGui::GetWindowHeight());
        const float w = b.x - a.x, h = b.y - a.y;
        ImVec4 primaryGlow = m_theme.gradientPrimary; primaryGlow.w = m_resolvedLightTheme ? 0.07f : 0.09f;
        ImVec4 secondaryGlow = m_theme.gradientSecondary; secondaryGlow.w = m_resolvedLightTheme ? 0.06f : 0.10f;
        const ImU32 clear = ImGui::GetColorU32(ImVec4(primaryGlow.x, primaryGlow.y, primaryGlow.z, 0));
        const ImU32 clearSecondary = ImGui::GetColorU32(ImVec4(secondaryGlow.x, secondaryGlow.y, secondaryGlow.z, 0));
        background->AddRectFilledMultiColor(a, ImVec2(a.x + w * 0.6f, a.y + h * 0.5f),
            ImGui::GetColorU32(primaryGlow), clear, clear, clear);
        background->AddRectFilledMultiColor(ImVec2(a.x + w * 0.4f, a.y + h * 0.45f), b,
            clearSecondary, clearSecondary, ImGui::GetColorU32(secondaryGlow), clearSecondary);
    }
    renderWorkspaceHeader();
    const float headerHeight = ui::controlHeight()+16*s;
    ImGui::SetCursorPosY(headerHeight);
    if (!m_showBackupManager && !m_showAppsWorkspace && !m_showConnectionsWorkspace) {
        renderWorkspaceToolbar();
        ImGui::SetCursorPosY(headerHeight+ui::controlHeight()+12*s);
        if (m_compareEnabled) {
            ImGui::BeginChild("##CompareControls", ImVec2(0, 72*s));
            renderCompareToolbar();
            ImGui::EndChild();
        }
    }
    float dockHeight = transferDockHeight();
    float statusHeight = std::max(36*s, ImGui::GetFontSize()+18*s);
    float bodyY = ImGui::GetCursorPosY()+8*s;
    float bodyHeight = std::max(120*s, vp->WorkSize.y-bodyY-dockHeight-statusHeight-8*s);
    ImGui::SetCursorPosY(bodyY);
    ImGui::SetCursorPosX(14*s);
    if (m_showBackupManager || m_showAppsWorkspace || m_showConnectionsWorkspace) {
        if (m_showConnectionsWorkspace) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
        ImGui::BeginChild("##WorkspacePage",ImVec2(-14*s,bodyHeight),
            m_showConnectionsWorkspace ? ImGuiChildFlags_None : ImGuiChildFlags_Borders);
        if (m_showConnectionsWorkspace) ImGui::PopStyleVar();
        if (m_showBackupManager) renderBackupManagerWindow();
        else if (m_showAppsWorkspace) renderAppsPanel(m_appsWorkspacePanel,PanelSide::Left);
        else renderConnectionManager(m_connectionsWorkspacePanel);
        ImGui::EndChild();
    } else {
        // The content region can be narrower than the margin during resizes, and a
        // negative width would make the clamp bounds below cross.
        float width = std::max(0.0f, ImGui::GetContentRegionAvail().x-14*s);
        float gap=12*s;
        float leftWidth=std::clamp((width-gap)*m_prefs.paneSplit, std::min(240*s,width*0.3f), std::max(width*0.7f,width-240*s-gap));
        auto pane = [&](FilePanel& panel, PanelSide side, float paneWidth) {
            bool active=m_lastFocusedPanel==&panel || (!m_lastFocusedPanel && side==PanelSide::Right);
            ImVec4 border=ImGui::GetStyleColorVec4(ImGuiCol_Border);
            if(active) {
                ImVec4 accent=ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                border=ImVec4(border.x+(accent.x-border.x)*0.45f,border.y+(accent.y-border.y)*0.45f,border.z+(accent.z-border.z)*0.45f,1);
            }
            {
                const ImVec2 paneMin=ImGui::GetCursorScreenPos();
                const ImVec2 paneMax(paneMin.x+paneWidth,paneMin.y+bodyHeight);
                const float rounding=ImGui::GetStyle().ChildRounding;
                ImDrawList* parentDraw=ImGui::GetWindowDrawList();
                if(active) {
                    ImVec4 glow=ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);glow.w=m_resolvedLightTheme?0.10f:0.07f;
                    parentDraw->AddRectFilled(ImVec2(paneMin.x-3*s,paneMin.y-2*s),ImVec2(paneMax.x+3*s,paneMax.y+5*s),ImGui::GetColorU32(glow),rounding+4*s);
                }
                const ImVec4 body=ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
                ui::gradientRect(parentDraw,paneMin,paneMax,ui::shade(body,m_resolvedLightTheme?0.0f:0.03f),
                    ui::shade(body,m_resolvedLightTheme?-0.02f:-0.10f),rounding);
            }
            ImGui::PushStyleColor(ImGuiCol_Border,border);
            ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12*s,10*s));
            ImGui::BeginChild(side==PanelSide::Left?"##LeftPanel":"##RightPanel",ImVec2(paneWidth,bodyHeight),ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar);
            ImGui::PopStyleVar();ImGui::PopStyleColor(2);
            auto min=ImGui::GetWindowPos();auto size=ImGui::GetWindowSize();
            if(active) {
                ImDrawList* paneDraw=ImGui::GetWindowDrawList();
                const float inset=ImGui::GetStyle().ChildRounding;
                const float mid=min.x+size.x*0.5f;
                ImVec4 bright=ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);bright.w=0.85f;
                ImVec4 tail=m_theme.gradientSecondary;tail.w=0.0f;
                ImVec4 clear=bright;clear.w=0.0f;
                ImVec4 secondary=m_theme.gradientSecondary;secondary.w=0.85f;
                paneDraw->AddRectFilledMultiColor(ImVec2(min.x+inset,min.y+1),ImVec2(mid,min.y+1+2*s),
                    ImGui::GetColorU32(clear),ImGui::GetColorU32(bright),ImGui::GetColorU32(bright),ImGui::GetColorU32(clear));
                paneDraw->AddRectFilledMultiColor(ImVec2(mid,min.y+1),ImVec2(min.x+size.x-inset,min.y+1+2*s),
                    ImGui::GetColorU32(secondary),ImGui::GetColorU32(tail),ImGui::GetColorU32(tail),ImGui::GetColorU32(secondary));
            }
            if(side==PanelSide::Left){m_leftPanelMin=min;m_leftPanelMax=ImVec2(min.x+size.x,min.y+size.y);}
            else {m_rightPanelMin=min;m_rightPanelMax=ImVec2(min.x+size.x,min.y+size.y);}
            if(ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))m_lastFocusedPanel=&panel;
            renderPanel(panel,side);
            FilePanel* source = nullptr;
            const ImGuiPayload* drag = ImGui::GetDragDropPayload();
            if (drag && drag->IsDataType("FILE_DRAG") && drag->DataSize == sizeof(DragPayload)) {
                source = static_cast<const DragPayload*>(drag->Data)->sourcePanel;
            }
            bool validSource = source == &m_leftPanel || source == &m_rightPanel;
            bool canDrop = validSource && source != &panel && !panel.isApps && !panel.isConnections &&
                panelAvailable(panel) && panelAvailable(*source) &&
                !(source->isAndroid && panel.isAndroid && source->deviceSlot == panel.deviceSlot);
            if (auto drop = ui::endDropTargetChild<DragPayload>("FILE_DRAG", canDrop)) {
                startTransferFromDrag(*drop->sourcePanel, panel);
                m_dragSourcePanel = nullptr;
            }
        };
        pane(m_leftPanel,PanelSide::Left,leftWidth);
        ImGui::SameLine(0,0);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
        ImGui::Button("##PaneDivider",ImVec2(gap,bodyHeight));
        ImGui::PopStyleColor();ImGui::PopStyleVar();
        if(ImGui::IsItemHovered()||ImGui::IsItemActive())ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if(ImGui::IsItemActive()&&ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            m_prefs.paneSplit=std::clamp(m_prefs.paneSplit+io.MouseDelta.x/(width-gap),0.3f,0.7f);
        if(ImGui::IsItemFocused()) {
            if(ImGui::IsKeyPressed(ImGuiKey_LeftArrow))m_prefs.paneSplit=std::max(0.3f,m_prefs.paneSplit-0.02f);
            if(ImGui::IsKeyPressed(ImGuiKey_RightArrow))m_prefs.paneSplit=std::min(0.7f,m_prefs.paneSplit+0.02f);
        }
        if(ImGui::IsItemDeactivated())m_prefs.save();
        auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2((a.x+b.x)*0.5f,(a.y+b.y)*0.5f-14*s),ImVec2((a.x+b.x)*0.5f,(a.y+b.y)*0.5f+14*s),ImGui::GetColorU32(ImGuiCol_Border),2*s);
        ImGui::SameLine(0,0);
        pane(m_rightPanel,PanelSide::Right,width-gap-leftWidth);
    }
    ImGui::SetCursorPosX(0);
    ImGui::SetCursorPosY(bodyY+bodyHeight+8*s);
    renderTransferDock(dockHeight);
    ImGui::SetCursorPosY(bodyY+bodyHeight+8*s+dockHeight);
    renderStatusBar();
    ImGui::End();
    renderOpenFileCard();
    renderWorkspaceDrawer();
    if(m_showDeviceSettings)renderDeviceBar();


    // --- Transfer overlay (separate window) ---
    renderTransferOverlay();

    renderNewFolderPopup();
    renderRenamePopup();
    renderDeleteConfirmPopup();
    renderCopyMoveDialog();
    renderCrossDeviceDialog();
    renderWifiWizard();
    renderWifiPairingDialog();
    renderAboutPopup();
    renderNotificationPopup();
    renderApkInstallDialog();
    renderCloseConfirmDialog();
    if (m_showPreferences) renderPreferencesWindow();
    if (m_showNicConfig) renderNicConfigWindow();
    if (m_showThemeWindow) renderThemeWindow();

    if (m_showDebugWindow) renderDebugWindow();

    // Connection mode popup
    if (m_showConnectionPopup) {
        ImGui::OpenPopup("Connection Mode");
        m_showConnectionPopup = false;
        m_connectionPopupScanned = false;
        m_detectedIps.clear();
        // Scan IPs asynchronously when popup opens
        std::string scanSerial;
        {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size())
                scanSerial = m_devices[m_selectedDevice].serial;
        }
        if (!scanSerial.empty()) {
            postAsync("Scanning device IPs...", [this, scanSerial]() {
                const char* ifaces[] = {"rndis0", "usb0", "ncm0"};
                const char* labels[] = {"USB Tethering (rndis0)", "USB (usb0)", "USB NCM (ncm0)"};
                std::vector<DetectedIp> found;
                for (int i = 0; i < 3; i++) {
                    std::string cmd = "-s " + scanSerial + " shell \"ip -4 addr show " + std::string(ifaces[i]) + " 2>/dev/null | grep inet\"";
                    std::string out = m_device.runAdbCommand(cmd);
                    auto pos = out.find("inet ");
                    if (pos == std::string::npos) continue;
                    pos += 5;
                    auto slash = out.find('/', pos);
                    auto space = out.find(' ', pos);
                    auto endp = std::min(slash, space);
                    if (endp == std::string::npos || endp <= pos) continue;
                    std::string ip = out.substr(pos, endp - pos);
                    if (ip == "127.0.0.1") continue;
                    found.push_back({ ifaces[i], labels[i], ip });
                }
                m_detectedIps = std::move(found);
                m_connectionPopupScanned = true;
            });
        } else {
            m_connectionPopupScanned = true;
        }
    }
    if (ui::beginDialog("Connection Mode", 460)) {
        const float s = ui::scale();
        const bool direct = m_device.isDirectConnection();
        bool closeRequested = ui::dialogHeader(direct ? ui::Icon::Wifi : ui::Icon::Usb, ui::accentColor(ui::Accent::Info),
            "Connection mode", direct ? "Direct TCP: " + m_device.deviceIp() : std::string("ADB Forward over USB"), true);
        if (closeRequested) ImGui::CloseCurrentPopup();

        // Show detected IPs (scanned asynchronously)
        ui::sectionLabel("DETECTED DEVICE IPS");

        std::string popupSerial;
        {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size())
                popupSerial = m_devices[m_selectedDevice].serial;
        }
        if (!m_connectionPopupScanned) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Scanning...");
        } else if (!popupSerial.empty()) {
            bool foundAny = !m_detectedIps.empty();
            for (auto& det : m_detectedIps) {
                ImGui::BulletText("%s: %s", det.label.c_str(), det.ip.c_str());
                ImGui::SameLine();

                bool isCurrent = m_device.isDirectConnection() && m_device.deviceIp() == det.ip;
                if (isCurrent) {
                    ImGui::TextColored(ImVec4(0.3f, 1, 0.5f, 1), "(active)");
                } else {
                    std::string btnId = "Connect##" + det.iface;
                    if (modernButton(btnId.c_str(), ImVec2(0, 0), ButtonTone::Primary)) {
                        std::string connectIp = det.ip;
                        std::string ser = popupSerial;
                        ImGui::CloseCurrentPopup();
                        postAsync("Connecting to " + connectIp + "...", [this, connectIp, ser]() {
                            if (m_device.connectDirect(connectIp)) {
                                m_statusMessage = "Direct TCP: " + connectIp;
                            } else {
                                m_device.startServer(ser, m_preferAdbForward, m_prefs.rootEnabledForSerial(deviceIdentity(ser)));
                                m_statusMessage = "Failed to connect to " + connectIp;
                            }
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                }
            }

            if (!foundAny) {
                ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "No network interfaces found.");
                ImGui::Spacing();
                if (modernButton("Enable USB Tethering", ImVec2(-1, ui::footerButtonHeight()), ButtonTone::Primary)) {
                    std::string ser = popupSerial;
                    ImGui::CloseCurrentPopup();
                    postAsync("Enabling USB tethering...", [this, ser]() {
                        if (m_device.tryEnableUsbTethering(ser)) {
                            m_statusMessage = "USB Tethering enabled!";
                        } else {
                            m_statusMessage = "Could not enable tethering - enable manually in device Settings";
                        }
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }
                ImGui::TextDisabled("Or enable manually: Settings > Network > Hotspot > USB Tethering");
            }
        }

        // Manual IP input
        ui::sectionLabel("MANUAL IP");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110 * s);
        bool enterIp = ImGui::InputText("##ManualIP", m_manualIp, sizeof(m_manualIp), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((modernButton("Connect##manual", ImVec2(-FLT_MIN, 0), ButtonTone::Primary) || enterIp) && strlen(m_manualIp) > 0) {
            std::string connectIp(m_manualIp);
            std::string ser = popupSerial;
            ImGui::CloseCurrentPopup();
            postAsync("Connecting to " + connectIp + "...", [this, connectIp, ser]() {
                if (m_device.connectDirect(connectIp)) {
                    m_statusMessage = "Direct TCP: " + connectIp;
                } else {
                    if (!ser.empty()) m_device.startServer(ser, m_preferAdbForward, m_prefs.rootEnabledForSerial(deviceIdentity(ser)));
                    m_statusMessage = "Failed to connect to " + connectIp;
                }
                m_statusTime = std::chrono::steady_clock::now();
            });
        }

        ImGui::Spacing();

        // Switch back to ADB forward
        if (m_device.isDirectConnection()) {
            if (modernButton("Switch to ADB Forward", ImVec2(-1, 0))) {
                std::string ser = popupSerial;
                ImGui::CloseCurrentPopup();
                postAsync("Switching to ADB Forward...", [this, ser]() {
                    m_device.disconnectTcp();
                    // Set up ADB forward and connect directly — don't use startServer which would pick Direct TCP again
                    m_device.runAdbCommand("-s " + ser + " forward tcp:" + std::to_string(AFM_PORT) + " tcp:" + std::to_string(AFM_PORT));
                    if (m_device.connectTcp("127.0.0.1", AFM_PORT) && m_device.verifyConnection()) {
                        m_statusMessage = "Switched to ADB Forward";
                    } else {
                        m_device.disconnectTcp();
                        m_statusMessage = "ADB Forward failed — reconnecting...";
                        m_device.startServer(ser, m_preferAdbForward, m_prefs.rootEnabledForSerial(deviceIdentity(ser)));
                    }
                    m_statusTime = std::chrono::steady_clock::now();
                });
            }
        }

        const float closeW = 110 * s;
        ui::dialogFooter(closeW);
        if (modernButton("Close##ConnectionModeClose", ImVec2(closeW, ui::footerButtonHeight()))) {
            ImGui::CloseCurrentPopup();
        }
        ui::endDialog();
    }

    renderTransferAttention();

    // Update Windows taskbar progress
    {
        bool anyActive = false;
        float bestProg = 0;
        BatchState bestState = BatchState::Queued;
        {
            std::lock_guard<std::mutex> lk(m_batchMutex);
            for (auto& b : m_batchQueue) {
                auto s = b->state.load();
                if (s == BatchState::Running || s == BatchState::Paused || s == BatchState::Verifying || s == BatchState::WaitingConflict) {
                    anyActive = true;
                    bestProg = b->totalProgress.load();
                    bestState = s;
                    break;
                }
            }
        }
        if (anyActive) {
            if (bestState == BatchState::Paused || bestState == BatchState::WaitingConflict)
                setTaskbarPaused();
            else
                updateTaskbarProgress(bestProg, true);
        } else {
            // Check if last batch failed
            bool lastFailed = false;
            {
                std::lock_guard<std::mutex> lk(m_batchMutex);
                if (!m_batchQueue.empty()) {
                    auto s = m_batchQueue.back()->state.load();
                    if (s == BatchState::Failed) lastFailed = true;
                }
            }
            if (lastFailed)
                setTaskbarError();
            else
                updateTaskbarProgress(0, false);
        }
    }

    m_firstFrame = false;
}

bool App::wantsHighFps() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2] || io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)
        return true;
    if (ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        return true;

    if (m_isDragging || m_pendingOleDrag || m_pendingScale > 0.0f || m_themeNeedsReposition || m_showCloseConfirm)
        return true;

    if (m_shellEntranceAnimation < 0.999f || deviceSession(0).client.isConnecting() || deviceSession(1).client.isConnecting())
        return true;
    for (const auto& [id, value] : m_buttonHoverAnimation) {
        if (value > 0.001f && value < 0.999f) return true;
    }

    if (m_asyncBusy.load() || m_pollBusy.load() || m_tetheringInProgress.load())
        return true;

    if (m_compareEnabled && m_compareDirty)
        return true;
    {
        std::lock_guard<std::mutex> lk(m_compareMutex);
        if (m_compareContentBusy)
            return true;
    }

    if ((m_overlayVisible || m_overlayFade > 0.01f) && m_overlayFade < 0.99f)
        return true;

    {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        for (const auto& batch : m_batchQueue) {
            BatchState state = batch->state.load();
            if (state == BatchState::Preparing || state == BatchState::Queued || state == BatchState::Running ||
                state == BatchState::Verifying || state == BatchState::WaitingConflict) {
                return true;
            }
        }
    }

    return false;
}

void App::renderMenuBar() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10*ui::scale(), 10*ui::scale()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10*ui::scale(), 9*ui::scale()));
    if (ImGui::BeginPopup("##WorkspaceMenu")) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Refresh All", "F5")) {
                m_leftPanel.needsRefresh = true;
                m_rightPanel.needsRefresh = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                if (m_prefs.confirmOnClose) {
                    m_showCloseConfirm = true;
                } else {
                    g_reallyQuit = true;
                    PostMessage(g_mainHwnd, WM_CLOSE, 0, 0);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Preferences...")) {
                m_showPreferences = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Show Hidden (Windows)", nullptr, &m_leftPanel.showHidden);
            ImGui::MenuItem("Show Hidden (Android)", nullptr, &m_rightPanel.showHidden);
            ImGui::Separator();
            if (ImGui::MenuItem("Transfer details", nullptr, m_overlayVisible)) {
                m_overlayVisible = !m_overlayVisible;
            }
            if (ImGui::MenuItem("Appearance...")) {
                m_showThemeWindow = true;
            }
            ImGui::MenuItem("Activity log", "F12", &m_showDebugWindow);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Backup Manager")) {
            m_showBackupManager = true;
            m_showAppsWorkspace = m_showConnectionsWorkspace = false;
            m_backupManagerApps.clear();
            m_backupManagerAppSelectionAnchor = -1;
            m_backupManagerNeedsAppRefresh = true;
            m_backupManagerNeedsBackupRefresh = true;
            m_backupRootAccess = BackupRootAccess::Unknown;
            m_backupRootAccessSerial.clear();
        }
        if (ImGui::BeginMenu("Transfer")) {
            if (ImGui::MenuItem("Copy to Android  >>", "F6")) startTransfer(false);
            if (ImGui::MenuItem("<< Copy to Windows", "F7")) startTransfer(true);
            ImGui::Separator();
            if (ImGui::MenuItem("Register APK Double-Click Installer")) {
                registerApkAssociation();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Connection")) {
            bool hasDevice = false;
            std::string curSerial;
            {
                std::lock_guard<std::mutex> lk(m_deviceMutex);
                if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size() && m_devices[m_selectedDevice].state == "device") {
                    hasDevice = true;
                    curSerial = m_devices[m_selectedDevice].serial;
                }
            }
            if (!hasDevice && deviceSession(0).connected && m_device.isServerRunning()) {
                hasDevice = true;
                curSerial = deviceSession(0).serial;
            }

            if (m_device.isServerRunning()) {
                if (m_device.isDirectConnection())
                    ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "Direct TCP: %s", m_device.deviceIp().c_str());
                else
                    ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1), "ADB Forward");
                ImGui::Separator();
            }

            if (m_asyncBusy) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Working: %s", m_asyncStatus.c_str());
            } else if (hasDevice) {
                bool isDirect = m_device.isDirectConnection();
                bool isConnected = m_device.isServerRunning();

#if 0 // Hidden: superseded by USB 2x / WiFi 2x multi-pipe options
                if (ImGui::MenuItem("Connection Mode...")) {
                    m_showConnectionPopup = true;
                }
#endif
                if (ImGui::MenuItem("Multi-NIC Configuration...")) {
                    m_showNicConfig = true;
                    if (m_detectedNics.empty())
                        m_detectedNics = enumerateNics();
                    m_nicTestResults.assign(m_detectedNics.size(), false);
                    m_nicTestDone.assign(m_detectedNics.size(), false);
                    m_nicTestReason.assign(m_detectedNics.size(), "");
                }
#if 0 // Hidden: superseded by USB 2x / WiFi 2x multi-pipe options. Logic preserved.
                ImGui::Separator();

                // Switch between connection types
                if (isDirect) {
                    if (ImGui::MenuItem("Switch to ADB Forward")) {
                        m_preferAdbForward = true;
                        std::string ser = curSerial;
                        postAsync("Switching to ADB Forward...", [this, ser]() {
                            m_device.disconnectTcp();
                            m_device.runAdbCommand("-s " + ser + " forward tcp:" + std::to_string(AFM_PORT) + " tcp:" + std::to_string(AFM_PORT));
                            if (m_device.connectTcp("127.0.0.1", AFM_PORT) && m_device.verifyConnection()) {
                                m_statusMessage = "Switched to ADB Forward";
                            } else {
                                m_device.disconnectTcp();
                                m_statusMessage = "ADB Forward failed";
                                m_device.startServer(ser, m_preferAdbForward, m_prefs.rootEnabledForSerial(deviceIdentity(ser)));
                            }
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                } else {
                    if (ImGui::MenuItem("Switch to Direct TCP")) {
                        m_preferAdbForward = false;
                        std::string ser = curSerial;
                        postAsync("Switching to Direct TCP...", [this, ser]() {
                            LOG_INFO("UI", "Manual switch to Direct TCP");
                            std::string ip = m_device.detectDeviceIp(ser);
                            // No tethering IP? Enable it first
                            if (ip.empty()) {
                                LOG_INFO("UI", "No tethering IP — enabling tethering");
                                m_tetheringInProgress = true;
                                m_device.tryEnableUsbTethering(ser);
                                m_tetheringInProgress = false;
                                ip = m_device.detectDeviceIp(ser);
                            }
                            if (!ip.empty()) {
                                if (m_device.connectDirect(ip)) {
                                    m_statusMessage = "Direct TCP: " + ip;
                                } else {
                                    m_statusMessage = "Direct TCP failed: " + m_device.lastError();
                                }
                            } else {
                                m_statusMessage = "No USB tethering IP found — tethering may not be supported";
                            }
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Enable USB Tethering")) {
                    std::string ser = curSerial;
                    postAsync("Enabling USB tethering...", [this, ser]() {
                        LOG_INFO("UI", "Manual tethering enable");
                        if (m_device.tryEnableUsbTethering(ser)) {
                            m_statusMessage = "USB tethering enabled";
                            std::string ip = m_device.detectDeviceIp(ser);
                            if (!ip.empty() && m_device.connectDirect(ip))
                                m_statusMessage = "Tethering + Direct TCP: " + ip;
                        } else {
                            m_statusMessage = "Tethering unavailable";
                        }
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }
#endif

                ImGui::Separator();

                // Dokan virtual drive mount (per device slot)
                for (int slot = 0; slot < deviceSessionCount(); slot++) {
                    if (!deviceSession(slot).connected) continue;
                    auto& mgr = DeviceMountManager::instance(slot);
                    std::string driveLetter = DeviceMountManager::availableMountPoint();
                    std::string slotLabel = deviceSession(slot).serial;
                    if (slotLabel.size() > 12) slotLabel = slotLabel.substr(0, 12) + "..";

                    if (mgr.isMounted()) {
                        std::string mp = mgr.mountPoint();
                        ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "Mounted: %s (%s)", mp.c_str(), slotLabel.c_str());
                        std::string unmountId = "Unmount " + mp + "##" + std::to_string(slot);
                        if (ImGui::MenuItem(unmountId.c_str())) {
                            postAsync("Unmounting...", [slot]() {
                                DeviceMountManager::instance(slot).unmount();
                            });
                        }
                        std::string openId = "Open " + mp + " in Explorer##" + std::to_string(slot);
                        if (ImGui::MenuItem(openId.c_str())) {
                            ShellExecuteA(nullptr, "explore", mp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        }
                    } else {
                        std::string mountId = "Mount " + slotLabel + " as " + driveLetter + "##" + std::to_string(slot);
                        if (ImGui::MenuItem(mountId.c_str())) {
                            std::string sr = deviceSession(slot).storageRoot;
                            postAsync("Mounting virtual drive...", [this, slot, sr, driveLetter]() {
                                if (DeviceMountManager::instance(slot).mount(&deviceSession(slot).client, sr, driveLetter)) {
                                    m_statusMessage = "Mounted: " + driveLetter;
                                    ShellExecuteA(nullptr, "explore", driveLetter.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                } else {
                                    m_statusMessage = "Mount failed — is Dokan driver installed?";
                                }
                                m_statusTime = std::chrono::steady_clock::now();
                            });
                        }
                    }
                }

                ImGui::Separator();

                // WiFi ADB options
                if (ImGui::BeginMenu("WiFi ADB")) {
                    if (ImGui::MenuItem("Setup Wizard...", nullptr, false, hasDevice)) {
                        openWifiWizard();
                    }
                    if (ImGui::MenuItem("Pair (Android 11+)...")) {
                        openWifiPairing();
                    }
                    ImGui::Separator();
                    if (!m_prefs.savedWifiDevices.empty()) {
                        ImGui::TextDisabled("Saved devices:");
                        for (int i = 0; i < (int)m_prefs.savedWifiDevices.size(); i++) {
                            auto& w = m_prefs.savedWifiDevices[i];
                            std::string label = w.wifiIp + ":" + std::to_string(w.port);
                            if (!w.serial.empty()) label += " (" + w.serial.substr(0, 8) + ")";
                            ImGui::PushID(i);
                            if (ImGui::MenuItem(label.c_str())) {
                                // Manual connect to saved device
                                std::string ip = w.wifiIp;
                                int port = w.port;
                                postAsync("Connecting WiFi...", [this, ip, port]() {
                                    std::string connectAddr = ip + ":" + std::to_string(port);
                                    std::string result = m_device.runAdbCommand("connect " + connectAddr, 4000);
                                    if (result.find("connected") != std::string::npos &&
                                        result.find("failed") == std::string::npos) {
                                        connectDeviceBySerialNow(connectAddr);
                                    } else {
                                        m_statusMessage = result;
                                    }
                                    m_statusTime = std::chrono::steady_clock::now();
                                });
                            }
                            ImGui::SameLine();
                            if (modernSmallButton("X")) {
                                m_prefs.savedWifiDevices.erase(m_prefs.savedWifiDevices.begin() + i);
                                m_prefs.save();
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndMenu();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Reconnect Server")) {
                    std::string ser = curSerial;
                    bool preferForward = m_preferAdbForward;
                    postAsync("Reconnecting server...", [this, ser, preferForward]() {
                        LOG_INFO("UI", std::string("Manual reconnect (prefer ") + (preferForward ? "ADB Forward" : "Direct TCP") + ")");
                        m_device.stopServer();
                        bool useRoot = m_prefs.rootEnabledForSerial(deviceIdentity(ser));
                        m_device.startServer(ser, preferForward, useRoot);
                        m_statusMessage = m_device.isServerRunning()
                            ? ("Reconnected via " + std::string(m_device.isDirectConnection() ? "Direct TCP" : "ADB Forward"))
                            : ("Reconnect failed: " + m_device.lastError());
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }

                if (ImGui::MenuItem("Restart ADB Daemon")) {
                    postAsync("Restarting ADB daemon...", [this]() {
                        LOG_INFO("UI", "Manual ADB daemon restart");
                        m_device.runAdbCommand("kill-server");
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        m_device.runAdbCommand("start-server");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        m_statusMessage = "ADB daemon restarted";
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }
            } else {
                ImGui::TextDisabled("No device connected");

                if (ImGui::MenuItem("Restart ADB Daemon")) {
                    postAsync("Restarting ADB daemon...", [this]() {
                        LOG_INFO("UI", "Manual ADB daemon restart");
                        m_device.runAdbCommand("kill-server");
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        m_device.runAdbCommand("start-server");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        m_statusMessage = "ADB daemon restarted";
                        m_statusTime = std::chrono::steady_clock::now();
                    });
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                m_showAboutPopup = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void App::renderCompareToolbar() {
    ImGui::Separator();

    bool compareWasEnabled = m_compareEnabled;
    if (modernButton(m_compareEnabled ? "Compare: On" : "Compare", ImVec2(0, 0), compareWasEnabled ? ButtonTone::Primary : ButtonTone::Secondary)) {
        m_compareEnabled = !m_compareEnabled;
        m_compareDirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Highlight items in the current folder that are missing from the other pane");

    ImGui::SameLine(0, 8);
    ImGui::BeginDisabled(!m_compareEnabled);

    const char* currentMode = "Both sides";
    if (m_compareMode == CompareHighlightMode::LeftOnly) currentMode = "Left side";
    else if (m_compareMode == CompareHighlightMode::RightOnly) currentMode = "Right side";

    ImGui::SetNextItemWidth(130.0f);
    if (ui::beginCombo("##CompareMode", currentMode)) {
        if (ImGui::Selectable("Both sides", m_compareMode == CompareHighlightMode::Both)) {
            m_compareMode = CompareHighlightMode::Both;
        }
        if (ImGui::Selectable("Left side", m_compareMode == CompareHighlightMode::LeftOnly)) {
            m_compareMode = CompareHighlightMode::LeftOnly;
        }
        if (ImGui::Selectable("Right side", m_compareMode == CompareHighlightMode::RightOnly)) {
            m_compareMode = CompareHighlightMode::RightOnly;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Choose which pane shows items that do not exist on the opposite pane");

    ImGui::SameLine(0, 8);
    if (ui::checkbox("Size/hash##CompareContent", &m_compareContentEnabled)) {
        std::lock_guard<std::mutex> lk(m_compareMutex);
        m_compareContentSignature.clear();
        m_compareContent.clear();
        m_compareSizeMismatchCount = 0;
        m_compareHashMatchCount = 0;
        m_compareHashMismatchCount = 0;
        m_compareHashErrorCount = 0;
        m_compareContentGeneration++;
        m_compareDirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For names that exist on both sides, compare file size and SHA-256 hash");

    ImGui::SameLine(0, 8);
    if (modernSmallButton("Recheck")) {
        m_leftPanel.needsRefresh = true;
        m_rightPanel.needsRefresh = true;
        {
            std::lock_guard<std::mutex> lk(m_compareMutex);
            m_compareContentSignature.clear();
            m_compareContentGeneration++;
        }
        m_compareDirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Refresh both folders and compare again");

    ImGui::EndDisabled();

    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    if (m_compareEnabled) {
        if (m_compareContentEnabled) {
            ImGui::TextDisabled("Missing L %d, R %d. Size %d. Hash ok %d, diff %d, errors %d%s",
                m_compareMissingLeftCount, m_compareMissingRightCount,
                m_compareSizeMismatchCount, m_compareHashMatchCount,
                m_compareHashMismatchCount, m_compareHashErrorCount,
                m_compareContentBusy ? ". Hashing..." : "");
        } else {
            ImGui::TextDisabled("Missing, left %d, right %d", m_compareMissingLeftCount, m_compareMissingRightCount);
        }
    } else {
        ImGui::TextDisabled("Compare current folders");
    }
    ImGui::Separator();
}

void App::updateCompareHighlights() {
    m_compareMissingLeft.clear();
    m_compareMissingRight.clear();
    m_compareMissingLeftCount = 0;
    m_compareMissingRightCount = 0;

    if (!m_compareEnabled) return;

    std::unordered_set<std::string> leftNames;
    std::unordered_set<std::string> rightNames;

    for (int i = 0; i < m_leftPanel.entryCount(); i++) {
        std::string name = m_leftPanel.entryName(i);
        if (!name.empty()) leftNames.insert(normalizeCompareName(name));
    }
    for (int i = 0; i < m_rightPanel.entryCount(); i++) {
        std::string name = m_rightPanel.entryName(i);
        if (!name.empty()) rightNames.insert(normalizeCompareName(name));
    }

    for (const auto& name : leftNames) {
        if (rightNames.find(name) == rightNames.end()) m_compareMissingLeft.insert(name);
    }
    for (const auto& name : rightNames) {
        if (leftNames.find(name) == leftNames.end()) m_compareMissingRight.insert(name);
    }

    m_compareMissingLeftCount = (int)m_compareMissingLeft.size();
    m_compareMissingRightCount = (int)m_compareMissingRight.size();

    std::unordered_map<std::string, int> leftIndexByName;
    std::unordered_map<std::string, int> rightIndexByName;
    for (int i = 0; i < m_leftPanel.entryCount(); i++) {
        std::string key = normalizeCompareName(m_leftPanel.entryName(i));
        leftIndexByName[key] = i;
    }
    for (int i = 0; i < m_rightPanel.entryCount(); i++) {
        std::string key = normalizeCompareName(m_rightPanel.entryName(i));
        rightIndexByName[key] = i;
    }

    bool shouldStartHash = false;
    std::string signature = std::to_string(m_compareContentGeneration + 1);
    if (m_compareContentEnabled) {
        std::lock_guard<std::mutex> lk(m_compareMutex);
        m_compareContentSignature = signature;
        m_compareContent.clear();
        m_compareSizeMismatchCount = 0;
        m_compareHashMatchCount = 0;
        m_compareHashMismatchCount = 0;
        m_compareHashErrorCount = 0;
        m_compareContentGeneration++;

        for (const auto& [name, leftIndex] : leftIndexByName) {
            auto rightIt = rightIndexByName.find(name);
            if (rightIt == rightIndexByName.end()) continue;
            int rightIndex = rightIt->second;
            if (m_leftPanel.entryIsDir(leftIndex) || m_rightPanel.entryIsDir(rightIndex)) continue;

            CompareContentInfo info;
            info.leftSize = m_leftPanel.entrySize(leftIndex);
            info.rightSize = m_rightPanel.entrySize(rightIndex);
            if (info.leftSize != info.rightSize) {
                info.state = CompareContentState::SizeMismatch;
                info.detail = "File sizes are different";
                m_compareSizeMismatchCount++;
            } else {
                info.state = CompareContentState::HashPending;
                shouldStartHash = true;
            }
            m_compareContent[name] = std::move(info);
        }

        if (shouldStartHash) m_compareContentBusy = true;
        else m_compareContentBusy = false;
    } else {
        std::lock_guard<std::mutex> lk(m_compareMutex);
        m_compareContentSignature.clear();
        m_compareContent.clear();
        m_compareContentBusy = false;
        m_compareSizeMismatchCount = 0;
        m_compareHashMatchCount = 0;
        m_compareHashMismatchCount = 0;
        m_compareHashErrorCount = 0;
    }

    if (shouldStartHash) startCompareHashCheck(signature);
}

bool App::isCompareHighlighted(const FilePanel& panel, PanelSide side, int index) const {
    if (!m_compareEnabled || !panel.validIndex(index)) return false;

    if (side == PanelSide::Left && m_compareMode == CompareHighlightMode::RightOnly) return false;
    if (side == PanelSide::Right && m_compareMode == CompareHighlightMode::LeftOnly) return false;

    std::string name = normalizeCompareName(panel.entryName(index));
    if (side == PanelSide::Left) {
        return m_compareMissingLeft.find(name) != m_compareMissingLeft.end();
    }
    return m_compareMissingRight.find(name) != m_compareMissingRight.end();
}

std::string App::favoriteLabelForPath(const std::string& path, bool isAndroid) {
    if (path.empty()) return isAndroid ? "/" : "PC";
    std::string trimmed = path;
    while (trimmed.size() > 1 && (lastCharOr(trimmed) == '/' || lastCharOr(trimmed) == '\\')) trimmed.pop_back();
    if (!isAndroid && trimmed.size() == 2 && trimmed[1] == ':') return trimmed + "\\";
    size_t pos = trimmed.find_last_of(isAndroid ? "/" : "\\");
    if (pos == std::string::npos) return trimmed;
    std::string label = trimmed.substr(pos + 1);
    if (label.empty()) return isAndroid ? "/" : trimmed;
    return label;
}

bool App::isFavoritePath(const FilePanel& panel) const {
    if (panel.isApps) return false;
    for (const auto& fav : m_prefs.favoritePaths) {
        if (fav.isAndroid == panel.isAndroid &&
            fav.deviceSlot == panel.deviceSlot &&
            fav.path == panel.currentPath) {
            return true;
        }
    }
    return false;
}

void App::addFavoritePath(const FilePanel& panel) {
    if (panel.isApps || panel.currentPath.empty() || isFavoritePath(panel)) return;

    FavoritePath fav;
    fav.path = panel.currentPath;
    fav.isAndroid = panel.isAndroid;
    fav.deviceSlot = panel.deviceSlot;
    fav.label = favoriteLabelForPath(panel.currentPath, panel.isAndroid);
    m_prefs.favoritePaths.push_back(std::move(fav));
    m_prefs.save();

    m_statusMessage = "Favorite added";
    m_statusTime = std::chrono::steady_clock::now();
}

void App::removeFavoritePath(int index) {
    if (index < 0 || index >= (int)m_prefs.favoritePaths.size()) return;
    m_prefs.favoritePaths.erase(m_prefs.favoritePaths.begin() + index);
    m_prefs.save();
    m_statusMessage = "Favorite removed";
    m_statusTime = std::chrono::steady_clock::now();
}

void App::renderFavoritesBar(FilePanel& panel, PanelSide side) {
    const float scale = ui::scale();
    bool favorite = isFavoritePath(panel);
    if (ui::button("##FavoriteFolder", ui::Icon::Star,
        favorite ? ui::Appearance::Selected : ui::Appearance::Quiet, ImVec2(26*scale,26*scale),
        favorite ? "Remove folder from favorites" : "Add folder to favorites")) {
        if (favorite) {
            for (int i=0; i<(int)m_prefs.favoritePaths.size(); ++i) {
                const auto& f = m_prefs.favoritePaths[i];
                if (f.isAndroid==panel.isAndroid && f.deviceSlot==panel.deviceSlot && f.path==panel.currentPath) {
                    removeFavoritePath(i);
                    break;
                }
            }
        } else addFavoritePath(panel);
    }
    struct Shortcut { std::string label, path; int favoriteIndex=-1; };
    std::vector<Shortcut> shortcuts;
    for (int i=0; i<(int)m_prefs.favoritePaths.size(); ++i) {
        const auto& f=m_prefs.favoritePaths[i];
        if (f.isAndroid!=panel.isAndroid || (f.isAndroid && f.deviceSlot!=panel.deviceSlot)) continue;
        shortcuts.push_back({f.label.empty()?favoriteLabelForPath(f.path,f.isAndroid):f.label,f.path,i});
    }
    if (!panel.isAndroid) {
        for (const auto& drive:getWindowsDrives()) shortcuts.push_back({drive,drive+"\\"});
    } else {
        int slot=panel.deviceSlot;
        std::string root=deviceSession(slot).storageRoot.empty()?"/sdcard":deviceSession(slot).storageRoot;
        for (size_t i=0; i<deviceSession(slot).volumes.size(); ++i)
            shortcuts.push_back({i==0?"Internal":"SD "+std::to_string(i),deviceSession(slot).volumes[i]});
        if (deviceSession(slot).volumes.empty()) shortcuts.push_back({"Storage",root});
        for (const char* folder:{"Download","DCIM","Documents","Pictures","Music"})
            shortcuts.push_back({folder,root+"/"+folder});
        shortcuts.push_back({"/","/"});
    }
    for (size_t i=0; i<shortcuts.size(); ++i) {
        const auto& link=shortcuts[i];
        ImGui::SameLine(0,5*scale);
        float width=ImGui::CalcTextSize(link.label.c_str()).x+22*scale;
        if (width+30*scale>ImGui::GetContentRegionAvail().x) {
            if (ui::button("##MoreShortcuts",ui::Icon::Down,ui::Appearance::Quiet,ImVec2(26*scale,26*scale),"More folder shortcuts"))
                ImGui::OpenPopup("##FolderShortcutMenu");
            if (ImGui::BeginPopup("##FolderShortcutMenu")) {
                for (size_t j=i; j<shortcuts.size(); ++j) {
                    ImGui::PushID((int)j);
                    if (ImGui::MenuItem(shortcuts[j].label.c_str())) navigateToDirectory(panel,shortcuts[j].path);
                    ui::tooltip(shortcuts[j].path.c_str());
                    if (shortcuts[j].favoriteIndex>=0 && ImGui::IsItemClicked(ImGuiMouseButton_Right))
                        removeFavoritePath(shortcuts[j].favoriteIndex);
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            break;
        }
        ImGui::PushID((int)i);
        std::string linkPath=link.path, currentPath=panel.currentPath;
        while (linkPath.size()>1 && (linkPath.back()=='\\'||linkPath.back()=='/')) linkPath.pop_back();
        while (currentPath.size()>1 && (currentPath.back()=='\\'||currentPath.back()=='/')) currentPath.pop_back();
        const bool here=_stricmp(linkPath.c_str(),currentPath.c_str())==0;
        if (ui::button(link.label.c_str(),ui::Icon::None,here?ui::Appearance::Selected:ui::Appearance::Secondary,ImVec2(width,24*scale),link.path.c_str()))
            navigateToDirectory(panel,link.path);
        if (link.favoriteIndex>=0 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) removeFavoritePath(link.favoriteIndex);
        ImGui::PopID();
    }
}

std::string App::compareEntryPath(const FilePanel& panel, int index) const {
    if (panel.isApps) return panel.entryName(index);
    std::string path = panel.currentPath;
    char sep = panel.isAndroid ? '/' : '\\';
    appendPathSeparator(path, sep);
    path += panel.entryName(index);
    return path;
}

void App::startCompareHashCheck(const std::string& signature) {
    struct HashTask {
        std::string name;
        bool leftAndroid = false;
        bool rightAndroid = false;
        int leftSlot = 0;
        int rightSlot = 0;
        std::string leftPath;
        std::string rightPath;
        uint64_t size = 0;
    };

    std::vector<HashTask> tasks;
    {
        std::lock_guard<std::mutex> lk(m_compareMutex);
        for (const auto& [name, info] : m_compareContent) {
            if (info.state != CompareContentState::HashPending) continue;

            int leftIndex = -1;
            int rightIndex = -1;
            for (int i = 0; i < m_leftPanel.entryCount(); i++) {
                if (normalizeCompareName(m_leftPanel.entryName(i)) == name) { leftIndex = i; break; }
            }
            for (int i = 0; i < m_rightPanel.entryCount(); i++) {
                if (normalizeCompareName(m_rightPanel.entryName(i)) == name) { rightIndex = i; break; }
            }
            if (leftIndex < 0 || rightIndex < 0) continue;

            HashTask task;
            task.name = name;
            task.leftAndroid = m_leftPanel.isAndroid;
            task.rightAndroid = m_rightPanel.isAndroid;
            task.leftSlot = m_leftPanel.deviceSlot;
            task.rightSlot = m_rightPanel.deviceSlot;
            task.leftPath = compareEntryPath(m_leftPanel, leftIndex);
            task.rightPath = compareEntryPath(m_rightPanel, rightIndex);
            task.size = info.leftSize;
            tasks.push_back(std::move(task));
        }
    }

    if (tasks.empty()) {
        std::lock_guard<std::mutex> lk(m_compareMutex);
        if (m_compareContentSignature == signature) m_compareContentBusy = false;
        return;
    }

    int generation;
    {
        std::lock_guard<std::mutex> lk(m_compareMutex);
        generation = m_compareContentGeneration;
    }

    postAsync("Hashing matching files...", [this, tasks = std::move(tasks), signature, generation]() {
        {
            std::lock_guard<std::mutex> lk(m_compareMutex);
            if (generation != m_compareContentGeneration || signature != m_compareContentSignature) {
                return;
            }
        }

        auto hashFor = [this](bool android, int slot, const std::string& path, uint64_t size,
                              std::string& hash, std::string& detail) -> bool {
            if (android) {
                DeviceClient& dev = deviceForSlot(slot);
                if (!dev.isServerRunning()) {
                    detail = "Android side is not connected";
                    return false;
                }
                if (dev.getRemoteSha256(path, hash, detail, size)) return true;
                if (detail.find("Unknown command") != std::string::npos) {
                    std::string serial = dev.connectedSerial();
                    bool preferAdbForward = !dev.isDirectConnection();
                    bool useRoot = dev.useRoot();
                    if (!serial.empty()) {
                        LOG_INFO("SHA256", "Restarting Android server for SHA-256 support");
                        dev.stopServer();
                        if (dev.startServer(serial, preferAdbForward, useRoot)) {
                            return dev.getRemoteSha256(path, hash, detail, size);
                        }
                        detail = "Restart failed: " + dev.lastError();
                    }
                }
                return false;
            }
            return computeCompareLocalSha256(path, hash, detail);
        };

        std::unordered_map<std::string, CompareContentInfo> results;
        int hashMatch = 0;
        int hashMismatch = 0;
        int hashError = 0;

        for (const auto& task : tasks) {
            CompareContentInfo info;
            info.leftSize = task.size;
            info.rightSize = task.size;

            std::string leftDetail;
            std::string rightDetail;
            bool leftOk = hashFor(task.leftAndroid, task.leftSlot, task.leftPath, task.size, info.leftHash, leftDetail);
            bool rightOk = hashFor(task.rightAndroid, task.rightSlot, task.rightPath, task.size, info.rightHash, rightDetail);

            if (!leftOk || !rightOk) {
                info.state = CompareContentState::HashError;
                info.detail = !leftOk ? leftDetail : rightDetail;
                hashError++;
            } else if (info.leftHash == info.rightHash) {
                info.state = CompareContentState::HashMatch;
                info.detail = "Size and hash match";
                hashMatch++;
            } else {
                info.state = CompareContentState::HashMismatch;
                info.detail = "Same size, different hash";
                hashMismatch++;
            }
            results[task.name] = std::move(info);
        }

        {
            std::lock_guard<std::mutex> lk(m_compareMutex);
            if (generation == m_compareContentGeneration && signature == m_compareContentSignature) {
                for (auto& [name, info] : results) m_compareContent[name] = std::move(info);
                m_compareHashMatchCount = hashMatch;
                m_compareHashMismatchCount = hashMismatch;
                m_compareHashErrorCount = hashError;
                m_compareContentBusy = false;
            }
        }
    });
}

void App::renderDeviceBar() {
    ImGui::SetNextWindowSize(ImVec2(470*ui::scale(),580*ui::scale()),ImGuiCond_FirstUseEver);
    if(!ImGui::Begin("Advanced connection controls",&m_showDeviceSettings)) {ImGui::End();return;}
    ImGui::TextWrapped("These controls manage the primary ADB connection. Per-device stream settings are in Device details.");
    ImGui::Spacing();
    if (m_device.getAdbPath().empty()) {
        ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "ADB not found!");
        ImGui::SameLine(); ImGui::TextDisabled("Extract the full ZIP or add Android SDK Platform Tools to PATH");
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(m_theme.gradientPrimary, "DEVICE");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(300);

        // Snapshot under lock to avoid race with poll thread
        std::vector<DeviceInfo> devSnap;
        int selSnap;
        {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            devSnap = m_devices;
            selSnap = m_selectedDevice;
        }

        auto physicalIdentity = [&](const std::string& serial) {
            for (const auto& saved : m_prefs.savedWifiDevices) {
                if (saved.wifiIp.empty()) continue;
                std::string wifiSerial = saved.wifiIp + ":" + std::to_string(saved.port);
                if ((saved.serial == serial || wifiSerial == serial) &&
                    !saved.serial.empty() && saved.serial != wifiSerial)
                    return saved.serial;
            }
            return serial;
        };
        const int adbSnapshotCount = (int)devSnap.size();
        for (int slot = 0; slot < deviceSessionCount(); ++slot) {
            if (!deviceSession(slot).connected || !deviceSession(slot).client.isServerRunning()) continue;
            std::string identity = physicalIdentity(deviceSession(slot).serial);
            bool represented = false;
            for (const auto& device : devSnap) {
                if (physicalIdentity(device.serial) == identity) {
                    represented = true;
                    break;
                }
            }
            if (represented) continue;
            DeviceInfo connected;
            connected.serial = deviceSession(slot).serial;
            connected.state = "device";
            auto nameIt = m_deviceDisplayNames.find(connected.serial);
            connected.model = nameIt != m_deviceDisplayNames.end() && !nameIt->second.empty()
                ? nameIt->second : connected.serial;
            devSnap.push_back(std::move(connected));
        }

        if (devSnap.empty()) {
            ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "No device connected");
        } else {
            std::vector<int> physicalDeviceIndices;
            std::map<std::string, int> physicalIndex;
            for (int i = 0; i < (int)devSnap.size(); ++i) {
                std::string identity = physicalIdentity(devSnap[i].serial);
                auto existing = physicalIndex.find(identity);
                if (existing == physicalIndex.end()) {
                    physicalIndex.emplace(identity, i);
                    physicalDeviceIndices.push_back(i);
                } else if (isWifiSerial(devSnap[existing->second].serial) && !isWifiSerial(devSnap[i].serial)) {
                    int previousIndex = existing->second;
                    existing->second = i;
                    auto indexIt = std::find(physicalDeviceIndices.begin(), physicalDeviceIndices.end(), previousIndex);
                    if (indexIt != physicalDeviceIndices.end()) *indexIt = i;
                }
            }
            // Use marketing name if available, otherwise fall back to adb model
            auto getDevName = [&](int idx) -> std::string {
                auto it = m_deviceDisplayNames.find(devSnap[idx].serial);
                const std::string& name = (it != m_deviceDisplayNames.end()) ? it->second : devSnap[idx].model;
                return name + " (" + devSnap[idx].serial + ")";
            };
            int previewIndex = selSnap;
            if (deviceSession(0).connected && m_device.isServerRunning()) {
                auto current = physicalIndex.find(physicalIdentity(deviceSession(0).serial));
                if (current != physicalIndex.end()) previewIndex = current->second;
            } else if (selSnap >= 0 && selSnap < adbSnapshotCount) {
                auto current = physicalIndex.find(physicalIdentity(devSnap[selSnap].serial));
                if (current != physicalIndex.end()) previewIndex = current->second;
            }
            std::string preview = (previewIndex >= 0 && previewIndex < (int)devSnap.size())
                ? getDevName(previewIndex) : "Select...";
            if (ui::beginCombo("##DeviceCombo", preview.c_str())) {
                for (int i : physicalDeviceIndices) {
                    std::string label = getDevName(i);
                    bool itemIsPrimary = deviceSession(0).connected && m_device.isServerRunning() &&
                        physicalIdentity(deviceSession(0).serial) == physicalIdentity(devSnap[i].serial);
                    if (devSnap[i].state != "device") {
                        label += " [" + devSnap[i].state + "]";
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0.5f,0.5f,1));
                    }
                    if (ImGui::Selectable(label.c_str(), itemIsPrimary)) {
                        std::string selectedSerial = devSnap[i].serial;
                        bool alreadySelected = itemIsPrimary;
                        int existingSlot = m_deviceSessions.find(deviceIdentity(selectedSerial));
                        bool alreadyConnected = existingSlot >= 0 && deviceSession(existingSlot).connected &&
                            deviceForSlot(existingSlot).isServerRunning();
                        if (alreadyConnected && !alreadySelected) {
                            m_statusMessage = "Device is already connected. Select it from an Android pane.";
                            m_statusTime = std::chrono::steady_clock::now();
                        }
                        if (i < adbSnapshotCount && devSnap[i].state == "device" &&
                            !alreadySelected && !alreadyConnected) {
                            postAsync("Connecting to " + selectedSerial + "...", [this, selectedSerial]() {
                                connectDeviceBySerialNow(selectedSerial);
                            });
                        }
                    }
                    if (devSnap[i].state != "device") ImGui::PopStyleColor();
                }
                ImGui::EndCombo();
            }
            if (selSnap >= 0 && selSnap < (int)devSnap.size() && devSnap[selSnap].state == "device") {
                ImGui::SameLine();

                // Show connection mode - detect actual transport
                ImGui::AlignTextToFramePadding();
                if (m_device.isServerRunning()) {
                    bool primaryUsesWifi = m_device.isDirectConnection() || isWifiSerial(devSnap[selSnap].serial);
                    if (m_dualChannelAvailable) {
                        if (m_activeChannelCount >= 4)
                            ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "%d channels", m_activeChannelCount);
                        else if (primaryUsesWifi)
                            ImGui::TextColored(ImVec4(0.3f,0.8f,1,1), "WiFi (%d channels)", m_activeChannelCount);
                        else if (m_secondaryChannelType == "USB")
                            ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "USB (dual)");
                        else
                            ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "USB + %s", m_secondaryChannelType.c_str());
                    } else if (primaryUsesWifi) {
                        ImGui::TextColored(ImVec4(0.3f,0.8f,1,1), "WiFi");
                    } else {
                        ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1), "USB");
                    }
                } else {
                    ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "Connecting...");
                }

                auto drawPipeSelector = [&](const char* label, const char* id, int& value) -> bool {
                    bool changed = false;
                    // Keep this group on the same frame height as the device selector and
                    // checkbox controls. Only its horizontal padding is compact.
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(label);
                    for (int pipes = 1; pipes <= 4; pipes++) {
                        ImGui::SameLine(0, 3);
                        bool selected = (value == pipes);
                        std::string btn = "x" + std::to_string(pipes) + "##" + id + std::to_string(pipes);
                        const float pipeButtonWidth = ImGui::CalcTextSize("x4").x +
                            ImGui::GetStyle().FramePadding.x * 1.56f;
                        bool pressed = modernButton(btn.c_str(), ImVec2(pipeButtonWidth, 0),
                            selected ? ButtonTone::Primary : ButtonTone::Secondary);
                        if (pressed && value != pipes) {
                            value = pipes;
                            changed = true;
                        }
                    }
                    return changed;
                };

                // Pipe controls, directly visible for quick tuning
                auto& devicePipes = m_prefs.pipePreferencesFor(devSnap[selSnap].serial);
                ImGui::Spacing();
                {
                    if (drawPipeSelector("USB", "usbdevbar", devicePipes.usbPipeCount)) {
                        m_prefs.save();
                        m_statusMessage = "USB pipes: x" + std::to_string(devicePipes.usbPipeCount);
                        m_statusTime = std::chrono::steady_clock::now();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Parallel ADB connections over USB. Higher values can improve throughput on fast devices.\nApplies on next device reconnect or transfer channel setup.");
                }
                ImGui::Spacing();
                {
                    if (drawPipeSelector("WiFi", "wifidevbar", devicePipes.wifiPipeCount)) {
                        m_prefs.save();
                        m_statusMessage = "WiFi pipes: x" + std::to_string(devicePipes.wifiPipeCount);
                        m_statusTime = std::chrono::steady_clock::now();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Parallel direct TCP connections over WiFi. Higher values can help WiFi 6/7 devices.\nApplies on next device reconnect or transfer channel setup.");
                }

                ImGui::Spacing();
                {
                    int slot = m_deviceSessions.find(deviceIdentity(devSnap[selSnap].serial));
                    if (slot >= 0) renderDeviceRootButton(slot);
                }

                // WiFi auto-connect toggle
                ImGui::Spacing();
                bool wifiAuto = m_prefs.wifiAutoConnect;
                if (ui::checkbox("WiFi Auto##devbar", &wifiAuto)) {
                    m_prefs.wifiAutoConnect = wifiAuto;
                    m_prefs.save();
                    if (!wifiAuto) {
                        // Disconnect secondary channel and disable dual-channel
                        m_secondaryChannel.disconnectTcp();
                        m_dualChannelAvailable = false;
                        m_secondaryChannelType.clear();
                        m_wifiAutoSetupDone = false;
                        m_statusMessage = "WiFi auto-connect disabled";
                        m_statusTime = std::chrono::steady_clock::now();
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Automatically set up WiFi for dual-channel transfers\nwhen this device connects via USB");

                // Keep awake toggle (uses a wakelock held by a background adb shell process)
                ImGui::Spacing();
                bool keepAwake = m_keepAwake;
                if (ui::checkbox("Keep Awake##devbar", &keepAwake)) {
                    std::string serial = devSnap[selSnap].serial;
                    std::string adbPath = m_device.getAdbPath();
                    if (keepAwake && !m_keepAwake) {
                        // Spawn a background adb shell that holds a wakelock
                        // When the process dies (disconnect, app close), the lock auto-releases
                        std::string cmd = "\"" + adbPath + "\" -s " + serial +
                            " shell \"echo afm_keepawake > /sys/power/wake_lock && cat > /dev/null\"";
                        STARTUPINFOA si{}; si.cb = sizeof(si);
                        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
                        PROCESS_INFORMATION pi{};
                        if (CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE,
                                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                            CloseHandle(pi.hThread);
                            m_wakeLockProcess = pi.hProcess;
                            m_keepAwake = true;
                            m_statusMessage = "Keep awake enabled (wakelock)";
                            m_statusTime = std::chrono::steady_clock::now();
                        } else {
                            // Fallback for non-root: use svc power stayon
                            m_device.runAdbCommand("-s " + serial + " shell svc power stayon usb");
                            cmd = "\"" + adbPath + "\" -s " + serial +
                                " shell \"trap 'svc power stayon false' EXIT; cat > /dev/null\"";
                            si = {}; si.cb = sizeof(si);
                            si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
                            pi = {};
                            if (CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE,
                                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                                CloseHandle(pi.hThread);
                                m_wakeLockProcess = pi.hProcess;
                            }
                            m_keepAwake = true;
                            m_statusMessage = "Keep awake enabled";
                            m_statusTime = std::chrono::steady_clock::now();
                        }
                    } else if (!keepAwake && m_keepAwake) {
                        // Kill the background process, which releases the wakelock
                        if (m_wakeLockProcess) {
                            TerminateProcess(m_wakeLockProcess, 0);
                            CloseHandle(m_wakeLockProcess);
                            m_wakeLockProcess = nullptr;
                        }
                        m_keepAwake = false;
                        // Clean up wakelock entry async to avoid UI hang
                        postAsync("Disabling keep awake...", [this, serial]() {
                            m_device.runAdbCommand("-s " + serial + " shell \"echo afm_keepawake > /sys/power/wake_unlock 2>/dev/null; svc power stayon false 2>/dev/null\"");
                            m_statusMessage = "Keep awake disabled";
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Prevent the device from sleeping while connected.\nUses a wakelock that auto-releases if the connection drops.");

                // (Dual channel button removed — replaced by USB x2 / WiFi x2 toggles above)

                // Disconnect button for WiFi devices
                if (isWifiSerial(devSnap[selSnap].serial)) {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.15f, 0.15f, 1));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.2f, 0.2f, 1));
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x * 0.78f,
                            ImGui::GetStyle().FramePadding.y));
                    bool disconnectPressed = modernButton("Disconnect##wifi");
                    ImGui::PopStyleVar();
                    if (disconnectPressed) {
                        std::string serial = devSnap[selSnap].serial;
                        // Kill wakelock process if active (will auto-release on device)
                        if (m_keepAwake && m_wakeLockProcess) {
                            TerminateProcess(m_wakeLockProcess, 0);
                            CloseHandle(m_wakeLockProcess);
                            m_wakeLockProcess = nullptr;
                            m_keepAwake = false;
                        }
                        postAsync("Disconnecting...", [this, serial]() {
                            m_device.stopServer();
                            m_device.runAdbCommand("disconnect " + serial);
                            m_statusMessage = "Disconnected: " + serial;
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                    ImGui::PopStyleColor(2);
                }
            }
        }

    }
    ImGui::End();
}

void App::renderConnectionActivity() {
    struct ConnectionCard {
        int slot = 0;
        std::string serial;
        std::string name;
        std::string stage;
        bool connected = false;
        bool connecting = false;
        bool retrying = false;
        bool dualTransport = false;
    };

    std::vector<ConnectionCard> cards;
    cards.reserve(deviceSessionCount());
    std::set<std::string> onlineSerials;
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        for (const auto& device : m_devices) {
            if (device.state == "device") onlineSerials.insert(device.serial);
        }
    }
    auto now = std::chrono::steady_clock::now();
    for (int slot = 0; slot < deviceSessionCount(); ++slot) {
        DeviceClient& client = deviceSession(slot).client;
        ConnectionCard card;
        card.slot = slot;
        card.connected = deviceSession(slot).connected && client.isServerRunning();
        card.connecting = client.isConnecting();
        card.serial = client.connectedSerial();
        if (card.serial.empty()) card.serial = deviceSession(slot).serial;
        card.retrying = !card.connected && !card.connecting && now < deviceSession(slot).retryAfter;
        if (card.serial.empty()) continue;

        bool listedOnline = false;
        {
            std::lock_guard<std::mutex> lock(m_deviceMutex);
            for (const auto& device : m_devices) {
                if (device.serial == card.serial && device.state == "device") {
                    listedOnline = true;
                    break;
                }
            }
        }
        if (listedOnline || card.connected || card.connecting) {
            deviceSession(slot).missingSince = {};
        } else {
            if (deviceSession(slot).missingSince.time_since_epoch().count() == 0)
                deviceSession(slot).missingSince = now;
            bool graceExpired = now - deviceSession(slot).missingSince >= std::chrono::seconds(8);
            if (graceExpired && !card.retrying) continue;
        }

        auto nameIt = m_deviceDisplayNames.find(card.serial);
        card.name = nameIt != m_deviceDisplayNames.end() && !nameIt->second.empty()
            ? nameIt->second : card.serial;
        bool primaryIsWifi = client.isDirectConnection() || isWifiSerial(card.serial);
        bool companionTransportOnline = false;
        for (const auto& saved : m_prefs.savedWifiDevices) {
            if (saved.wifiIp.empty()) continue;
            std::string wifiSerial = saved.wifiIp + ":" + std::to_string(saved.port);
            if (saved.serial == card.serial || wifiSerial == card.serial) {
                bool usbOnline = saved.serial != wifiSerial && onlineSerials.count(saved.serial);
                bool wifiAdbOnline = onlineSerials.count(wifiSerial);
                companionTransportOnline = primaryIsWifi ? usbOnline : wifiAdbOnline;
                break;
            }
        }
        bool activeTransferDual = card.slot == 0 && m_dualChannelAvailable &&
            ((primaryIsWifi && m_secondaryChannelType == "USB") ||
             (!primaryIsWifi && m_secondaryChannelType == "WiFi"));
        card.dualTransport = card.connected &&
            (companionTransportOnline || activeTransferDual);
        if (card.connected) {
            card.stage = card.dualTransport ? "Connected over USB + WiFi" :
                (primaryIsWifi ? "Connected over WiFi" : "Connected over USB");
        } else if (card.connecting) {
            card.stage = client.statusText();
            if (card.stage.empty()) card.stage = "Preparing device connection";
        } else if (card.retrying) {
            auto seconds = std::max(1LL, std::chrono::duration_cast<std::chrono::seconds>(
                deviceSession(card.slot).retryAfter - now).count() + 1);
            card.stage = "Retrying in " + std::to_string(seconds) + " seconds";
        } else {
            card.stage = listedOnline ? "Waiting for device" : "Device disconnected";
        }
        cards.push_back(std::move(card));
    }

    // Multiple transports can point to the same physical device. Present those as
    // one device card and combine their connection state instead of double-counting it.
    std::vector<ConnectionCard> uniqueCards;
    uniqueCards.reserve(cards.size());
    for (auto& card : cards) {
        auto existing = std::find_if(uniqueCards.begin(), uniqueCards.end(), [&](const ConnectionCard& item) {
            return item.serial == card.serial;
        });
        if (existing == uniqueCards.end()) {
            uniqueCards.push_back(std::move(card));
            continue;
        }

        bool existingWasConnected = existing->connected;
        bool bothConnected = existingWasConnected && card.connected;
        existing->connected = existing->connected || card.connected;
        existing->connecting = existing->connecting || card.connecting;
        existing->retrying = existing->retrying || card.retrying;
        existing->dualTransport = existing->dualTransport || card.dualTransport;
        if (existing->dualTransport && existing->connected) {
            existing->stage = "Connected over USB + WiFi";
        } else if (bothConnected) {
            existing->stage = "Connected on multiple channels";
        } else if (card.connecting || card.retrying || (!existingWasConnected && card.connected)) {
            existing->stage = card.stage;
        }
    }
    cards = std::move(uniqueCards);
    if (cards.empty()) return;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    int activeCount = 0;
    for (const auto& card : cards) if (card.connecting || card.retrying) activeCount++;

    bool isActive = activeCount > 0;
    if (isActive || (int)cards.size() != m_connectionActivityPreviousCardCount)
        m_connectionActivityVisibleUntil = now + std::chrono::seconds(isActive ? 2 : 4);
    else if (m_connectionActivityWasActive)
        m_connectionActivityVisibleUntil = now + std::chrono::seconds(4);
    m_connectionActivityWasActive = isActive;
    m_connectionActivityPreviousCardCount = (int)cards.size();

    // Full cards use spare space beside the device controls. The compact indicator
    // remains available after they fade out and at widths where cards would overlap.
    constexpr float minActivityPanelViewportWidth = 1700.0f;
    bool canShowFullCards = viewport->WorkSize.x >= minActivityPanelViewportWidth;
    bool manuallyExpanded = now < m_connectionActivityManualUntil;
    float targetAlpha = (canShowFullCards || manuallyExpanded) &&
        (isActive || now < m_connectionActivityVisibleUntil || manuallyExpanded) ? 1.0f : 0.0f;
    float animationStep = 1.0f - expf(-ImGui::GetIO().DeltaTime * 7.0f);
    m_connectionActivityAlpha += (targetAlpha - m_connectionActivityAlpha) * animationStep;
    if (m_connectionActivityAlpha < 0.01f) m_connectionActivityAlpha = 0.0f;

    int dualTransportCount = 0;
    for (const auto& card : cards) if (card.dualTransport) dualTransportCount++;
    std::string compactSummary = activeCount > 0
        ? std::to_string(activeCount) + " active"
        : std::to_string(cards.size()) + (cards.size() == 1 ? " device ready" : " devices ready");
    if (dualTransportCount > 0 && activeCount == 0)
        compactSummary += " | " + std::to_string(dualTransportCount) + " USB + WiFi";
    if (viewport->WorkSize.x < 1050.0f && activeCount == 0 && dualTransportCount == 0)
        compactSummary = std::to_string(cards.size()) + " ready";

    if (!canShowFullCards || m_connectionActivityAlpha < 0.98f) {
        ImDrawList* compactDraw = ImGui::GetForegroundDrawList();
        float compactLineHeight = ImGui::GetTextLineHeight();
        float chipWidth = ImGui::CalcTextSize(compactSummary.c_str()).x + 34.0f;
        ImVec2 chipMax(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f,
            viewport->WorkPos.y + compactLineHeight + 14.0f);
        ImVec2 chipMin(chipMax.x - chipWidth, viewport->WorkPos.y + 4.0f);
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        bool chipHovered = mousePos.x >= chipMin.x && mousePos.x <= chipMax.x &&
            mousePos.y >= chipMin.y && mousePos.y <= chipMax.y;
        ImVec4 chipBackground = m_resolvedLightTheme ? ImVec4(0.92f, 0.95f, 0.99f, 0.98f)
            : ImVec4(0.04f, 0.06f, 0.09f, 0.98f);
        if (chipHovered) {
            ImVec4 hover = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
            chipBackground.x = (chipBackground.x + hover.x) * 0.5f;
            chipBackground.y = (chipBackground.y + hover.y) * 0.5f;
            chipBackground.z = (chipBackground.z + hover.z) * 0.5f;
        }
        compactDraw->AddRectFilled(chipMin, chipMax, ImGui::ColorConvertFloat4ToU32(chipBackground), 7.0f);
        compactDraw->AddRect(chipMin, chipMax, ImGui::GetColorU32(ImGuiCol_Border), 7.0f);
        ImVec4 indicatorColor = activeCount > 0 ? ImVec4(0.30f, 0.68f, 1.0f, 1.0f)
            : ImVec4(0.25f, 0.92f, 0.55f, 1.0f);
        compactDraw->AddCircleFilled(ImVec2(chipMin.x + 13.0f, (chipMin.y + chipMax.y) * 0.5f), 4.0f,
            ImGui::ColorConvertFloat4ToU32(indicatorColor));
        compactDraw->AddText(ImVec2(chipMin.x + 24.0f, chipMin.y + 5.0f),
            ImGui::GetColorU32(ImGuiCol_Text), compactSummary.c_str());

        if (chipHovered && ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left]) {
            m_connectionActivityVisibleUntil = now + std::chrono::seconds(6);
            m_connectionActivityManualUntil = m_connectionActivityVisibleUntil;
        }
        if (chipHovered) {
            ImGui::BeginTooltip();
            ImGui::TextColored(m_theme.gradientPrimary, "Click to show connection cards");
            ImGui::Separator();
            for (const auto& card : cards) {
                ImGui::BulletText("%s", card.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", card.stage.c_str());
            }
            ImGui::EndTooltip();
        }
    }

    if ((!canShowFullCards && !manuallyExpanded) || m_connectionActivityAlpha == 0.0f) return;

    float width = canShowFullCards ? std::min(580.0f, viewport->WorkSize.x * 0.48f)
        : std::min(580.0f, viewport->WorkSize.x - 24.0f);
    float lineHeight = ImGui::GetTextLineHeight();
    float headerTop = 9.0f;
    float cardTopOffset = headerTop + lineHeight + 10.0f;
    float cardHeight = lineHeight * 2.0f + 24.0f;
    float height = cardTopOffset + cardHeight + 9.0f;
    ImVec2 panelMin(viewport->WorkPos.x + viewport->WorkSize.x - width - 16.0f,
        viewport->WorkPos.y + 58.0f - (1.0f - m_connectionActivityAlpha) * 8.0f);
    ImVec2 panelMax(panelMin.x + width, panelMin.y + height);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    auto fadedColor = [&](ImVec4 color) {
        color.w *= m_connectionActivityAlpha;
        return ImGui::ColorConvertFloat4ToU32(color);
    };
    auto fadedStyleColor = [&](ImGuiCol color) {
        return fadedColor(ImGui::GetStyleColorVec4(color));
    };
    ImVec4 panelBackground = m_resolvedLightTheme ? ImVec4(0.975f, 0.984f, 1.0f, 0.97f)
        : ImVec4(0.027f, 0.035f, 0.052f, 0.97f);
    ImVec4 panelBorder = ImGui::GetStyleColorVec4(ImGuiCol_Border);
    draw->AddRectFilled(panelMin, panelMax, fadedColor(panelBackground), 10.0f);
    draw->AddRect(panelMin, panelMax, fadedColor(panelBorder), 10.0f, 0, 1.0f);

    draw->AddText(ImVec2(panelMin.x + 13.0f, panelMin.y + 9.0f),
        fadedColor(m_theme.gradientPrimary),
        "DEVICE CONNECTIONS");
    std::string summary = activeCount > 0 ? std::to_string(activeCount) + " active" : "All ready";
    draw->AddText(ImVec2(panelMax.x - 13.0f - ImGui::CalcTextSize(summary.c_str()).x, panelMin.y + 9.0f),
        fadedStyleColor(ImGuiCol_TextDisabled), summary.c_str());

    float gap = 8.0f;
    float cardWidth = (width - 24.0f - gap * (cards.size() - 1)) / (float)cards.size();
    float cardTop = panelMin.y + cardTopOffset;
    for (int i = 0; i < (int)cards.size(); ++i) {
        const auto& card = cards[i];
        ImVec4 accent = card.connected ? ImVec4(0.25f, 0.92f, 0.55f, 1.0f) :
            (card.connecting ? ImVec4(0.30f, 0.68f, 1.0f, 1.0f) : ImVec4(1.0f, 0.68f, 0.24f, 1.0f));
        ImU32 accentColor = fadedColor(accent);
        ImVec2 cardMin(panelMin.x + 12.0f + i * (cardWidth + gap), cardTop);
        ImVec2 cardMax(cardMin.x + cardWidth, cardMin.y + cardHeight);
        ImVec4 cardBase = m_resolvedLightTheme ? ImVec4(0.94f, 0.96f, 0.99f, 0.98f)
            : ImVec4(0.022f, 0.029f, 0.044f, 0.98f);
        float cardTint = m_resolvedLightTheme ? 0.07f : 0.075f;
        cardBase.x += accent.x * cardTint;
        cardBase.y += accent.y * cardTint;
        cardBase.z += accent.z * cardTint;
        draw->AddRectFilled(cardMin, cardMax, fadedColor(cardBase), 7.0f);
        draw->AddRect(cardMin, cardMax, fadedStyleColor(ImGuiCol_Border), 7.0f);
        ImVec2 indicator(cardMin.x + 14.0f, cardMin.y + 8.0f + lineHeight * 0.5f);
            if (card.connecting) {
                float phase = (float)ImGui::GetTime() * 4.2f;
                for (int dot = 0; dot < 6; ++dot) {
                    float angle = phase + dot * 1.0471976f;
                    float alpha = 0.25f + 0.75f * (float)(dot + 1) / 6.0f;
                    draw->AddCircleFilled(ImVec2(indicator.x + cosf(angle) * 5.0f,
                        indicator.y + sinf(angle) * 5.0f), 1.6f,
                        fadedColor(ImVec4(accent.x, accent.y, accent.z, alpha)));
                }
            } else {
                float pulse = card.connected ? 1.0f : 0.65f + 0.35f * sinf((float)ImGui::GetTime() * 3.0f);
                draw->AddCircleFilled(indicator, 4.5f,
                    fadedColor(ImVec4(accent.x, accent.y, accent.z, pulse)));
            }

        std::string title = card.name;
        if (title.size() > 27) title = title.substr(0, 24) + "...";
        std::string stage = card.stage;
        if (stage.size() > 36) stage = stage.substr(0, 33) + "...";
        float titleY = cardMin.y + 7.0f;
        float stageY = titleY + lineHeight + 5.0f;
        draw->AddText(ImVec2(cardMin.x + 28.0f, titleY), fadedStyleColor(ImGuiCol_Text), title.c_str());
        draw->AddText(ImVec2(cardMin.x + 28.0f, stageY), fadedStyleColor(ImGuiCol_TextDisabled), stage.c_str());

            float barLeft = cardMin.x + 8.0f;
            float barRight = cardMax.x - 8.0f;
            float barY = cardMax.y - 5.0f;
            draw->AddRectFilled(ImVec2(barLeft, barY), ImVec2(barRight, barY + 2.0f),
                fadedStyleColor(ImGuiCol_FrameBg), 1.0f);
            if (card.connecting) {
                float travel = barRight - barLeft;
                float head = fmodf((float)ImGui::GetTime() * 120.0f, travel + 55.0f) - 55.0f;
                float start = std::max(barLeft, barLeft + head);
                float end = std::min(barRight, barLeft + head + 55.0f);
                if (end > start) draw->AddRectFilled(ImVec2(start, barY), ImVec2(end, barY + 2.0f),
                    fadedColor(accent), 1.0f);
            } else if (card.connected) {
                draw->AddRectFilled(ImVec2(barLeft, barY), ImVec2(barRight, barY + 2.0f),
                    accentColor, 1.0f);
            }
    }
}

void App::renderPanel(FilePanel& panel, PanelSide side) {
    if (panel.isApps) {
        renderAppsPanel(panel, side);
        return;
    }

    renderPaneHeader(panel, side);
    if (panel.isAndroid && deviceSession(panel.deviceSlot).rootRequestPending) {
        ImGui::BeginDisabled();
        renderPaneNavigation(panel);
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextWrapped("Changing root access. If your phone asks for permission, approve the ADB root request.");
        return;
    }
    if (!panel.pendingDeviceSerial.empty()) {
        ImGui::BeginDisabled();
        renderPaneNavigation(panel);
        ImGui::EndDisabled();
        if (ImGui::BeginTable("##OpeningDeviceFiles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 66 * ui::scale());
            ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 128 * ui::scale());
            ImGui::TableHeadersRow();
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextWrapped("%s", panel.deviceOpenError.empty() ? "Loading files..." : panel.deviceOpenError.c_str());
        if (!panel.deviceOpenError.empty() && ui::button("Try again", ui::Icon::Refresh, ui::Appearance::Secondary)) {
            panel.deviceOpenError.clear();
            connectFromManager(panel.pendingDeviceSerial, false);
        }
        return;
    }
    const char* label = panel.isAndroid ? "Android" : "Windows";
    ImVec4 labelColor = panel.isAndroid ? ImVec4(0.3f,0.85f,0.4f,1) : ImVec4(0.40f,0.65f,1,1);

    // The connection manager is a persistent pane mode. It is also used as the
    // setup fallback when an Android pane has no connected device.
    if (panel.isConnections || (panel.isAndroid &&
        !deviceFor(panel).isServerRunning())) {
        renderConnectionManager(panel);
        return;
    }

    // Keep the navigation controls stable. Only the directory listing transitions.
    float directoryContentAlpha = 1.0f;
    {
        auto& navTime = (side == PanelSide::Left) ? m_lastNavTimeLeft : m_lastNavTimeRight;
        if (panel.navigationTransitionReady) {
            navTime = std::chrono::steady_clock::now();
            panel.navigationTransitionReady = false;
        }
        float elapsed = (float)std::chrono::duration<double>(std::chrono::steady_clock::now() - navTime).count();
        float progress = std::clamp(elapsed / 0.20f, 0.0f, 1.0f);
        float eased = progress * progress * (3.0f - 2.0f * progress);
        directoryContentAlpha = 0.55f + 0.45f * eased;
    }

    renderPaneNavigation(panel);

    // Keep search, refresh and folder shortcuts on consistent baselines.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(32*ui::scale(),7*ui::scale()));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,9*ui::scale());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-32*ui::scale());
    const ImVec2 filterPosition=ImGui::GetCursorScreenPos();
    ImGui::InputTextWithHint(("##Filter" + std::string(label)).c_str(),
        "Filter this folder, e.g. *.mp4", panel.searchFilter, sizeof(panel.searchFilter));
    ui::icon(ui::Icon::Search,ImVec2(filterPosition.x+10*ui::scale(),filterPosition.y+(ImGui::GetItemRectSize().y-15*ui::scale())*0.5f),
        15*ui::scale(),ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ImGui::PopStyleVar(2);
    ImGui::SameLine(0,4*ui::scale());
    if(ui::button("##RefreshFolder",ui::Icon::Refresh,ui::Appearance::Quiet,ImVec2(28*ui::scale(),28*ui::scale()),"Refresh folder"))panel.needsRefresh=true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::BeginChild("##FolderShortcuts",ImVec2(0,ui::controlHeight(26)),ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
    renderFavoritesBar(panel, side);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // Ctrl+A — select all
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_A) && ImGui::GetIO().KeyCtrl &&
        !ImGui::IsAnyItemActive()) {
        for (int i = 0; i < panel.entryCount(); i++) panel.selectedIndices.insert(i);
    }

    ImGuiTableFlags tf = ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * directoryContentAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10*ui::scale(),(m_prefs.compactRows?3.0f:6.0f)*ui::scale()));
    ImVec2 tableSize(0,std::max(40.0f,ImGui::GetContentRegionAvail().y-std::max(28*ui::scale(),ImGui::GetFontSize()+12*ui::scale())));
    if (ImGui::BeginTable(("##FilesV2" + std::string(label)).c_str(), 3, tf, tableSize)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 1.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 66*ui::textScale());
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 128*ui::textScale());
        ImGui::TableSetColumnEnabled(2,ImGui::GetWindowWidth()>420*ui::textScale());
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Handle sort specs
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty && specs->SpecsCount > 0) {
                int col = specs->Specs[0].ColumnIndex;
                bool desc = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                ++panel.entriesRevision;
                panel.sortColumn = col;
                panel.sortDescending = desc;
                // Re-sort entries
                if (panel.isAndroid) {
                    std::sort(panel.androidEntries.begin(), panel.androidEntries.end(),
                        [col, desc](const DeviceFileEntry& a, const DeviceFileEntry& b) {
                            if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
                            int cmp;
                            if (col == 1) cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
                            else if (col == 2) cmp = (a.mtime < b.mtime) ? -1 : (a.mtime > b.mtime) ? 1 : 0;
                            else cmp = _stricmp(a.name.c_str(), b.name.c_str());
                            return desc ? cmp > 0 : cmp < 0;
                        });
                } else {
                    std::sort(panel.windowsEntries.begin(), panel.windowsEntries.end(),
                        [col, desc](const WindowsFileEntry& a, const WindowsFileEntry& b) {
                            if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
                            int cmp;
                            if (col == 1) cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
                            else if (col == 2) cmp = a.dateModified.compare(b.dateModified);
                            else cmp = _stricmp(a.name.c_str(), b.name.c_str());
                            return desc ? cmp > 0 : cmp < 0;
                        });
                }
                specs->SpecsDirty = false;
            }
        }

        // ".." - hide at drive root (e.g. "C:\") or filesystem root ("/")
        bool atRoot = false;
        if (!panel.isAndroid) {
            // Windows: at root if path is "X:\" (3 chars)
            atRoot = (panel.currentPath.size() <= 3 && panel.currentPath.size() >= 2 && panel.currentPath[1] == ':');
        } else {
            atRoot = (panel.currentPath == "/");
        }
        if (!atRoot) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (ImGui::Selectable("   ..##up", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                if (ImGui::IsMouseDoubleClicked(0)) navigateUp(panel);
            ImGui::TableNextColumn(); ImGui::TableNextColumn();
        }

        std::string filter(panel.searchFilter);

        // Track row screen positions for rubber-band selection
        struct RowInfo { int index; float yMin, yMax; };
        std::vector<RowInfo> visibleRows;
        bool rowHovered = false;

        std::unique_lock<std::mutex> compareContentLock;
        if (m_compareEnabled && m_compareContentEnabled) {
            compareContentLock = std::unique_lock<std::mutex>(m_compareMutex);
        }

        int count = panel.entryCount();
        if (panel.filterRevision != panel.entriesRevision || panel.cachedFilter != filter ||
            panel.cachedShowHidden != panel.showHidden || panel.cachedEntryCount != count) {
            panel.filteredIndices.clear();
            for (int i = 0; i < count; ++i) {
                const std::string& name = panel.isAndroid ? panel.androidEntries[i].name : panel.windowsEntries[i].name;
                if (!fileNameMatchesFilter(name, filter)) continue;
                if (!panel.showHidden && !panel.isAndroid &&
                    ((!name.empty() && name[0] == '.') || panel.windowsEntries[i].isHidden)) continue;
                panel.filteredIndices.push_back(i);
            }
            panel.filterRevision = panel.entriesRevision;
            panel.cachedFilter = filter;
            panel.cachedShowHidden = panel.showHidden;
            panel.cachedEntryCount = count;
        }
        ImGuiListClipper clipper;
        clipper.Begin((int)panel.filteredIndices.size());
        while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            int i = panel.filteredIndices[row];
            if (!panel.validIndex(i)) continue;
            std::string name = panel.entryName(i);

            bool isDir = panel.entryIsDir(i);
            bool isSel = panel.selectedIndices.count(i) > 0;
            CompareContentInfo contentInfo;
            if (compareContentLock.owns_lock()) {
                auto contentIt = m_compareContent.find(normalizeCompareName(name));
                if (contentIt != m_compareContent.end()) contentInfo = contentIt->second;
            }
            CompareContentState contentState = contentInfo.state;

            ImGui::TableNextRow();
            bool compareHighlight = isCompareHighlighted(panel, side, i);
            if (contentState == CompareContentState::SizeMismatch ||
                contentState == CompareContentState::HashMismatch) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(110, 34, 34, 155));
            } else if (contentState == CompareContentState::HashError) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(90, 52, 115, 150));
            } else if (contentState == CompareContentState::HashMatch) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(30, 82, 48, 120));
            } else if (contentState == CompareContentState::HashPending) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(70, 70, 78, 100));
            } else if (compareHighlight) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(122, 92, 18, 150));
            }
            ImGui::TableNextColumn();

            // Track row screen position for rubber-band
            float rowYMin = ImGui::GetCursorScreenPos().y;

            std::string icon = getFileIcon(name, isDir);
            ui::Icon glyph = isDir ? ui::Icon::Folder : ui::Icon::File;
            if(icon=="[I]")glyph=ui::Icon::Image;
            if(icon=="[V]"||icon=="[M]")glyph=ui::Icon::Video;
            if(icon=="[A]")glyph=ui::Icon::Music;
            if(icon=="[K]")glyph=ui::Icon::Apps;
            if(icon=="[Z]")glyph=ui::Icon::Archive;
            ImVec2 iconPosition = ImGui::GetCursorScreenPos();
            iconPosition.x += 6*ui::scale();
            iconPosition.y += (ImGui::GetTextLineHeight()-16*ui::scale())*0.5f;
            std::string compareTag;
            if (contentState == CompareContentState::SizeMismatch) compareTag = " [size diff]";
            else if (contentState == CompareContentState::HashPending) compareTag = " [hashing]";
            else if (contentState == CompareContentState::HashMatch) compareTag = " [hash ok]";
            else if (contentState == CompareContentState::HashMismatch) compareTag = " [hash diff]";
            else if (contentState == CompareContentState::HashError) compareTag = " [hash error]";
            std::string sid = "##File" + std::to_string(i);
            float nameWidth = ImGui::GetContentRegionAvail().x - 6*ui::scale();
            ImVec2 textPosition = ImGui::GetCursorScreenPos();
            textPosition.x += 30*ui::scale();

            bool pushedTextColor = true;
            if (isDir) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
            else if (contentState == CompareContentState::SizeMismatch ||
                     contentState == CompareContentState::HashMismatch) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f,0.55f,0.50f,1));
            else if (contentState == CompareContentState::HashError) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f,0.65f,1.0f,1));
            else if (contentState == CompareContentState::HashMatch) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f,1.0f,0.68f,1));
            else if (contentState == CompareContentState::HashPending) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f,0.78f,0.84f,1));
            else if (compareHighlight) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f,0.86f,0.42f,1));
            else pushedTextColor = false;
            if (isSel) {
                ImVec4 selection = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                selection.w = m_resolvedLightTheme ? 0.24f : 0.17f;
                ImGui::PushStyleColor(ImGuiCol_Header, selection);
            }
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));

            ImGuiSelectableFlags sf = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
            const bool rowClicked = ImGui::Selectable(sid.c_str(), isSel, sf);
            if (isSel) {
                const ImVec2 barMin = ImGui::GetItemRectMin(), barMax = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(barMin.x + 1*ui::scale(), barMin.y + 3*ui::scale()),
                    ImVec2(barMin.x + 4*ui::scale(), barMax.y - 3*ui::scale()), ImGui::GetColorU32(ImGuiCol_CheckMark), 2*ui::scale());
            }
            if (rowClicked) {
                ImGuiIO& io = ImGui::GetIO();
                if (io.KeyCtrl) { if (isSel) panel.selectedIndices.erase(i); else panel.selectedIndices.insert(i); }
                else if (io.KeyShift && panel.focusedIndex >= 0) {
                    int lo = std::min(panel.focusedIndex, i), hi = std::max(panel.focusedIndex, i);
                    for (int j = lo; j <= hi; j++) panel.selectedIndices.insert(j);
                } else { panel.selectedIndices.clear(); panel.selectedIndices.insert(i); }
                panel.focusedIndex = i;
                m_lastFocusedPanel = &panel;

                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (isDir) {
                        std::string sep = panel.isAndroid ? "/" : "\\";
                        std::string np = panel.currentPath;
                        appendPathSeparator(np, sep[0]);
                        np += name;
                        navigateToDirectory(panel, np);
                    } else if (endsWithCI(name, ".mcraw") && !panel.isAndroid) {
                        // Windows side: mount MCRAW via ProjFS and navigate to mount point
                        std::string mcrawFullPath = panel.currentPath;
                        appendPathSeparator(mcrawFullPath, '\\');
                        mcrawFullPath += name;
                        requestMcrawMount(panel, mcrawFullPath);
                    } else if (endsWithCI(name, ".mcraw") && panel.isAndroid) {
                        // Android side: navigate into MCRAW as virtual directory (protocol-based)
                        std::string np = panel.currentPath;
                        appendPathSeparator(np, '/');
                        np += name;
                        navigateToDirectory(panel, np);
                    } else if (panel.insideMcraw && panel.isAndroid) {
                        // Double-clicked virtual item inside Android MCRAW — pull to temp and open
                        std::string vname = name;
                        std::string mcraw = panel.mcrawFilePath;
                        const int slot = panel.deviceSlot;
                        postAsync("Extracting " + vname + "...", [this, mcraw, vname, slot]() {
                            char tmpDir[MAX_PATH];
                            GetTempPathA(MAX_PATH, tmpDir);
                            std::string tmpPath = std::string(tmpDir) + vname;
                            uint64_t outSize = 0;
                            if (deviceForSlot(slot).pullMcrawItem(mcraw, vname, tmpPath, outSize)) {
                                ShellExecuteA(nullptr, "open", tmpPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                            } else {
                                m_statusMessage = "Failed to extract " + vname;
                                m_statusTime = std::chrono::steady_clock::now();
                            }
                        });
                    } else if (!panel.isAndroid) {
                        // Open file on Windows side
                        std::string fp = panel.currentPath;
                        appendPathSeparator(fp, '\\');
                        fp += name;
                        if (endsWithCI(name, ".apk"))
                            requestApkInstall(fp);
                        else {
                            const std::wstring widePath = toWide(fp);
                            ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        }
                    } else {
                        // Double-clicked a file on Android side — pull to temp and open
                        openAndroidFile(panel, i);
                    }
                }
            }

            if (ImGui::IsItemHovered()) {
                rowHovered = true;
                ImVec2 rowMin = ImGui::GetItemRectMin();
                ImVec2 rowMax = ImGui::GetItemRectMax();
                if (panel.hoveredEntryIndex != i) {
                    panel.previousHoveredRowMin = panel.hoveredRowMin;
                    panel.previousHoveredRowMax = panel.hoveredRowMax;
                    panel.hoveredEntryIndex = i;
                    panel.hoverTransitionTime = std::chrono::steady_clock::now();
                }
                panel.hoveredRowMin = rowMin;
                panel.hoveredRowMax = rowMax;
            }
            ImGui::PopStyleColor(); // header hovered

            if (contentState != CompareContentState::Unknown && ImGui::IsItemHovered()) {
                if (contentState == CompareContentState::SizeMismatch) {
                    ImGui::SetTooltip("Size differs\nLeft: %s\nRight: %s",
                        formatSize(contentInfo.leftSize).c_str(), formatSize(contentInfo.rightSize).c_str());
                } else if (contentState == CompareContentState::HashPending) {
                    ImGui::SetTooltip("Hash check is queued or running");
                } else if (contentState == CompareContentState::HashMatch) {
                    ImGui::SetTooltip("Size and SHA-256 hash match\nSHA-256: %s", contentInfo.leftHash.c_str());
                } else if (contentState == CompareContentState::HashMismatch) {
                    ImGui::SetTooltip("Same size, different SHA-256 hash\nLeft: %s\nRight: %s",
                        contentInfo.leftHash.c_str(), contentInfo.rightHash.c_str());
                } else if (contentState == CompareContentState::HashError) {
                    ImGui::SetTooltip("Hash check failed\n%s", contentInfo.detail.c_str());
                }
            }

            // --- Drag source — auto-select item if not already selected ---
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                if (panel.selectedIndices.count(i) == 0) {
                    panel.selectedIndices.clear();
                    panel.selectedIndices.insert(i);
                    panel.focusedIndex = i;
                }
                DragPayload dp{&panel};
                ImGui::SetDragDropPayload("FILE_DRAG", &dp, sizeof(dp));
                int selCount = (int)panel.selectedIndices.size();
                if (selCount == 1) {
                    ImGui::Text("Copy: %s", name.c_str());
                } else {
                    ImGui::Text("Copy %d items", selCount);
                }
                m_dragSourcePanel = &panel;
                m_isDragging = true;
                ImGui::EndDragDropSource();
            }

            ImVec4 glyphTint = m_resolvedLightTheme ? ImVec4(0.32f,0.45f,0.66f,1) : ImVec4(0.56f,0.71f,0.91f,1);
            if (isDir) glyphTint = m_resolvedLightTheme ? ImVec4(0.72f,0.50f,0.05f,1) : ImVec4(0.96f,0.77f,0.32f,1);
            else if (glyph == ui::Icon::Image) glyphTint = m_resolvedLightTheme ? ImVec4(0.05f,0.55f,0.40f,1) : ImVec4(0.43f,0.91f,0.72f,1);
            else if (glyph == ui::Icon::Video) glyphTint = m_resolvedLightTheme ? ImVec4(0.75f,0.20f,0.45f,1) : ImVec4(0.98f,0.56f,0.77f,1);
            else if (glyph == ui::Icon::Music) glyphTint = m_resolvedLightTheme ? ImVec4(0.45f,0.30f,0.80f,1) : ImVec4(0.77f,0.71f,0.99f,1);
            else if (glyph == ui::Icon::Apps) glyphTint = m_resolvedLightTheme ? ImVec4(0.10f,0.55f,0.25f,1) : ImVec4(0.53f,0.94f,0.60f,1);
            else if (glyph == ui::Icon::Archive) glyphTint = m_resolvedLightTheme ? ImVec4(0.70f,0.40f,0.10f,1) : ImVec4(0.99f,0.73f,0.45f,1);
            ImU32 glyphColor = ImGui::GetColorU32(glyphTint);
            ui::icon(glyph,iconPosition,16*ui::scale(),glyphColor);
            ui::drawTextClipped(textPosition,name+compareTag,nameWidth-24*ui::scale(),ImGui::GetColorU32(ImGuiCol_Text));
            if (isSel) ImGui::PopStyleColor(); // header
            if (pushedTextColor) ImGui::PopStyleColor(); // text

            // Right-click context - preserve multi-selection if clicking an already-selected item
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                if (panel.selectedIndices.count(i) == 0) {
                    // Clicked outside selection - select only this item
                    panel.selectedIndices.clear();
                    panel.selectedIndices.insert(i);
                }
                panel.focusedIndex = i; m_contextPanel = &panel; m_contextIndex = i;
                ImGui::OpenPopup("##FileContextMenu");
            }

            ImGui::TableNextColumn();
            if (!isDir) ImGui::TextDisabled("%s", formatSize(panel.entrySize(i)).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", panel.entryDate(i).c_str());

            // Record row extent for rubber-band
            float rowYMax = ImGui::GetCursorScreenPos().y;
            visibleRows.push_back({i, rowYMin, rowYMax});
        }

        if (!rowHovered && panel.hoveredEntryIndex != -1) {
            panel.previousHoveredRowMin = panel.hoveredRowMin;
            panel.previousHoveredRowMax = panel.hoveredRowMax;
            panel.hoveredEntryIndex = -1;
            panel.hoverTransitionTime = std::chrono::steady_clock::now();
        }

        renderContextMenu(panel);

        // Right-click on empty space in the table area — background context menu
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered() &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            m_contextPanel = &panel;
            m_contextIndex = -1;
            panel.selectedIndices.clear();
            m_lastFocusedPanel = &panel;
            ImGui::OpenPopup("##BgContextMenu");
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10*ui::scale(), 10*ui::scale()));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10*ui::scale(), 9*ui::scale()));
        if (ImGui::BeginPopup("##BgContextMenu")) {
            if (m_contextPanel == &panel) {
                if (ImGui::MenuItem("New Folder...")) {
                    memset(m_newFolderName, 0, sizeof(m_newFolderName));
                    m_contextPanel = &panel; m_showNewFolderPopup = true;
                }
                if (!m_clipboardPaths.empty()) {
                    const char* pasteLabel = m_clipboardCut ? "Move Here" : "Paste";
                    if (ImGui::MenuItem(pasteLabel, "Ctrl+V")) {
                        performClipboardPaste(panel);
                    }
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);

        ImGui::EndTable();

        const float hoverElapsed = (float)std::chrono::duration<double>(
            std::chrono::steady_clock::now() - panel.hoverTransitionTime).count();
        const float hoverProgress = std::clamp(hoverElapsed / 0.12f, 0.0f, 1.0f);
        const float hoverEase = hoverProgress * hoverProgress * (3.0f - 2.0f * hoverProgress);
        ImDrawList* hoverDraw = ImGui::GetWindowDrawList();
        if (panel.hoveredEntryIndex != -1) {
            ImVec4 hoverTint = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            hoverTint.w = 0.08f * hoverEase;
            hoverDraw->AddRectFilled(panel.hoveredRowMin, panel.hoveredRowMax, ImGui::GetColorU32(hoverTint), 6*ui::scale());
        }
        if (hoverProgress < 1.0f && panel.previousHoveredRowMax.y > panel.previousHoveredRowMin.y) {
            ImVec4 fadeTint = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            fadeTint.w = 0.08f * (1.0f - hoverEase);
            hoverDraw->AddRectFilled(panel.previousHoveredRowMin, panel.previousHoveredRowMax, ImGui::GetColorU32(fadeTint), 6*ui::scale());
        }

        // --- Rubber-band (lasso) selection ---
        ImVec2 tableMin = ImGui::GetItemRectMin();
        ImVec2 tableMax = ImGui::GetItemRectMax();
        ImVec2 mousePos = ImGui::GetMousePos();

        // Start rubber-band on mouse down in the table area (not on a Selectable)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl &&
            mousePos.x >= tableMin.x && mousePos.x <= tableMax.x &&
            mousePos.y >= tableMin.y && mousePos.y <= tableMax.y &&
            !ImGui::IsAnyItemHovered() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            panel.rubberBandActive = true;
            panel.rubberBandStart = mousePos;
            panel.rubberBandEnd = mousePos;
            if (!ImGui::GetIO().KeyShift)
                panel.selectedIndices.clear();
            m_lastFocusedPanel = &panel;
        }

        if (panel.rubberBandActive) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                panel.rubberBandEnd = mousePos;

                // Calculate selection rect (normalized)
                float selMinY = std::min(panel.rubberBandStart.y, panel.rubberBandEnd.y);
                float selMaxY = std::max(panel.rubberBandStart.y, panel.rubberBandEnd.y);
                float selMinX = std::min(panel.rubberBandStart.x, panel.rubberBandEnd.x);
                float selMaxX = std::max(panel.rubberBandStart.x, panel.rubberBandEnd.x);

                // Select rows that overlap the rubber-band rect
                panel.selectedIndices.clear();
                for (auto& row : visibleRows) {
                    if (row.yMax > selMinY && row.yMin < selMaxY) {
                        panel.selectedIndices.insert(row.index);
                    }
                }

                // Draw the rubber-band rectangle
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(
                    ImVec2(selMinX, selMinY), ImVec2(selMaxX, selMaxY),
                    IM_COL32(60, 120, 220, 50));
                dl->AddRect(
                    ImVec2(selMinX, selMinY), ImVec2(selMaxX, selMaxY),
                    IM_COL32(80, 140, 240, 180));
            } else {
                panel.rubberBandActive = false;
            }
        }
    }

    ImGui::PopStyleVar(2);
    ImGui::Separator();
    ImGui::TextDisabled("%d items%s",panel.entryCount(),panel.selectedIndices.empty()?"":("  |  "+std::to_string(panel.selectedIndices.size())+" selected").c_str());
}

void App::drawProgressBar(float fraction, const char* overlayText, float height,
                           ImVec4 barColor, ImVec4 bgColor) {
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (m_resolvedLightTheme) bgColor = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    ImVec2 size(ImGui::GetContentRegionAvail().x, height);
    ImGui::PushID(overlayText ? overlayText : "");
    ImGui::PushID((int)std::round(height * 10.0f));
    ImGui::InvisibleButton("##ModernProgress", size);
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImGui::PopID();
    ImGui::PopID();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float rounding = std::min(ImGui::GetStyle().FrameRounding, height * 0.5f);
    draw->AddRectFilled(ImVec2(mn.x, mn.y + 2.0f), ImVec2(mx.x, mx.y + 2.0f),
        IM_COL32(0, 0, 0, m_resolvedLightTheme ? 22 : 70), rounding);
    draw->AddRectFilled(mn, mx, ImGui::ColorConvertFloat4ToU32(bgColor), rounding);
    if (fraction > 0.0f) {
        ImVec2 fillMax(mn.x + (mx.x - mn.x) * fraction, mx.y);
        draw->AddRectFilled(mn, fillMax, ImGui::ColorConvertFloat4ToU32(barColor), rounding);
        ImVec4 highlight(std::min(1.0f, barColor.x + 0.18f), std::min(1.0f, barColor.y + 0.18f),
            std::min(1.0f, barColor.z + 0.18f), 0.72f);
        draw->AddLine(ImVec2(mn.x + rounding, mn.y + 1.0f),
            ImVec2(std::max(mn.x + rounding, fillMax.x - rounding), mn.y + 1.0f),
            ImGui::ColorConvertFloat4ToU32(highlight), 1.0f);
    }
    draw->AddRect(mn, mx, ImGui::GetColorU32(ImGuiCol_Border), rounding);
    const char* progressText = overlayText ? overlayText : "";
    ImVec2 ts = ImGui::CalcTextSize(progressText);
    ImVec2 tp(mn.x + (mx.x - mn.x - ts.x) * 0.5f, mn.y + (mx.y - mn.y - ts.y) * 0.5f);
    draw->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, m_resolvedLightTheme ? 60 : 180), progressText);
    draw->AddText(tp, ImGui::GetColorU32(ImGuiCol_Text), progressText);
}

void App::updateTransferPresentation() {
    const auto now = std::chrono::steady_clock::now();
    const auto inspected = m_overlayVisible ? m_transferDetailsBatch.lock() : nullptr;
    std::lock_guard<std::mutex> lock(m_batchMutex);
    for (auto it = m_batchQueue.begin(); it != m_batchQueue.end();) {
        const auto& batch = *it;
        const auto state = batch->state.load();
        const bool needsInteraction = batch->waitingConflict || batch->disconnected || batch->waitingForUserRetry;
        const bool hasErrors = state == BatchState::Completed &&
            (batch->errorSkippedFiles > 0 || batch->crcFailCount() > 0);
        if (m_transferResultRetention.shouldRemove(batch->logId, state, now,
                batch == inspected, needsInteraction, hasErrors)) {
            it = m_batchQueue.erase(it);
        } else {
            ++it;
        }
    }
}

void App::renderTransferAttention() {
    constexpr const char* popupName = "##TransferDecision";
    std::shared_ptr<TransferBatch> batch;
    TransferBatch::ConflictRequest request;
    {
        std::lock_guard<std::mutex> lock(m_batchMutex);
        for (const auto& candidate : m_batchQueue) {
            if (candidate->pendingConflict(request)) {
                batch = candidate;
                break;
            }
        }
    }
    if (batch && (m_transferAttentionBatchId != batch->logId || m_transferAttentionRequest.id != request.id)) {
        m_transferAttentionBatchId = batch->logId;
        m_transferAttentionRequest = request;
        m_transferAttentionApplyAll = false;
        m_transferAttentionFocus = true;
        if (g_mainHwnd && GetForegroundWindow() != g_mainHwnd) {
            FLASHWINFO flash{sizeof(FLASHWINFO), g_mainHwnd, FLASHW_TRAY, 2, 0};
            FlashWindowEx(&flash);
        }
    }
    if (batch && !ImGui::IsPopupOpen(popupName) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        ImGui::OpenPopup(popupName);
        m_transferAttentionFocus = true;
    }
    if (!ImGui::IsPopupOpen(popupName)) return;

    const float s = ui::scale();
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const float preferredWidth = std::max(620.0f * s, ImGui::GetFontSize() * 27.0f + 48 * s);
    ImGui::SetNextWindowSize(ImVec2(std::min(preferredWidth, viewport->WorkSize.x * 0.92f), 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(viewport->WorkSize.x * 0.92f, viewport->WorkSize.y * 0.90f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s, 22 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12 * s, 12 * s));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.01f, 0.02f, 0.04f, 0.62f));
    if (ImGui::BeginPopupModal(popupName, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!batch) {
            ImGui::CloseCurrentPopup();
        } else {
            const bool light = m_resolvedLightTheme;
            const ImVec4 attention = light ? ImVec4(0.59f, 0.33f, 0.04f, 1) : ImVec4(1.0f, 0.75f, 0.30f, 1);
            const ImVec2 top = ImGui::GetCursorScreenPos();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(top, ImVec2(top.x + 42 * s, top.y + 42 * s),
                ImGui::GetColorU32(ImVec4(attention.x, attention.y, attention.z, 0.14f)), 10 * s);
            ui::icon(ui::Icon::Copy, ImVec2(top.x + 10 * s, top.y + 10 * s), 22 * s, ImGui::GetColorU32(attention));
            ImGui::Dummy(ImVec2(42 * s, 42 * s));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImFont* headingFont = ui::connectionHeadingFont ? ui::connectionHeadingFont : ui::semiboldFont;
            if (headingFont) ImGui::PushFont(headingFont);
            ImGui::TextColored(attention, "Replace this file?");
            if (headingFont) ImGui::PopFont();
            ImGui::TextUnformatted("Your transfer needs a decision.");
            ImGui::EndGroup();
            ImGui::Separator();

            ImGui::PushTextWrapPos(0);
            if (ui::semiboldFont) ImGui::PushFont(ui::semiboldFont);
            ImGui::TextUnformatted(request.fileName.c_str());
            if (ui::semiboldFont) ImGui::PopFont();
            ImGui::TextUnformatted("A file with this name already exists at the destination.");
            ImGui::PopTextWrapPos();

            const std::string source = batch->isLocalCopy || (!batch->isPull && !batch->isCrossDevice)
                ? "This PC" : deviceDisplayName(batch->srcDeviceSlot);
            const std::string destination = batch->isLocalCopy || (batch->isPull && !batch->isCrossDevice)
                ? "This PC" : deviceDisplayName(batch->dstDeviceSlot);
            const float pathWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - 28 * s);
            const float pathHeight = ImGui::CalcTextSize(request.sourcePath.c_str(), nullptr, false, pathWidth).y +
                ImGui::CalcTextSize(request.destPath.c_str(), nullptr, false, pathWidth).y +
                ImGui::GetTextLineHeight() * 2 + 64 * s;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 12 * s));
            ImGui::BeginChild("##ConflictPaths", ImVec2(0, std::min(pathHeight, 190 * s)), ImGuiChildFlags_Borders);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("FROM  %s", source.c_str());
            ImGui::TextUnformatted(request.sourcePath.c_str());
            ImGui::TextDisabled("ALREADY IN  %s", destination.c_str());
            ImGui::TextUnformatted(request.destPath.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            ImGui::PopStyleVar();

            ImGui::TextWrapped("Replace overwrites the destination file. Skip keeps the existing file.");
            ui::checkbox("Apply to remaining conflicts", &m_transferAttentionApplyAll);
            ImGui::Spacing();
            const float buttonWidth = (ImGui::GetContentRegionAvail().x - 12 * s) * 0.5f;
            ConflictAction action = ConflictAction::None;
            if (m_transferAttentionFocus) ImGui::SetKeyboardFocusHere();
            if (ui::button("Skip file", ui::Icon::Forward, ui::Appearance::Secondary, ImVec2(buttonWidth, ui::controlHeight(42))))
                action = m_transferAttentionApplyAll ? ConflictAction::SkipAll : ConflictAction::Skip;
            ImGui::SetItemDefaultFocus();
            m_transferAttentionFocus = false;
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.97f, 0.69f, 0.24f, 1));
            if (ui::button("Replace file", ui::Icon::Copy, ui::Appearance::Primary, ImVec2(buttonWidth, ui::controlHeight(42))))
                action = m_transferAttentionApplyAll ? ConflictAction::OverwriteAll : ConflictAction::Overwrite;
            ImGui::PopStyleColor();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) action = ConflictAction::Skip;

            bool submitted = false;
            if (action != ConflictAction::None) {
                std::lock_guard<std::mutex> lock(m_batchMutex);
                submitted = batch->respondToConflict(request.id, action);
            }
            if (submitted) {
                const char* decision = action == ConflictAction::Overwrite ? "Replace" :
                    action == ConflictAction::OverwriteAll ? "Replace remaining conflicts" :
                    action == ConflictAction::SkipAll ? "Skip remaining conflicts" : "Skip";
                batch->logEvent(LogLevel::Info, std::string("Conflict decision: ") + decision + " for " + request.destPath);
                m_batchCV.notify_all();
                ImGui::CloseCurrentPopup();
            }
            if (ui::button("Cancel transfer", ui::Icon::Close, ui::Appearance::Quiet)) {
                {
                    std::lock_guard<std::mutex> lock(m_batchMutex);
                    batch->requestStop();
                }
                batch->logEvent(LogLevel::Info, "Cancelled while waiting for a file conflict decision");
                m_batchCV.notify_all();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

void App::renderTransferOverlay() {
    std::shared_ptr<TransferBatch> batch;
    int queueSize = 0;
    {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        queueSize = (int)m_batchQueue.size();
        // Find active or most recent batch
        for (auto& b : m_batchQueue) {
            BatchState s = b->state.load();
            if (s == BatchState::Running || s == BatchState::Paused || s == BatchState::Verifying || s == BatchState::WaitingConflict) { batch = b; break; }
        }
        // If none active, show the last completed/stopped one
        if (!batch && !m_batchQueue.empty()) {
            batch = m_batchQueue.back();
        }
    }

    if (!batch) {
        m_overlayVisible = false;
        return;
    }

    if (auto selected = m_transferDetailsBatch.lock(); selected && m_overlayVisible) batch = selected;
    else if (m_overlayVisible) m_transferDetailsBatch = batch;
    BatchState st = batch->state.load();

    // Smooth fade for overlay
    if (m_overlayVisible)
        m_overlayFade = std::min(m_overlayFade + ImGui::GetIO().DeltaTime * 6.0f, 1.0f);
    else
        m_overlayFade = std::max(m_overlayFade - ImGui::GetIO().DeltaTime * 8.0f, 0.0f);

    if (m_overlayFade <= 0.01f && !m_overlayVisible) {
        // Show a small indicator in status bar instead (handled in renderStatusBar)
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_overlayFade);

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    // Auto-size: fixed width, height expands to fit content
    ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    static float trackedWidth = 520.0f;
    // Track the max width this window has needed (only grows, never shrinks during a batch)
    // Use content width to ensure text/bars always fit
    float minW = std::max(trackedWidth, (batch->useMultiNic || batch->useParallelChannels) ? 650.0f : 520.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(minW, 0), ImVec2(vpSize.x * 0.9f, vpSize.y * 0.85f));

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    bool open = m_overlayVisible;
    ImGui::Begin("Transfer Progress", &open, wf);
    m_overlayVisible = open; // user closed via X
    if (batch->state == BatchState::Preparing) {
        ImGui::Text("Scanning source folders: %llu items found", (unsigned long long)batch->discoveredItems.load());
        if (modernButton("Cancel", ImVec2(90, 0))) batch->requestStop();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }


    float totalProg   = batch->totalProgress.load();
    float curFileProg = batch->curFileProgress.load();
    uint64_t totalBytes = batch->totalBytes.load();
    uint64_t totalTx    = batch->totalTransferred.load();
    uint64_t curFSize   = batch->curFileSize.load();
    uint64_t curFTx     = batch->curFileTransferred.load();
    double speed  = batch->speedBytesPerSec.load();
    double eta    = batch->etaSeconds.load();
    int curIdx    = batch->currentFileIndex.load();
    int totalFiles = batch->totalFiles();
    if (totalBytes > 0 && totalTx > totalBytes) totalTx = totalBytes;
    totalProg = std::clamp(totalProg, 0.0f, 1.0f);
    curFileProg = std::clamp(curFileProg, 0.0f, 1.0f);

    uint64_t totalDisplayTx = totalTx;
    float totalDisplayProg = totalBytes > 0
        ? (float)std::clamp((double)totalDisplayTx / (double)totalBytes, 0.0, 1.0)
        : totalProg;
    auto formatSizeFixed = [](uint64_t bytes, int decimals) {
        const char* u[] = {"B", "KB", "MB", "GB", "TB"};
        int i = 0;
        double s = (double)bytes;
        while (s >= 1024.0 && i < 4) { s /= 1024.0; i++; }

        char buf[48];
        if (i == 0) snprintf(buf, sizeof(buf), "%llu B", bytes);
        else snprintf(buf, sizeof(buf), "%.*f %s", decimals, s, u[i]);
        return std::string(buf);
    };
    auto formatProgressPair = [&](uint64_t done, uint64_t total) {
        if (total > 0 && done > total) done = total;
        std::string doneText = formatSize(done);
        std::string totalText = formatSize(total);
        if (total > 0 && done < total && doneText == totalText) {
            doneText = formatSizeFixed(done, 2);
            totalText = formatSizeFixed(total, 2);
            if (doneText == totalText) {
                doneText = formatSizeFixed(done, 3);
                totalText = formatSizeFixed(total, 3);
            }
            if (doneText == totalText) {
                doneText = std::to_string(done) + " B";
                totalText = std::to_string(total) + " B";
            }
        }
        return std::pair<std::string, std::string>(doneText, totalText);
    };
    std::string verb = batch->isMove ? "Move" : "Copy";
    std::string direction;
    if (batch->isCrossDevice)
        direction = "Copy: Android -> Android (direct relay)";
    else if (batch->isLocalCopy)
        direction = verb + ": Windows -> Windows";
    else if (batch->isPull)
        direction = verb + ": Android -> Windows";
    else
        direction = verb + ": Windows -> Android";
    int transferDeviceSlot = -1;
    if (!batch->isLocalCopy && !batch->isCrossDevice)
        transferDeviceSlot = batch->isPull ? batch->srcDeviceSlot : batch->dstDeviceSlot;
    if (batch->useMultiNic)
        direction += " [" + std::to_string(batch->numChannels) + " NICs]";
    else if (batch->useParallelChannels)
        direction += " [" + std::to_string(batch->numChannels) + " channels]";
    else if (transferDeviceSlot >= 0)
        direction += " [single channel]";

    // Header
    ImGui::TextColored(ImVec4(0.5f,0.7f,1,1), "%s", direction.c_str());
    ImGui::SameLine();
    if (queueSize > 1) ImGui::TextDisabled("(%d batches queued)", queueSize);
    if (transferDeviceSlot > 0 && !batch->useParallelChannels && !batch->useMultiNic) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1), "Secondary device, single channel");
    }

    ImGui::Separator();

    // --- Current file / per-channel progress ---
    if (st == BatchState::Running || st == BatchState::Paused || st == BatchState::WaitingConflict) {
        if (batch->useParallelChannels || batch->useMultiNic) {
            // Multi-channel: show separate progress for each channel
            ImVec4 chColors[] = {
                {0.25f, 0.55f, 0.95f, 0.9f}, // blue
                {0.30f, 0.75f, 0.40f, 0.9f}, // green
                {0.85f, 0.55f, 0.25f, 0.9f}, // orange
                {0.75f, 0.35f, 0.85f, 0.9f}, // purple
                {0.85f, 0.75f, 0.25f, 0.9f}, // yellow
                {0.35f, 0.80f, 0.80f, 0.9f}, // cyan
                {0.85f, 0.35f, 0.45f, 0.9f}, // red
                {0.55f, 0.75f, 0.55f, 0.9f}, // light green
            };

            int nCh = batch->numChannels;
            for (int ch = 0; ch < nCh; ch++) {
                auto& cp = batch->channels[ch];
                if (cp.channelName.empty() && cp.filesCompleted.load() == 0 && cp.bytesTransferred.load() == 0) continue;

                double spd = cp.speed.load();
                uint64_t tx = cp.bytesTransferred.load();

                // Show current file being transferred by this channel
                std::string curFile = cp.currentFile;
                bool isActive = (spd > 0.01 || !curFile.empty());

                // Hide channels that are permanently dead
                if (!isActive && tx == 0) continue;

                // Channel name + speed on one line
                ImGui::TextColored(chColors[ch % 8], "%s", cp.channelName.c_str());
                ImGui::SameLine();
                if (spd > 0.01)
                    ImGui::Text("%s  %s", formatSize(tx).c_str(), formatSpeed(spd).c_str());
                else if (tx > 0)
                    ImGui::TextDisabled("%s  (idle)", formatSize(tx).c_str());
                else
                    ImGui::TextDisabled("(connecting...)");

                // Current file for this channel
                if (!curFile.empty() && curFile[0] != '(') {
                    ImGui::SameLine();
                    ImGui::TextDisabled("  %s", curFile.c_str());
                }
            }
        } else {
            // Single-channel: show current file
            std::string curName = (curIdx < totalFiles) ? batch->files[curIdx].displayName : "";
            ImGui::Text("File %d / %d:", curIdx + 1, totalFiles);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.8f,0.9f,1,1), "%s", curName.c_str());

            char curOvl[256];
            snprintf(curOvl, sizeof(curOvl), "%s / %s  (%.1f%%)",
                     formatSize(curFTx).c_str(), formatSize(curFSize).c_str(), curFileProg * 100.0f);
            drawProgressBar(curFileProg, curOvl, 20.0f,
                            ImVec4(0.25f,0.55f,0.95f,0.9f), ImVec4(0.04f,0.04f,0.06f,1));

            ImGui::Spacing();
        }
    }

    // --- Total progress ---
    ImGui::Text("Total:");
    ImGui::SameLine();
    ImGui::TextDisabled("%d files, %s", totalFiles, formatSize(totalBytes).c_str());

    char totalOvl[256];
    if (st == BatchState::Completed) {
        double avgSpd = batch->finalAvgSpeed.load();
        double totalSec = batch->finalTimeSec.load();
        auto sizePair = formatProgressPair(totalTx, totalBytes);
        snprintf(totalOvl, sizeof(totalOvl), "%s / %s  -  %s avg  -  Done in %s",
                 sizePair.first.c_str(), sizePair.second.c_str(),
                 formatSpeed(avgSpd).c_str(), formatETA(totalSec).c_str());
        drawProgressBar(1.0f, totalOvl, 22.0f,
                        ImVec4(0.2f,0.7f,0.3f,0.9f), ImVec4(0.04f,0.04f,0.06f,1));

        // Per-channel summary for parallel transfers
        if ((batch->useParallelChannels || batch->useMultiNic) && batch->numChannels > 1) {
            ImGui::Spacing();
            if (batch->isCrossDevice)
                ImGui::TextDisabled("  Effective speed is shown in the green total bar. Rows below show per-channel relay throughput.");
            int nCh = batch->numChannels;
            for (int ch = 0; ch < nCh; ch++) {
                auto& cp = batch->channels[ch];
                if (cp.bytesTransferred.load() == 0 && cp.filesCompleted.load() == 0) continue;
                double sec = cp.elapsedSec.load();
                if (sec < 0.01) sec = 0.01;
                double spd = (double)cp.bytesTransferred.load() / sec;
                ImGui::TextDisabled("  %s: %d blocks, %s in %s (%s avg)",
                    cp.channelName.c_str(), cp.filesCompleted.load(),
                    formatSize(cp.bytesTransferred.load()).c_str(),
                    formatETA(sec).c_str(), formatSpeed(spd).c_str());
            }
        }
    } else if (st == BatchState::Failed || st == BatchState::Stopped) {
        auto sizePair = formatProgressPair(totalTx, totalBytes);
        snprintf(totalOvl, sizeof(totalOvl), "%s / %s  -  %s",
                 sizePair.first.c_str(), sizePair.second.c_str(),
                 st == BatchState::Stopped ? "Stopped" : "Failed");
        drawProgressBar(totalDisplayProg, totalOvl, 22.0f,
                        ImVec4(0.7f,0.2f,0.2f,0.9f), ImVec4(0.04f,0.04f,0.06f,1));
    } else if (st == BatchState::Verifying) {
        // Transfer done, CRC verification in progress — two phases
        int phase = batch->crcPhase.load();
        float crcProg = batch->crcProgress.load();

        if (phase == 1) {
            // Phase 1: server computing — animated pulse since we have no server-side progress
            float t = (float)fmod(ImGui::GetTime() * 0.8, 1.0);
            snprintf(totalOvl, sizeof(totalOvl), "Verifying: %s  -  Device computing CRC...",
                     batch->crcFileName.c_str());
            drawProgressBar(t, totalOvl, 22.0f,
                            ImVec4(0.5f,0.35f,0.8f,0.7f), ImVec4(0.04f,0.04f,0.06f,1));
        } else if (phase == 2) {
            // Phase 2: local CRC — real progress
            snprintf(totalOvl, sizeof(totalOvl), "Verifying: %s  -  Local CRC %.0f%%",
                     batch->crcFileName.c_str(), crcProg * 100.0f);
            drawProgressBar(crcProg, totalOvl, 22.0f,
                            ImVec4(0.5f,0.4f,0.9f,0.9f), ImVec4(0.04f,0.04f,0.06f,1));
        } else {
            snprintf(totalOvl, sizeof(totalOvl), "Verifying: %s",
                     batch->crcFileName.c_str());
            drawProgressBar(0.0f, totalOvl, 22.0f,
                            ImVec4(0.5f,0.4f,0.9f,0.9f), ImVec4(0.04f,0.04f,0.06f,1));
        }
    } else {
        std::string etaStr = (eta > 0) ? ("  ETA: " + formatETA(eta)) : "";
        std::string speedStr = formatSpeed(speed);
        auto sizePair = formatProgressPair(totalDisplayTx, totalBytes);
        snprintf(totalOvl, sizeof(totalOvl), "%s / %s  (%.1f%%)  -  %s%s",
                 sizePair.first.c_str(), sizePair.second.c_str(),
                 totalDisplayProg * 100.0f, speedStr.c_str(), etaStr.c_str());
        float barFrac = totalDisplayProg;
        ImVec4 barCol;
        if (batch->disconnected.load()) barCol = ImVec4(0.8f,0.4f,0.1f,0.9f); // orange when disconnected
        else if (st == BatchState::Paused || st == BatchState::WaitingConflict) barCol = ImVec4(0.7f,0.6f,0.1f,0.9f);
        else barCol = ImVec4(0.25f,0.55f,0.95f,0.9f);
        drawProgressBar(barFrac, totalOvl, 22.0f, barCol, ImVec4(0.04f,0.04f,0.06f,1));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Control buttons ---
    bool isRunning = (st == BatchState::Running);
    bool isPaused  = (st == BatchState::Paused);
    bool isDone    = (st == BatchState::Completed || st == BatchState::Failed || st == BatchState::Stopped);
    bool waitingRetry = batch->waitingForUserRetry.load();
    bool waitingConflict = batch->waitingConflict.load();

    if (waitingConflict) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Waiting for your file replacement decision.");
    } else if (waitingRetry) {
        // Waiting for user decision after retries exhausted
        ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Reconnection failed. Retry or cancel?");
        ImGui::Spacing();

        if (modernButton("Retry", ImVec2(120, 0), ButtonTone::Primary)) {
            batch->userRetryRequested = true; batch->logEvent(LogLevel::Info,"Retry requested");
            m_batchCV.notify_all();
        }

        ImGui::SameLine();

        if (modernButton("Cancel Transfer", ImVec2(140, 0), ButtonTone::Danger)) {
            batch->requestStop(); batch->logEvent(LogLevel::Info,"Cancel requested");
            batch->userRetryRequested = false;
            // Stop is handled by progress callback checking stopRequested every chunk
            m_batchCV.notify_all();
        }

    } else if (batch->disconnected.load() && !isDone) {
        // Device disconnected during transfer - show status and manual retry
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Device disconnected!");
        ImGui::Spacing();
        if (!batch->errorMessage.empty())
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", batch->errorMessage.c_str());
        ImGui::Spacing();

        if (modernButton("Force Retry Now", ImVec2(140, 0), ButtonTone::Primary)) {
            {
                std::lock_guard<std::mutex> lk(m_batchMutex);
                ++batch->retryGeneration;
            }
            m_batchCV.notify_all();
            batch->errorMessage = "User requested retry...";
        }
        ImGui::SameLine();

        if (modernButton("Cancel Transfer", ImVec2(140, 0), ButtonTone::Danger)) {
            batch->requestStop(); batch->logEvent(LogLevel::Info,"Cancel requested");
            batch->userRetryRequested = false;
            // Stop is handled by progress callback checking stopRequested every chunk
            m_batchCV.notify_all();
        }

    } else if (!isDone) {
        if (isPaused) {
            if (modernButton("Resume", ImVec2(90, 0), ButtonTone::Primary)) {
                batch->pauseRequested = false; batch->logEvent(LogLevel::Info,"Resume requested");
                m_batchCV.notify_all();
            }
        } else {
            if (modernButton("Pause", ImVec2(90, 0), ButtonTone::Secondary)) {
                batch->pauseRequested = true; batch->logEvent(LogLevel::Info,"Pause requested");
            }
        }

        ImGui::SameLine();
        if (!m_confirmStopTransfer) {
            if (modernButton("Stop", ImVec2(90, 0), ButtonTone::Danger)) {
                m_confirmStopTransfer = true;
            }
        } else {
            ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "Stop transfer?");
            ImGui::SameLine();
            if (modernButton("Yes", ImVec2(60, 0), ButtonTone::Danger)) {
                batch->requestStop(); batch->logEvent(LogLevel::Info,"Cancel requested");
                batch->pauseRequested = false; batch->logEvent(LogLevel::Info,"Resume requested");
                // Stop is handled by progress callback checking stopRequested every chunk
                m_batchCV.notify_all();
                m_confirmStopTransfer = false;
            }
            ImGui::SameLine();
            if (modernButton("No", ImVec2(60, 0))) {
                m_confirmStopTransfer = false;
            }
        }

        ImGui::SameLine();
        if (modernButton("Hide", ImVec2(90, 0))) {
            m_overlayVisible = false;
        }
    } else {
        if (modernButton("Close", ImVec2(90, 0))) {
            m_overlayVisible = false;
            m_overlayWasOpen = false;
            m_transferDetailsBatch.reset();
        }

        if (st == BatchState::Failed && !batch->errorMessage.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "%s", batch->errorMessage.c_str());
        }

        // CRC results summary
        if (!batch->crcResults.empty()) {
            int pass = batch->crcPassCount();
            int fail = batch->crcFailCount();
            ImGui::Spacing();

            // Compute total CRC time across all files
            double totalCrcMs = 0, totalRemoteMs = 0, totalLocalMs = 0;
            for (auto& r : batch->crcResults) {
                totalCrcMs += r.totalMs;
                totalRemoteMs += r.remoteMs;
                totalLocalMs += r.localMs;
            }

            if (fail == 0) {
                ImGui::TextColored(ImVec4(0.3f,1,0.5f,1), "CRC verified: %d/%d files OK", pass, (int)batch->crcResults.size());
            } else {
                ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "CRC: %d passed, %d failed", pass, fail);
            }
            // Total CRC timing summary
            if (totalCrcMs > 0) {
                ImGui::TextDisabled("  CRC time: %.1fs total (device: %.1fs, local: %.1fs)",
                                    totalCrcMs / 1000.0, totalRemoteMs / 1000.0, totalLocalMs / 1000.0);
            }

            // Show details for failed CRCs (always), and passed (in a collapsible)
            if (fail > 0) {
                // Retry button at the top so it's immediately visible
                if (isDone && modernButton("Retry Failed Files")) {
                    auto retryBatch = std::make_shared<TransferBatch>();
                    retryBatch->isPull = batch->isPull;
                    retryBatch->isLocalCopy = batch->isLocalCopy;
                    retryBatch->srcDeviceSlot = batch->srcDeviceSlot;
                    retryBatch->dstDeviceSlot = batch->dstDeviceSlot;
                    retryBatch->useParallelChannels = batch->useParallelChannels;
                    retryBatch->useMultiNic = batch->useMultiNic;
                    retryBatch->numChannels = batch->numChannels;
                    for (auto& r : batch->crcResults) {
                        if (r.passed || r.sourcePath.empty()) continue;
                        BatchFileItem item;
                        item.sourcePath = r.sourcePath;
                        item.destPath = r.destPath;
                        item.displayName = r.fileName;
                        item.fileSize = r.fileSize;
                        retryBatch->files.push_back(std::move(item));
                    }
                    if (!retryBatch->files.empty()) {
                        // Auto-overwrite for retries (the whole point is to re-copy)
                        retryBatch->conflictAllDecision = ConflictAction::OverwriteAll;
                        // Calculate totalBytes
                        uint64_t retryTotal = 0;
                        for (auto& f : retryBatch->files)
                            retryTotal += f.fileSize;
                        retryBatch->totalBytes = retryTotal;
                        {
                            std::lock_guard<std::mutex> lk(m_batchMutex);
                            retryBatch->logQueued();
                            m_batchQueue.push_back(retryBatch);
                        }
                        m_batchCV.notify_one();
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(re-copies failed files)");
                for (auto& r : batch->crcResults) {
                    if (r.passed) continue;
                    ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "  FAIL: %s", r.fileName.c_str());
                    ImGui::TextColored(ImVec4(0.7f,0.5f,0.5f,1), "        %s", r.detail.c_str());
                }
            }
            if (pass > 0 && ImGui::TreeNode("Passed files")) {
                for (auto& r : batch->crcResults) {
                    if (!r.passed) continue;
                    // Show timing per file
                    double throughputMBs = (r.fileSize > 0 && r.totalMs > 0)
                        ? (r.fileSize / (1024.0 * 1024.0)) / (r.totalMs / 1000.0) : 0;
                    ImGui::TextColored(ImVec4(0.5f,0.8f,0.5f,1), "  OK: %s", r.fileName.c_str());
                    ImGui::TextDisabled("      %s", r.detail.c_str());
                    if (r.totalMs > 0) {
                        ImGui::TextDisabled("      Device: %.0fms  Local: %.0fms  Total: %.0fms  (%.1f MB/s)",
                                            r.remoteMs, r.localMs, r.totalMs, throughputMBs);
                    }
                }
                ImGui::TreePop();
            }
        }
    }

    // Paused indicator
    if (isPaused) {
        ImGui::SameLine(0, 20);
        ImGui::TextColored(ImVec4(1,0.8f,0.2f,1), "PAUSED");
    }

    // Skipped files indicator (shown when completed)
    if (isDone && batch->skippedFiles > 0) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1), "%d file(s) skipped (already existed)",
                           batch->skippedFiles);
    }

    // Error-skipped files summary
    if (isDone && batch->errorSkippedFiles.load() > 0) {
        ImGui::Spacing();
        int errSkipped = batch->errorSkippedFiles.load();
        ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%d file(s) skipped due to errors", errSkipped);
        {
            std::lock_guard<std::mutex> lk(batch->errorSkippedMutex);
            if (!batch->errorSkippedNames.empty() && ImGui::TreeNode("Skipped files")) {
                for (auto& name : batch->errorSkippedNames)
                    ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "  %s", name.c_str());
                ImGui::TreePop();
            }
        }
    }

    // Track max width so the window only grows, never shrinks mid-transfer
    // Track max of window size and content width (whichever is larger)
    float curW = std::max(ImGui::GetWindowSize().x, ImGui::GetContentRegionAvail().x + ImGui::GetStyle().WindowPadding.x * 2);
    if (curW > trackedWidth) trackedWidth = curW;
    if (isDone) trackedWidth = 520.0f;

    ImGui::End();
    ImGui::PopStyleVar(); // alpha fade
}


void App::startAppUninstall(FilePanel& panel, bool useRoot) {
    struct AppToRemove {
        std::string packageName;
        std::string displayName;
    };

    std::vector<AppToRemove> apps;
    for (int index : panel.selectedIndices) {
        if (!panel.validIndex(index)) continue;
        const auto& entry = panel.appEntries[index];
        apps.push_back({ entry.packageName, entry.appName.empty() ? entry.packageName : entry.appName });
    }
    if (apps.empty()) return;

    std::string prompt = useRoot ? "Use root to uninstall " : "Uninstall ";
    prompt += std::to_string(apps.size()) + " selected app";
    if (apps.size() != 1) prompt += "s";
    prompt += " for the current Android user?";
    if (MessageBoxA(g_mainHwnd, prompt.c_str(), useRoot ? "Confirm Root Uninstall" : "Confirm Uninstall",
                    MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    int slot = panel.deviceSlot;
    std::string serial = deviceSession(slot).serial;
    m_appUninstallActive = true;
    m_appUninstallUseRoot = useRoot;
    m_appUninstallCurrent = 0;
    m_appUninstallTotal = (int)apps.size();
    m_appUninstallSucceeded = 0;
    m_appUninstallFailed = 0;
    m_appUninstallPackage = "Preparing uninstall";
    m_appUninstallPanel = &panel;

    postAsync(useRoot ? "Root uninstalling apps..." : "Uninstalling apps...",
        [this, apps = std::move(apps), slot, serial, refreshPanel = &panel, useRoot]() {
            int succeeded = 0;
            int failed = 0;
            std::string details;

            if (useRoot && !deviceForSlot(slot).isRootAvailable(serial)) {
                UiMessage message;
                message.hasAppUninstallProgress = true;
                message.appUninstallActive = false;
                message.appUninstallUseRoot = true;
                message.appUninstallTotal = (int)apps.size();
                message.appUninstallFailed = (int)apps.size();
                message.hasNotification = true;
                message.notificationTitle = "Root Access Unavailable";
                message.notificationMessage =
                    "Root uninstall requires a rooted device with root permission granted for this device.";
                message.notificationIsError = true;
                postUiMessage(std::move(message));
                return;
            }

            for (int index = 0; index < (int)apps.size(); ++index) {
                UiMessage progress;
                progress.hasAppUninstallProgress = true;
                progress.appUninstallActive = true;
                progress.appUninstallUseRoot = useRoot;
                progress.appUninstallCurrent = index + 1;
                progress.appUninstallTotal = (int)apps.size();
                progress.appUninstallSucceeded = succeeded;
                progress.appUninstallFailed = failed;
                progress.appUninstallPackage = apps[index].displayName;
                postUiMessage(std::move(progress));

                std::string output;
                if (deviceForSlot(slot).uninstallPackage(
                        serial, apps[index].packageName, output, false, true, useRoot)) {
                    ++succeeded;
                    LOG_INFO("Apps","Uninstalled: "+apps[index].packageName+" from "+serial);
                } else {
                    ++failed;
                    LOG_ERROR("Apps","Uninstall failed: "+apps[index].packageName+" on "+serial+": "+output);
                    details += apps[index].displayName + " (" + apps[index].packageName + "): " + output + "\n";
                }

                UiMessage completedItem;
                completedItem.hasAppUninstallProgress = true;
                completedItem.appUninstallActive = true;
                completedItem.appUninstallUseRoot = useRoot;
                completedItem.appUninstallCurrent = index + 1;
                completedItem.appUninstallTotal = (int)apps.size();
                completedItem.appUninstallSucceeded = succeeded;
                completedItem.appUninstallFailed = failed;
                completedItem.appUninstallPackage = apps[index].displayName;
                postUiMessage(std::move(completedItem));
            }

            UiMessage result;
            result.hasAppUninstallProgress = true;
            result.appUninstallActive = false;
            result.appUninstallUseRoot = useRoot;
            result.appUninstallCurrent = (int)apps.size();
            result.appUninstallTotal = (int)apps.size();
            result.appUninstallSucceeded = succeeded;
            result.appUninstallFailed = failed;
            result.refreshFilePanel = refreshPanel;
            result.hasStatus = true;
            result.status = "Removed " + std::to_string(succeeded) + " of " + std::to_string(apps.size()) + " app(s)";
            result.hasNotification = true;
            result.notificationTitle = failed == 0 ? "Uninstall Complete" : "Uninstall Finished With Errors";
            if (failed == 0) {
                result.notificationMessage = "Removed " + std::to_string(succeeded) + " app(s) for the current Android user.";
            } else {
                result.notificationMessage = "Removed " + std::to_string(succeeded) + " of " +
                    std::to_string(apps.size()) + " app(s).\n\n" + details;
                result.notificationIsError = true;
            }
            postUiMessage(std::move(result));
        });
}

void App::renderAppsPanel(FilePanel& panel, PanelSide side) {
    renderPaneHeader(panel,side);
    if (!deviceFor(panel).isServerRunning()) {
        ImGui::TextWrapped("Connect an Android device to view installed apps.");
        if(ui::button("Connect a device",ui::Icon::Phone,ui::Appearance::Secondary)) {
            m_showConnectionsWorkspace=true;m_showAppsWorkspace=false;m_connectionsWorkspacePanel.isConnections=true;
        }
        return;
    }
    std::string sideId = side == PanelSide::Left ? "L" : "R";
    const float s=ui::scale();
    if(ui::button("Install APK",ui::Icon::Plus,ui::Appearance::Primary)) {
        wchar_t path[32768] = {};
        OPENFILENAMEW dialog = {};
        dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=g_mainHwnd;
        dialog.lpstrFilter=L"Android packages (*.apk)\0*.apk\0All files\0*.*\0";
        dialog.lpstrFile=path;dialog.nMaxFile=(DWORD)std::size(path);
        dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(GetOpenFileNameW(&dialog))requestApkInstall(pathToUtf8(std::filesystem::path(path)));
    }
    ImGui::SameLine(0,8*s);
    bool hasSelection=!panel.selectedIndices.empty();
    ImGui::BeginDisabled(!hasSelection||m_asyncBusy.load());
    if(ui::button("Uninstall",ui::Icon::Trash,ui::Appearance::Secondary))startAppUninstall(panel,false);
    ImGui::EndDisabled();
    ImGui::SameLine(0,4*s);
    if(ui::button("##AppActions",ui::Icon::More,ui::Appearance::Quiet,ImVec2(32*s,32*s),"App actions"))ImGui::OpenPopup("##AppActionsMenu");
    if(ImGui::BeginPopup("##AppActionsMenu")) {
        if(ImGui::MenuItem("Uninstall with root",nullptr,false,hasSelection&&!m_asyncBusy))startAppUninstall(panel,true);
        ui::tooltip("Uses root for the package manager command.");
        ImGui::EndPopup();
    }
    float refreshWidth=ImGui::CalcTextSize("Refresh").x+45*s;
    ImGui::SameLine(ImGui::GetWindowWidth()-ImGui::GetStyle().WindowPadding.x-refreshWidth);
    if(ui::button("Refresh##Apps",ui::Icon::Refresh,ui::Appearance::Secondary))panel.needsRefresh=true;
    auto uninstallSelectedAppsFromMenu=[this,&panel](bool useRoot){startAppUninstall(panel,useRoot);};
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10*s,9*s));
    ImGui::SetNextItemWidth(140*s);
    const char* appTypeLabels[]={"All apps","User apps","System apps"};
    ui::combo("##AppTypeFilter",&panel.appTypeFilter,appTypeLabels,3);
    ImGui::SameLine(0,8*s);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(34*s,9*s));
    ImGui::SetNextItemWidth(-1);
    const ImVec2 appSearchPos=ImGui::GetCursorScreenPos();
    ImGui::InputTextWithHint("##AppsSearch","Search by app name or package",panel.searchFilter,sizeof(panel.searchFilter));
    ui::icon(ui::Icon::Search,ImVec2(appSearchPos.x+11*s,appSearchPos.y+(ImGui::GetItemRectSize().y-15*s)*0.5f),15*s,ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ImGui::PopStyleVar();
    if (m_appUninstallActive && m_appUninstallPanel == &panel) {
        ImVec2 available = ImGui::GetContentRegionAvail();
        ImGui::BeginChild(("##AppUninstallProgress" + sideId).c_str(), available, 0,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImVec2 panelSize = ImGui::GetWindowSize();
        float cardWidth = std::min(480.0f, std::max(250.0f, panelSize.x - 24.0f));
        float cardHeight = 176.0f;
        ImGui::SetCursorPos(ImVec2(std::max(12.0f, (panelSize.x - cardWidth) * 0.5f),
                                   std::max(12.0f, (panelSize.y - cardHeight) * 0.38f)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.065f, 0.105f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.16f, 0.35f, 0.58f, 0.90f));
        ImGui::BeginChild(("##AppUninstallCard" + sideId).c_str(), ImVec2(cardWidth, cardHeight),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImVec2 cardPos = ImGui::GetWindowPos();
        ImVec2 spinnerCenter(cardPos.x + 34.0f, cardPos.y + 50.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float phase = (float)ImGui::GetTime() * 8.0f;
        for (int dotIndex = 0; dotIndex < 12; ++dotIndex) {
            float angle = ((float)dotIndex / 12.0f) * 6.2831853f - 1.5707963f;
            float intensity = 0.20f + 0.80f * std::max(0.0f, std::cos(angle - phase));
            ImVec2 dot(spinnerCenter.x + std::cos(angle) * 12.0f,
                       spinnerCenter.y + std::sin(angle) * 12.0f);
            drawList->AddCircleFilled(dot, 2.4f, IM_COL32(74, 174, 255, (int)(255.0f * intensity)));
        }

        ImGui::SetCursorPos(ImVec2(64.0f, 18.0f));
        ImGui::TextColored(ImVec4(0.36f, 0.72f, 1.0f, 1.0f), "%s",
                           m_appUninstallUseRoot ? "Root uninstall in progress" : "Uninstall in progress");
        ImGui::SetCursorPos(ImVec2(64.0f, 45.0f));
        if (m_appUninstallCurrent > 0) {
            ImGui::Text("App %d of %d", m_appUninstallCurrent, m_appUninstallTotal);
        } else {
            ImGui::TextDisabled("Preparing selected apps");
        }

        ImGui::SetCursorPos(ImVec2(16.0f, 78.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(cardWidth - 16.0f);
        ImGui::TextWrapped("%s", m_appUninstallPackage.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        float progress = m_appUninstallTotal > 0
            ? (float)(m_appUninstallSucceeded + m_appUninstallFailed) / (float)m_appUninstallTotal
            : 0.0f;
        ImGui::SetCursorPos(ImVec2(16.0f, 114.0f));
        ImGui::ProgressBar(progress, ImVec2(cardWidth - 32.0f, 10.0f), "");
        ImGui::SetCursorPos(ImVec2(16.0f, 137.0f));
        ImGui::TextDisabled("%d removed", m_appUninstallSucceeded);
        if (m_appUninstallFailed > 0) {
            ImGui::SameLine(0, 14.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.38f, 1.0f), "%d failed", m_appUninstallFailed);
        }
        ImGui::SameLine(0, 14.0f);
        ImGui::TextDisabled("Keep the device connected");

        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::EndChild();
        return;
    }

    std::string filter(panel.searchFilter);
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
    auto appVisible = [&](const InstalledAppEntry& app) {
        if (panel.appTypeFilter == 1 && app.isSystem) return false;
        if (panel.appTypeFilter == 2 && !app.isSystem) return false;
        if (filter.empty()) return true;
        std::string hay = app.appName + " " + app.packageName + " " + app.apkPath;
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        return hay.find(filter) != std::string::npos;
    };

    std::vector<int> visibleAppIndices;
    visibleAppIndices.reserve(panel.appEntries.size());
    for (int i = 0; i < (int)panel.appEntries.size(); ++i)
        if (appVisible(panel.appEntries[i])) visibleAppIndices.push_back(i);

    ImGuiIO& appListIo = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        appListIo.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A) && !ImGui::IsAnyItemActive()) {
        for (int i : visibleAppIndices) panel.selectedIndices.insert(i);
        m_lastFocusedPanel = &panel;
    }

    if(ui::button("Select all",ui::Icon::Check,ui::Appearance::Quiet,ImVec2(110*s,28*s))) {
        for(int index:visibleAppIndices)panel.selectedIndices.insert(index);
    }
    ImGui::SameLine(0,4*s);
    ImGui::BeginDisabled(panel.selectedIndices.empty());
    if(ui::button("Clear selection",ui::Icon::Close,ui::Appearance::Quiet,ImVec2(140*s,28*s)))panel.selectedIndices.clear();
    ImGui::EndDisabled();
    ImGui::SameLine(0,16*s);
    {
        const std::string summary=panel.refreshInProgress?"Loading apps...":std::to_string(visibleAppIndices.size())+" apps, "+
            std::to_string(panel.selectedIndices.size())+" selected";
        const ImVec2 chip=ImGui::GetCursorScreenPos();
        const ImVec2 textSize=ImGui::CalcTextSize(summary.c_str());
        const ImVec2 chipSize(textSize.x+24*s,26*s);
        ImDrawList* draw=ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(chip.x,chip.y+1*s),ImVec2(chip.x+chipSize.x,chip.y+1*s+chipSize.y),ImGui::GetColorU32(ImGuiCol_Button),chipSize.y*0.5f);
        draw->AddText(ImVec2(chip.x+12*s,chip.y+1*s+(chipSize.y-textSize.y)*0.5f),ImGui::GetColorU32(ImGuiCol_TextDisabled),summary.c_str());
        ImGui::Dummy(ImVec2(chipSize.x,28*s));
    }
    ImGui::Spacing();

    ImGuiTableFlags tf = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable;
    if (ImGui::BeginTable(("##AppsTbl" + sideId).c_str(), 4, tf)) {
        ImGui::TableSetupColumn("App Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 0.28f);
        ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch, 0.32f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("APK Path", ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty && specs->SpecsCount > 0) {
                int col = specs->Specs[0].ColumnIndex;
                bool desc = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                std::sort(panel.appEntries.begin(), panel.appEntries.end(),
                    [col, desc](const InstalledAppEntry& a, const InstalledAppEntry& b) {
                        int cmp = 0;
                        if (col == 1) cmp = _stricmp(a.packageName.c_str(), b.packageName.c_str());
                        else if (col == 2) cmp = (int)a.isSystem - (int)b.isSystem;
                        else if (col == 3) cmp = _stricmp(a.apkPath.c_str(), b.apkPath.c_str());
                        else {
                            const std::string& aName = a.appName.empty() ? a.packageName : a.appName;
                            const std::string& bName = b.appName.empty() ? b.packageName : b.appName;
                            cmp = _stricmp(aName.c_str(), bName.c_str());
                        }
                        return desc ? cmp > 0 : cmp < 0;
                    });
                panel.selectedIndices.clear();
                specs->SpecsDirty = false;
            }
        }

        ui::PanelStyle theme(m_resolvedLightTheme);
        ImVec4 selectionTint = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        selectionTint.w = m_resolvedLightTheme ? 0.22f : 0.15f;
        ImGui::PushStyleColor(ImGuiCol_Header, selectionTint);
        const float rowHeight = std::max(32*s, ImGui::GetTextLineHeight()+12*s);
        for (int i : visibleAppIndices) {
            const auto& app = panel.appEntries[i];

            bool selected = panel.selectedIndices.count(i) > 0;
            ImGui::TableNextRow(0, rowHeight);
            ImGui::TableNextColumn();
            std::string displayName = app.appName.empty() ? app.packageName : app.appName;
            std::string rowId = "##app" + std::to_string(i);
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            const float nameWidth = ImGui::GetContentRegionAvail().x;
            const bool rowClicked = ImGui::Selectable(rowId.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, rowHeight - 4*s));
            {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                if (selected) {
                    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                    draw->AddRectFilled(ImVec2(a.x, a.y + 2*s), ImVec2(a.x + 3*s, b.y - 2*s), ImGui::GetColorU32(ImGuiCol_CheckMark), 2*s);
                }
                const ImVec4 tints[] = { theme.blue, theme.green, theme.gold, ui::rgb(196, 181, 253), ui::rgb(249, 168, 212) };
                size_t hash = 0;
                for (char c : app.packageName) hash = hash * 31 + (unsigned char)c;
                const ImVec4 tint = tints[hash % 5];
                ImVec4 tileFill = tint; tileFill.w = 0.16f;
                const float tile = 24*s;
                const ImVec2 tilePos(rowStart.x + 6*s, rowStart.y + (rowHeight - 4*s - tile) * 0.5f);
                draw->AddRectFilled(tilePos, ImVec2(tilePos.x + tile, tilePos.y + tile), theme.color(tileFill), 7*s);
                char initial[2] = { displayName.empty() ? '?' : (char)std::toupper((unsigned char)displayName[0]), 0 };
                ImFont* initialFont = ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
                const ImVec2 initialSize = initialFont->CalcTextSizeA(13*s, FLT_MAX, 0, initial);
                draw->AddText(initialFont, 13*s, ImVec2(tilePos.x + (tile - initialSize.x) * 0.5f, tilePos.y + (tile - initialSize.y) * 0.5f),
                    theme.color(tint), initial);
                ui::drawTextClipped(ImVec2(tilePos.x + tile + 10*s, rowStart.y + (rowHeight - 4*s - ImGui::GetTextLineHeight()) * 0.5f),
                    displayName, nameWidth - tile - 22*s, ImGui::GetColorU32(ImGuiCol_Text));
            }
            if (rowClicked) {
                ImGuiIO& io = ImGui::GetIO();
                auto anchorIt = std::find(visibleAppIndices.begin(), visibleAppIndices.end(), panel.focusedIndex);
                auto clickedIt = std::find(visibleAppIndices.begin(), visibleAppIndices.end(), i);
                if (io.KeyShift && anchorIt != visibleAppIndices.end() && clickedIt != visibleAppIndices.end()) {
                    if (!io.KeyCtrl) panel.selectedIndices.clear();
                    auto first = std::min(anchorIt, clickedIt);
                    auto last = std::max(anchorIt, clickedIt);
                    for (auto it = first; it != last + 1; ++it) panel.selectedIndices.insert(*it);
                } else if (io.KeyCtrl) {
                    if (selected) panel.selectedIndices.erase(i);
                    else panel.selectedIndices.insert(i);
                } else {
                    panel.selectedIndices.clear();
                    panel.selectedIndices.insert(i);
                }
                panel.focusedIndex = i;
                m_lastFocusedPanel = &panel;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected) {
                panel.selectedIndices.clear();
                panel.selectedIndices.insert(i);
                panel.focusedIndex = i;
                m_lastFocusedPanel = &panel;
            }
            if (ImGui::BeginPopupContextItem(("##AppsContext" + sideId + "_" + std::to_string(i)).c_str())) {
                ImGui::TextDisabled("%d app%s selected", (int)panel.selectedIndices.size(),
                                    panel.selectedIndices.size() == 1 ? "" : "s");
                ImGui::Separator();
                ImGui::BeginDisabled(m_asyncBusy.load());
                if (ImGui::MenuItem("Uninstall")) uninstallSelectedAppsFromMenu(false);
                if (ImGui::MenuItem("Uninstall with Root")) uninstallSelectedAppsFromMenu(true);
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            const float textOffset = (rowHeight - 4*s - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffset);
            ui::textClipped(app.packageName, ImGui::GetContentRegionAvail().x, ImGui::GetColorU32(ImGuiCol_TextDisabled));
            ImGui::TableNextColumn();
            {
                const ImVec2 cell = ImGui::GetCursorScreenPos();
                const float pillWidth = std::min(ImGui::GetContentRegionAvail().x, 64*s);
                theme.pill(ImVec2(cell.x + (ImGui::GetContentRegionAvail().x - pillWidth) * 0.5f, cell.y + (rowHeight - 4*s - 22*s) * 0.5f),
                    ImVec2(pillWidth, 22*s), ui::Icon::None, app.isSystem ? "System" : "User", app.isSystem ? theme.gold : theme.blue);
                ImGui::Dummy(ImVec2(pillWidth, rowHeight - 4*s));
            }
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffset);
            ui::textClipped(app.apkPath, ImGui::GetContentRegionAvail().x, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        }
        ImGui::PopStyleColor();
        ImGui::EndTable();
        if (visibleAppIndices.empty()) {
            const ImVec2 tableMin = ImGui::GetItemRectMin();
            const ImVec2 tableMax = ImGui::GetItemRectMax();
            const char* title = panel.refreshInProgress ? "Loading installed apps..." : "No apps match your filter";
            const char* detail = panel.refreshInProgress ? "Reading the app list from your phone." : "Try another app name, package or type.";
            ImFont* titleFont = ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
            const ImVec2 titleSize = titleFont->CalcTextSizeA(ImGui::GetFontSize() * 1.15f, FLT_MAX, 0, title);
            const ImVec2 detailSize = ImGui::CalcTextSize(detail);
            const float centerX = (tableMin.x + tableMax.x) * 0.5f;
            const float y = tableMin.y + std::min(180*s, (tableMax.y - tableMin.y) * 0.35f);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            theme.tile(ui::Icon::Apps, ImVec2(centerX - 24*s, y - 64*s), 48*s / theme.s, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            draw->AddText(titleFont, ImGui::GetFontSize() * 1.15f, ImVec2(centerX - titleSize.x * 0.5f, y), theme.color(theme.text), title);
            draw->AddText(ImVec2(centerX - detailSize.x * 0.5f, y + titleSize.y + 8*s), theme.color(theme.muted), detail);
        }
    }
}

std::string App::getBackupsRootPath() const {
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path dir = (len > 0 && len < MAX_PATH)
        ? std::filesystem::path(exePath).parent_path()
        : std::filesystem::current_path();
    return pathToUtf8(dir / "Backups");
}

void App::refreshBackupManagerApps() {
    int slot = m_backupManagerSlot;
    if (!deviceSession(slot).connected || deviceSession(slot).serial.empty()) {
        m_backupManagerApps.clear();
        m_backupManagerNeedsAppRefresh = false;
        return;
    }
    std::string serial = deviceSession(slot).serial;
    postAsync("Loading backup app list...", [this, slot, serial]() {
        std::vector<InstalledAppEntry> apps = deviceForSlot(slot).listInstalledApps(serial, true);
        std::vector<BackupManagerAppRow> rows;
        rows.reserve(apps.size());
        for (auto& app : apps) rows.push_back({ app, false });
        UiMessage message;
        message.setBackupApps = true;
        message.backupApps = std::move(rows);
        message.markBackupAppsFresh = true;
        postUiMessage(std::move(message));
    });
}

void App::probeBackupManagerRootAccess() {
    int slot = m_backupManagerSlot;
    if (!deviceSession(slot).connected || deviceSession(slot).serial.empty()) return;
    std::string serial = deviceSession(slot).serial;
    if (m_backupRootAccess == BackupRootAccess::Checking && m_backupRootAccessSerial == serial) return;
    if (m_backupRootAccessSerial == serial && m_backupRootAccess != BackupRootAccess::Unknown) return;

    m_backupRootAccess = BackupRootAccess::Checking;
    m_backupRootAccessSerial = serial;
    std::string adbPath = deviceForSlot(slot).getAdbPath();
    postAsync("Checking backup capabilities...", [this, slot, serial, adbPath]() {
        DeviceClient probe;
        if (!adbPath.empty()) probe.setAdbPath(adbPath);
        bool available = !probe.getAdbPath().empty() && probe.isRootAvailable(serial);
        UiMessage message;
        message.backupRootProbeComplete = true;
        message.backupRootAvailable = available;
        message.backupRootSlot = slot;
        message.backupRootSerial = serial;
        postUiMessage(std::move(message));
    });
}

void App::refreshBackupManagerBackups() {
    std::vector<BackupManagerBackupRow> rows;
    std::filesystem::path root = toFsPath(getBackupsRootPath());
    std::error_code ec;
    std::filesystem::path jobsRoot = root / "Jobs";
    if (std::filesystem::exists(jobsRoot, ec)) {
        for (auto jobIt = std::filesystem::directory_iterator(jobsRoot, ec);
             !ec && jobIt != std::filesystem::directory_iterator(); jobIt.increment(ec)) {
            if (jobIt->is_regular_file(ec) && _wcsicmp(jobIt->path().extension().c_str(), L".zip") == 0) {
                BackupArchive archive;
                std::string archiveError;
                if (!archive.open(jobIt->path(), archiveError)) {
                    LOG_ERROR("BackupManager", "Could not open backup archive " + pathToUtf8(jobIt->path()) + ": " + archiveError);
                    continue;
                }
                std::string jobData;
                if (!archive.readEntry("job.json", jobData, archiveError)) continue;
                nlohmann::json meta = nlohmann::json::parse(jobData, nullptr, false);
                if (meta.is_discarded()) continue;
                BackupManagerBackupRow row;
                row.isJob = true;
                row.isArchive = true;
                row.record.packageName = meta.value("name", "Backup job");
                row.record.serial = meta.value("serial", "");
                row.record.created = meta.value("created", pathToUtf8(jobIt->path().stem()));
                row.record.backupPath = pathToUtf8(jobIt->path());
                row.appCount = meta.value("appCount", 0);
                row.record.sizeBytes = jobIt->file_size(ec);
                row.contentsSummary = std::to_string(row.appCount) + " app";
                if (row.appCount != 1) row.contentsSummary += "s";
                row.contentsSummary += "  ZIP store archive";
                if (meta.contains("contents") && meta["contents"].is_array()) {
                    for (const auto& item : meta["contents"]) {
                        if (item.is_string()) row.contentsSummary += " " + item.get<std::string>();
                    }
                }
                if (meta.contains("packages") && meta["packages"].is_array()) {
                    for (const auto& package : meta["packages"])
                        if (package.is_string()) row.searchText += " " + package.get<std::string>();
                }
                for (const auto& entry : archive.entries()) {
                    if (entry.name.size() < 11 || entry.name.compare(entry.name.size() - 11, 11, "backup.json") != 0) continue;
                    std::string appData;
                    if (!archive.readEntry(entry.name, appData, archiveError)) continue;
                    nlohmann::json appMeta = nlohmann::json::parse(appData, nullptr, false);
                    if (!appMeta.is_discarded())
                        row.searchText += " " + appMeta.value("label", "") + " " + appMeta.value("packageName", "");
                }
                rows.push_back(std::move(row));
                continue;
            }
            if (!jobIt->is_directory(ec)) continue;
            std::ifstream jf(jobIt->path() / "job.json", std::ios::binary);
            if (!jf) continue;
            nlohmann::json meta = nlohmann::json::parse(jf, nullptr, false);
            if (meta.is_discarded()) continue;
            BackupManagerBackupRow row;
            row.isJob = true;
            row.record.packageName = meta.value("name", "Backup job");
            row.record.serial = meta.value("serial", "");
            row.record.created = meta.value("created", pathToUtf8(jobIt->path().filename()));
            row.record.backupPath = pathToUtf8(jobIt->path());
            row.appCount = meta.value("appCount", 0);
            row.record.sizeBytes = directorySizeBytes(jobIt->path());
            row.contentsSummary = std::to_string(row.appCount) + " app";
            if (row.appCount != 1) row.contentsSummary += "s";
            if (meta.contains("contents") && meta["contents"].is_array()) {
                std::string parts;
                for (auto& item : meta["contents"]) {
                    std::string s = item.get<std::string>();
                    if (s.empty()) continue;
                    if (!parts.empty()) parts += " ";
                    parts += s;
                }
                if (!parts.empty()) row.contentsSummary += "  " + parts;
            }
            if (meta.contains("packages") && meta["packages"].is_array()) {
                for (const auto& package : meta["packages"])
                    if (package.is_string()) row.searchText += " " + package.get<std::string>();
            }
            std::filesystem::path jobApps = jobIt->path() / "Apps";
            std::error_code labelEc;
            for (auto packageIt = std::filesystem::directory_iterator(jobApps, labelEc);
                 !labelEc && packageIt != std::filesystem::directory_iterator(); packageIt.increment(labelEc)) {
                if (!packageIt->is_directory(labelEc)) continue;
                for (auto backupIt = std::filesystem::directory_iterator(packageIt->path(), labelEc);
                     !labelEc && backupIt != std::filesystem::directory_iterator(); backupIt.increment(labelEc)) {
                    std::ifstream appMetaFile(backupIt->path() / "backup.json", std::ios::binary);
                    if (!appMetaFile) continue;
                    nlohmann::json appMeta = nlohmann::json::parse(appMetaFile, nullptr, false);
                    if (appMeta.is_discarded()) continue;
                    row.searchText += " " + appMeta.value("label", "") + " " + appMeta.value("packageName", "");
                }
            }
            rows.push_back(std::move(row));
        }
    }

    if (std::filesystem::exists(root, ec)) {
        for (auto pkgIt = std::filesystem::directory_iterator(root, ec);
             !ec && pkgIt != std::filesystem::directory_iterator(); pkgIt.increment(ec)) {
            if (!pkgIt->is_directory(ec)) continue;
            if (_stricmp(pathToUtf8(pkgIt->path().filename()).c_str(), "Jobs") == 0) continue;
            for (auto bakIt = std::filesystem::directory_iterator(pkgIt->path(), ec);
                 !ec && bakIt != std::filesystem::directory_iterator(); bakIt.increment(ec)) {
                if (!bakIt->is_directory(ec)) continue;
                std::ifstream mf(bakIt->path() / "backup.json", std::ios::binary);
                if (!mf) continue;
                nlohmann::json meta = nlohmann::json::parse(mf, nullptr, false);
                if (meta.is_discarded()) continue;
                AppBackupRecord rec;
                rec.packageName = meta.value("packageName", "");
                rec.label = meta.value("label", "");
                rec.serial = meta.value("serial", "");
                rec.created = meta.value("created", "");
                rec.backupPath = pathToUtf8(bakIt->path());
                if (meta.contains("has")) {
                    rec.hasApk = meta["has"].value("apk", false);
                    rec.hasData = meta["has"].value("data", false);
                    rec.hasDeviceProtectedData = meta["has"].value("data_de", false);
                    rec.hasExternalData = meta["has"].value("external", false);
                    rec.hasObb = meta["has"].value("obb", false);
                    rec.hasMedia = meta["has"].value("media", false);
                }
                rec.sizeBytes = directorySizeBytes(bakIt->path());
                if (!rec.packageName.empty()) {
                    BackupManagerBackupRow row;
                    row.record = rec;
                    row.contentsSummary = "";
                    if (rec.hasApk) row.contentsSummary += "APK ";
                    if (rec.hasData) row.contentsSummary += "Data ";
                    if (rec.hasDeviceProtectedData) row.contentsSummary += "DE ";
                    if (rec.hasExternalData) row.contentsSummary += "Ext ";
                    if (rec.hasObb) row.contentsSummary += "OBB ";
                    if (rec.hasMedia) row.contentsSummary += "Media ";
                    rows.push_back(std::move(row));
                }
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const BackupManagerBackupRow& a, const BackupManagerBackupRow& b) {
        if (a.record.created != b.record.created) return a.record.created > b.record.created;
        return _stricmp(a.record.packageName.c_str(), b.record.packageName.c_str()) < 0;
    });
    m_backupManagerBackups = std::move(rows);
    m_backupManagerBackupSelectionAnchor = -1;
    m_backupManagerNeedsBackupRefresh = false;
}

bool App::backupManagerAppVisible(const BackupManagerAppRow& row) const {
    if (row.app.isSystem && !m_backupShowSystemApps) return false;
    std::string filter(m_backupManagerSearch);
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
    if (filter.empty()) return true;
    std::string hay = row.app.appName + " " + row.app.packageName + " " + row.app.apkPath;
    std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
    return hay.find(filter) != std::string::npos;
}

void App::setBackupManagerAppSelection(bool selected, bool visibleOnly) {
    for (auto& row : m_backupManagerApps) {
        if (row.app.isSystem && !m_backupShowSystemApps) {
            row.selected = false;
            continue;
        }
        if (!visibleOnly || backupManagerAppVisible(row))
            row.selected = selected;
    }
}

void App::invertBackupManagerAppSelection(bool visibleOnly) {
    for (auto& row : m_backupManagerApps) {
        if (row.app.isSystem && !m_backupShowSystemApps) {
            row.selected = false;
            continue;
        }
        if (!visibleOnly || backupManagerAppVisible(row))
            row.selected = !row.selected;
    }
}

void App::openBackupManagerJob(const BackupManagerBackupRow& row) {
    m_backupManagerJobApps.clear();
    m_backupManagerJobAppSelectionAnchor = -1;
    m_backupManagerJobOpen = true;
    m_backupManagerOpenJobPath = row.record.backupPath;
    m_backupManagerOpenJobTitle = row.record.packageName;

    if (row.isArchive) {
        BackupArchive archive;
        std::string archiveError;
        if (!archive.open(toFsPath(row.record.backupPath), archiveError)) {
            LOG_ERROR("BackupManager", "Could not open backup archive: " + archiveError);
            return;
        }
        for (const auto& entry : archive.entries()) {
            if (entry.name.size() < 11 || entry.name.compare(entry.name.size() - 11, 11, "backup.json") != 0) continue;
            std::string appData;
            if (!archive.readEntry(entry.name, appData, archiveError)) continue;
            nlohmann::json meta = nlohmann::json::parse(appData, nullptr, false);
            if (meta.is_discarded()) continue;
            BackupManagerJobAppRow appRow;
            appRow.archivePath = row.record.backupPath;
            appRow.archivePrefix = entry.name.substr(0, entry.name.size() - 11);
            while (!appRow.archivePrefix.empty() && appRow.archivePrefix.back() == '/') appRow.archivePrefix.pop_back();
            auto& rec = appRow.record;
            rec.packageName = meta.value("packageName", "");
            rec.label = meta.value("label", "");
            rec.serial = meta.value("serial", "");
            rec.created = meta.value("created", "");
            rec.backupPath = makeBackupArchiveReference(appRow.archivePath, appRow.archivePrefix);
            if (meta.contains("has")) {
                rec.hasApk = meta["has"].value("apk", false);
                rec.hasData = meta["has"].value("data", false);
                rec.hasDeviceProtectedData = meta["has"].value("data_de", false);
                rec.hasExternalData = meta["has"].value("external", false);
                rec.hasObb = meta["has"].value("obb", false);
                rec.hasMedia = meta["has"].value("media", false);
            }
            const std::string sizePrefix = appRow.archivePrefix + "/";
            for (const auto& appEntry : archive.entries())
                if (appEntry.name.compare(0, sizePrefix.size(), sizePrefix) == 0) rec.sizeBytes += appEntry.size;
            if (!rec.packageName.empty()) m_backupManagerJobApps.push_back(std::move(appRow));
        }
        std::sort(m_backupManagerJobApps.begin(), m_backupManagerJobApps.end(), [](const BackupManagerJobAppRow& a, const BackupManagerJobAppRow& b) {
            return _stricmp(a.record.packageName.c_str(), b.record.packageName.c_str()) < 0;
        });
        return;
    }

    std::filesystem::path appsRoot = toFsPath(row.record.backupPath);
    if (row.isJob) appsRoot /= "Apps";
    std::error_code ec;
    for (auto pkgIt = std::filesystem::directory_iterator(appsRoot, ec);
         !ec && pkgIt != std::filesystem::directory_iterator(); pkgIt.increment(ec)) {
        if (!pkgIt->is_directory(ec)) continue;
        for (auto bakIt = std::filesystem::directory_iterator(pkgIt->path(), ec);
             !ec && bakIt != std::filesystem::directory_iterator(); bakIt.increment(ec)) {
            if (!bakIt->is_directory(ec)) continue;
            std::ifstream mf(bakIt->path() / "backup.json", std::ios::binary);
            if (!mf) continue;
            nlohmann::json meta = nlohmann::json::parse(mf, nullptr, false);
            if (meta.is_discarded()) continue;
            AppBackupRecord rec;
            rec.packageName = meta.value("packageName", "");
            rec.label = meta.value("label", "");
            rec.serial = meta.value("serial", "");
            rec.created = meta.value("created", "");
            rec.backupPath = pathToUtf8(bakIt->path());
            if (meta.contains("has")) {
                rec.hasApk = meta["has"].value("apk", false);
                rec.hasData = meta["has"].value("data", false);
                rec.hasDeviceProtectedData = meta["has"].value("data_de", false);
                rec.hasExternalData = meta["has"].value("external", false);
                rec.hasObb = meta["has"].value("obb", false);
                rec.hasMedia = meta["has"].value("media", false);
            }
            rec.sizeBytes = directorySizeBytes(bakIt->path());
            if (!rec.packageName.empty()) m_backupManagerJobApps.push_back({ rec, false });
        }
    }
    std::sort(m_backupManagerJobApps.begin(), m_backupManagerJobApps.end(), [](const BackupManagerJobAppRow& a, const BackupManagerJobAppRow& b) {
        return _stricmp(a.record.packageName.c_str(), b.record.packageName.c_str()) < 0;
    });

    if (!row.isJob && m_backupManagerJobApps.empty()) {
        m_backupManagerJobApps.push_back({ row.record, false });
    }
}

void App::closeBackupManagerJob() {
    m_backupManagerJobOpen = false;
    m_backupManagerOpenJobPath.clear();
    m_backupManagerOpenJobTitle.clear();
    m_backupManagerJobApps.clear();
    m_backupManagerJobAppSelectionAnchor = -1;
}

std::vector<App::ArchiveTransportChannel> App::buildArchiveTransportChannels(
    DeviceClient& baseClient, int slot, int localPortBase) {
    std::vector<ArchiveTransportChannel> channels;
    const std::string baseSerial = baseClient.connectedSerial().empty()
        ? deviceSession(slot).serial
        : baseClient.connectedSerial();
    if (companion::isSerial(baseSerial)) {
        const DevicePipePreference pipes = m_prefs.pipePreferencesFor(baseSerial);
        ArchiveTransportChannel base;
        base.dev = &baseClient;
        base.name = companionPrimaryName(baseClient);
        base.endpoint = baseClient.connectedHost() + ":" + std::to_string(baseClient.serverPort());
        channels.push_back(std::move(base));
        for (const auto& endpoint : companionChannelTargets(baseClient, std::clamp(pipes.usbPipeCount, 1, 4),
                std::clamp(pipes.wifiPipeCount, 1, 4), TransferBatch::MAX_CHANNELS)) {
            auto client = std::make_unique<DeviceClient>();
            client->setSerial(baseSerial);
            client->setServerPort(endpoint.port);
            if (!client->connectTcp(endpoint.host, endpoint.port) || !client->verifyConnection()) continue;
            ArchiveTransportChannel channel;
            channel.dev = client.get();
            channel.owned = std::move(client);
            channel.name = companionChannelName(endpoint.kind);
            channel.endpoint = endpoint.host + ":" + std::to_string(endpoint.port);
            channels.push_back(std::move(channel));
        }
        return channels;
    }
    const int remotePort = baseClient.serverPort();
    std::string usbSerial;
    std::string wifiIp;

    if (isWifiSerial(baseSerial)) {
        auto colon = baseSerial.rfind(':');
        wifiIp = baseSerial.substr(0, colon);
    } else {
        usbSerial = baseSerial;
    }

    std::string savedWifiIp;
    for (const auto& saved : m_prefs.savedWifiDevices) {
        const std::string wifiSerial = saved.wifiIp.empty()
            ? ""
            : saved.wifiIp + ":" + std::to_string(saved.port);
        if (saved.serial == baseSerial || wifiSerial == baseSerial ||
            (!usbSerial.empty() && saved.serial == usbSerial)) {
            if (usbSerial.empty()) usbSerial = saved.serial;
            savedWifiIp = saved.wifiIp;
            break;
        }
    }

    std::string hardwareSerial;
    if (!baseSerial.empty()) {
        hardwareSerial = baseClient.runAdbCommand(
            "-s " + baseSerial + " shell getprop ro.serialno");
        while (!hardwareSerial.empty() &&
            (hardwareSerial.back() == '\n' || hardwareSerial.back() == '\r' ||
             hardwareSerial.back() == ' ' || hardwareSerial.back() == '\t')) {
            hardwareSerial.pop_back();
        }
    }

    std::vector<DeviceInfo> deviceSnapshot;
    {
        std::lock_guard<std::mutex> lock(m_deviceMutex);
        deviceSnapshot = m_devices;
    }
    if (usbSerial.empty() && !hardwareSerial.empty()) {
        for (const auto& device : deviceSnapshot) {
            if (device.state != "device" || isWifiSerial(device.serial)) continue;
            std::string candidateHardware = baseClient.runAdbCommand(
                "-s " + device.serial + " shell getprop ro.serialno");
            while (!candidateHardware.empty() &&
                (candidateHardware.back() == '\n' || candidateHardware.back() == '\r' ||
                 candidateHardware.back() == ' ' || candidateHardware.back() == '\t')) {
                candidateHardware.pop_back();
            }
            if (candidateHardware == hardwareSerial) {
                usbSerial = device.serial;
                break;
            }
        }
    }

    // Ask the phone for its current address first. Saved addresses go stale
    // whenever the router hands out a new DHCP lease.
    if (wifiIp.empty() && !baseSerial.empty()) {
        const std::string wlanOutput = baseClient.runAdbCommand(
            "-s " + baseSerial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
        const auto inetPosition = wlanOutput.find("inet ");
        if (inetPosition != std::string::npos) {
            const auto start = inetPosition + 5;
            const auto slash = wlanOutput.find('/', start);
            if (slash != std::string::npos) wifiIp = wlanOutput.substr(start, slash - start);
        }
    }
    std::vector<std::string> wifiCandidates;
    if (!wifiIp.empty()) wifiCandidates.push_back(wifiIp);
    if (!savedWifiIp.empty() && savedWifiIp != wifiIp) wifiCandidates.push_back(savedWifiIp);
    size_t wifiCandidateIndex = 0;

    const DevicePipePreference pipePreference = m_prefs.pipePreferencesFor(baseSerial);
    const int requestedUsb = std::clamp(pipePreference.usbPipeCount, 1, 4);
    const int requestedWifi = std::clamp(pipePreference.wifiPipeCount, 1, 4);
    const bool baseUsesWifi = baseClient.isDirectConnection() || isWifiSerial(baseSerial);

    ArchiveTransportChannel base;
    base.dev = &baseClient;
    base.name = baseUsesWifi
        ? (baseClient.isDirectConnection() ? "WiFi (Direct)" : "WiFi (ADB)")
        : "USB (ADB)";
    base.endpoint = baseClient.isDirectConnection()
        ? wifiIp + ":" + std::to_string(remotePort)
        : "127.0.0.1:" + std::to_string(remotePort);
    channels.push_back(std::move(base));

    int usbCount = baseUsesWifi ? 0 : 1;
    int wifiCount = baseUsesWifi ? 1 : 0;
    int nextPort = localPortBase;

    auto addUsbChannel = [&]() -> bool {
        if (usbSerial.empty()) return false;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const int localPort = nextPort++;
            const std::string forwardResult = baseClient.runAdbCommand(
                "-s " + usbSerial + " forward tcp:" + std::to_string(localPort) +
                " tcp:" + std::to_string(remotePort));
            if (forwardResult.find("offline") != std::string::npos ||
                forwardResult.find("not found") != std::string::npos) {
                return false;
            }
            if (forwardResult.find("error") != std::string::npos) continue;

            auto client = std::make_unique<DeviceClient>();
            client->setAdbPath(baseClient.getAdbPath());
            client->setServerPort(baseClient.serverPort());
            client->setLocalPort(localPort);
            if (!client->connectTcp("127.0.0.1", localPort) || !client->verifyConnection()) {
                client->disconnectTcp();
                baseClient.runAdbCommand("-s " + usbSerial + " forward --remove tcp:" +
                    std::to_string(localPort));
                continue;
            }

            ArchiveTransportChannel channel;
            channel.dev = client.get();
            channel.owned = std::move(client);
            channel.name = "USB (ADB)";
            channel.endpoint = "127.0.0.1:" + std::to_string(localPort) +
                " -> " + usbSerial + ":" + std::to_string(remotePort);
            channel.forwardSerial = usbSerial;
            channel.forwardPort = localPort;
            channels.push_back(std::move(channel));
            ++usbCount;
            return true;
        }
        return false;
    };

    bool wifiAddressConfirmed = false;
    auto addWifiChannel = [&]() -> bool {
        std::unique_ptr<DeviceClient> client;
        while (wifiCandidateIndex < wifiCandidates.size()) {
            wifiIp = wifiCandidates[wifiCandidateIndex];
            client = std::make_unique<DeviceClient>();
            client->setAdbPath(baseClient.getAdbPath());
            client->setServerPort(baseClient.serverPort());
            if (client->connectTcp(wifiIp, remotePort) && client->verifyConnection()) break;
            client.reset();
            // Once an address has carried a pipe, a later failure means the
            // helper is saturated, not that the address is wrong.
            if (wifiAddressConfirmed) return false;
            LOG_WARN("BackupManager", "WiFi helper not reachable at " + wifiIp + ", trying next address");
            ++wifiCandidateIndex;
        }
        if (!client) return false;
        wifiAddressConfirmed = true;

        ArchiveTransportChannel channel;
        channel.dev = client.get();
        channel.owned = std::move(client);
        channel.name = "WiFi (Direct)";
        channel.endpoint = wifiIp + ":" + std::to_string(remotePort);
        channels.push_back(std::move(channel));
        ++wifiCount;
        return true;
    };

    while (usbCount < requestedUsb && channels.size() < TransferBatch::MAX_CHANNELS) {
        if (!addUsbChannel()) break;
    }
    while (wifiCount < requestedWifi && channels.size() < TransferBatch::MAX_CHANNELS) {
        if (!addWifiChannel()) break;
    }

    LOG_INFO("BackupManager", "Prepared " + std::to_string(channels.size()) +
        " archive transport channel(s), USB " + std::to_string(usbCount) + "/" +
        std::to_string(requestedUsb) + ", WiFi " + std::to_string(wifiCount) + "/" +
        std::to_string(requestedWifi) + ", helper port " + std::to_string(remotePort));
    for (size_t index = 0; index < channels.size(); ++index) {
        LOG_INFO("BackupManager", "Archive channel " + std::to_string(index + 1) + ": " +
            channels[index].name + " at " + channels[index].endpoint);
    }
    return channels;
}

void App::closeArchiveTransportChannels(
    DeviceClient& baseClient, std::vector<ArchiveTransportChannel>& channels) {
    for (auto& channel : channels) {
        if (channel.owned) channel.owned->disconnectTcp();
        if (!channel.forwardSerial.empty() && channel.forwardPort > 0) {
            baseClient.runAdbCommand("-s " + channel.forwardSerial +
                " forward --remove tcp:" + std::to_string(channel.forwardPort));
        }
    }
    channels.clear();
}

void App::startBackupManagerBackup() {
    int slot = m_backupManagerSlot;
    std::vector<std::string> packages;
    std::unordered_map<std::string, std::string> appLabels;
    for (const auto& row : m_backupManagerApps)
        if (row.selected && (m_backupShowSystemApps || !row.app.isSystem)) {
            packages.push_back(row.app.packageName);
            appLabels[row.app.packageName] = row.app.appName;
        }
    if (packages.empty()) return;
    LOG_INFO("BackupManager", "Backup requested for " + std::to_string(packages.size()) + " package(s)");
    std::string prompt = "Back up " + std::to_string(packages.size()) + " selected app(s) to the PC?";
    if (MessageBoxA(g_mainHwnd, prompt.c_str(), "Confirm Backup", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    std::string serial = deviceSession(slot).serial;
    std::string adbPath = deviceForSlot(slot).getAdbPath();
    AppBackupOptions options;
    options.includeApk = m_backupIncludeApk;
    options.includeData = m_backupIncludeData;
    options.includeDeviceProtectedData = m_backupIncludeDeData;
    options.includeExternalData = m_backupIncludeExternalData;
    options.includeObb = m_backupIncludeObb;
    options.includeMedia = m_backupIncludeMedia;
    options.excludeCache = m_backupExcludeCache;
    bool requiresRoot = options.includeData || options.includeDeviceProtectedData;
    std::string created = backupJobStamp();
    std::filesystem::path jobDir = toFsPath(getBackupsRootPath()) / "Jobs" / created;
    std::filesystem::path appsDir = jobDir / "Apps";
    std::error_code ec;
    std::filesystem::create_directories(appsDir, ec);
    std::string root = pathToUtf8(appsDir);
    LOG_INFO("BackupManager", "Backup confirmed. serial=" + serial + " job=" + pathToUtf8(jobDir));
    m_backupProgressActive = true;
    m_backupProgressIsRestore = false;
    m_backupProgressCurrent = 0;
    m_backupProgressTotal = (int)packages.size();
    m_backupProgressStage = 0;
    m_backupProgressStageTotal = 1;
    m_backupProgressBytes = 0;
    m_backupProgressTotalBytes = 0;
    m_backupProgressBytesPerSecond = 0.0;
    m_backupProgressEtaSeconds = -1.0;
    m_backupProgressLastTransferUpdate = {};
    m_backupProgressPackage.clear();
    m_backupProgressStageLabel = "Starting backup";
    m_backupPauseRequested = false;
    m_backupCancelRequested = false;
    m_backupProgressPackages.clear();
    m_backupProgressPackages.reserve(packages.size());
    for (const auto& packageName : packages) {
        auto label = appLabels.find(packageName);
        m_backupProgressPackages.push_back(
            label != appLabels.end() && !label->second.empty() ? label->second : packageName);
    }
    m_backupProgressActiveAppIndex = -1;
    m_backupAppProgress.assign(packages.size(), 0.0f);
    m_backupDisplayedAppProgress.assign(packages.size(), 0.0f);
    m_backupAppActive.assign(packages.size(), false);
    m_backupDisplayedOverallProgress = 0.0f;

    postAsync("Backing up apps...", [this, slot, serial, adbPath, packages, appLabels, root, created, jobPath = pathToUtf8(jobDir), options, requiresRoot]() {
        int ok = 0;
        bool archiveOk = true;
        std::string details;
        std::vector<std::string> successfulPackages;
        try {
            if (serial.empty()) {
                LOG_ERROR("BackupManager", "Backup aborted. Selected device serial was empty.");
                UiMessage message;
                message.hasBackupProgress = true;
                message.backupProgressActive = false;
                message.hasNotification = true;
                message.notificationTitle = "Backup Failed";
                message.notificationMessage = "The selected device is no longer connected.";
                message.notificationIsError = true;
                postUiMessage(std::move(message));
                return;
            }
            DeviceClient backupClient;
            if (!adbPath.empty()) backupClient.setAdbPath(adbPath);
            if (backupClient.getAdbPath().empty() && !backupClient.findAdb()) {
                LOG_ERROR("BackupManager", "Backup aborted. ADB was not found.");
                UiMessage message;
                message.hasBackupProgress = true;
                message.backupProgressActive = false;
                message.hasNotification = true;
                message.notificationTitle = "Backup Failed";
                message.notificationMessage = "ADB was not found.";
                message.notificationIsError = true;
                postUiMessage(std::move(message));
                return;
            }

            DeviceClient& slotClient = deviceForSlot(slot);
            if (requiresRoot && !backupClient.isRootAvailable(serial)) {
                LOG_ERROR("BackupManager", "Root access check failed for " + serial);
                UiMessage message;
                message.hasBackupProgress = true;
                message.backupProgressActive = false;
                message.hasNotification = true;
                message.notificationTitle = "Root Required";
                message.notificationMessage = "Private app data requires root access. Choose Standard Backup to continue with APK and shared app folders only.";
                message.notificationIsError = true;
                postUiMessage(std::move(message));
                return;
            }
            if (requiresRoot && (!slotClient.isServerRunning() || !slotClient.useRoot())) {
                LOG_INFO("BackupManager", "Starting root device server for backup on " + serial);
                if (!slotClient.startServer(serial, m_preferAdbForward, true)) {
                    UiMessage message;
                    message.hasBackupProgress = true;
                    message.backupProgressActive = false;
                    message.hasNotification = true;
                    message.notificationTitle = "Root Required";
                    message.notificationMessage = "Backup Manager requires its device helper to run as root. Enable Root for this device and approve the request in your root manager.\n\nDetails: " + slotClient.lastError();
                    message.notificationIsError = true;
                    postUiMessage(std::move(message));
                    return;
                }
            }
            if (requiresRoot) {
                m_prefs.setRootEnabledForSerial(deviceIdentity(serial), true);
                m_prefs.save();
            }
            if (!slotClient.isServerRunning()) {
                LOG_INFO("BackupManager", "Starting device helper for archive backup on " + serial);
                if (!slotClient.startServer(serial, m_preferAdbForward, requiresRoot)) {
                    UiMessage message;
                    message.hasBackupProgress = true;
                    message.backupProgressActive = false;
                    message.hasNotification = true;
                    message.notificationTitle = "Backup Failed";
                    message.notificationMessage = "The device helper could not be started.\n\nDetails: " + slotClient.lastError();
                    message.notificationIsError = true;
                    postUiMessage(std::move(message));
                    return;
                }
            }

            std::vector<ArchiveBackupApp> archiveApps;
            archiveApps.reserve(packages.size());
            for (const auto& packageName : packages) {
                auto label = appLabels.find(packageName);
                archiveApps.push_back({packageName, label == appLabels.end() ? packageName : label->second});
            }
            std::string remoteArchive;
            uint64_t remoteArchiveSize = 0;
            std::vector<bool> backedUp;
            std::string archiveDetails;
            auto prepareProgress = [this](const AppInstallProgress& progress) {
                UiMessage message;
                message.hasBackupProgress = true;
                message.backupProgressActive = true;
                message.backupProgressCurrent = progress.completedApps;
                message.backupProgressTotal = progress.totalApps;
                message.backupProgressPackage = progress.packageName;
                message.backupProgressStageLabel = progress.stageLabel;
                message.backupProgressAppIndex = progress.backupIndex;
                message.backupProgressAppFraction = progress.appProgress;
                postUiMessage(std::move(message));
                return !m_backupCancelRequested.load();
            };
            LOG_INFO("BackupManager", "Preparing one device-local archive for " +
                std::to_string(packages.size()) + " app(s)");
            bool prepared = slotClient.createDeviceBackupArchive(serial, created, archiveApps, options,
                remoteArchive, remoteArchiveSize, backedUp, archiveDetails, prepareProgress);
            if (!prepared) {
                LOG_ERROR("BackupManager", "Device archive preparation failed: " + archiveDetails);
                std::error_code removeEc;
                std::filesystem::remove_all(toFsPath(jobPath), removeEc);
                UiMessage message;
                message.hasBackupProgress = true;
                message.backupProgressActive = false;
                message.hasNotification = true;
                message.notificationTitle = "Backup Failed";
                message.notificationMessage = archiveDetails.empty()
                    ? "The device could not create the backup archive."
                    : archiveDetails;
                message.notificationIsError = true;
                postUiMessage(std::move(message));
                return;
            }

            const std::filesystem::path archivePath = toFsPath(jobPath).parent_path() /
                (toFsPath(created).wstring() + L".zip");
            const std::filesystem::path temporaryArchivePath = archivePath.wstring() + L".tmp";
            HANDLE seed = CreateFileW(temporaryArchivePath.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            bool downloadOk = seed != INVALID_HANDLE_VALUE;
            if (downloadOk) {
                LARGE_INTEGER size{};
                size.QuadPart = (LONGLONG)remoteArchiveSize;
                downloadOk = SetFilePointerEx(seed, size, nullptr, FILE_BEGIN) && SetEndOfFile(seed);
                CloseHandle(seed);
            }

            auto archiveChannels = buildArchiveTransportChannels(slotClient, slot, 6040);
            std::vector<DeviceClient*> downloadWorkers;
            for (auto& channel : archiveChannels) downloadWorkers.push_back(channel.dev);
            if (downloadWorkers.empty()) downloadWorkers.push_back(&slotClient);
            const int activeUsbPipes = (int)std::count_if(archiveChannels.begin(), archiveChannels.end(),
                [](const ArchiveTransportChannel& channel) { return channel.name.find("USB") != std::string::npos; });
            const int activeWifiPipes = (int)archiveChannels.size() - activeUsbPipes;
            const std::string activePipeSummary = std::to_string(activeUsbPipes) + " USB + " +
                std::to_string(activeWifiPipes) + " WiFi pipes";
            LOG_INFO("BackupManager", "Downloading the device archive over " + activePipeSummary);

            const uint64_t maximumStripeSize = remoteArchiveSize >= 8ull * 1024ull * 1024ull * 1024ull
                ? 128ull * 1024ull * 1024ull
                : 64ull * 1024ull * 1024ull;
            const uint64_t fairStripeSize = remoteArchiveSize /
                std::max<uint64_t>(1, (uint64_t)downloadWorkers.size() * 4);
            const uint64_t archiveStripeSize = std::clamp<uint64_t>(fairStripeSize,
                8ull * 1024ull * 1024ull, maximumStripeSize);
            std::atomic<uint64_t> nextArchiveOffset{0};
            std::atomic<uint64_t> downloadedBytes{0};
            std::atomic<bool> transferFailed{!downloadOk};
            std::vector<uint64_t> channelBytes(downloadWorkers.size(), 0);
            std::vector<double> channelSeconds(downloadWorkers.size(), 0.0);
            const auto archiveTransferStart = std::chrono::steady_clock::now();
            TransferRateEstimator transferRate;
            auto downloadWorker = [&](DeviceClient* client, size_t channelIndex) {
                const auto channelStart = std::chrono::steady_clock::now();
                HANDLE output = CreateFileW(temporaryArchivePath.c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (output == INVALID_HANDLE_VALUE) {
                    transferFailed = true;
                    return;
                }
                while (!transferFailed.load()) {
                    {
                        std::unique_lock<std::mutex> lock(m_backupControlMutex);
                        m_backupControlCV.wait(lock, [this]() {
                            return !m_backupPauseRequested.load() || m_backupCancelRequested.load();
                        });
                    }
                    if (m_backupCancelRequested.load()) break;
                    uint64_t offset = nextArchiveOffset.fetch_add(archiveStripeSize);
                    if (offset >= remoteArchiveSize) break;
                    uint64_t length = std::min(archiveStripeSize, remoteArchiveSize - offset);
                    uint64_t received = client->readRangeStreaming(remoteArchive, offset, length,
                        [&](const void* data, uint32_t byteCount, uint64_t chunkOffset) {
                            if (m_backupCancelRequested.load()) return false;
                            LARGE_INTEGER position{};
                            position.QuadPart = (LONGLONG)chunkOffset;
                            DWORD written = 0;
                            if (!SetFilePointerEx(output, position, nullptr, FILE_BEGIN) ||
                                !WriteFile(output, data, byteCount, &written, nullptr) || written != byteCount) {
                                transferFailed = true;
                                return false;
                            }
                            channelBytes[channelIndex] += byteCount;
                            uint64_t total = downloadedBytes.fetch_add(byteCount) + byteCount;
                            const TransferRateSample rate = transferRate.update(total, remoteArchiveSize);
                            UiMessage message;
                            message.hasBackupProgress = true;
                            message.backupProgressActive = true;
                            message.backupProgressIsRestore = false;
                            message.backupProgressCurrent = (int)std::count(backedUp.begin(), backedUp.end(), true);
                            message.backupProgressTotal = (int)packages.size();
                            message.backupProgressBytes = total;
                            message.backupProgressTotalBytes = remoteArchiveSize;
                            message.backupProgressBytesPerSecond = rate.bytesPerSecond;
                            message.backupProgressEtaSeconds = rate.etaSeconds;
                            message.backupProgressStageLabel = "Downloading backup archive";
                            message.backupProgressUsbPipes = activeUsbPipes;
                            message.backupProgressWifiPipes = activeWifiPipes;
                            postUiMessage(std::move(message));
                            return true;
                        });
                    if (received != length && !m_backupCancelRequested.load()) transferFailed = true;
                }
                FlushFileBuffers(output);
                CloseHandle(output);
                channelSeconds[channelIndex] = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - channelStart).count();
            };
            std::vector<std::thread> archiveThreads;
            if (downloadOk) {
                for (size_t index = 0; index < downloadWorkers.size(); ++index)
                    archiveThreads.emplace_back(downloadWorker, downloadWorkers[index], index);
                for (auto& thread : archiveThreads) thread.join();
            }
            const double archiveTransferSeconds = std::max(0.001, std::chrono::duration<double>(
                std::chrono::steady_clock::now() - archiveTransferStart).count());
            for (size_t index = 0; index < downloadWorkers.size(); ++index) {
                const double seconds = std::max(0.001, channelSeconds[index]);
                LOG_INFO("BackupManager", "Archive download channel " + std::to_string(index + 1) +
                    " " + archiveChannels[index].name + ": " + formatSize(channelBytes[index]) +
                    " at " + formatSpeed((double)channelBytes[index] / seconds));
            }
            LOG_INFO("BackupManager", "Archive download aggregate: " + formatSize(downloadedBytes.load()) +
                " in " + std::to_string(archiveTransferSeconds) + " s, " +
                formatSpeed((double)downloadedBytes.load() / archiveTransferSeconds));
            closeArchiveTransportChannels(slotClient, archiveChannels);
            slotClient.deleteFile(remoteArchive);

            bool archiveCancelled = m_backupCancelRequested.load();
            std::string verifyError;
            BackupArchive verification;
            if (!archiveCancelled && !transferFailed.load() && downloadedBytes.load() == remoteArchiveSize) {
                downloadOk = verification.open(temporaryArchivePath, verifyError) &&
                    verification.find("job.json") != nullptr;
            } else {
                downloadOk = false;
            }
            if (downloadOk) {
                std::error_code renameEc;
                std::filesystem::rename(temporaryArchivePath, archivePath, renameEc);
                if (renameEc) {
                    std::filesystem::remove(archivePath, renameEc);
                    renameEc.clear();
                    std::filesystem::rename(temporaryArchivePath, archivePath, renameEc);
                }
                downloadOk = !renameEc;
                if (!downloadOk) verifyError = renameEc.message();
            }
            if (!downloadOk) {
                std::error_code removeEc;
                std::filesystem::remove(temporaryArchivePath, removeEc);
            }
            {
                std::error_code removeEc;
                std::filesystem::remove_all(toFsPath(jobPath), removeEc);
            }

            ok = (int)std::count(backedUp.begin(), backedUp.end(), true);
            LOG_INFO("BackupManager", std::string(downloadOk ? "Archive backup complete: " : "Archive backup failed: ") +
                pathToUtf8(archivePath));
            UiMessage archiveResult;
            archiveResult.refreshBackupList = true;
            archiveResult.hasStatus = true;
            archiveResult.status = archiveCancelled ? "Backup cancelled" :
                (downloadOk ? "Backed up " + std::to_string(ok) + " app(s)" : "Backup failed");
            archiveResult.hasBackupProgress = true;
            archiveResult.backupProgressActive = false;
            archiveResult.backupProgressCurrent = ok;
            archiveResult.backupProgressTotal = (int)packages.size();
            archiveResult.backupProgressStageLabel = archiveCancelled ? "Backup cancelled" :
                (downloadOk ? "Backup finished" : "Backup failed");
            archiveResult.hasNotification = true;
            if (archiveCancelled) {
                archiveResult.notificationTitle = "Backup Cancelled";
                archiveResult.notificationMessage = "The partial archive was removed.";
                archiveResult.notificationIsError = false;
            } else if (downloadOk && ok == (int)packages.size()) {
                archiveResult.notificationTitle = "Backup Complete";
                archiveResult.notificationMessage = "All selected apps were prepared on the device and saved as one indexed archive.";
                archiveResult.notificationIsError = false;
            } else if (downloadOk) {
                archiveResult.notificationTitle = "Backup Finished With Errors";
                archiveResult.notificationMessage = "Completed apps: " + std::to_string(ok) + " of " +
                    std::to_string(packages.size()) + "\n\n" + archiveDetails;
                archiveResult.notificationIsError = true;
            } else {
                archiveResult.notificationTitle = "Backup Failed";
                archiveResult.notificationMessage = verifyError.empty()
                    ? "The backup archive could not be downloaded or verified."
                    : "The backup archive could not be verified.\n\nDetails: " + verifyError;
                archiveResult.notificationIsError = true;
            }
            postUiMessage(std::move(archiveResult));
            return;

            DeviceClient& backupWorkClient = slotClient.isServerRunning() ? slotClient : backupClient;
            std::vector<std::unique_ptr<DeviceClient>> parallelClients;
            std::vector<DeviceClient*> workers;
            if (backupWorkClient.isServerRunning() && backupWorkClient.useRoot()) {
                std::vector<DeviceClient*> channelSources{ &backupWorkClient };
                if (slot == 0 && m_dualChannelAvailable && m_secondaryChannel.isServerRunning()) {
                    channelSources.push_back(&m_secondaryChannel);
                }
                if (slot == 0) {
                    for (auto& extra : m_extraChannels) {
                        if (extra.dev && extra.dev->isServerRunning()) channelSources.push_back(extra.dev.get());
                    }
                }
                int targetWorkers = std::min((int)channelSources.size(), (int)packages.size());
                for (int i = 0; i < targetWorkers; ++i) {
                    auto client = std::make_unique<DeviceClient>();
                    if (!client->connectParallelFrom(*channelSources[i])) continue;
                    workers.push_back(client.get());
                    parallelClients.push_back(std::move(client));
                }
            }
            if (workers.empty()) workers.push_back(&backupWorkClient);
            LOG_INFO("BackupManager", "Using " + std::to_string(workers.size()) +
                " active transport channel(s) for backup");

            std::atomic<int> nextPackage{0};
            std::atomic<int> completedPackages{0};
            std::mutex resultMutex;
            auto backupWorker = [&](DeviceClient* client) {
                while (true) {
                    {
                        std::unique_lock<std::mutex> lock(m_backupControlMutex);
                        m_backupControlCV.wait(lock, [this]() {
                            return !m_backupPauseRequested.load() || m_backupCancelRequested.load();
                        });
                    }
                    if (m_backupCancelRequested.load()) return;
                    int packageIndex = nextPackage.fetch_add(1);
                    if (packageIndex >= (int)packages.size()) return;
                    const auto& pkg = packages[packageIndex];
                    AppBackupRecord rec;
                    std::string out;
                    try {
                        LOG_INFO("BackupManager", "Starting package backup: " + pkg);
                        auto progressCb = [this, packageIndex, &completedPackages, totalPackages = (int)packages.size()](const AppBackupProgress& progress) {
                            {
                                std::unique_lock<std::mutex> lock(m_backupControlMutex);
                                m_backupControlCV.wait(lock, [this]() {
                                    return !m_backupPauseRequested.load() || m_backupCancelRequested.load();
                                });
                            }
                            if (m_backupCancelRequested.load()) return false;
                            float byteFraction = progress.bytesTotal > 0
                                ? std::clamp((float)progress.bytesTransferred / (float)progress.bytesTotal, 0.0f, 1.0f)
                                : 0.0f;
                            float appFraction = ((float)progress.stageIndex + byteFraction) /
                                (float)std::max(1, progress.stageCount);
                            UiMessage message;
                            message.hasBackupProgress = true;
                            message.backupProgressActive = true;
                            message.backupProgressCurrent = completedPackages.load();
                            message.backupProgressTotal = totalPackages;
                            message.backupProgressStage = progress.stageIndex;
                            message.backupProgressStageTotal = progress.stageCount;
                            message.backupProgressBytes = progress.bytesTransferred;
                            message.backupProgressTotalBytes = progress.bytesTotal;
                            message.backupProgressPackage = progress.packageName;
                            message.backupProgressStageLabel = progress.stageLabel;
                            message.backupProgressAppIndex = packageIndex;
                            message.backupProgressAppFraction = std::clamp(appFraction, 0.0f, 1.0f);
                            postUiMessage(std::move(message));
                            return true;
                        };
                        auto label = appLabels.find(pkg);
                        bool packageOk = client->backupApp(serial, pkg,
                            label == appLabels.end() ? std::string{} : label->second,
                            root, options, rec, out, progressCb);
                        if (packageOk) {
                            std::lock_guard<std::mutex> lock(resultMutex);
                            ok++;
                            successfulPackages.push_back(pkg);
                            LOG_INFO("BackupManager", "Package backup complete: " + pkg + " -> " + rec.backupPath);
                        } else if (!m_backupCancelRequested.load()) {
                            std::lock_guard<std::mutex> lock(resultMutex);
                            details += pkg + ": " + out + "\n";
                            LOG_ERROR("BackupManager", "Package backup failed: " + pkg + ": " + out);
                        }
                    } catch (const std::exception& e) {
                        std::lock_guard<std::mutex> lock(resultMutex);
                        details += pkg + ": " + e.what() + "\n";
                        LOG_ERROR("BackupManager", "Package backup exception: " + pkg + ": " + e.what());
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(resultMutex);
                        details += pkg + ": Unexpected backup failure.\n";
                        LOG_ERROR("BackupManager", "Package backup unknown exception: " + pkg);
                    }
                    completedPackages.fetch_add(1);
                    if (!m_backupCancelRequested.load()) {
                        UiMessage completedMessage;
                        completedMessage.hasBackupProgress = true;
                        completedMessage.backupProgressActive = true;
                        completedMessage.backupProgressTotal = (int)packages.size();
                        completedMessage.backupProgressCurrent = completedPackages.load();
                        completedMessage.backupProgressAppIndex = packageIndex;
                        completedMessage.backupProgressAppFraction = 1.0f;
                        postUiMessage(std::move(completedMessage));
                    }
                }
            };
            std::vector<std::thread> backupThreads;
            for (DeviceClient* client : workers) backupThreads.emplace_back(backupWorker, client);
            for (auto& thread : backupThreads) thread.join();
            bool cancelled = m_backupCancelRequested.load();
            LOG_INFO("BackupManager", std::string(cancelled ? "Backup cancelled. ok=" : "Backup finished. ok=") + std::to_string(ok) + " total=" + std::to_string(packages.size()));
            nlohmann::json job;
            job["format"] = "FastEnoughBackupJob";
            job["formatVersion"] = 2;
            job["archiveMode"] = "ZIP64 store";
            job["name"] = "Backup job " + created;
            job["created"] = created;
            job["serial"] = serial;
            job["appCount"] = ok;
            job["requestedAppCount"] = packages.size();
            job["cancelled"] = cancelled;
            job["packages"] = successfulPackages;
            nlohmann::json contents = nlohmann::json::array();
            if (options.includeApk) contents.push_back("APK");
            if (options.includeData) contents.push_back("Data");
            if (options.includeDeviceProtectedData) contents.push_back("DE");
            if (options.includeExternalData) contents.push_back("External");
            if (options.includeObb) contents.push_back("OBB");
            if (options.includeMedia) contents.push_back("Media");
            job["contents"] = contents;
            std::ofstream jf(toFsPath(jobPath) / "job.json", std::ios::binary);
            jf << job.dump(2);
            jf.close();
            if (ok > 0) {
                UiMessage archiveMessage;
                archiveMessage.hasBackupProgress = true;
                archiveMessage.backupProgressActive = true;
                archiveMessage.backupProgressCurrent = ok;
                archiveMessage.backupProgressTotal = (int)packages.size();
                archiveMessage.backupProgressStageLabel = "Creating indexed backup archive";
                postUiMessage(std::move(archiveMessage));
                const std::filesystem::path archivePath = toFsPath(jobPath).parent_path() / (toFsPath(created).wstring() + L".zip");
                std::string archiveError;
                archiveOk = BackupArchive::createStore(toFsPath(jobPath), archivePath, archiveError);
                if (archiveOk) {
                    std::error_code removeEc;
                    std::filesystem::remove_all(toFsPath(jobPath), removeEc);
                    if (removeEc) LOG_ERROR("BackupManager", "Could not remove archived staging directory: " + removeEc.message());
                    LOG_INFO("BackupManager", "Backup archive complete: " + pathToUtf8(archivePath));
                } else {
                    details += "Archive creation failed: " + archiveError + "\n";
                    LOG_ERROR("BackupManager", "Backup archive creation failed: " + archiveError);
                }
            }
            if (cancelled && ok == 0) {
                std::error_code removeEc;
                std::filesystem::remove_all(toFsPath(jobPath), removeEc);
            }
            UiMessage message;
            message.refreshBackupList = true;
            message.hasStatus = true;
            message.status = cancelled ? "Backup cancelled after " + std::to_string(ok) + " app(s)" :
                "Backed up " + std::to_string(ok) + " app(s)";
            message.hasBackupProgress = true;
            message.backupProgressActive = false;
            message.backupProgressCurrent = ok;
            message.backupProgressTotal = (int)packages.size();
            message.backupProgressStageLabel = cancelled ? "Backup cancelled" : "Backup finished";
            message.hasNotification = true;
            std::string backupSummary = "Completed apps: " + std::to_string(ok) + " of " +
                std::to_string(packages.size()) + "\n\nIncluded content\n";
            if (options.includeApk) backupSummary += "APK files\n";
            if (options.includeData) backupSummary += "App data\n";
            if (options.includeDeviceProtectedData) backupSummary += "Device protected data\n";
            if (options.includeExternalData) backupSummary += "External data\n";
            if (options.includeObb) backupSummary += "OBB data\n";
            if (options.includeMedia) backupSummary += "Media data\n";
            if (cancelled) {
                message.notificationTitle = "Backup Cancelled";
                message.notificationMessage = backupSummary + "\n" + (ok > 0
                    ? "Completed app backups were kept. Partial app files were removed."
                    : "No completed app backups were kept.");
                message.notificationIsError = false;
            } else if (ok == (int)packages.size() && archiveOk) {
                message.notificationTitle = "Backup Complete";
                message.notificationMessage = backupSummary + "\nAll selected apps were saved to the Backups folder.";
                message.notificationIsError = false;
            } else {
                if (details.size() > 3000) details = details.substr(0, 3000) + "\nMore errors were omitted.";
                message.notificationTitle = "Backup Finished With Errors";
                message.notificationMessage = backupSummary + "\nIssues\n" +
                    (details.empty() ? "Some backups failed." : details);
                message.notificationIsError = true;
            }
            postUiMessage(std::move(message));
        } catch (const std::exception& e) {
            LOG_ERROR("BackupManager", std::string("Backup task exception: ") + e.what());
            UiMessage message;
            message.hasBackupProgress = true;
            message.backupProgressActive = false;
            message.hasNotification = true;
            message.notificationTitle = "Backup Failed";
            message.notificationMessage = e.what();
            message.notificationIsError = true;
            postUiMessage(std::move(message));
        } catch (...) {
            LOG_ERROR("BackupManager", "Backup task unknown exception");
            UiMessage message;
            message.hasBackupProgress = true;
            message.backupProgressActive = false;
            message.hasNotification = true;
            message.notificationTitle = "Backup Failed";
            message.notificationMessage = "Unexpected backup failure.";
            message.notificationIsError = true;
            postUiMessage(std::move(message));
        }
    });
}

void App::startBackupManagerRestore() {
    std::vector<std::string> backups;
    if (m_backupManagerJobOpen) {
        for (const auto& row : m_backupManagerJobApps) {
            if (row.selected) backups.push_back(row.record.backupPath);
        }
        if (backups.empty()) {
            for (const auto& row : m_backupManagerJobApps)
                backups.push_back(row.record.backupPath);
        }
    } else {
        for (const auto& row : m_backupManagerBackups) {
            if (!row.selected) continue;
            if (!row.isJob) {
                backups.push_back(row.record.backupPath);
                continue;
            }
            if (row.isArchive) {
                BackupArchive archive;
                std::string archiveError;
                if (!archive.open(toFsPath(row.record.backupPath), archiveError)) continue;
                for (const auto& entry : archive.entries()) {
                    if (entry.name.size() < 11 || entry.name.compare(entry.name.size() - 11, 11, "backup.json") != 0) continue;
                    std::string prefix = entry.name.substr(0, entry.name.size() - 11);
                    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
                    backups.push_back(makeBackupArchiveReference(row.record.backupPath, prefix));
                }
                continue;
            }
            std::filesystem::path appsRoot = toFsPath(row.record.backupPath) / "Apps";
            std::error_code ec;
            for (auto pkgIt = std::filesystem::directory_iterator(appsRoot, ec);
                 !ec && pkgIt != std::filesystem::directory_iterator(); pkgIt.increment(ec)) {
                if (!pkgIt->is_directory(ec)) continue;
                for (auto bakIt = std::filesystem::directory_iterator(pkgIt->path(), ec);
                     !ec && bakIt != std::filesystem::directory_iterator(); bakIt.increment(ec)) {
                    if (bakIt->is_directory(ec) && std::filesystem::exists(bakIt->path() / "backup.json", ec))
                        backups.push_back(pathToUtf8(bakIt->path()));
                }
            }
        }
    }
    startBackupManagerRestorePaths(std::move(backups));
}

void App::startBackupManagerRestorePaths(std::vector<std::string> backups) {
    int slot = m_backupManagerSlot;
    if (backups.empty()) return;
    LOG_INFO("BackupManager", "Restore requested for " + std::to_string(backups.size()) + " backup(s)");
    std::string prompt = "Restore " + std::to_string(backups.size()) + " selected backup(s) to the selected device?";
    if (MessageBoxA(g_mainHwnd, prompt.c_str(), "Confirm Restore", MB_YESNO | MB_ICONWARNING) != IDYES) return;

    std::string serial = deviceSession(slot).serial;
    std::string adbPath = deviceForSlot(slot).getAdbPath();
    AppBackupOptions options;
    options.includeApk = m_backupIncludeApk;
    options.includeData = m_backupIncludeData;
    options.includeDeviceProtectedData = m_backupIncludeDeData;
    options.includeExternalData = m_backupIncludeExternalData;
    options.includeObb = m_backupIncludeObb;
    options.includeMedia = m_backupIncludeMedia;
    options.grantRuntimePermissions = m_backupRestorePermissions;
    options.allowDowngrade = m_backupAllowDowngrade;
    bool requiresRoot = options.includeData || options.includeDeviceProtectedData;

    LOG_INFO("BackupManager", "Restore confirmed. serial=" + serial);
    m_backupProgressActive = true;
    m_backupProgressIsRestore = true;
    m_backupProgressCurrent = 0;
    m_backupProgressTotal = (int)backups.size();
    m_backupProgressStage = 0;
    m_backupProgressStageTotal = 1;
    m_backupProgressBytes = 0;
    m_backupProgressTotalBytes = 0;
    m_backupProgressBytesPerSecond = 0.0;
    m_backupProgressEtaSeconds = -1.0;
    m_backupProgressLastTransferUpdate = {};
    m_backupProgressPackage.clear();
    m_backupProgressStageLabel = "Starting restore";
    m_backupPauseRequested = false;
    m_backupCancelRequested = false;
    m_backupProgressPackages.clear();
    m_backupProgressActiveAppIndex = -1;
    std::unordered_map<std::string, std::string> installedLabels;
    for (const auto& row : m_backupManagerApps) {
        if (!row.app.appName.empty()) installedLabels[row.app.packageName] = row.app.appName;
    }
    for (const auto& backup : backups) {
        nlohmann::json meta;
        std::string metadataError;
        readBackupMetadata(backup, meta, metadataError);
        std::string fallback = pathToUtf8(toFsPath(backup).filename());
        if (meta.is_discarded()) {
            m_backupProgressPackages.push_back(std::move(fallback));
            continue;
        }
        std::string packageName = meta.value("packageName", fallback);
        std::string appName = meta.value("label", "");
        if (appName.empty()) {
            auto installed = installedLabels.find(packageName);
            if (installed != installedLabels.end()) appName = installed->second;
        }
        m_backupProgressPackages.push_back(appName.empty() ? packageName : appName);
    }
    m_backupAppProgress.assign(backups.size(), 0.0f);
    m_backupDisplayedAppProgress.assign(backups.size(), 0.0f);
    m_backupAppActive.assign(backups.size(), false);
    m_backupDisplayedOverallProgress = 0.0f;

    postAsync("Restoring backups...", [this, slot, serial, adbPath, backups, options, requiresRoot]() mutable {
        int ok = 0;
        std::string details;
        const int requestedRestoreCount = (int)backups.size();
        try {
            DeviceClient restoreClient;
            if (!adbPath.empty()) restoreClient.setAdbPath(adbPath);
            if (restoreClient.getAdbPath().empty() && !restoreClient.findAdb()) {
                LOG_ERROR("BackupManager", "Restore aborted. ADB was not found.");
                UiMessage message;
                message.hasBackupProgress = true;
                message.backupProgressActive = false;
                message.backupProgressIsRestore = true;
                message.hasNotification = true;
                message.notificationTitle = "Restore Failed";
                message.notificationMessage = "ADB was not found.";
                message.notificationIsError = true;
                postUiMessage(std::move(message));
                return;
            }

            DeviceClient& slotClient = deviceForSlot(slot);
            if (serial.empty() || (requiresRoot && !restoreClient.isRootAvailable(serial))) {
                LOG_ERROR("BackupManager", "Restore root access check failed for " + serial);
                UiMessage message;
                message.hasBackupProgress = true;
                message.backupProgressActive = false;
                message.backupProgressIsRestore = true;
                message.hasNotification = true;
                message.notificationTitle = "Root Required";
                message.notificationMessage = "Private app data requires root access. Choose Standard Backup to restore APK and shared app folders only.";
                message.notificationIsError = true;
                postUiMessage(std::move(message));
                return;
            }
            if (requiresRoot && (!slotClient.isServerRunning() || !slotClient.useRoot())) {
                LOG_INFO("BackupManager", "Starting root device server for restore on " + serial);
                if (!slotClient.startServer(serial, m_preferAdbForward, true)) {
                    UiMessage message;
                    message.hasBackupProgress = true;
                    message.backupProgressActive = false;
                    message.backupProgressIsRestore = true;
                    message.hasNotification = true;
                    message.notificationTitle = "Root Required";
                    message.notificationMessage = "Backup Manager requires its device helper to run as root. Enable Root for this device and approve the request in your root manager.\n\nDetails: " + slotClient.lastError();
                    message.notificationIsError = true;
                    postUiMessage(std::move(message));
                    return;
                }
            }
            if (!slotClient.isServerRunning()) {
                LOG_INFO("BackupManager", "Starting device helper for archive restore on " + serial);
                if (!slotClient.startServer(serial, m_preferAdbForward, false)) {
                    UiMessage message;
                    message.hasBackupProgress = true;
                    message.backupProgressActive = false;
                    message.backupProgressIsRestore = true;
                    message.hasNotification = true;
                    message.notificationTitle = "Restore Failed";
                    message.notificationMessage = "The device helper could not be started.\n\nDetails: " + slotClient.lastError();
                    message.notificationIsError = true;
                    postUiMessage(std::move(message));
                    return;
                }
            }
            if (requiresRoot) {
                m_prefs.setRootEnabledForSerial(deviceIdentity(serial), true);
                m_prefs.save();
            }
            DeviceClient& restoreWorkClient = slotClient.isServerRunning() ? slotClient : restoreClient;

            struct ArchiveRestoreItem {
                int originalIndex = 0;
                std::string prefix;
                std::string metadataJson;
            };
            std::unordered_map<std::string, std::vector<ArchiveRestoreItem>> archiveGroups;
            std::vector<std::string> directoryBackups;
            for (size_t index = 0; index < backups.size(); ++index) {
                std::string archivePath;
                std::string prefix;
                if (!splitBackupArchiveReference(backups[index], archivePath, prefix)) {
                    directoryBackups.push_back(backups[index]);
                    continue;
                }
                nlohmann::json metadata;
                std::string metadataError;
                if (!readBackupMetadata(backups[index], metadata, metadataError)) {
                    details += backups[index] + ": " + metadataError + "\n";
                    continue;
                }
                archiveGroups[archivePath].push_back({(int)index, prefix, metadata.dump()});
            }

            if (!archiveGroups.empty()) {
                int restoredFromArchives = 0;
                int failedFromArchives = 0;
                for (const auto& group : archiveGroups) {
                    if (m_backupCancelRequested.load()) break;
                    std::vector<std::string> prefixes;
                    std::vector<ArchiveRestoreSelection> selections;
                    prefixes.reserve(group.second.size());
                    selections.reserve(group.second.size());
                    for (const auto& item : group.second) {
                        prefixes.push_back(item.prefix);
                        selections.push_back({item.prefix, item.metadataJson});
                    }

                    std::string archiveError;
                    BackupArchive sourceArchive;
                    bool sourceArchiveOpen = sourceArchive.open(toFsPath(group.first), archiveError);
                    std::unordered_set<std::string> selectedPrefixes;
                    for (auto prefix : prefixes) {
                        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
                        selectedPrefixes.insert(std::move(prefix));
                    }
                    std::unordered_set<std::string> availablePrefixes;
                    if (sourceArchiveOpen) {
                        static constexpr std::string_view metadataSuffix = "/backup.json";
                        for (const auto& entry : sourceArchive.entries()) {
                            if (entry.name.size() <= metadataSuffix.size() ||
                                entry.name.compare(entry.name.size() - metadataSuffix.size(),
                                    metadataSuffix.size(), metadataSuffix) != 0) continue;
                            availablePrefixes.insert(entry.name.substr(
                                0, entry.name.size() - metadataSuffix.size()));
                        }
                    }
                    bool restoreWholeArchive = sourceArchiveOpen && !availablePrefixes.empty();
                    if (restoreWholeArchive) {
                        for (const auto& prefix : availablePrefixes) {
                            if (!selectedPrefixes.contains(prefix)) {
                                restoreWholeArchive = false;
                                break;
                            }
                        }
                    }

                    std::filesystem::path uploadArchive = toFsPath(group.first);
                    bool removeUploadArchive = false;
                    if (restoreWholeArchive) {
                        LOG_INFO("BackupManager", "All archive apps selected, uploading the original backup directly");
                    } else {
                        uploadArchive = std::filesystem::temp_directory_path() /
                            (L"FastEnough-restore-selection-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                             std::to_wstring(GetTickCount64()) + L".zip");
                        if (!BackupArchive::createSelection(toFsPath(group.first), prefixes, uploadArchive, archiveError)) {
                            details += group.first + ": " + archiveError + "\n";
                            failedFromArchives += (int)group.second.size();
                            continue;
                        }
                        removeUploadArchive = true;
                    }
                    std::error_code sizeError;
                    uint64_t archiveSize = std::filesystem::file_size(uploadArchive, sizeError);
                    const std::string remoteArchive = "/data/local/tmp/afm-restore-job-" +
                        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + ".zip";
                    if (sizeError || !restoreWorkClient.createFile(remoteArchive, archiveSize)) {
                        details += group.first + ": Could not create the device restore archive: " +
                            restoreWorkClient.lastError() + "\n";
                        failedFromArchives += (int)group.second.size();
                        if (removeUploadArchive) std::filesystem::remove(uploadArchive, sizeError);
                        continue;
                    }

                    auto archiveChannels = buildArchiveTransportChannels(restoreWorkClient, slot, 6110);
                    std::vector<DeviceClient*> uploadWorkers;
                    for (auto& channel : archiveChannels) uploadWorkers.push_back(channel.dev);
                    if (uploadWorkers.empty()) uploadWorkers.push_back(&restoreWorkClient);
                    const int activeUsbPipes = (int)std::count_if(archiveChannels.begin(), archiveChannels.end(),
                        [](const ArchiveTransportChannel& channel) { return channel.name.find("USB") != std::string::npos; });
                    const int activeWifiPipes = (int)archiveChannels.size() - activeUsbPipes;
                    const std::string activePipeSummary = std::to_string(activeUsbPipes) + " USB + " +
                        std::to_string(activeWifiPipes) + " WiFi pipes";
                    LOG_INFO("BackupManager", "Uploading selected restore archive over " + activePipeSummary);

                    const uint64_t maximumStripeSize = archiveSize >= 8ull * 1024ull * 1024ull * 1024ull
                        ? 128ull * 1024ull * 1024ull
                        : 64ull * 1024ull * 1024ull;
                    const uint64_t fairStripeSize = archiveSize /
                        std::max<uint64_t>(1, (uint64_t)uploadWorkers.size() * 4);
                    const uint64_t archiveStripeSize = std::clamp<uint64_t>(fairStripeSize,
                        8ull * 1024ull * 1024ull, maximumStripeSize);
                    const uint64_t stripeCount = (archiveSize + archiveStripeSize - 1) / archiveStripeSize;
                    std::atomic<uint64_t> nextStripe{0};
                    std::atomic<uint64_t> uploadedBytes{0};
                    std::atomic<bool> uploadFailed{false};
                    std::vector<uint64_t> channelBytes(uploadWorkers.size(), 0);
                    std::vector<double> channelSeconds(uploadWorkers.size(), 0.0);
                    const auto archiveTransferStart = std::chrono::steady_clock::now();
                    TransferRateEstimator transferRate;
                    auto uploadWorker = [&](DeviceClient* client, size_t channelIndex) {
                        const auto channelStart = std::chrono::steady_clock::now();
                        while (!uploadFailed.load()) {
                            {
                                std::unique_lock<std::mutex> lock(m_backupControlMutex);
                                m_backupControlCV.wait(lock, [this]() {
                                    return !m_backupPauseRequested.load() || m_backupCancelRequested.load();
                                });
                            }
                            if (m_backupCancelRequested.load()) {
                                uploadFailed = true;
                                break;
                            }
                            uint64_t stripe = nextStripe.fetch_add(1);
                            if (stripe >= stripeCount) break;
                            uint64_t offset = stripe * archiveStripeSize;
                            uint64_t length = std::min<uint64_t>(archiveStripeSize, archiveSize - offset);
                            uint64_t written = client->writeRangeStreaming(remoteArchive, offset,
                                pathToUtf8(uploadArchive), offset, length);
                            if (written != length) {
                                uploadFailed = true;
                                break;
                            }
                            channelBytes[channelIndex] += length;
                            uint64_t totalUploaded = uploadedBytes.fetch_add(length) + length;
                            const TransferRateSample rate = transferRate.update(totalUploaded, archiveSize);
                            UiMessage uploadMessage;
                            uploadMessage.hasBackupProgress = true;
                            uploadMessage.backupProgressActive = true;
                            uploadMessage.backupProgressIsRestore = true;
                            uploadMessage.backupProgressBytes = totalUploaded;
                            uploadMessage.backupProgressTotalBytes = archiveSize;
                            uploadMessage.backupProgressBytesPerSecond = rate.bytesPerSecond;
                            uploadMessage.backupProgressEtaSeconds = rate.etaSeconds;
                            uploadMessage.backupProgressStageLabel = "Uploading restore archive";
                            uploadMessage.backupProgressUsbPipes = activeUsbPipes;
                            uploadMessage.backupProgressWifiPipes = activeWifiPipes;
                            postUiMessage(std::move(uploadMessage));
                        }
                        channelSeconds[channelIndex] = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - channelStart).count();
                    };
                    std::vector<std::thread> uploadThreads;
                    for (size_t index = 0; index < uploadWorkers.size(); ++index)
                        uploadThreads.emplace_back(uploadWorker, uploadWorkers[index], index);
                    for (auto& thread : uploadThreads) thread.join();
                    const double archiveTransferSeconds = std::max(0.001, std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - archiveTransferStart).count());
                    for (size_t index = 0; index < uploadWorkers.size(); ++index) {
                        const double seconds = std::max(0.001, channelSeconds[index]);
                        LOG_INFO("BackupManager", "Archive upload channel " + std::to_string(index + 1) +
                            " " + archiveChannels[index].name + ": " + formatSize(channelBytes[index]) +
                            " at " + formatSpeed((double)channelBytes[index] / seconds));
                    }
                    LOG_INFO("BackupManager", "Archive upload aggregate: " + formatSize(uploadedBytes.load()) +
                        " in " + std::to_string(archiveTransferSeconds) + " s, " +
                        formatSpeed((double)uploadedBytes.load() / archiveTransferSeconds));
                    closeArchiveTransportChannels(restoreWorkClient, archiveChannels);
                    if (uploadFailed.load()) {
                        restoreWorkClient.deleteFile(remoteArchive);
                        details += group.first + ": Archive upload failed\n";
                        failedFromArchives += (int)group.second.size();
                        if (removeUploadArchive) std::filesystem::remove(uploadArchive, sizeError);
                        continue;
                    }

                    std::vector<bool> restoredApps;
                    std::string restoreOutput;
                    auto archiveProgress = [this, &group](const AppInstallProgress& progress) {
                        if (progress.backupIndex < 0 || progress.backupIndex >= (int)group.second.size()) return true;
                        UiMessage message;
                        message.hasBackupProgress = true;
                        message.backupProgressActive = true;
                        message.backupProgressIsRestore = true;
                        message.backupProgressPackage = progress.packageName;
                        message.backupProgressStageLabel = progress.stageLabel;
                        message.backupProgressAppIndex = group.second[progress.backupIndex].originalIndex;
                        message.backupProgressAppFraction = progress.appProgress;
                        message.backupProgressCurrent = progress.completedApps;
                        postUiMessage(std::move(message));
                        return !m_backupCancelRequested.load();
                    };
                    restoreWorkClient.restoreStagedBackupArchive(remoteArchive, selections, options,
                        restoredApps, restoreOutput, archiveProgress);
                    for (bool restored : restoredApps) {
                        if (restored) restoredFromArchives++;
                        else failedFromArchives++;
                    }
                    if (!restoreOutput.empty()) details += restoreOutput;
                    if (removeUploadArchive) std::filesystem::remove(uploadArchive, sizeError);
                }

                ok = restoredFromArchives;
                if (directoryBackups.empty()) {
                    bool cancelled = m_backupCancelRequested.load();
                    LOG_INFO("BackupManager", "Archive restore finished. ok=" + std::to_string(ok) +
                        " total=" + std::to_string(requestedRestoreCount));
                    UiMessage message;
                    message.refreshAppList = true;
                    message.hasStatus = true;
                    message.status = cancelled ? "Restore cancelled" : "Restored " + std::to_string(ok) + " backup(s)";
                    message.hasBackupProgress = true;
                    message.backupProgressActive = false;
                    message.backupProgressIsRestore = true;
                    message.backupProgressCurrent = ok;
                    message.backupProgressTotal = requestedRestoreCount;
                    message.backupProgressStageLabel = cancelled ? "Restore cancelled" : "Restore finished";
                    message.hasNotification = true;
                    if (cancelled) {
                        message.notificationTitle = "Restore Cancelled";
                        message.notificationMessage = "Restored " + std::to_string(ok) + " of " +
                            std::to_string(requestedRestoreCount) + " selected app(s) before stopping.";
                        message.notificationIsError = false;
                    } else if (ok == requestedRestoreCount) {
                        message.notificationTitle = "Restore Complete";
                        message.notificationMessage = "All selected apps were restored from the device-local archive.";
                        message.notificationIsError = false;
                    } else {
                        if (details.size() > 3000) details = details.substr(0, 3000) + "\nMore errors were omitted.";
                        message.notificationTitle = "Restore Finished With Errors";
                        const int failed = requestedRestoreCount - ok;
                        message.notificationMessage = "Restored " + std::to_string(ok) + " of " +
                            std::to_string(requestedRestoreCount) + " apps. " + std::to_string(failed) +
                            (failed == 1 ? " app failed." : " apps failed.");
                        if (!details.empty()) message.notificationMessage += "\n\n" + details;
                        message.notificationIsError = true;
                    }
                    postUiMessage(std::move(message));
                    return;
                }
                backups = std::move(directoryBackups);
            }

            AppBackupOptions restoreOptions = options;
            std::vector<bool> apkInstalled(backups.size(), true);
            bool usedBatchInstall = options.includeApk && restoreWorkClient.isServerRunning() && restoreWorkClient.useRoot();
            if (usedBatchInstall) {
                UiMessage batchMessage;
                batchMessage.hasBackupProgress = true;
                batchMessage.backupProgressActive = true;
                batchMessage.backupProgressIsRestore = true;
                batchMessage.backupProgressCurrent = 0;
                batchMessage.backupProgressTotal = (int)backups.size();
                batchMessage.backupProgressStageLabel = "Uploading APK batch";
                postUiMessage(std::move(batchMessage));
                std::string installOutput;
                auto installProgress = [this](uint64_t done, uint64_t total) {
                    {
                        std::unique_lock<std::mutex> lock(m_backupControlMutex);
                        m_backupControlCV.wait(lock, [this]() {
                            return !m_backupPauseRequested.load() || m_backupCancelRequested.load();
                        });
                    }
                    if (m_backupCancelRequested.load()) return false;
                    UiMessage message;
                    message.hasBackupProgress = true;
                    message.backupProgressActive = true;
                    message.backupProgressIsRestore = true;
                    message.backupProgressBytes = done;
                    message.backupProgressTotalBytes = total;
                    message.backupProgressStageLabel = "Uploading APK batch";
                    postUiMessage(std::move(message));
                    return true;
                };
                auto appInstallProgress = [this](const AppInstallProgress& progress) {
                    {
                        std::unique_lock<std::mutex> lock(m_backupControlMutex);
                        m_backupControlCV.wait(lock, [this]() {
                            return !m_backupPauseRequested.load() || m_backupCancelRequested.load();
                        });
                    }
                    if (m_backupCancelRequested.load()) return false;
                    UiMessage message;
                    message.hasBackupProgress = true;
                    message.backupProgressActive = true;
                    message.backupProgressIsRestore = true;
                    message.backupProgressTotal = (int)m_backupProgressPackages.size();
                    message.backupProgressPackage = progress.packageName;
                    message.backupProgressStageLabel = progress.stageLabel;
                    message.backupProgressAppIndex = progress.backupIndex;
                    message.backupProgressAppFraction = progress.appProgress * 0.45f;
                    postUiMessage(std::move(message));
                    return true;
                };
                if (!restoreWorkClient.installBackupApksBatch(backups, options, apkInstalled, installOutput,
                        installProgress, appInstallProgress)) {
                    details += installOutput;
                    LOG_ERROR("BackupManager", "Batch APK install completed with errors: " + installOutput);
                }
                restoreOptions.includeApk = false;
            }
            std::vector<std::unique_ptr<DeviceClient>> restoreParallelClients;
            std::vector<DeviceClient*> restoreWorkers;
            if (restoreWorkClient.isServerRunning() && restoreWorkClient.useRoot()) {
                std::vector<DeviceClient*> channelSources{ &restoreWorkClient };
                if (slot == 0 && m_dualChannelAvailable && m_secondaryChannel.isServerRunning()) {
                    channelSources.push_back(&m_secondaryChannel);
                }
                if (slot == 0) {
                    for (auto& extra : m_extraChannels) {
                        if (extra.dev && extra.dev->isServerRunning()) channelSources.push_back(extra.dev.get());
                    }
                }
                int workerCount = std::min((int)channelSources.size(), (int)backups.size());
                for (int i = 0; i < workerCount; ++i) {
                    auto client = std::make_unique<DeviceClient>();
                    if (!client->connectParallelFrom(*channelSources[i])) continue;
                    restoreWorkers.push_back(client.get());
                    restoreParallelClients.push_back(std::move(client));
                }
            }
            if (restoreWorkers.empty()) restoreWorkers.push_back(&restoreWorkClient);
            LOG_INFO("BackupManager", "Using " + std::to_string(restoreWorkers.size()) +
                " active transport channel(s) for restore");

            std::atomic<int> nextBackup{0};
            std::atomic<int> restoredCount{0};
            std::mutex restoreResultMutex;
            auto restoreWorker = [&](DeviceClient* client) {
                while (true) {
                    {
                        std::unique_lock<std::mutex> lock(m_backupControlMutex);
                        m_backupControlCV.wait(lock, [this]() {
                            return !m_backupPauseRequested.load() || m_backupCancelRequested.load();
                        });
                    }
                    if (m_backupCancelRequested.load()) return;
                    int backupIndex = nextBackup.fetch_add(1);
                    if (backupIndex >= (int)backups.size()) return;
                    const auto& backup = backups[backupIndex];
                    if (!apkInstalled[backupIndex]) {
                        LOG_ERROR("BackupManager", "Skipping data restore because APK install failed: " + backup);
                        continue;
                    }

                    std::string out;
                    std::string packageName;
                    {
                        std::ifstream mf(toFsPath(backup) / "backup.json", std::ios::binary);
                        nlohmann::json meta = nlohmann::json::parse(mf, nullptr, false);
                        if (!meta.is_discarded()) packageName = meta.value("packageName", "");
                    }
                    UiMessage progressMessage;
                    progressMessage.hasBackupProgress = true;
                    progressMessage.backupProgressActive = true;
                    progressMessage.backupProgressIsRestore = true;
                    progressMessage.backupProgressCurrent = restoredCount.load();
                    progressMessage.backupProgressTotal = (int)backups.size();
                    progressMessage.backupProgressStage = 0;
                    progressMessage.backupProgressStageTotal = 1;
                    progressMessage.backupProgressPackage = packageName.empty() ? backup : packageName;
                    progressMessage.backupProgressStageLabel = "Restoring app";
                    postUiMessage(std::move(progressMessage));
                    LOG_INFO("BackupManager", "Starting restore: " + backup);
                    auto progressCb = [this, backupIndex, usedBatchInstall, totalBackups = (int)backups.size()](const AppBackupProgress& progress) {
                        {
                            std::unique_lock<std::mutex> lock(m_backupControlMutex);
                            m_backupControlCV.wait(lock, [this]() {
                                return !m_backupPauseRequested.load() || m_backupCancelRequested.load();
                            });
                        }
                        if (m_backupCancelRequested.load()) return false;
                        float byteFraction = progress.bytesTotal > 0
                            ? std::clamp((float)progress.bytesTransferred / (float)progress.bytesTotal, 0.0f, 1.0f)
                            : 0.0f;
                        float stageFraction = ((float)progress.stageIndex + byteFraction) /
                            (float)std::max(1, progress.stageCount);
                        float appFraction = usedBatchInstall
                            ? 0.45f + std::clamp(stageFraction, 0.0f, 1.0f) * 0.55f
                            : std::clamp(stageFraction, 0.0f, 1.0f);
                        UiMessage message;
                        message.hasBackupProgress = true;
                        message.backupProgressActive = true;
                        message.backupProgressIsRestore = true;
                        message.backupProgressCurrent = backupIndex;
                        message.backupProgressTotal = totalBackups;
                        message.backupProgressStage = progress.stageIndex;
                        message.backupProgressStageTotal = progress.stageCount;
                        message.backupProgressBytes = progress.bytesTransferred;
                        message.backupProgressTotalBytes = progress.bytesTotal;
                        message.backupProgressPackage = progress.packageName;
                        message.backupProgressStageLabel = progress.stageLabel;
                        message.backupProgressAppIndex = backupIndex;
                        message.backupProgressAppFraction = appFraction;
                        postUiMessage(std::move(message));
                        return true;
                    };
                    if (client->restoreAppBackup(serial, backup, restoreOptions, out, progressCb)) {
                        int completed = restoredCount.fetch_add(1) + 1;
                        UiMessage completeMessage;
                        completeMessage.hasBackupProgress = true;
                        completeMessage.backupProgressActive = true;
                        completeMessage.backupProgressIsRestore = true;
                        completeMessage.backupProgressCurrent = completed;
                        completeMessage.backupProgressTotal = (int)backups.size();
                        completeMessage.backupProgressPackage = packageName;
                        completeMessage.backupProgressStageLabel = "App restored";
                        completeMessage.backupProgressAppIndex = backupIndex;
                        completeMessage.backupProgressAppFraction = 1.0f;
                        postUiMessage(std::move(completeMessage));
                        LOG_INFO("BackupManager", "Restore complete: " + backup);
                    } else {
                        std::lock_guard<std::mutex> lock(restoreResultMutex);
                        details += backup + ":\n" + out + "\n";
                        LOG_ERROR("BackupManager", "Restore failed: " + backup + ": " + out);
                    }
                }
            };
            std::vector<std::thread> restoreThreads;
            for (DeviceClient* worker : restoreWorkers) restoreThreads.emplace_back(restoreWorker, worker);
            for (auto& thread : restoreThreads) thread.join();
            ok += restoredCount.load();
            bool cancelled = m_backupCancelRequested.load();
            LOG_INFO("BackupManager", std::string(cancelled ? "Restore cancelled. ok=" : "Restore finished. ok=") +
                std::to_string(ok) + " total=" + std::to_string(requestedRestoreCount));
            UiMessage message;
            message.refreshAppList = true;
            message.hasStatus = true;
            message.status = cancelled ? "Restore cancelled" : "Restored " + std::to_string(ok) + " backup(s)";
            message.hasBackupProgress = true;
            message.backupProgressActive = false;
            message.backupProgressIsRestore = true;
            message.backupProgressCurrent = ok;
            message.backupProgressTotal = requestedRestoreCount;
            message.backupProgressStageLabel = "Restore finished";
            message.hasNotification = true;
            std::string restoreSummary = "Restored apps: " + std::to_string(ok) + " of " +
                std::to_string(requestedRestoreCount) + "\n\nRequested content\n";
            if (options.includeApk) restoreSummary += "APK installation\n";
            if (options.includeData) restoreSummary += "App data\n";
            if (options.includeDeviceProtectedData) restoreSummary += "Device protected data\n";
            if (options.includeExternalData) restoreSummary += "External data\n";
            if (options.includeObb) restoreSummary += "OBB data\n";
            if (options.includeMedia) restoreSummary += "Media data\n";
            if (cancelled) {
                message.notificationTitle = "Restore Cancelled";
                message.notificationMessage = "Restored " + std::to_string(ok) + " of " +
                    std::to_string(requestedRestoreCount) + " selected app(s) before stopping.";
                message.notificationIsError = false;
            } else if (ok == requestedRestoreCount) {
                message.notificationTitle = "Restore Complete";
                message.notificationMessage = restoreSummary + "\nAll selected apps were restored successfully.";
                message.notificationIsError = false;
            } else {
                message.notificationTitle = "Restore Finished With Errors";
                message.notificationMessage = restoreSummary + "\nIssues\n" +
                    (details.empty() ? "Some restores failed." : details);
                message.notificationIsError = true;
            }
            postUiMessage(std::move(message));
        } catch (const std::exception& e) {
            LOG_ERROR("BackupManager", std::string("Restore task exception: ") + e.what());
            UiMessage message;
            message.hasBackupProgress = true;
            message.backupProgressActive = false;
            message.backupProgressIsRestore = true;
            message.hasNotification = true;
            message.notificationTitle = "Restore Failed";
            message.notificationMessage = e.what();
            message.notificationIsError = true;
            postUiMessage(std::move(message));
        } catch (...) {
            LOG_ERROR("BackupManager", "Restore task unknown exception");
            UiMessage message;
            message.hasBackupProgress = true;
            message.backupProgressActive = false;
            message.backupProgressIsRestore = true;
            message.hasNotification = true;
            message.notificationTitle = "Restore Failed";
            message.notificationMessage = "Unexpected restore failure.";
            message.notificationIsError = true;
            postUiMessage(std::move(message));
        }
    });
}

void App::renderBackupManagerWindow() {
    if (m_backupManagerNeedsBackupRefresh) refreshBackupManagerBackups();
    if (m_backupManagerNeedsAppRefresh && !m_asyncBusy.load()) refreshBackupManagerApps();
    probeBackupManagerRootAccess();

    ui::PanelStyle theme(m_resolvedLightTheme);
    const float s = theme.s;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 12 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 9 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 8 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9 * s, 6 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8 * s, 5 * s));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.muted);
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, theme.raised);
    ImGui::PushStyleColor(ImGuiCol_TableRowBg, theme.background);
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, theme.surface);
    {
        ImVec4 selection = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        selection.w = m_resolvedLightTheme ? 0.22f : 0.15f;
        ImGui::PushStyleColor(ImGuiCol_Header, selection);
    }

    struct BackupOption { const char* label; bool* value; bool rootOnly = false; };
    BackupOption options[] = {
        {"APK", &m_backupIncludeApk}, {"App data", &m_backupIncludeData, true},
        {"Device protected", &m_backupIncludeDeData, true}, {"External", &m_backupIncludeExternalData},
        {"OBB", &m_backupIncludeObb}, {"Media", &m_backupIncludeMedia},
        {"Skip cache folders", &m_backupExcludeCache}, {"Show system apps", &m_backupShowSystemApps},
        {"Restore permissions", &m_backupRestorePermissions}, {"Allow downgrade", &m_backupAllowDowngrade}
    };
    const float toolbarWidth = ImGui::GetContentRegionAvail().x - 28 * s;
    float optionX = 0;
    int optionRows = 1;
    for (const auto& option : options) {
        float w = ImGui::CalcTextSize(option.label).x + ImGui::GetFrameHeight() + 34 * s;
        if (optionX > 0 && optionX + w > toolbarWidth) { ++optionRows; optionX = 0; }
        optionX += w;
    }
    const bool narrowToolbar = toolbarWidth < 980 * s;
    const float titleRowH = (narrowToolbar ? 82 : 46) * s;
    const float headerH = titleRowH + 60 * s + optionRows * (ImGui::GetFrameHeight() + 8 * s);
    ImGui::BeginChild("##BackupToolbar", ImVec2(0, headerH), ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 toolbar = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    theme.tile(ui::Icon::Backup, toolbar, 32, theme.blue);
    theme.label(ImVec2(toolbar.x + 46 * s, toolbar.y + 4 * s), "Backup Manager", 202 * s, theme.text, 20, true);
    if (theme.button("##BackupToFiles", "Back to File Manager", ui::Icon::Back,
        ImVec2(toolbar.x + 253 * s, toolbar.y), ImVec2(188 * s, 32 * s), theme.muted)) m_showBackupManager = false;
    std::string root = getBackupsRootPath();
    float storageX = narrowToolbar ? toolbar.x : toolbar.x + 458 * s;
    float storageY = toolbar.y + (narrowToolbar ? 44 * s : 0);
    float folderX = toolbar.x + width - 128 * s;
    theme.label(ImVec2(storageX, storageY + 8 * s), "Storage: " + root, folderX - storageX - 18 * s, theme.muted, 13);
    ImGui::SetCursorScreenPos(ImVec2(storageX, storageY));
    ImGui::InvisibleButton("##BackupStoragePath", ImVec2(std::max(1.0f, folderX - storageX - 18 * s), 32 * s));
    ui::tooltip(root.c_str());
    if (theme.button("##OpenBackupFolder", "Open Folder", ui::Icon::Folder,
        ImVec2(folderX, storageY), ImVec2(128 * s, 32 * s), theme.text)) {
        CreateDirectoryW(toWide(root).c_str(), nullptr);
        ShellExecuteW(nullptr, L"explore", toWide(root).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    float deviceY = toolbar.y + titleRowH;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(toolbar.x, deviceY - 6 * s), ImVec2(toolbar.x + width, deviceY - 6 * s), theme.color(theme.border));
    theme.label(ImVec2(toolbar.x, deviceY + (32 * s - 13 * s) * 0.5f - 1 * s), "Device", 48 * s, theme.muted, 13);
    auto slotLabel = [&](int slot) -> std::string {
        if (!deviceSession(slot).connected) return "No device";
        auto it = m_deviceDisplayNames.find(deviceSession(slot).serial);
        return it != m_deviceDisplayNames.end() ? it->second : deviceSession(slot).serial;
    };
    ImGui::SetCursorScreenPos(ImVec2(toolbar.x + 56 * s, deviceY));
    ImGui::SetNextItemWidth(240 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * s, std::max(2.0f, (32 * s - ImGui::GetFontSize()) * 0.5f)));
    const bool deviceComboOpen = ui::beginCombo("##BackupDevice", slotLabel(m_backupManagerSlot).c_str());
    ImGui::PopStyleVar();
    if (deviceComboOpen) {
        for (int slot = 0; slot < deviceSessionCount(); ++slot) {
            ImGui::BeginDisabled(!deviceSession(slot).connected);
            bool selected = m_backupManagerSlot == slot;
            std::string label = slotLabel(slot) + "##backupSlot" + std::to_string(slot);
            if (ImGui::Selectable(label.c_str(), selected)) {
                m_backupManagerSlot = slot;
                m_backupManagerApps.clear();
                m_backupManagerAppPage = 0;
                m_backupManagerAppSelectionAnchor = -1;
                m_backupManagerNeedsAppRefresh = true;
                m_backupRootAccess = BackupRootAccess::Unknown;
                m_backupRootAccessSerial.clear();
                m_showBackupRootChoice = false;
                refreshBackupManagerApps();
            }
            if (selected) ImGui::SetItemDefaultFocus();
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    if (theme.button("##RefreshBackups", "Refresh", ui::Icon::Refresh,
        ImVec2(toolbar.x + 310 * s, deviceY), ImVec2(104 * s, 32 * s), theme.text)) {
        m_backupManagerNeedsAppRefresh = true;
        m_backupManagerNeedsBackupRefresh = true;
    }
    bool hasDevice = deviceSession(m_backupManagerSlot).connected;
    bool backupRootEnabled = m_backupRootAccess == BackupRootAccess::Available;
    const char* capability = !hasDevice ? "Connect a device to check backup capabilities" :
        m_backupRootAccess == BackupRootAccess::Checking ? "Checking root access..." :
        backupRootEnabled ? "Root access available, full backup enabled" :
        m_backupRootAccess == BackupRootAccess::Unavailable ? "Standard backup, private app data unavailable" : "Checking backup capabilities...";
    theme.status(ImVec2(toolbar.x + 438 * s, deviceY + 8 * s), capability,
        backupRootEnabled, width - 438 * s);
    float optionsY = deviceY + 45 * s;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(toolbar.x, optionsY - 6 * s), ImVec2(toolbar.x + width, optionsY - 6 * s), theme.color(theme.border));
    optionX = 0;
    for (auto& option : options) {
        float w = ImGui::CalcTextSize(option.label).x + ImGui::GetFrameHeight() + 34 * s;
        if (optionX > 0 && optionX + w > width) { optionsY += ImGui::GetFrameHeight() + 10 * s; optionX = 0; }
        {
            const ImVec2 chipMin(toolbar.x + optionX, optionsY - 2 * s);
            const ImVec2 chipMax(toolbar.x + optionX + w - 10 * s, optionsY + ImGui::GetFrameHeight() + 2 * s);
            const bool on = *option.value && !(option.rootOnly && !backupRootEnabled);
            ImVec4 fill = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark); fill.w = on ? 0.09f : 0.0f;
            ImVec4 edge = on ? ImGui::GetStyleColorVec4(ImGuiCol_CheckMark) : theme.border; edge.w = on ? 0.35f : 0.8f;
            ImGui::GetWindowDrawList()->AddRectFilled(chipMin, chipMax, theme.color(fill), (chipMax.y - chipMin.y) * 0.5f);
            ImGui::GetWindowDrawList()->AddRect(chipMin, chipMax, theme.color(edge), (chipMax.y - chipMin.y) * 0.5f);
        }
        ImGui::SetCursorScreenPos(ImVec2(toolbar.x + optionX + 6 * s, optionsY));
        ImGui::BeginDisabled(option.rootOnly && !backupRootEnabled);
        if (ui::checkbox(option.label, option.value) && option.value == &m_backupShowSystemApps) {
            m_backupManagerAppPage = 0;
            if (!m_backupShowSystemApps)
                for (auto& row : m_backupManagerApps) if (row.app.isSystem) row.selected = false;
        }
        ImGui::EndDisabled();
        if (option.rootOnly && !backupRootEnabled) ui::tooltip("Root access is required for private app data.");
        optionX += w;
    }
    ImGui::EndChild();

    if (m_showBackupRootChoice) {
        if (!ImGui::IsPopupOpen("Backup capabilities")) {
            ImGui::OpenPopup("Backup capabilities");
            if (!m_backupRootChoicePositionInitialized) {
                ImGuiViewport* viewport = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                                viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                m_backupRootChoicePositionInitialized = true;
            }
        }
        m_showBackupRootChoice = false;
    }
    if (ui::beginDialog("Backup capabilities", 520)) {
        if (backupRootEnabled || !hasDevice) {
            ImGui::CloseCurrentPopup();
        } else {
            const bool checkingRoot = m_backupRootAccess == BackupRootAccess::Checking ||
                m_backupRootAccess == BackupRootAccess::Unknown;
            ui::dialogHeader(ui::Icon::Backup,
                ui::accentColor(checkingRoot ? ui::Accent::Info : ui::Accent::Warning),
                checkingRoot ? "Checking root access..." : "Root access was not detected",
                "Choose how you want to continue for this device.");
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted("Fast Enough can still back up APK files and shared app folders. "
                "Private app data and device-protected data require root access.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 2 * s));
            if (ui::choiceCard("##UseStandardBackup", ui::Icon::Backup, ui::accentColor(ui::Accent::Success),
                    "Use Standard Backup", "Back up APKs and shared folders without root.", 0, "No root needed")) {
                m_backupIncludeData = false;
                m_backupIncludeDeData = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::BeginDisabled(checkingRoot);
            if (ui::choiceCard("##CheckRootAgain", ui::Icon::Refresh, ui::accentColor(ui::Accent::Info),
                    "Check Root Again", "Approve the request in your root manager, then check again.")) {
                m_backupRootAccess = BackupRootAccess::Unknown;
                m_backupRootAccessSerial.clear();
            }
            ImGui::EndDisabled();
        }
        ui::endDialog();
    }

    bool anyApp = false;
    for (const auto& row : m_backupManagerApps)
        if (row.selected && (m_backupShowSystemApps || !row.app.isSystem)) { anyApp = true; break; }
    bool anyBackup = false;
    for (const auto& row : m_backupManagerBackups) if (row.selected) { anyBackup = true; break; }
    int selectedJobCount = 0;
    for (const auto& row : m_backupManagerBackups)
        if (row.selected && row.isJob) selectedJobCount++;

    bool showBackupProgress = m_backupProgressActive || (!m_backupProgressStageLabel.empty() && m_asyncBusy.load());
    const bool backupCardHasAppRow = m_backupProgressTotalBytes == 0 && !m_backupAppProgress.empty() &&
        m_backupProgressActiveAppIndex >= 0;
    const float backupCardHeight = (backupCardHasAppRow ? 182.0f : 142.0f) * s;
    float bottomBarH = showBackupProgress ? 46 * s + backupCardHeight + 12 * s : 52 * s;
    float availableY = ImGui::GetContentRegionAvail().y;
    float bodyH = availableY - bottomBarH - ImGui::GetStyle().ItemSpacing.y;
    if (bodyH < 80.0f) bodyH = 80.0f;
    float columnW = (ImGui::GetContentRegionAvail().x - 12 * s) * 0.54f;

    std::string filter(m_backupManagerSearch);
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
    std::string backupFilter(m_backupManagerBackupSearch);
    std::transform(backupFilter.begin(), backupFilter.end(), backupFilter.begin(), ::tolower);
    std::string jobAppFilter(m_backupManagerJobAppSearch);
    std::transform(jobAppFilter.begin(), jobAppFilter.end(), jobAppFilter.begin(), ::tolower);

    auto applyRowSelection = [](auto& rows, const std::vector<int>& visibleIndices,
                                int rowIndex, int& anchor) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift && anchor >= 0) {
            auto anchorPosition = std::find(visibleIndices.begin(), visibleIndices.end(), anchor);
            auto rowPosition = std::find(visibleIndices.begin(), visibleIndices.end(), rowIndex);
            if (anchorPosition != visibleIndices.end() && rowPosition != visibleIndices.end()) {
                if (!io.KeyCtrl)
                    for (auto& candidate : rows) candidate.selected = false;
                auto first = std::min(anchorPosition, rowPosition);
                auto last = std::max(anchorPosition, rowPosition);
                for (auto position = first; position <= last; ++position)
                    rows[*position].selected = true;
                return;
            }
        }
        if (io.KeyCtrl) {
            rows[rowIndex].selected = !rows[rowIndex].selected;
        } else {
            for (auto& candidate : rows) candidate.selected = false;
            rows[rowIndex].selected = true;
        }
        anchor = rowIndex;
    };

    auto appDisplayName = [&](const AppBackupRecord& record) -> std::string {
        if (!record.label.empty()) return record.label;
        for (const auto& installed : m_backupManagerApps)
            if (installed.app.packageName == record.packageName && !installed.app.appName.empty())
                return installed.app.appName;
        return record.packageName;
    };

    auto deleteSelectedBackupJobs = [this]() {
        std::vector<std::string> paths;
        for (const auto& row : m_backupManagerBackups)
            if (row.selected && row.isJob) paths.push_back(row.record.backupPath);
        if (paths.empty()) return;
        std::string prompt = "Permanently delete " + std::to_string(paths.size()) + " selected backup job";
        if (paths.size() != 1) prompt += "s";
        prompt += " and all files inside?";
        if (MessageBoxA(g_mainHwnd, prompt.c_str(), "Delete Backup Jobs", MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
        std::string jobsRoot = pathToUtf8(toFsPath(getBackupsRootPath()) / "Jobs");
        postAsync("Deleting backup jobs...", [this, paths, jobsRoot]() {
            std::error_code ec;
            std::filesystem::path allowed = std::filesystem::weakly_canonical(toFsPath(jobsRoot), ec);
            int deleted = 0;
            for (const auto& value : paths) {
                ec.clear();
                std::filesystem::path target = std::filesystem::weakly_canonical(toFsPath(value), ec);
                if (ec || _wcsicmp(target.parent_path().c_str(), allowed.c_str()) != 0) {
                    LOG_ERROR("BackupManager", "Refused to delete backup outside the Jobs folder: " + value);
                    continue;
                }
                uintmax_t removed = std::filesystem::remove_all(target, ec);
                if (!ec && removed > 0) deleted++;
                else if (ec) LOG_ERROR("BackupManager", "Could not delete backup job: " + value + ": " + ec.message());
            }
            UiMessage message;
            message.refreshBackupList = true;
            message.hasStatus = true;
            message.status = "Deleted " + std::to_string(deleted) + " backup job(s)";
            message.hasNotification = true;
            message.notificationTitle = deleted == (int)paths.size() ? "Backup Jobs Deleted" : "Some Jobs Could Not Be Deleted";
            message.notificationMessage = "Deleted " + std::to_string(deleted) + " of " + std::to_string(paths.size()) + " selected backup job(s).";
            message.notificationIsError = deleted != (int)paths.size();
            postUiMessage(std::move(message));
        });
    };

    auto link = [&](const char* title) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.blue);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.raised);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.raised);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4 * s, 4 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        bool clicked = ImGui::Button(title);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        return clicked;
    };
    auto paneHeading = [&](const std::string& title, const char* id, const char* hint, char* search, size_t capacity, int count = -1) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        bool stacked = w < 470 * s;
        float titleWidth = std::min(250 * s, w * .46f);
        theme.label(ImVec2(p.x + 2 * s, p.y + 8.5f * s), title, stacked ? w : titleWidth, theme.text, 19, true);
        if (count >= 0) {
            ImFont* titleFont = ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
            const float titleSpan = titleFont->CalcTextSizeA(19 * s, FLT_MAX, 0, title.c_str()).x;
            const std::string countText = std::to_string(count);
            const ImVec2 countSize = ImGui::CalcTextSize(countText.c_str());
            const ImVec2 badgeMin(p.x + 2 * s + titleSpan + 10 * s, p.y + 8.5f * s + (19 * s - 22 * s) * 0.5f + 1 * s);
            const ImVec2 badgeMax(badgeMin.x + countSize.x + 18 * s, badgeMin.y + 22 * s);
            if (badgeMax.x < p.x + (stacked ? w : titleWidth)) {
                ImGui::GetWindowDrawList()->AddRectFilled(badgeMin, badgeMax, theme.color(theme.raised), 11 * s);
                ImGui::GetWindowDrawList()->AddText(ImVec2(badgeMin.x + 9 * s, badgeMin.y + (22 * s - countSize.y) * 0.5f),
                    theme.color(theme.muted), countText.c_str());
            }
        }
        bool changed = theme.search(id, hint, search, capacity,
            ImVec2(stacked ? p.x : p.x + titleWidth, p.y + (stacked ? 36 * s : 0)), stacked ? w : w - titleWidth);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + (stacked ? 80 : 44) * s));
        return changed;
    };
    auto cellText = [&](const std::string& value, ImVec4 ink) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        theme.label(ImVec2(p.x, p.y + 3 * s), value, w, ink, 13);
        ImGui::Dummy(ImVec2(std::max(1.0f, w), 22 * s));
        if (ImGui::IsItemHovered() && ImGui::CalcTextSize(value.c_str()).x > w) ImGui::SetTooltip("%s", value.c_str());
    };
    auto selectionCheckbox = [&](const char* id, bool& selected) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3 * s);
        bool changed = ui::checkbox(id, &selected);
        ImGui::PopStyleVar();
        return changed;
    };
    auto restoreTarget = [&](const char* id, const char* title) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 size(ImGui::GetContentRegionAvail().x, 46 * s);
        bool pressed = theme.button(id, title, ui::Icon::Up, p, size, theme.blue);
        auto* draw = ImGui::GetWindowDrawList();
        for (float x = p.x + 6 * s; x < p.x + size.x - 6 * s; x += 7 * s) {
            float end = std::min(x + 3 * s, p.x + size.x - 6 * s);
            draw->AddLine(ImVec2(x, p.y), ImVec2(end, p.y), theme.color(theme.blue));
            draw->AddLine(ImVec2(x, p.y + size.y), ImVec2(end, p.y + size.y), theme.color(theme.blue));
        }
        for (float y = p.y + 6 * s; y < p.y + size.y - 6 * s; y += 7 * s) {
            float end = std::min(y + 3 * s, p.y + size.y - 6 * s);
            draw->AddLine(ImVec2(p.x, y), ImVec2(p.x, end), theme.color(theme.blue));
            draw->AddLine(ImVec2(p.x + size.x, y), ImVec2(p.x + size.x, end), theme.color(theme.blue));
        }
        return pressed;
    };
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoBordersInBody;
    float tableH = 0;
    ImGui::BeginChild("##BackupAppsPane", ImVec2(columnW, bodyH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    int appCount = 0;
    for (const auto& row : m_backupManagerApps) if (m_backupShowSystemApps || !row.app.isSystem) ++appCount;
    if (paneHeading("Installed Apps", "##BackupAppSearch", "Search apps...",
        m_backupManagerSearch, sizeof(m_backupManagerSearch), appCount)) {
        m_backupManagerAppPage = 0;
        filter = m_backupManagerSearch;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    }
    std::vector<int> visibleAppIndices;
    for (int i = 0; i < (int)m_backupManagerApps.size(); ++i) {
        const auto& row = m_backupManagerApps[i];
        if (row.app.isSystem && !m_backupShowSystemApps) continue;
        std::string hay = row.app.appName + " " + row.app.packageName + " " + row.app.apkPath;
        std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (filter.empty() || hay.find(filter) != std::string::npos) visibleAppIndices.push_back(i);
    }
    const int filteredCount = (int)visibleAppIndices.size();
    const int pageSize = m_backupManagerAppsPerPage > 0 ? m_backupManagerAppsPerPage : std::max(1, filteredCount);
    const int pageCount = std::max(1, (filteredCount + pageSize - 1) / pageSize);
    m_backupManagerAppPage = std::clamp(m_backupManagerAppPage, 0, pageCount - 1);
    int firstApp = m_backupManagerAppPage * pageSize;
    int lastApp = std::min(filteredCount, firstApp + pageSize);
    std::vector<int> pageIndices(visibleAppIndices.begin() + firstApp, visibleAppIndices.begin() + lastApp);
    if (link("Select Visible")) for (int i : pageIndices) m_backupManagerApps[i].selected = true;
    ImGui::SameLine();
    if (link("Select All")) setBackupManagerAppSelection(true, false);
    ImGui::SameLine();
    if (link("Clear")) setBackupManagerAppSelection(false, false);
    ImGui::SameLine();
    if (link("Invert Visible")) for (int i : pageIndices) m_backupManagerApps[i].selected = !m_backupManagerApps[i].selected;
    tableH = std::max(60 * s, ImGui::GetContentRegionAvail().y - 44 * s);
    ImVec2 tableOrigin = ImGui::GetCursorScreenPos();
    if (ImGui::BeginTable("##BackupAppsTable", 5, tableFlags, ImVec2(0, tableH))) {
        ImGui::TableSetupColumn("##Selected", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 24 * s);
        ImGui::TableSetupColumn("App Name", ImGuiTableColumnFlags_WidthStretch, .30f);
        ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch, .40f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 74 * s);
        ImGui::TableSetupColumn("APK Path", ImGuiTableColumnFlags_WidthStretch, .30f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGui::TableSetColumnIndex(0);
        bool allSelected = !pageIndices.empty() && std::all_of(pageIndices.begin(), pageIndices.end(), [&](int i) { return m_backupManagerApps[i].selected; });
        ImGui::BeginDisabled(pageIndices.empty());
        if (selectionCheckbox("##AllBackupApps", allSelected)) for (int i : pageIndices) m_backupManagerApps[i].selected = allSelected;
        ImGui::EndDisabled();
        for (int i : pageIndices) {
            auto& row = m_backupManagerApps[i];
            ImGui::PushID(i);
            ImGui::TableNextRow(0, 32 * s);
            ImGui::TableSetColumnIndex(1);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float nameW = ImGui::GetContentRegionAvail().x;
            if (ImGui::Selectable("##BackupAppRow", row.selected,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 22 * s)) &&
                ImGui::GetIO().MousePos.x >= p.x)
                applyRowSelection(m_backupManagerApps, visibleAppIndices, i, m_backupManagerAppSelectionAnchor);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !row.selected) {
                for (auto& candidate : m_backupManagerApps) candidate.selected = false;
                row.selected = true;
                m_backupManagerAppSelectionAnchor = i;
            }
            if (ImGui::BeginPopupContextItem("##BackupAppContext")) {
                ImGui::BeginDisabled(!hasDevice || m_asyncBusy.load());
                if (ImGui::MenuItem("Back Up Selected Apps")) startBackupManagerBackup();
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            std::string name = row.app.appName.empty() ? row.app.packageName : row.app.appName;
            {
                const ImVec4 tints[] = { theme.blue, theme.green, theme.gold, ui::rgb(196, 181, 253), ui::rgb(249, 168, 212) };
                size_t hash = 0;
                for (char c : row.app.packageName) hash = hash * 31 + (unsigned char)c;
                const ImVec4 tint = tints[hash % 5];
                ImVec4 tileFill = tint; tileFill.w = 0.16f;
                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 22 * s, p.y + 22 * s), theme.color(tileFill), 6 * s);
                char initial[2] = { name.empty() ? '?' : (char)std::toupper((unsigned char)name[0]), 0 };
                ImFont* initialFont = ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
                const ImVec2 initialSize = initialFont->CalcTextSizeA(12 * s, FLT_MAX, 0, initial);
                ImGui::GetWindowDrawList()->AddText(initialFont, 12 * s,
                    ImVec2(p.x + (22 * s - initialSize.x) * 0.5f, p.y + (22 * s - initialSize.y) * 0.5f), theme.color(tint), initial);
            }
            theme.label(ImVec2(p.x + 31 * s, p.y + 3 * s), name, nameW - 31 * s, theme.text, 13);
            if (ImGui::IsItemHovered() && ImGui::CalcTextSize(name.c_str()).x > nameW - 31 * s) ImGui::SetTooltip("%s", name.c_str());
            ImGui::TableSetColumnIndex(2);
            cellText(row.app.packageName, theme.muted);
            ImGui::TableSetColumnIndex(3);
            p = ImGui::GetCursorScreenPos();
            ImVec4 ink = row.app.isSystem ? theme.gold : theme.blue;
            const char* typeLabel = row.app.isSystem ? "System" : "User";
            const float pillWidth = std::min(ImGui::GetContentRegionAvail().x, 62 * s);
            theme.pill(ImVec2(p.x + (ImGui::GetContentRegionAvail().x - pillWidth) * 0.5f, p.y), ImVec2(pillWidth, 22 * s),
                ui::Icon::None, typeLabel, ink);
            ImGui::Dummy(ImVec2(pillWidth, 22 * s));
            ImGui::TableSetColumnIndex(4);
            cellText(row.app.apkPath, theme.muted);
            ImGui::TableSetColumnIndex(0);
            if (selectionCheckbox("##SelectApp", row.selected)) m_backupManagerAppSelectionAnchor = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (visibleAppIndices.empty()) {
        ImVec2 p(tableOrigin.x + 24 * s, tableOrigin.y + 66 * s);
        bool loading = hasDevice && (m_backupManagerNeedsAppRefresh || m_asyncBusy.load()) && m_backupManagerApps.empty();
        theme.label(p, !hasDevice ? "Connect a device to see its apps" : loading ? "Loading installed apps..." : "No apps match your search",
            columnW - 70 * s, theme.text, 17, true);
        theme.label(ImVec2(p.x, p.y + 31 * s), !hasDevice ? "Choose a phone from Connections to get started." : loading ?
            "Reading the app list from your phone." : "Try another app name or package.", columnW - 70 * s, theme.muted, 13);
    }
    ImVec2 pagination = ImGui::GetCursorScreenPos();
    float paneWidth = ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(m_backupManagerAppPage == 0);
    if (theme.button("##PreviousAppsPage", "", ui::Icon::Back, pagination, ImVec2(28 * s, 30 * s), theme.muted)) --m_backupManagerAppPage;
    ImGui::EndDisabled();
    float pageX = pagination.x + 34 * s;
    int startPage = std::max(0, std::min(m_backupManagerAppPage - 2, pageCount - 5));
    for (int page = startPage; page < std::min(pageCount, startPage + 5); ++page) {
        ImGui::PushID(page);
        if (theme.button("##AppsPage", std::to_string(page + 1).c_str(), ui::Icon::None,
            ImVec2(pageX, pagination.y), ImVec2(28 * s, 30 * s), page == m_backupManagerAppPage ? theme.blue : theme.muted,
            page == m_backupManagerAppPage ? 4 : 0)) m_backupManagerAppPage = page;
        ImGui::PopID();
        pageX += 33 * s;
    }
    ImGui::BeginDisabled(m_backupManagerAppPage + 1 >= pageCount);
    if (theme.button("##NextAppsPage", "", ui::Icon::Forward, ImVec2(pageX, pagination.y), ImVec2(28 * s, 30 * s), theme.muted)) ++m_backupManagerAppPage;
    ImGui::EndDisabled();
    float pageSizeX = pagination.x + paneWidth - 104 * s;
    std::string pageSummary = std::to_string(filteredCount == 0 ? 0 : firstApp + 1) + " to " + std::to_string(lastApp) + " of " + std::to_string(filteredCount) + " apps";
    float summaryX = std::max(pageX + 42 * s, pageSizeX - 170 * s);
    theme.label(ImVec2(summaryX, pagination.y + 8 * s), pageSummary, pageSizeX - summaryX - 10 * s, theme.muted, 12);
    ImGui::SetCursorScreenPos(ImVec2(pageSizeX, pagination.y));
    ImGui::SetNextItemWidth(104 * s);
    std::string pageSizeLabel = m_backupManagerAppsPerPage == 0 ? "All apps" : std::to_string(m_backupManagerAppsPerPage) + " / page";
    if (ui::beginCombo("##BackupPageSize", pageSizeLabel.c_str())) {
        for (int count : {15, 30, 50, 0}) {
            std::string label = count ? std::to_string(count) + " / page" : "All apps";
            if (ImGui::Selectable(label.c_str(), count == m_backupManagerAppsPerPage)) {
                m_backupManagerAppsPerPage = count;
                m_backupManagerAppPage = 0;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 12 * s);

    ImGui::BeginChild("##BackupRestorePane", ImVec2(0, bodyH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        if (m_backupManagerJobOpen) {
            paneHeading("Backup Job", "##BackupJobAppSearch", "Search app name or package...",
                m_backupManagerJobAppSearch, sizeof(m_backupManagerJobAppSearch));
            jobAppFilter = m_backupManagerJobAppSearch;
            std::transform(jobAppFilter.begin(), jobAppFilter.end(), jobAppFilter.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        } else {
            paneHeading("Saved Backups", "##BackupSearch", "Search jobs, apps, or packages...",
                m_backupManagerBackupSearch, sizeof(m_backupManagerBackupSearch));
            backupFilter = m_backupManagerBackupSearch;
            std::transform(backupFilter.begin(), backupFilter.end(), backupFilter.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        }
        if (m_backupManagerJobOpen) {
            std::vector<int> visibleJobAppIndices;
            for (int i = 0; i < (int)m_backupManagerJobApps.size(); ++i) {
                const auto& record = m_backupManagerJobApps[i].record;
                std::string hay = appDisplayName(record) + " " + record.packageName + " " +
                    record.created + " " + record.backupPath;
                std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
                if (jobAppFilter.empty() || hay.find(jobAppFilter) != std::string::npos)
                    visibleJobAppIndices.push_back(i);
            }
            if (link("Back to Jobs")) closeBackupManagerJob();
            ImGui::SameLine();
            if (link("Select Visible")) {
                for (int index : visibleJobAppIndices) m_backupManagerJobApps[index].selected = true;
            }
            ImGui::SameLine();
            if (link("Clear")) {
                for (auto& row : m_backupManagerJobApps) row.selected = false;
            }
            bool anyJobApp = false;
            for (const auto& row : m_backupManagerJobApps) if (row.selected) { anyJobApp = true; break; }
            ImGui::SameLine();
            ImGui::BeginDisabled(!hasDevice || m_asyncBusy.load() || m_backupManagerJobApps.empty());
            if (link("Restore Whole Job")) {
                std::vector<std::string> paths;
                for (const auto& row : m_backupManagerJobApps) paths.push_back(row.record.backupPath);
                startBackupManagerRestorePaths(std::move(paths));
            }
            ImGui::EndDisabled();
            bool restoreJobSelection = false;
            ImGui::BeginDisabled(!hasDevice || m_asyncBusy.load() || !anyJobApp);
            restoreJobSelection = restoreTarget("##RestoreJobTarget", "Restore Selected Apps  |  Drop Selection Here");
            ImGui::EndDisabled();
            if (ImGui::BeginDragDropTarget()) {
                if (ImGui::AcceptDragDropPayload("BM_JOB_APP_SELECTION") && hasDevice && !m_asyncBusy.load())
                    restoreJobSelection = true;
                ImGui::EndDragDropTarget();
            }
            if (restoreJobSelection) {
                std::vector<std::string> paths;
                for (const auto& row : m_backupManagerJobApps)
                    if (row.selected) paths.push_back(row.record.backupPath);
                if (!paths.empty()) startBackupManagerRestorePaths(std::move(paths));
            }

            tableH = ImGui::GetContentRegionAvail().y;
            if (tableH < 80.0f) tableH = 80.0f;
            if (ImGui::BeginTable("##BackupJobAppsTable", 6, tableFlags,
                    ImVec2(0, tableH))) {
                ImGui::TableSetupColumn("##Selected", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 24 * s);
                ImGui::TableSetupColumn("App Name", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch, 0.29f);
                ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, 145 * s);
                ImGui::TableSetupColumn("Contents", ImGuiTableColumnFlags_WidthStretch, 0.32f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 68 * s);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                ImGui::TableSetColumnIndex(0);
                bool allSelected = !visibleJobAppIndices.empty() && std::all_of(visibleJobAppIndices.begin(), visibleJobAppIndices.end(), [&](int i) { return m_backupManagerJobApps[i].selected; });
                ImGui::BeginDisabled(visibleJobAppIndices.empty());
                if (selectionCheckbox("##AllJobApps", allSelected)) for (int i : visibleJobAppIndices) m_backupManagerJobApps[i].selected = allSelected;
                ImGui::EndDisabled();
                for (int i : visibleJobAppIndices) {
                    auto& row = m_backupManagerJobApps[i];
                    std::string contents;
                    if (row.record.hasApk) contents += "APK ";
                    if (row.record.hasData) contents += "Data ";
                    if (row.record.hasDeviceProtectedData) contents += "DE ";
                    if (row.record.hasExternalData) contents += "Ext ";
                    if (row.record.hasObb) contents += "OBB ";
                    if (row.record.hasMedia) contents += "Media ";
                    ImGui::PushID(i);
                    ImGui::TableNextRow(0, 32 * s);
                    ImGui::TableSetColumnIndex(1);
                    ImVec2 rowPos = ImGui::GetCursorScreenPos();
                    float nameWidth = ImGui::GetContentRegionAvail().x;
                    std::string displayName = appDisplayName(row.record);
                    std::string id = displayName + "##bmjobapp" + std::to_string(i);
                    if (ImGui::Selectable(("##" + id).c_str(), row.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 22 * s)) && ImGui::GetIO().MousePos.x >= rowPos.x)
                        applyRowSelection(m_backupManagerJobApps, visibleJobAppIndices, i,
                            m_backupManagerJobAppSelectionAnchor);
                    theme.label(ImVec2(rowPos.x, rowPos.y + 3 * s), displayName, nameWidth, theme.text, 13);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !row.selected) {
                        for (auto& candidate : m_backupManagerJobApps) candidate.selected = false;
                        row.selected = true;
                        m_backupManagerJobAppSelectionAnchor = i;
                    }
                    if (ImGui::BeginPopupContextItem(("##BackupJobAppsContext" + std::to_string(i)).c_str())) {
                        int selectedCount = 0;
                        for (const auto& candidate : m_backupManagerJobApps)
                            if (candidate.selected) selectedCount++;
                        ImGui::TextDisabled("%d app%s selected", selectedCount, selectedCount == 1 ? "" : "s");
                        ImGui::Separator();
                        ImGui::BeginDisabled(!hasDevice || m_asyncBusy.load());
                        if (ImGui::MenuItem("Restore Selected Apps")) {
                            std::vector<std::string> paths;
                            for (const auto& candidate : m_backupManagerJobApps)
                                if (candidate.selected) paths.push_back(candidate.record.backupPath);
                            if (!paths.empty()) startBackupManagerRestorePaths(std::move(paths));
                        }
                        ImGui::EndDisabled();
                        ImGui::EndPopup();
                    }
                    if (ImGui::BeginDragDropSource()) {
                        if (!row.selected) {
                            for (auto& candidate : m_backupManagerJobApps) candidate.selected = false;
                            row.selected = true;
                            m_backupManagerJobAppSelectionAnchor = i;
                        }
                        int selectedCount = 0;
                        for (const auto& candidate : m_backupManagerJobApps)
                            if (candidate.selected) selectedCount++;
                        ImGui::SetDragDropPayload("BM_JOB_APP_SELECTION", &selectedCount, sizeof(selectedCount));
                        ImGui::Text("Restore %d selected app%s", selectedCount, selectedCount == 1 ? "" : "s");
                        ImGui::EndDragDropSource();
                    }
                    ImGui::TableNextColumn();
                    cellText(row.record.packageName, theme.muted);
                    ImGui::TableNextColumn();
                    std::string createdText = formatBackupCreated(row.record.created);
                    cellText(createdText, theme.muted);
                    ImGui::TableNextColumn();
                    cellText(contents, theme.muted);
                    ImGui::TableNextColumn();
                    cellText(formatSize(row.record.sizeBytes), theme.muted);
                    ImGui::TableSetColumnIndex(0);
                    if (selectionCheckbox("##SelectJobApps", row.selected)) m_backupManagerJobAppSelectionAnchor = i;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        } else {
            std::vector<int> visibleBackupIndices;
            for (int i = 0; i < (int)m_backupManagerBackups.size(); ++i) {
                const auto& row = m_backupManagerBackups[i];
                std::string hay = appDisplayName(row.record) + " " + row.record.packageName + " " +
                    row.record.created + " " + row.record.backupPath + " " + row.searchText;
                std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
                if (backupFilter.empty() || hay.find(backupFilter) != std::string::npos)
                    visibleBackupIndices.push_back(i);
            }
            if (link("Open Job")) {
                for (const auto& row : m_backupManagerBackups) {
                    if (!row.selected) continue;
                    openBackupManagerJob(row);
                    break;
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(selectedJobCount == 0 || m_asyncBusy.load());
            if (link("Delete Selected Jobs")) deleteSelectedBackupJobs();
            ImGui::EndDisabled();

            bool restoreBackupSelection = false;
            ImGui::BeginDisabled(!hasDevice || !anyBackup || m_asyncBusy.load());
            restoreBackupSelection = restoreTarget("##RestoreBackupsTarget", "Restore Selected Backups  |  Drop Selection Here");
            ImGui::EndDisabled();
            if (ImGui::BeginDragDropTarget()) {
                if (ImGui::AcceptDragDropPayload("BM_BACKUP_SELECTION") && hasDevice && !m_asyncBusy.load())
                    restoreBackupSelection = true;
                ImGui::EndDragDropTarget();
            }
            if (restoreBackupSelection) startBackupManagerRestore();

            tableH = ImGui::GetContentRegionAvail().y;
            if (tableH < 80.0f) tableH = 80.0f;
            if (ImGui::BeginTable("##BackupsTable", 5, tableFlags,
                    ImVec2(0, tableH))) {
                ImGui::TableSetupColumn("##Selected", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 24 * s);
                ImGui::TableSetupColumn("Backup", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, 145 * s);
                ImGui::TableSetupColumn("Contents", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 68 * s);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                ImGui::TableSetColumnIndex(0);
                bool allSelected = !visibleBackupIndices.empty() && std::all_of(visibleBackupIndices.begin(), visibleBackupIndices.end(), [&](int i) { return m_backupManagerBackups[i].selected; });
                ImGui::BeginDisabled(visibleBackupIndices.empty());
                if (selectionCheckbox("##AllSavedBackups", allSelected)) for (int i : visibleBackupIndices) m_backupManagerBackups[i].selected = allSelected;
                ImGui::EndDisabled();
                for (int i : visibleBackupIndices) {
                    auto& row = m_backupManagerBackups[i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow(0, 32 * s);
                    ImGui::TableSetColumnIndex(1);
                    ImVec2 rowPos = ImGui::GetCursorScreenPos();
                    float nameWidth = ImGui::GetContentRegionAvail().x;
                    std::string displayName = row.isJob ? row.record.packageName : appDisplayName(row.record);
                    std::string id = displayName + "##bmbak" + std::to_string(i);
                    if (ImGui::Selectable(("##" + id).c_str(), row.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 22 * s)) && ImGui::GetIO().MousePos.x >= rowPos.x)
                        applyRowSelection(m_backupManagerBackups, visibleBackupIndices, i,
                            m_backupManagerBackupSelectionAnchor);
                    theme.label(ImVec2(rowPos.x, rowPos.y + 3 * s), displayName, nameWidth, theme.text, 13);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !row.selected) {
                        for (auto& candidate : m_backupManagerBackups) candidate.selected = false;
                        row.selected = true;
                        m_backupManagerBackupSelectionAnchor = i;
                    }
                    if (ImGui::BeginPopupContextItem(("##BackupJobsContext" + std::to_string(i)).c_str())) {
                        int selectedCount = 0;
                        for (const auto& candidate : m_backupManagerBackups)
                            if (candidate.selected) selectedCount++;
                        ImGui::TextDisabled("%d backup%s selected", selectedCount, selectedCount == 1 ? "" : "s");
                        ImGui::Separator();
                        ImGui::BeginDisabled(m_asyncBusy.load());
                        if (ImGui::MenuItem("Open Job")) openBackupManagerJob(row);
                        ImGui::BeginDisabled(!hasDevice);
                        if (ImGui::MenuItem("Restore Selected Backups")) startBackupManagerRestore();
                        ImGui::EndDisabled();
                        if (ImGui::MenuItem("Delete Selected Jobs")) deleteSelectedBackupJobs();
                        ImGui::EndDisabled();
                        ImGui::EndPopup();
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        openBackupManagerJob(row);
                    }
                    if (ImGui::BeginDragDropSource()) {
                        if (!row.selected) {
                            for (auto& candidate : m_backupManagerBackups) candidate.selected = false;
                            row.selected = true;
                            m_backupManagerBackupSelectionAnchor = i;
                        }
                        int selectedCount = 0;
                        for (const auto& candidate : m_backupManagerBackups)
                            if (candidate.selected) selectedCount++;
                        ImGui::SetDragDropPayload("BM_BACKUP_SELECTION", &selectedCount, sizeof(selectedCount));
                        ImGui::Text("Restore %d selected backup%s", selectedCount,
                            selectedCount == 1 ? "" : "s");
                        ImGui::EndDragDropSource();
                    }
                    ImGui::TableNextColumn();
                    std::string createdText = formatBackupCreated(row.record.created);
                    cellText(createdText, theme.muted);
                    ImGui::TableNextColumn();
                    cellText(row.contentsSummary, theme.muted);
                    ImGui::TableNextColumn();
                    cellText(formatSize(row.record.sizeBytes), theme.muted);
                    ImGui::TableSetColumnIndex(0);
                    if (selectionCheckbox("##SelectSavedBackups", row.selected)) m_backupManagerBackupSelectionAnchor = i;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    ImGui::EndChild();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 6 * s));
    ImGui::BeginChild("##BackupManagerBottomBar", ImVec2(0, bottomBarH), 0, ImGuiWindowFlags_NoScrollbar);
    ImVec2 actions = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!hasDevice || !anyApp || m_asyncBusy.load());
    if (theme.button("##BackupSelectedAction", "Back Up Selected Apps", ui::Icon::Backup,
        actions, ImVec2(218 * s, 34 * s), ui::rgb(255, 255, 255), 1)) startBackupManagerBackup();
    ImGui::EndDisabled();
    bool hasRestoreSelection = anyBackup;
    if (m_backupManagerJobOpen) {
        hasRestoreSelection = std::any_of(m_backupManagerJobApps.begin(), m_backupManagerJobApps.end(),
            [](const auto& row) { return row.selected; });
    }
    ImGui::BeginDisabled(!hasDevice || !hasRestoreSelection || m_asyncBusy.load());
    if (theme.button("##RestoreSelectedAction", m_backupManagerJobOpen ? "Restore Selected Apps" : "Restore Selected Backups", ui::Icon::Refresh,
        ImVec2(actions.x + 230 * s, actions.y), ImVec2(230 * s, 34 * s), theme.muted)) {
        if (m_backupManagerJobOpen) {
            std::vector<std::string> paths;
            for (const auto& row : m_backupManagerJobApps) if (row.selected) paths.push_back(row.record.backupPath);
            startBackupManagerRestorePaths(std::move(paths));
        } else startBackupManagerRestore();
    }
    ImGui::EndDisabled();
    if (m_backupProgressActive) {
        const char* operation = m_backupProgressIsRestore ? "Restore" : "Backup";
        bool paused = m_backupPauseRequested.load();
        std::string pauseLabel = paused ? "Resume " : "Pause ";
        pauseLabel += operation;
        if (theme.button("##BackupPauseAction", pauseLabel.c_str(), paused ? ui::Icon::Play : ui::Icon::Pause,
            ImVec2(actions.x + 476 * s, actions.y), ImVec2(150 * s, 34 * s), theme.text)) {
            m_backupPauseRequested = !paused;
            m_backupProgressStageLabel = paused ? std::string("Resuming ") + operation :
                std::string(operation) + " paused";
            m_backupControlCV.notify_all();
        }
        ImGui::BeginDisabled(m_backupCancelRequested.load());
        std::string stopLabel = "Stop ";
        stopLabel += operation;
        if (theme.button("##BackupStopAction", stopLabel.c_str(), ui::Icon::Close,
            ImVec2(actions.x + 636 * s, actions.y), ImVec2(136 * s, 34 * s), theme.red, 3)) {
            m_backupCancelRequested = true;
            m_backupPauseRequested = false;
            m_backupProgressStageLabel = std::string("Stopping ") + operation;
            m_backupControlCV.notify_all();
        }
        ImGui::EndDisabled();
    }

    std::string statusText;
    if (m_asyncBusy.load()) {
        statusText = m_asyncStatus;
    } else {
        auto elapsed = std::chrono::steady_clock::now() - m_statusTime;
        if (!m_statusMessage.empty() && elapsed < std::chrono::seconds(15))
            statusText = m_statusMessage;
        else if (hasDevice)
            statusText = "Backup capability ready";
        else
            statusText = "No device connected";
    }
    float usedWidth = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    float statusW = std::min(ImGui::CalcTextSize(statusText.c_str()).x + 24 * s,
        ImGui::GetWindowWidth() - usedWidth - 24 * s);
    theme.status(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - statusW - 6 * s, actions.y + 9 * s),
        statusText, hasDevice, statusW);
    ImGui::SetCursorScreenPos(ImVec2(actions.x, actions.y + 46 * s));

    if (showBackupProgress) {
        const bool restore = m_backupProgressIsRestore;
        const bool paused = m_backupPauseRequested.load();
        const bool archiveTransfer = m_backupProgressTotalBytes > 0;
        const bool perAppView = !archiveTransfer && !m_backupAppProgress.empty();

        float smoothing = std::clamp(ImGui::GetIO().DeltaTime * 7.0f, 0.0f, 1.0f);
        float overall = 0.0f;
        int appIndex = -1;
        int appsDone = 0;
        int appsTotal = std::max(1, m_backupProgressTotal);
        std::string title;
        std::string subtitle;

        if (archiveTransfer) {
            overall = std::clamp((float)m_backupProgressBytes / (float)m_backupProgressTotalBytes, 0.0f, 1.0f);
            title = restore ? "Uploading restore archive" : "Downloading backup archive";
            subtitle = restore ? "Sending your backup to the phone" : "Saving the device archive to this PC";
        } else if (perAppView) {
            if (m_backupDisplayedAppProgress.size() != m_backupAppProgress.size())
                m_backupDisplayedAppProgress.assign(m_backupAppProgress.size(), 0.0f);
            float targetOverall = 0.0f;
            for (size_t i = 0; i < m_backupAppProgress.size(); ++i) {
                m_backupDisplayedAppProgress[i] +=
                    (m_backupAppProgress[i] - m_backupDisplayedAppProgress[i]) * smoothing;
                targetOverall += m_backupAppProgress[i];
                if (m_backupAppProgress[i] >= 0.999f) ++appsDone;
            }
            targetOverall /= (float)m_backupAppProgress.size();
            m_backupDisplayedOverallProgress +=
                (targetOverall - m_backupDisplayedOverallProgress) * smoothing;
            overall = m_backupDisplayedOverallProgress;
            appsTotal = (int)m_backupAppProgress.size();
            if (!restore) appsDone = std::clamp(m_backupProgressCurrent, 0, appsTotal);

            appIndex = m_backupProgressActiveAppIndex;
            if (appIndex < 0 || appIndex >= (int)m_backupAppProgress.size() || !m_backupAppActive[appIndex])
                appIndex = -1;
            if (appIndex < 0) {
                for (int i = (int)m_backupAppProgress.size() - 1; i >= 0; --i)
                    if (m_backupAppActive[i]) { appIndex = i; break; }
            }
            if (appIndex >= (int)m_backupProgressPackages.size()) appIndex = -1;
            title = restore ? "Restoring apps" : "Backing up apps";
            subtitle = m_backupProgressStageLabel;
        } else {
            int stageTotal = std::max(1, m_backupProgressStageTotal);
            float stageFraction = (float)std::clamp(m_backupProgressStage, 0, stageTotal) / (float)stageTotal;
            overall = std::clamp(((float)std::clamp(m_backupProgressCurrent, 0, appsTotal) +
                std::clamp(stageFraction, 0.0f, 1.0f)) / (float)appsTotal, 0.0f, 1.0f);
            appsDone = std::clamp(m_backupProgressCurrent, 0, appsTotal);
            title = std::string(restore ? "Restoring " : "Backing up ") +
                std::to_string(std::min(m_backupProgressCurrent + 1, appsTotal)) + " of " + std::to_string(appsTotal);
            subtitle = m_backupProgressPackage.empty() ? m_backupProgressStageLabel
                : m_backupProgressPackage + (m_backupProgressStageLabel.empty() ? "" : "  |  " + m_backupProgressStageLabel);
        }
        if (paused) subtitle = std::string(restore ? "Restore" : "Backup") + " paused";
        if (subtitle.empty()) subtitle = "Preparing...";

        const bool finished = overall >= 0.999f;
        const ImVec4 trackFrom = paused ? theme.gold : finished ? theme.green : m_theme.gradientPrimary;
        const ImVec4 trackTo = paused ? ui::mix(theme.gold, ui::rgb(255, 140, 40), 0.4f)
            : finished ? ui::mix(theme.green, theme.blue, 0.35f) : m_theme.gradientSecondary;
        const ImVec4 accent = paused ? theme.gold : finished ? theme.green : theme.blue;

        const ImVec2 cardPos = ImGui::GetCursorScreenPos();
        const float cardW = std::max(240 * s, ImGui::GetContentRegionAvail().x - 2 * s);
        const float pad = 16 * s;
        const float cardH = backupCardHeight;
        theme.box(cardPos, ImVec2(cardW, cardH), theme.raised, theme.surface, ui::withAlpha(accent, 0.45f), 12);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float innerW = cardW - pad * 2;
        float y = cardPos.y + pad;
        const float x = cardPos.x + pad;

        // Header: icon tile, title and subtitle, large percentage on the right.
        const float tileSize = 40 * s;
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + tileSize, y + tileSize), theme.color(ui::withAlpha(accent, 0.15f)), 10 * s);
        ui::icon(archiveTransfer ? (restore ? ui::Icon::Up : ui::Icon::Down) : (restore ? ui::Icon::Refresh : ui::Icon::Backup),
            ImVec2(x + 9 * s, y + 9 * s), 22 * s, theme.color(accent));
        char percentText[16];
        snprintf(percentText, sizeof(percentText), "%.0f%%", overall * 100.0f);
        ImFont* percentFont = ui::connectionHeadingFont ? ui::connectionHeadingFont : ImGui::GetFont();
        const float percentSize = 26 * s;
        const float percentW = percentFont->CalcTextSizeA(percentSize, FLT_MAX, 0, percentText).x;
        draw->AddText(percentFont, percentSize, ImVec2(x + innerW - percentW, y + (tileSize - percentSize) * 0.5f),
            theme.color(theme.text), percentText);
        const float textX = x + tileSize + 14 * s;
        const float textW = innerW - tileSize - 14 * s - percentW - 16 * s;
        theme.label(ImVec2(textX, y + 1 * s), title, textW, theme.text, 15, true);
        theme.label(ImVec2(textX, y + 22 * s), subtitle, textW, theme.muted, 13);
        y += tileSize + 14 * s;

        ui::progressTrack(ImVec2(x, y), innerW, 8 * s, overall, trackFrom, trackTo);
        y += 8 * s + 16 * s;

        if (backupCardHasAppRow && appIndex >= 0) {
            const float appFraction = std::clamp(m_backupDisplayedAppProgress[appIndex], 0.0f, 1.0f);
            char appPercent[16];
            snprintf(appPercent, sizeof(appPercent), "%.0f%%", appFraction * 100.0f);
            const float appPercentW = ImGui::CalcTextSize(appPercent).x;
            theme.label(ImVec2(x, y), "CURRENT APP", 110 * s, theme.muted, 11, true);
            theme.label(ImVec2(x + 110 * s, y - 1 * s), m_backupProgressPackages[appIndex],
                innerW - 110 * s - appPercentW - 12 * s, theme.text, 13, true);
            draw->AddText(ImVec2(x + innerW - appPercentW, y - 1 * s), theme.color(theme.muted), appPercent);
            y += 20 * s;
            ui::progressTrack(ImVec2(x, y), innerW, 4 * s, appFraction, ui::withAlpha(trackFrom, 0.85f), ui::withAlpha(trackTo, 0.85f));
            y += 4 * s + 16 * s;
        }

        // Stats row.
        draw->AddLine(ImVec2(x, y - 6 * s), ImVec2(x + innerW, y - 6 * s), theme.color(ui::withAlpha(theme.border, 0.7f)), 1.0f);
        y += 4 * s;
        const bool rateFresh = m_backupProgressLastTransferUpdate.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() - m_backupProgressLastTransferUpdate < std::chrono::seconds(2) && !paused;
        struct Stat { const char* label; std::string value; ImVec4 ink; };
        std::vector<Stat> stats;
        if (archiveTransfer || m_backupProgressBytes > 0) {
            std::string moved = formatSize(m_backupProgressBytes);
            if (m_backupProgressTotalBytes > 0) moved += " of " + formatSize(m_backupProgressTotalBytes);
            stats.push_back({"TRANSFERRED", moved, theme.text});
            stats.push_back({"SPEED", rateFresh && m_backupProgressBytesPerSecond > 0.0
                ? formatSpeed(m_backupProgressBytesPerSecond) : std::string(paused ? "Paused" : "Measuring..."), theme.green});
            stats.push_back({"TIME LEFT", rateFresh && m_backupProgressEtaSeconds > 0.5
                ? formatETA(m_backupProgressEtaSeconds) : std::string(finished ? "Done" : "Estimating..."), theme.gold});
        } else {
            stats.push_back({"APPS", std::to_string(appsDone) + " of " + std::to_string(appsTotal) + " done", theme.text});
            stats.push_back({"STAGE", m_backupProgressStageLabel.empty() ? std::string("Working") : m_backupProgressStageLabel, theme.text});
        }
        const bool showPipes = archiveTransfer && (m_backupProgressUsbPipes + m_backupProgressWifiPipes) > 0;
        const int columns = (int)stats.size() + (showPipes ? 1 : 0);
        const float columnW = innerW / (float)std::max(1, columns);
        for (size_t i = 0; i < stats.size(); ++i) {
            const float cx = x + columnW * (float)i;
            theme.label(ImVec2(cx, y), stats[i].label, columnW - 12 * s, theme.muted, 11, true);
            theme.label(ImVec2(cx, y + 17 * s), stats[i].value, columnW - 12 * s, stats[i].ink, 14, true);
        }
        if (showPipes) {
            const float cx = x + columnW * (float)stats.size();
            theme.label(ImVec2(cx, y), "CONNECTION", columnW, theme.muted, 11, true);
            float px = cx;
            auto pipePill = [&](ui::Icon glyph, const char* name, int count) {
                const bool active = count > 0;
                const ImVec4 ink = active ? theme.blue : theme.muted;
                const std::string text = std::string(name) + " x" + std::to_string(count);
                const float textWidth = ImGui::CalcTextSize(text.c_str()).x;
                const float pillW = 14 * s + 6 * s + textWidth + 18 * s;
                const ImVec2 pillMin(px, y + 15 * s);
                const ImVec2 pillMax(px + pillW, y + 15 * s + 22 * s);
                draw->AddRectFilled(pillMin, pillMax, theme.color(ui::withAlpha(ink, active ? 0.14f : 0.08f)), 11 * s);
                draw->AddRect(pillMin, pillMax, theme.color(ui::withAlpha(ink, active ? 0.45f : 0.25f)), 11 * s, 0, 1.0f);
                ui::icon(glyph, ImVec2(px + 9 * s, pillMin.y + 4 * s), 14 * s, theme.color(ink));
                draw->AddText(ImVec2(px + 9 * s + 14 * s + 6 * s, pillMin.y + (22 * s - ImGui::GetTextLineHeight()) * 0.5f),
                    theme.color(ink), text.c_str());
                px += pillW + 6 * s;
            };
            pipePill(ui::Icon::Usb, "USB", m_backupProgressUsbPipes);
            pipePill(ui::Icon::Wifi, "WiFi", m_backupProgressWifiPipes);
        }
        ImGui::Dummy(ImVec2(cardW, cardH));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(5);

}

void App::requestMcrawMount(FilePanel& panel, const std::string& path) {
    FilePanel* target = &panel;
    const auto generation = panel.listingGeneration;
    postAsync("Opening MotionCam frames...", [this, target, path, generation] {
        UiMessage message;
        message.mountedPanel = target;
        message.mountedGeneration = generation;
        message.mountedSource = path;
        message.mountedPath = McrawMountManager::instance().mountMcraw(path);
        if (message.mountedPath.empty()) {
            message.hasStatus = true;
            message.status = "Could not open MotionCam frames";
        }
        postUiMessage(std::move(message));
    });
}

void App::openAndroidFile(FilePanel& panel, int index) {
    if (!panel.isAndroid || !panel.validIndex(index)) return;
    if (panel.entryIsDir(index)) return;

    std::string name = panel.entryName(index);
    std::string remotePath = panel.currentPath;
    appendPathSeparator(remotePath, '/');
    remotePath += name;

    if (m_openFile.active.load()) {
        m_statusMessage = "Already opening a file. Wait for it to finish or cancel it first.";
        m_statusTime = std::chrono::steady_clock::now();
        return;
    }
    if (m_openFileThread.joinable()) m_openFileThread.join();

    m_statusMessage = "Opening: " + name + "...";
    m_statusTime = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_openFile.mutex);
        m_openFile.name = name;
        m_openFile.detail.clear();
        m_openFile.started = std::chrono::steady_clock::now();
        m_openFile.finished = {};
    }
    m_openFile.bytes = 0;
    m_openFile.total = panel.entrySize(index);
    m_openFile.bytesPerSecond = 0.0;
    m_openFile.etaSeconds = -1.0;
    m_openFile.cancelRequested = false;
    m_openFile.cancellable = false;
    m_openFile.outcome = OpenFileRunning;
    m_openFile.active = true;
    m_openFileDismissed = false;

    int slot = panel.deviceSlot;
    m_openFileThread = std::thread([this, remotePath, name, slot]() {
        char tempBuf[MAX_PATH];
        GetTempPathA(MAX_PATH, tempBuf);
        std::string tempDir = std::string(tempBuf) + "FastEnough\\";
        CreateDirectoryA(tempDir.c_str(), nullptr);
        std::string localPath = tempDir + name;
        uint64_t outSize = 0;

        // A dedicated connection keeps browsing responsive and lets the download be
        // cancelled without dropping the main session.
        DeviceClient& base = deviceForSlot(slot);
        DeviceClient dedicated;
        DeviceClient* client = &base;
        {
            const std::string host = base.connectedHost();
            const int port = host == "127.0.0.1" ? base.localPort() : base.serverPort();
            // This connection only borrows the running helper. Without this, destroying it
            // would stop the helper and remove the port forward for the whole session.
            dedicated.setOwnsServer(false);
            dedicated.setAdbPath(base.getAdbPath());
            dedicated.setServerPort(base.serverPort());
            dedicated.setLocalPort(base.localPort());
            if (!host.empty() && dedicated.connectTcp(host, port) && dedicated.verifyConnection()) {
                client = &dedicated;
                m_openFile.cancellable = true;
            } else {
                dedicated.disconnectTcp();
                LOG_WARN("UI", "Opening over the main connection, cancel is unavailable");
            }
        }

        TransferRateEstimator rate;
        const bool cancellable = client == &dedicated;
        auto progress = [&](uint64_t done, uint64_t total) {
            m_openFile.bytes = done;
            if (total) m_openFile.total = total;
            const TransferRateSample sample = rate.update(done, total);
            m_openFile.bytesPerSecond = sample.bytesPerSecond;
            m_openFile.etaSeconds = sample.etaSeconds;
            return !cancellable || !m_openFile.cancelRequested.load();
        };

        LOG_INFO("UI", "Pulling to temp: " + remotePath + " -> " + localPath);
        const bool ok = client->pullFile(remotePath, localPath, outSize, progress);
        const std::string pullError = client->lastError();
        if (cancellable) dedicated.disconnectTcp();

        int outcome = OpenFileFailed;
        std::string detail;
        if (cancellable && m_openFile.cancelRequested.load()) {
            outcome = OpenFileCancelled;
            std::error_code error;
            std::filesystem::remove(toFsPath(localPath), error);
            LOG_INFO("UI", "Open cancelled: " + remotePath);
        } else if (ok) {
            LOG_INFO("UI", "Opening: " + localPath);
            const std::wstring wideLocalPath = toWide(localPath);
            HINSTANCE launchResult = ShellExecuteW(
                nullptr, L"open", wideLocalPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(launchResult) > 32) {
                outcome = OpenFileOpened;
            } else {
                LOG_ERROR("UI", "Failed to launch file: " + localPath +
                    " (ShellExecute error " +
                    std::to_string(reinterpret_cast<INT_PTR>(launchResult)) + ")");
                detail = "Windows has no app set up to open this file type.";
            }
        } else {
            LOG_ERROR("UI", "Failed to pull file: " + remotePath + ": " + pullError);
            detail = pullError.empty() ? "The file could not be downloaded from the phone." : pullError;
        }
        {
            std::lock_guard<std::mutex> lock(m_openFile.mutex);
            m_openFile.detail = detail;
            m_openFile.finished = std::chrono::steady_clock::now();
        }
        m_openFile.outcome = outcome;
        m_openFile.active = false;
    });
}

void App::renderOpenFileCard() {
    const bool active = m_openFile.active.load();
    const int outcome = m_openFile.outcome.load();
    std::string name, detail;
    std::chrono::steady_clock::time_point started, finished;
    {
        std::lock_guard<std::mutex> lock(m_openFile.mutex);
        name = m_openFile.name;
        detail = m_openFile.detail;
        started = m_openFile.started;
        finished = m_openFile.finished;
    }
    if (name.empty() || m_openFileDismissed) return;
    const auto now = std::chrono::steady_clock::now();
    float alpha = 1.0f;
    if (active) {
        // Skip the card for files that finish almost instantly.
        if (now - started < std::chrono::milliseconds(150)) return;
    } else {
        const float shown = outcome == OpenFileFailed ? 6.0f : 2.2f;
        const float since = (float)std::chrono::duration<double>(now - finished).count();
        if (finished.time_since_epoch().count() == 0 || since > shown) return;
        if (outcome == OpenFileOpened && std::chrono::duration<double>(finished - started).count() < 0.4) return;
        alpha = std::clamp((shown - since) / 0.35f, 0.0f, 1.0f);
    }

    const float s = ui::scale();
    ui::PanelStyle theme(m_resolvedLightTheme);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size(std::min(440 * s, viewport->WorkSize.x - 32 * s), 112 * s);
    const float statusHeight = std::max(36 * s, ImGui::GetFontSize() + 18 * s);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - size.x - 20 * s,
        viewport->WorkPos.y + viewport->WorkSize.y - statusHeight - size.y - 14 * s));
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##OpenFileCard", nullptr, flags)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec4 accent = outcome == OpenFileFailed ? theme.red : outcome == OpenFileCancelled ? theme.muted :
            outcome == OpenFileOpened ? theme.green : ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        ImVec4 shadow(0, 0, 0, m_resolvedLightTheme ? 0.12f : 0.40f);
        draw->AddRectFilled(ImVec2(p.x + 4 * s, p.y + 8 * s), ImVec2(p.x + size.x + 2 * s, p.y + size.y + 10 * s), theme.color(shadow), 16 * s);
        ImVec4 edge = accent; edge.w = 0.55f;
        const ImVec4 body = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        theme.box(p, size, ui::shade(body, m_resolvedLightTheme ? 0.0f : 0.05f), body, edge, 14);

        const float pad = 16 * s;
        const float tile = 40 * s;
        const ui::Icon glyph = outcome == OpenFileOpened ? ui::Icon::Check : outcome == OpenFileFailed ? ui::Icon::Close :
            outcome == OpenFileCancelled ? ui::Icon::Close : ui::Icon::Down;
        theme.tile(glyph, ImVec2(p.x + pad, p.y + pad), tile / theme.s, accent);

        const char* verb = outcome == OpenFileOpened ? "Opened" : outcome == OpenFileFailed ? "Could not open" :
            outcome == OpenFileCancelled ? "Cancelled" : "Opening";
        const bool showCancel = active && m_openFile.cancellable.load();
        const float actionWidth = showCancel ? 92 * s : 30 * s;
        const float textX = p.x + pad + tile + 14 * s;
        const float textWidth = p.x + size.x - pad - actionWidth - 10 * s - textX;
        theme.label(ImVec2(textX, p.y + pad + 1 * s), std::string(verb) + " " + name, textWidth, theme.text, 14, true);

        const uint64_t done = m_openFile.bytes.load();
        const uint64_t total = m_openFile.total.load();
        std::string subtitle;
        if (active) {
            subtitle = total > 0 ? formatSize(done) + " of " + formatSize(total) : formatSize(done) + " downloaded";
            const double speed = m_openFile.bytesPerSecond.load();
            if (speed > 0) subtitle += "  |  " + formatSpeed(speed);
            const double eta = m_openFile.etaSeconds.load();
            if (eta > 0.5) subtitle += "  |  " + formatETA(eta) + " left";
        } else if (outcome == OpenFileOpened) {
            subtitle = "Opened with the default Windows app";
        } else if (outcome == OpenFileCancelled) {
            subtitle = "The download was stopped";
        } else {
            subtitle = detail;
        }
        theme.label(ImVec2(textX, p.y + pad + 24 * s), subtitle, textWidth, outcome == OpenFileFailed ? theme.red : theme.muted, 12);

        // Progress track across the bottom of the card.
        const ImVec2 trackPos(p.x + pad, p.y + size.y - pad - 6 * s);
        const float trackWidth = size.x - pad * 2;
        if (active && total == 0) {
            draw->AddRectFilled(trackPos, ImVec2(trackPos.x + trackWidth, trackPos.y + 6 * s),
                theme.color(m_resolvedLightTheme ? ui::rgb(222, 230, 241) : ui::rgb(24, 35, 51)), 3 * s);
            const float sweep = std::fmod((float)ImGui::GetTime() * 0.8f, 1.0f);
            const float segment = trackWidth * 0.28f;
            const float x0 = trackPos.x + (trackWidth + segment) * sweep - segment;
            draw->PushClipRect(trackPos, ImVec2(trackPos.x + trackWidth, trackPos.y + 6 * s), true);
            draw->AddRectFilled(ImVec2(x0, trackPos.y), ImVec2(x0 + segment, trackPos.y + 6 * s), theme.color(accent), 3 * s);
            draw->PopClipRect();
        } else {
            const float fraction = active ? (total > 0 ? (float)((double)done / (double)total) : 0.0f) : 1.0f;
            const ImVec4 to = active ? m_theme.gradientSecondary : accent;
            ui::progressTrack(trackPos, trackWidth, 6 * s, fraction, accent, to);
        }
        if (active && total > 0) {
            char percent[16];
            snprintf(percent, sizeof(percent), "%.0f%%", 100.0 * (double)done / (double)total);
            const float percentWidth = ImGui::CalcTextSize(percent).x;
            draw->AddText(ImVec2(p.x + size.x - pad - percentWidth, trackPos.y - ImGui::GetTextLineHeight() - 6 * s),
                theme.color(theme.text), percent);
        }

        ImGui::SetCursorScreenPos(ImVec2(p.x + size.x - pad - actionWidth, p.y + pad + 4 * s));
        if (showCancel) {
            ImGui::BeginDisabled(m_openFile.cancelRequested.load());
            if (ui::button(m_openFile.cancelRequested.load() ? "Stopping##OpenFileCancel" : "Cancel##OpenFileCancel",
                    ui::Icon::None, ui::Appearance::Secondary, ImVec2(actionWidth, 30 * s)))
                m_openFile.cancelRequested = true;
            ImGui::EndDisabled();
        } else if (!active) {
            if (ui::button("##OpenFileDismiss", ui::Icon::Close, ui::Appearance::Quiet, ImVec2(30 * s, 30 * s), "Dismiss"))
                m_openFileDismissed = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void App::renderCopyMoveDialog() {
    if (!m_showCopyMoveDialog || !m_pendingBatch) return;

    ImGui::OpenPopup("Copy or Move?");
    if (!ui::beginDialog("Copy or Move?", 480)) return;

    const int fileCount = (int)m_pendingBatch->files.size();
    const std::string selection = fileCount == 1 ? m_pendingBatch->files[0].displayName
        : std::to_string(fileCount) + " files selected";
    const bool cancel = ui::dialogHeader(ui::Icon::Transfer, ui::accentColor(ui::Accent::Info),
        "Copy or move?", selection, true);

    // Moves within one drive are a rename, so they finish instantly.
    bool sameDrive = false;
    if (m_pendingBatch->isLocalCopy && fileCount > 0) {
        auto& src = m_pendingBatch->files[0].sourcePath;
        auto& dst = m_pendingBatch->files[0].destPath;
        if (src.size() >= 2 && dst.size() >= 2)
            sameDrive = (toupper(src[0]) == toupper(dst[0]));
    }

    auto queueBatch = [&](bool move) {
        m_pendingBatch->isMove = move;
        {
            std::lock_guard<std::mutex> lk(m_batchMutex);
            prepareTransferManifest(m_pendingBatch);
            m_batchQueue.push_back(m_pendingBatch);
        }
        m_batchCV.notify_one();
        m_overlayWasOpen = false;
        m_pendingBatch.reset();
        m_showCopyMoveDialog = false;
        ImGui::CloseCurrentPopup();
    };

    if (ui::choiceCard("##CopyChoice", ui::Icon::Copy, ui::accentColor(ui::Accent::Info), "Copy",
            "Keep the originals and place a copy in the destination.")) {
        queueBatch(false);
    } else if (ui::choiceCard("##MoveChoice", ui::Icon::Move, ui::accentColor(ui::Accent::Warning), "Move",
            sameDrive ? "Same drive, so the move finishes instantly." : "Transfer the items and remove them from the source.",
            0, sameDrive ? "Instant" : nullptr)) {
        queueBatch(true);
    } else if (cancel) {
        m_showCopyMoveDialog = false;
        ImGui::CloseCurrentPopup();
    }
    ui::endDialog();
}

void App::tryAutoWifiConnect(const std::string& serial) {
    if (m_wifiAutoSetupDone || m_dualChannelAvailable) return;
    if (!m_prefs.wifiAutoConnect) return; // user disabled WiFi auto-connect

    for (auto& w : m_prefs.savedWifiDevices) {
        if (w.wirelessDebugging) continue;
        if (w.serial == serial && w.autoConnect && !w.wifiIp.empty()) {
            // Check if WiFi serial is already in the device list (tcpip already active)
            std::string wifiSerial = w.wifiIp + ":" + std::to_string(w.port);
            bool alreadyInTcpip = false;
            {
                std::lock_guard<std::mutex> lk(m_deviceMutex);
                for (auto& d : m_devices)
                    if (d.serial == wifiSerial) { alreadyInTcpip = true; break; }
            }

            // Verify WiFi is actually reachable before disrupting USB with tcpip
            // Quick check: try connecting directly first (if tcpip was already set from a previous session)
            LOG_INFO("WiFi", "Auto-connecting to saved WiFi device: " + w.wifiIp + ":" + std::to_string(w.port) +
                     (alreadyInTcpip ? " (already in tcpip mode)" : ""));
            m_wifiAutoSetupDone = true;

            std::string ip = w.wifiIp;
            int port = w.port;
            // Keep a verified direct helper channel alive before asking Android
            // to restart ADB in TCP mode. This channel is the app's bridge across
            // the unavoidable ADB USB transport reset.
            if (!alreadyInTcpip) {
                m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                m_secondaryChannel.disconnectTcp();
                if (!m_secondaryChannel.connectTcp(ip, AFM_PORT) || !m_secondaryChannel.verifyConnection()) {
                    m_wifiAutoSetupDone = false;
                    m_statusMessage = "WiFi helper is not reachable yet, keeping USB only";
                    m_statusTime = std::chrono::steady_clock::now();
                    LOG_WARN("WiFi", "Direct WiFi helper check failed, not switching ADB to TCP mode");
                    return;
                }
                m_dualChannelAvailable = true;
                m_secondaryChannelType = "WiFi";
                LOG_INFO("WiFi", "Direct WiFi helper verified before ADB TCP transition");
            }

            postAsync("Connecting WiFi ADB...", [this, serial, ip, port, alreadyInTcpip]() {
                if (!alreadyInTcpip) {
                    m_wifiTransitionActive = true;
                    m_device.runAdbCommand("-s " + serial + " tcpip " + std::to_string(port));
                    // Give adbd time to reopen its TCP listener. During this
                    // intentional handoff the poller keeps the USB session from
                    // being mistaken for a failed helper.
                    std::this_thread::sleep_for(std::chrono::seconds(8));
                    if (!m_device.connectDirect(ip)) {
                        LOG_WARN("WiFi", "Could not promote primary to direct WiFi during ADB TCP transition");
                    } else {
                        m_statusMessage = "Switching ADB to WiFi while keeping the file connection active";
                        m_statusTime = std::chrono::steady_clock::now();
                    }
                    m_wifiTransitionActive = false;
                }
                // Connect
                std::string result = m_device.runAdbCommand("connect " + ip + ":" + std::to_string(port));
                if (result.find("connected") != std::string::npos || result.find("already") != std::string::npos) {
                    LOG_INFO("WiFi", "WiFi ADB connected: " + ip);
                    // Track USB↔WiFi serial mapping
                    m_usbToWifiSerial = ip + ":" + std::to_string(port);
                    m_wifiToUsbSerial = serial;
                    // Establish secondary channel
                    m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                    if (m_secondaryChannel.connectTcp(ip, AFM_PORT) && m_secondaryChannel.verifyConnection()) {
                        m_dualChannelAvailable = true;
                        m_secondaryChannelType = "WiFi";
                        m_statusMessage = "Dual channel active: USB + WiFi";
                        LOG_INFO("WiFi", "Dual channel established via auto-connect");
                    } else {
                        m_statusMessage = "WiFi connected but dual channel failed";
                    }
                } else {
                    LOG_WARN("WiFi", "WiFi auto-connect failed: " + result);
                }
                m_statusTime = std::chrono::steady_clock::now();
            });
            return;
        }
    }
}

void App::renderWifiBanner() {
    return; // Dual channel is now automatic via USB x2 / WiFi x2 toggles
    if (m_wifiBannerDismissed || m_wifiAutoSetupDone || m_dualChannelAvailable) return;
    if (!m_device.isServerRunning()) return;

    // Check if USB device has WiFi and isn't already saved
    std::string serial = m_device.connectedSerial();
    if (serial.empty()) return;
    for (auto& w : m_prefs.savedWifiDevices)
        if (w.serial == serial) return; // already saved

    // Detect WiFi IP (check once, not every frame)
    if (!m_wifiBannerShown) {
        // Rate-limit: only check every 10 seconds
        static auto lastCheck = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastCheck).count() < 10.0) return;
        lastCheck = now;

        // Run on async thread to avoid blocking UI
        postAsync("", [this, serial]() {
            std::string wlanOut = m_device.runAdbCommand("-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
            if (wlanOut.find("inet ") != std::string::npos)
                m_wifiBannerShown = true;
        });
        return;
    }

    // Show banner at top of main window
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.15f, 0.25f, 1));
    ImGui::BeginChild("##WifiBanner", ImVec2(0, ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 2), ImGuiChildFlags_Border);
    float pad = (ImGui::GetContentRegionAvail().y - ImGui::GetFontSize()) * 0.5f;
    ImGui::SetCursorPosY(pad);
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1, 1), "  WiFi detected on your phone.");
    ImGui::SameLine();
    ImGui::TextDisabled("Enable dual-channel for faster transfers?");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.55f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.70f, 1));
    if (modernSmallButton("Set Up")) {
        openWifiWizard();
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (modernSmallButton("Dismiss")) {
        m_wifiBannerDismissed = true;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::requestWizardWifiProbe() {
    const auto generation = ++m_wizardProbeGeneration;
    const std::string serial = m_wizardSerial;
    const std::string adbPath = m_device.getAdbPath();
    m_wizardProbeBusy = true;
    m_wizardWifiIp.clear();
    m_wizardProbeError.clear();
    postAsync("Checking phone WiFi...", [this, generation, serial, adbPath]() {
        DeviceClient probe;
        probe.setOwnsServer(false);
        probe.setAdbPath(adbPath);
        auto result = probe.runAdbCommandResult("-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"",
            10000, [this, generation]() { return m_shutdownPoll || m_wizardProbeGeneration.load() != generation; });
        UiMessage message;
        message.hasWizardProbe = true;
        message.wizardProbeGeneration = generation;
        message.wizardProbeSerial = serial;
        if (result.succeeded()) {
            const auto& output = result.standardOutput;
            auto inet = output.find("inet ");
            if (inet != std::string::npos) {
                auto start = inet + 5;
                auto slash = output.find('/', start);
                if (slash != std::string::npos) message.wizardProbeIp = output.substr(start, slash - start);
            }
        } else if (!result.cancelled) {
            message.wizardProbeError = "Could not check this phone. " + result.diagnostics();
        }
        postUiMessage(std::move(message));
    });
}

void App::openWifiWizard() {
    m_showWifiWizard = true;
    if (m_wizardBusy) return;
    ++m_wizardProbeGeneration;
    m_wizardProbeBusy = false;
    m_wizardStep = 0;
    m_wizardSucceeded = false;
    m_wizardStatus.clear();
    m_wizardWifiIp.clear();
    m_wizardProbeError.clear();
}

void App::startWizardWifiSetup() {
    if (m_wizardBusy || m_wizardSerial.empty() || m_wizardWifiIp.empty()) return;
    m_wizardStep = 2;
    m_wizardBusy = true;
    m_wizardSucceeded = false;
    m_wizardStatus = "Preparing your wireless connection...";
    const auto generation = ++m_wizardSetupGeneration;
    const std::string serial = m_wizardSerial;
    const std::string ip = m_wizardWifiIp;
    postAsync("Setting up WiFi ADB...", [this, generation, serial, ip]() {
        auto report = [&](std::string status, bool finished = false, bool succeeded = false) {
            UiMessage message;
            message.hasWizardSetupProgress = true;
            message.wizardSetupGeneration = generation;
            message.wizardSetupSerial = serial;
            message.wizardSetupStatus = std::move(status);
            message.wizardSetupFinished = finished;
            message.wizardSetupSucceeded = succeeded;
            postUiMessage(std::move(message));
        };
        try {
            report("Enabling wireless access on your phone...");
            m_device.runAdbCommand("-s " + serial + " tcpip 5555");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            report("Connecting to your phone over WiFi...");
            std::string result = m_device.runAdbCommand("connect " + ip + ":5555");
            bool connected = result.find("connected") != std::string::npos && result.find("failed") == std::string::npos;
            if (!connected) {
                report("Could not connect over WiFi. " + result, true, false);
                return;
            }
            report("Saving your wireless connection...");
            SavedWifiDevice saved;
            saved.serial = serial;
            saved.wifiIp = ip;
            saved.port = 5555;
            saved.autoConnect = true;
            saved.model = queryDeviceDisplayName(ip + ":5555");
            m_usbToWifiSerial = ip + ":5555";
            m_wifiToUsbSerial = serial;
            auto& devices = m_prefs.savedWifiDevices;
            devices.erase(std::remove_if(devices.begin(), devices.end(),
                [&](const auto& device) { return device.serial == serial; }), devices.end());
            devices.push_back(saved);
            m_prefs.wifiAutoConnect = true;
            m_prefs.save();
            report("Finishing wireless setup...");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (deviceSession(0).serial == serial || deviceSession(0).serial == ip + ":5555") {
                m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                if (m_secondaryChannel.connectTcp(ip, AFM_PORT) && m_secondaryChannel.verifyConnection()) {
                    m_dualChannelAvailable = true;
                    m_secondaryChannelType = "WiFi";
                }
            }
            m_wifiAutoSetupDone = true;
            report("Your phone is saved and will reconnect automatically on future launches.", true, true);
        } catch (const std::exception& error) {
            report("Could not finish setup. " + std::string(error.what()), true, false);
        } catch (...) {
            report("Could not finish setup. Check your connection and try again.", true, false);
        }
    });
}

void App::renderCrossDeviceDialog() {
    if (!m_showCrossDeviceDialog || !m_pendingBatch) return;

    ImGui::OpenPopup("Cross-Device Transfer");
    if (!ui::beginDialog("Cross-Device Transfer", 520)) return;

    const int fileCount = (int)m_pendingBatch->files.size();
    const bool cancel = ui::dialogHeader(ui::Icon::Transfer, ui::accentColor(ui::Accent::Info),
        "Transfer between devices",
        std::to_string(fileCount) + (fileCount == 1 ? " file" : " files") + " selected. Choose a transfer method.", true);

    const bool dokanAvailable = DeviceMountManager::instance(m_pendingBatch->srcDeviceSlot).isMounted();
    auto queueBatch = [&](bool useDokanRelay) {
        m_pendingBatch->useDokanRelay = useDokanRelay;
        {
            std::lock_guard<std::mutex> lk(m_batchMutex);
            prepareTransferManifest(m_pendingBatch);
            m_batchQueue.push_back(m_pendingBatch);
        }
        m_batchCV.notify_one();
        m_overlayWasOpen = false;
        m_pendingBatch.reset();
        m_showCrossDeviceDialog = false;
        ImGui::CloseCurrentPopup();
    };

    bool chosen = false;
    if (ui::choiceCard("##FastRelay", ui::Icon::Transfer, ui::accentColor(ui::Accent::Info), "Fast via PC",
            "Streams chunks through the PC and uses every available USB and WiFi channel.", 0, "Recommended")) {
        queueBatch(false);
        chosen = true;
    }
    if (!chosen) {
        ImGui::BeginDisabled(!dokanAvailable);
        if (ui::choiceCard("##CompatibilityRelay", ui::Icon::Link, ui::accentColor(ui::Accent::Warning),
                "Compatibility stream",
                dokanAvailable ? "Single stream through the PC. Use only if fast mode has trouble."
                               : "Mount the source device as a drive first (Connection > Mount as Drive).")) {
            queueBatch(true);
            chosen = true;
        }
        ImGui::EndDisabled();
    }
    if (!chosen && cancel) {
        m_showCrossDeviceDialog = false;
        ImGui::CloseCurrentPopup();
    }
    ui::endDialog();
}

void App::renderAboutPopup() {
    if (m_showAboutPopup) {
        ImGui::OpenPopup("About##AboutPopup");
        m_showAboutPopup = false;
    }

    if (ui::beginDialog("About##AboutPopup", 420)) {
        const float s = ui::scale();
        ui::PanelStyle theme(m_resolvedLightTheme);
        bool close = ui::dialogHeader(ui::Icon::Phone, ui::accentColor(ui::Accent::Info),
            "Fast Enough? - Android File Explorer", "Version 1.0.23", true);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.background);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 12 * s));
        ImGui::BeginChild("##AboutDetails", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        auto row = [&](const char* label, const char* value) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::SameLine(110 * s);
            ImGui::TextUnformatted(value);
        };
        row("Build date", __DATE__);
        row("Made by", "JohnTheFarm3r");
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        const float buttonW = 110 * s;
        ui::dialogFooter(buttonW);
        if (modernButton("Close##AboutClose", ImVec2(buttonW, ui::footerButtonHeight()), ButtonTone::Primary) || close)
            ImGui::CloseCurrentPopup();
        ui::endDialog();
    }
}

void App::renderNotificationPopup() {
    bool show = false;
    std::string title;
    std::string message;
    bool isError = false;
    {
        std::lock_guard<std::mutex> lk(m_notificationMutex);
        show = m_showNotification;
        title = m_notificationTitle;
        message = m_notificationMessage;
        isError = m_notificationIsError;
        if (show) {
            m_showNotification = false;
        }
    }
    if (show) {
        ImGui::OpenPopup("##NotificationPopup");
    }
    const float s = ui::scale();
    const bool isBackupResult = title.rfind("Backup ", 0) == 0 || title.rfind("Restore ", 0) == 0;
    const bool cancelled = title.find("Cancelled") != std::string::npos;
    const float buttonH = ui::footerButtonHeight();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool enterPressed = !show && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));

    if (!isBackupResult) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, viewport->WorkSize.y * 0.9f));
        if (ui::beginDialog("##NotificationPopup", 460)) {
            const ImVec4 accent = ui::accentColor(isError ? ui::Accent::Danger : ui::Accent::Info);
            bool close = ui::dialogHeader(isError ? ui::Icon::Close : ui::Icon::Info, accent, title.c_str(), {}, true);
            const float wrapWidth = ImGui::GetContentRegionAvail().x;
            const float messageHeight = ImGui::CalcTextSize(message.c_str(), nullptr, false, wrapWidth).y;
            const float maxMessageHeight = 320 * s;
            if (messageHeight > maxMessageHeight) {
                ImGui::BeginChild("##NotificationMessage", ImVec2(0, maxMessageHeight), 0);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
            } else {
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(message.c_str());
                ImGui::PopTextWrapPos();
            }
            const float okW = 110 * s;
            ui::dialogFooter(okW);
            if (modernButton("OK##NotificationOk", ImVec2(okW, buttonH), ButtonTone::Primary) || close || enterPressed)
                ImGui::CloseCurrentPopup();
            ui::endDialog();
        }
        return;
    }

    // Backup Manager results: status first, details only when there is more to say.
    const size_t firstBreak = message.find('\n');
    std::string summary = firstBreak == std::string::npos ? message : message.substr(0, firstBreak);
    std::string details = firstBreak == std::string::npos ? std::string() : message.substr(firstBreak + 1);
    while (!details.empty() && (details.front() == '\n' || details.front() == '\r' || details.front() == ' '))
        details.erase(details.begin());
    if (summary.empty()) summary = "The operation finished.";
    const bool hasDetails = !details.empty();

    const ui::Accent accentKind = isError ? ui::Accent::Danger : cancelled ? ui::Accent::Warning : ui::Accent::Success;
    const ImVec4 accent = ui::accentColor(accentKind);
    const char* statusText = cancelled ? "Cancelled" : (isError ? "Completed with issues" : "Completed successfully");
    const ui::Icon statusIcon = isError ? ui::Icon::Close : cancelled ? ui::Icon::Pause : ui::Icon::Check;

    if (hasDetails) {
        ImGui::SetNextWindowSize(ImVec2(std::min(680 * s, viewport->WorkSize.x * 0.82f),
            std::min(500 * s, viewport->WorkSize.y * 0.78f)), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(500 * s, viewport->WorkSize.x * 0.92f), 340 * s),
            ImVec2(viewport->WorkSize.x * 0.92f, viewport->WorkSize.y * 0.90f));
    }
    if (!ui::beginDialog("##NotificationPopup", 520, !hasDetails)) return;

    bool close = ui::dialogHeader(statusIcon, accent, title.c_str(), summary, true);
    ui::statusChip(statusText, accent);

    if (hasDetails) {
        ImGui::Dummy(ImVec2(0, 2 * s));
        ui::sectionLabel("DETAILS");
        ui::PanelStyle theme(m_resolvedLightTheme);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.background);
        ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 12 * s));
        ImGui::BeginChild("##ResultDetails", ImVec2(0, -(buttonH + 16 * s)), ImGuiChildFlags_Borders);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(details.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    const float doneW = 110 * s;
    const float copyW = 130 * s;
    ui::dialogFooter(hasDetails ? copyW + doneW + ImGui::GetStyle().ItemSpacing.x : doneW);
    if (hasDetails) {
        if (modernButton("Copy Details", ImVec2(copyW, buttonH))) {
            ImGui::SetClipboardText(message.c_str());
            m_statusMessage = "Result details copied";
            m_statusTime = std::chrono::steady_clock::now();
        }
        ImGui::SameLine();
    }
    if (modernButton("Done##BackupResultDone", ImVec2(doneW, buttonH), ButtonTone::Primary) || close || enterPressed)
        ImGui::CloseCurrentPopup();
    ui::endDialog();
}

void App::showNotification(const std::string& title, const std::string& message, bool isError) {
    DebugLog::instance().log(isError?LogLevel::Error:LogLevel::Info,"Activity",title+": "+message);
    std::lock_guard<std::mutex> lk(m_notificationMutex);
    m_notificationTitle = title;
    m_notificationMessage = message;
    m_notificationIsError = isError;
    m_showNotification = true;
}

void App::requestApkInstall(const std::string& apkPath) {
    if (apkPath.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_apkInstallMutex);
        if (m_apkInstallState == ApkInstallDialogState::Installing) return;
        m_apkInstallState = ApkInstallDialogState::Setup;
        m_apkInstallResult.clear();
        m_apkInstallStarted = {};
    }
    m_pendingApkPath = apkPath;
    m_pendingApkDeviceIndex = 0;
    m_apkInstallGrantPermissions = false;
    m_apkInstallAllowDowngrade = false;
    m_showApkInstallDialog = true;
}

void App::renderApkInstallDialog() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    // A minimized window reports a zero-sized viewport, so wait until it is restored.
    const bool viewportReady = viewport->WorkSize.x >= 200.0f && viewport->WorkSize.y >= 200.0f;
    if (m_showApkInstallDialog && viewportReady) {
        ImGui::OpenPopup("Install APK");
        m_showApkInstallDialog = false;
    }

    const float s = ui::scale();
    float maxH = std::max(260.0f, viewport->WorkSize.y - 32.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxH));
    // Keep the dialog centered as the window resizes or the dialog content changes size.
    if (!ui::beginDialog("Install APK", 580, true, 0, viewportReady)) return;

    ui::PanelStyle theme(m_resolvedLightTheme);
    const float buttonH = ui::footerButtonHeight();
    std::string apkName = m_pendingApkPath;
    auto slash = apkName.find_last_of("\\/");
    if (slash != std::string::npos) apkName = apkName.substr(slash + 1);

    ApkInstallDialogState installState;
    std::string installResult;
    std::chrono::steady_clock::time_point installStarted;
    {
        std::lock_guard<std::mutex> lk(m_apkInstallMutex);
        installState = m_apkInstallState;
        installResult = m_apkInstallResult;
        installStarted = m_apkInstallStarted;
    }

    auto beginCard = [&](const char* id, ImVec4 edge) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.background);
        ImGui::PushStyleColor(ImGuiCol_Border, edge);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 12 * s));
        ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    };
    auto endCard = [&]() {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    };

    if (installState == ApkInstallDialogState::Installing ||
        installState == ApkInstallDialogState::Succeeded ||
        installState == ApkInstallDialogState::Failed) {
        const bool installing = installState == ApkInstallDialogState::Installing;
        const bool succeeded = installState == ApkInstallDialogState::Succeeded;
        const ImVec4 accent = ui::accentColor(succeeded ? ui::Accent::Success :
            installing ? ui::Accent::Info : ui::Accent::Danger);
        const float elapsed = installStarted.time_since_epoch().count() == 0 ? 0.0f :
            (float)std::chrono::duration<double>(std::chrono::steady_clock::now() - installStarted).count();
        // adb install does not expose transfer or package-manager progress. Advance through the
        // usual transfer phase quickly, then keep the final portion for Android verification.
        const float estimatedProgress = std::min(0.88f, 0.20f + 0.68f * (1.0f - std::exp(-elapsed / 1.75f)));
        const float progress = installing ? estimatedProgress : 1.0f;

        bool close = ui::dialogHeader(succeeded ? ui::Icon::Check : installing ? ui::Icon::Apps : ui::Icon::Close,
            accent, succeeded ? "APK installed" : installing ? "Installing APK" : "APK installation failed",
            apkName, !installing);

        beginCard("##ApkInstallProgressCard", ui::withAlpha(accent, 0.45f));
        if (installing) {
            ImGui::PushStyleColor(ImGuiCol_Text, accent);
            ImGui::TextUnformatted("Sending and installing on Android");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
            ImGui::TextUnformatted("Keep the device connected until this finishes.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 2 * s));
            const ImVec2 trackPos = ImGui::GetCursorScreenPos();
            const float trackW = ImGui::GetContentRegionAvail().x;
            ui::progressTrack(trackPos, trackW, 8 * s, progress, m_theme.gradientPrimary, m_theme.gradientSecondary);
            ImGui::Dummy(ImVec2(trackW, 8 * s));
            ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
            ImGui::Text("Estimated progress  %.0f%%", progress * 100.0f);
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, accent);
            ImGui::TextUnformatted(succeeded ? "The app is ready on the selected device." : "Android could not install this APK.");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
            ImGui::PushTextWrapPos(0);
            if (succeeded) ImGui::Text("Device: %s", installResult.c_str());
            else ImGui::TextUnformatted(installResult.empty() ? "Android did not return an error message." : installResult.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        endCard();

        if (!installing) {
            const float closeW = 110 * s;
            ui::dialogFooter(closeW);
            if (modernButton("Close##ApkClose", ImVec2(closeW, buttonH), ButtonTone::Primary) || close)
                ImGui::CloseCurrentPopup();
        }
        ui::endDialog();
        return;
    }

    bool cancel = ui::dialogHeader(ui::Icon::Apps, ui::accentColor(ui::Accent::Info), "Install APK",
        "Choose where to install this app", true);

    beginCard("##ApkInstallFileCard", theme.border);
    if (ui::semiboldFont) ImGui::PushFont(ui::semiboldFont);
    ImGui::TextUnformatted(apkName.c_str());
    if (ui::semiboldFont) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(m_pendingApkPath.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    endCard();

    std::vector<DeviceInfo> online;
    {
        std::lock_guard<std::mutex> lk(m_deviceMutex);
        for (const auto& d : m_devices)
            if (d.state == "device") online.push_back(d);
    }

    if (online.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::accentColor(ui::Accent::Danger));
        ImGui::TextUnformatted("No Android device is connected.");
        ImGui::PopStyleColor();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("Connect a device with USB debugging enabled, then open this APK again.");
        ImGui::PopTextWrapPos();
    } else {
        if (m_pendingApkDeviceIndex < 0 || m_pendingApkDeviceIndex >= (int)online.size())
            m_pendingApkDeviceIndex = 0;
        auto deviceLabel = [](const DeviceInfo& device) {
            return device.model.empty() ? device.serial : device.model + " (" + device.serial + ")";
        };
        ui::sectionLabel("INSTALL TO");
        if (online.size() == 1) {
            ImGui::TextUnformatted(deviceLabel(online[0]).c_str());
        } else {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 8 * s));
            std::string preview = deviceLabel(online[m_pendingApkDeviceIndex]);
            if (ui::beginCombo("##ApkDevice", preview.c_str())) {
                for (int i = 0; i < (int)online.size(); i++) {
                    std::string label = deviceLabel(online[i]);
                    bool selected = i == m_pendingApkDeviceIndex;
                    if (ImGui::Selectable(label.c_str(), selected))
                        m_pendingApkDeviceIndex = i;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopStyleVar();
        }

        ui::sectionLabel("OPTIONS");
        ui::checkbox("Grant runtime permissions after install", &m_apkInstallGrantPermissions);
        ui::checkbox("Allow version downgrade", &m_apkInstallAllowDowngrade);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("An existing copy of the same app is replaced and keeps its data.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    const float btnW = 110 * s;
    const float totalW = online.empty() ? btnW : btnW * 2 + ImGui::GetStyle().ItemSpacing.x;
    ui::dialogFooter(totalW);
    if (!online.empty()) {
        ImGui::BeginDisabled(m_asyncBusy.load());
        if (modernButton("Install", ImVec2(btnW, buttonH), ButtonTone::Primary)) {
            std::string apk = m_pendingApkPath;
            DeviceInfo target = online[m_pendingApkDeviceIndex];
            bool grant = m_apkInstallGrantPermissions;
            bool downgrade = m_apkInstallAllowDowngrade;
            {
                std::lock_guard<std::mutex> lk(m_apkInstallMutex);
                m_apkInstallState = ApkInstallDialogState::Installing;
                m_apkInstallResult.clear();
                m_apkInstallStarted = std::chrono::steady_clock::now();
            }
            postAsync("Installing APK...", [this, apk, target, grant, downgrade]() {
                std::string out;
                bool ok = false;
                try {
                    ok = m_device.installApk(target.serial, apk, out, true, grant, downgrade);
                } catch (const std::exception& e) {
                    out = e.what();
                } catch (...) {
                    out = "Unexpected error while starting the Android installer.";
                }
                std::string result = target.model.empty() ? target.serial : target.model + " (" + target.serial + ")";
                if (ok) {
                    m_statusMessage = "APK installed";
                    m_leftPanel.needsRefresh = true;
                    m_rightPanel.needsRefresh = true;
                } else {
                    result = out.empty() ? "Android did not return an error message." : out;
                    m_statusMessage = "APK install failed";
                }
                m_statusTime = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lk(m_apkInstallMutex);
                    m_apkInstallResult = std::move(result);
                    m_apkInstallState = ok ? ApkInstallDialogState::Succeeded : ApkInstallDialogState::Failed;
                }
            });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (modernButton("Cancel##ApkCancel", ImVec2(btnW, buttonH)) || cancel) {
        ImGui::CloseCurrentPopup();
    }
    ui::endDialog();
}

void App::registerApkAssociation() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string exe = exePath;
    std::string exeName = exe;
    size_t slash = exeName.find_last_of("\\/");
    if (slash != std::string::npos) exeName = exeName.substr(slash + 1);

    const std::string progId = "FastEnough.apk";
    const std::string appName = "Fast Enough Android File Manager";
    const std::string command = "\"" + exe + "\" \"%1\"";

    auto setStringValue = [](HKEY root, const std::string& subkey, const char* valueName, const std::string& value) {
        HKEY key = nullptr;
        if (RegCreateKeyExA(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            RegSetValueExA(key, valueName, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)value.size() + 1);
            RegCloseKey(key);
        }
    };
    auto setEmptyValue = [](HKEY root, const std::string& subkey, const std::string& valueName) {
        HKEY key = nullptr;
        if (RegCreateKeyExA(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            RegSetValueExA(key, valueName.c_str(), 0, REG_NONE, nullptr, 0);
            RegCloseKey(key);
        }
    };

    // ProgID used by Open with and, when Windows allows it, as the per-user default.
    setStringValue(HKEY_CURRENT_USER, "Software\\Classes\\.apk", nullptr, progId);
    setEmptyValue(HKEY_CURRENT_USER, "Software\\Classes\\.apk\\OpenWithProgids", progId);
    setStringValue(HKEY_CURRENT_USER, "Software\\Classes\\.apk\\OpenWithList\\" + exeName, nullptr, "");
    setStringValue(HKEY_CURRENT_USER, "Software\\Classes\\" + progId, nullptr, "Android APK");
    setStringValue(HKEY_CURRENT_USER, "Software\\Classes\\" + progId, "FriendlyTypeName", "Android APK");
    setStringValue(HKEY_CURRENT_USER, "Software\\Classes\\" + progId + "\\DefaultIcon", nullptr, exe + ",0");
    setStringValue(HKEY_CURRENT_USER, "Software\\Classes\\" + progId + "\\shell\\open", nullptr, "Install with Fast Enough");
    setStringValue(HKEY_CURRENT_USER, "Software\\Classes\\" + progId + "\\shell\\open\\command", nullptr, command);

    // Register as an application so it appears directly in Windows Open with.
    std::string appKey = "Software\\Classes\\Applications\\" + exeName;
    setStringValue(HKEY_CURRENT_USER, appKey, "FriendlyAppName", appName);
    setStringValue(HKEY_CURRENT_USER, appKey + "\\shell\\open\\command", nullptr, command);
    setStringValue(HKEY_CURRENT_USER, appKey + "\\SupportedTypes", ".apk", "");

    // Register app capabilities for Default Apps UI.
    std::string capKey = "Software\\FastEnough\\Capabilities";
    setStringValue(HKEY_CURRENT_USER, capKey, "ApplicationName", appName);
    setStringValue(HKEY_CURRENT_USER, capKey, "ApplicationDescription", "Install APK files on connected Android devices.");
    setStringValue(HKEY_CURRENT_USER, capKey + "\\FileAssociations", ".apk", progId);
    setStringValue(HKEY_CURRENT_USER, "Software\\RegisteredApplications", appName.c_str(), "Software\\FastEnough\\Capabilities");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    showNotification("APK Handler Registered",
        "Fast Enough was added to Windows Open with for APK files.\n\n"
        "If APK files already have another default app, right-click an APK, choose Open with, then select Fast Enough Android File Manager and set it as the default.", false);
    ShellExecuteA(nullptr, "open", "ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
}

void App::renderCloseConfirmDialog() {
    if (m_showCloseConfirm) {
        ImGui::OpenPopup("Close Application?");
        m_showCloseConfirm = false;
    }

    if (ui::beginDialog("Close Application?", 440)) {
        const float s = ui::scale();
        bool cancel = ui::dialogHeader(ui::Icon::Close, ui::accentColor(ui::Accent::Warning),
            "Close Fast Enough?", "Are you sure you want to close the application?", true);

        const float buttonH = ui::footerButtonHeight();
        const float closeW = 130 * s;
        const float cancelW = 110 * s;
        ui::dialogFooter(closeW + cancelW + ImGui::GetStyle().ItemSpacing.x);
        if (modernButton("Close App", ImVec2(closeW, buttonH), ButtonTone::Danger)) {
            g_reallyQuit = true;
            PostMessage(g_mainHwnd, WM_CLOSE, 0, 0);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (modernButton("Cancel##CloseAppCancel", ImVec2(cancelW, buttonH)) || cancel) {
            ImGui::CloseCurrentPopup();
        }
        ui::endDialog();
    }
}

void App::renderPreferencesWindow() {
    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_Appearing);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin("Preferences", &m_showPreferences, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    bool changed = false;

    // --- ADB ---
    ImGui::TextColored(ImVec4(0.45f,0.70f,1,1), "ADB");
    ImGui::Separator();
    ImGui::Spacing();

    if (ui::checkbox("Restart ADB daemon on app launch", &m_prefs.restartAdbOnLaunch))
        changed = true;
    ImGui::TextDisabled("  Kills and restarts ADB when the app starts.");
    ImGui::TextDisabled("  Useful if ADB is in a bad state, but disrupts");
    ImGui::TextDisabled("  other ADB clients (Android Studio, scrcpy, etc.).");

    ImGui::Spacing();

    if (ui::checkbox("Kill ADB server when app closes", &m_prefs.killAdbOnClose))
        changed = true;
    ImGui::TextDisabled("  Stops the ADB daemon when exiting the app.");
    ImGui::TextDisabled("  Disable if you want ADB to stay running for");
    ImGui::TextDisabled("  other tools (Android Studio, scrcpy, etc.).");

    ImGui::Spacing();
    ImGui::Spacing();

    // --- General ---
    ImGui::TextColored(ImVec4(0.45f,0.70f,1,1), "General");
    ImGui::Separator();
    ImGui::Spacing();

    if (ui::checkbox("Ask for confirmation before closing", &m_prefs.confirmOnClose))
        changed = true;
    ImGui::TextDisabled("  Shows a Yes/No dialog when pressing the X button.");
    ImGui::TextDisabled("  When disabled, closing minimizes to the system tray.");

    ImGui::Spacing();
    ImGui::Spacing();

    // --- Transfer ---
    ImGui::TextColored(ImVec4(0.45f,0.70f,1,1), "Transfer");
    ImGui::Separator();
    ImGui::Spacing();

    if (ui::checkbox("Verify file integrity (CRC32) after transfer", &m_prefs.enableCrcVerification))
        changed = true;
    ImGui::TextDisabled("  Computes CRC32 on both sides after each file");
    ImGui::TextDisabled("  transfer to ensure data integrity.");

    ImGui::Spacing();

    ImGui::TextWrapped("Finished transfers clear automatically. Transfer details stay open while you review them.");
    ImGui::TextDisabled("Transfer results remain in the activity log (F12).");

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1,0.85f,0.4f,1), "Transfer Channels");
    ImGui::Spacing();

    auto drawPipeSelector = [&](const char* label, const char* id, int& value) -> bool {
        bool changed = false;
        const float s = ui::scale();
        // Label column fits the longest label so the buttons never overlap it.
        const float labelColumn = std::max(ImGui::CalcTextSize("Default USB pipes").x,
            ImGui::CalcTextSize("Default WiFi pipes").x) + 16 * s;
        const float rowStart = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(rowStart + labelColumn);
        for (int pipes = 1; pipes <= 4; pipes++) {
            if (pipes > 1) ImGui::SameLine(0, 6 * s);
            bool selected = (value == pipes);
            std::string btn = "x" + std::to_string(pipes) + "##" + id + std::to_string(pipes);
            if (modernButton(btn.c_str(), ImVec2(std::max(44.0f * s, ImGui::CalcTextSize("x4").x + 24 * s), 0.0f),
                    selected ? ButtonTone::Primary : ButtonTone::Secondary) && value != pipes) {
                value = pipes;
                changed = true;
            }
        }
        return changed;
    };

    {
        if (drawPipeSelector("Default USB pipes", "usbpref", m_prefs.usbPipeCount))
            changed = true;
        ImGui::TextDisabled("  Default for devices without their own saved pipe setting. Range: 1 to 4.");
        ImGui::TextDisabled("  Best for USB 3.x. Use lower values if a device becomes unstable.");
    }

    ImGui::Spacing();

    {
        if (drawPipeSelector("Default WiFi pipes", "wifipref", m_prefs.wifiPipeCount))
            changed = true;
        ImGui::TextDisabled("  Default for devices without their own saved pipe setting. Range: 1 to 4.");
        ImGui::TextDisabled("  Best for fast WiFi (WiFi 6/7). Use lower values if a device becomes unstable.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("  Per-device settings are available from the device bar and apply on next device connect.");
    ImGui::TextDisabled("  USB + WiFi can use up to 8 parallel channels.");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (changed) {
        m_prefs.save();
        m_statusMessage = "Preferences saved";
        m_statusTime = std::chrono::steady_clock::now();
    }

    float bw = 120.0f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
    if (modernButton("Close", ImVec2(bw, 0))) {
        m_showPreferences = false;
    }

    ImGui::End();
}

void App::renderNicConfigWindow() {
    float minW = 580.0f * (m_systemDpiScale * m_theme.userScale);
    ImGui::SetNextWindowSizeConstraints(ImVec2(minW, 0), ImVec2(FLT_MAX, FLT_MAX));
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin("Multi-NIC Configuration", &m_showNicConfig,
                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    bool changed = false;

    ImGui::TextColored(ImVec4(0.45f,0.70f,1,1), "Multi-NIC Parallel Transfers");
    ImGui::Separator();
    ImGui::Spacing();

    if (ui::checkbox("Enable multi-NIC parallel transfers", &m_prefs.enableMultiNic))
        changed = true;
    ImGui::TextDisabled("  Uses multiple network adapters simultaneously for");
    ImGui::TextDisabled("  faster file transfers. Requires direct TCP connection.");
    ImGui::TextDisabled("  Each NIC carries a portion of the data independently.");

    ImGui::Spacing();
    ImGui::Spacing();

    // Scan button
    ImGui::TextColored(ImVec4(0.45f,0.70f,1,1), "Network Adapters");
    ImGui::Separator();
    ImGui::Spacing();

    if (modernButton("Scan Network Adapters")) {
        m_detectedNics = enumerateNics();
        m_nicTestResults.assign(m_detectedNics.size(), false);
        m_nicTestDone.assign(m_detectedNics.size(), false);
        m_nicTestReason.assign(m_detectedNics.size(), "");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d found)", (int)m_detectedNics.size());

    ImGui::Spacing();

    if (!m_detectedNics.empty()) {
        if (ImGui::BeginTable("NICs", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, 35);
            ImGui::TableSetupColumn("Adapter", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)m_detectedNics.size(); i++) {
                auto& nic = m_detectedNics[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();

                // Checkbox: find or create binding for this NIC
                ImGui::TableSetColumnIndex(0);
                bool isEnabled = false;
                int bindIdx = -1;
                for (int b = 0; b < (int)m_prefs.multiNicBindings.size(); b++) {
                    if (m_prefs.multiNicBindings[b].localIp == nic.ipAddress) {
                        isEnabled = m_prefs.multiNicBindings[b].enabled;
                        bindIdx = b;
                        break;
                    }
                }
                if (ui::checkbox("##en", &isEnabled)) {
                    if (bindIdx >= 0) {
                        m_prefs.multiNicBindings[bindIdx].enabled = isEnabled;
                    } else {
                        NicBinding nb;
                        nb.adapterName = nic.adapterName;
                        nb.localIp = nic.ipAddress;
                        nb.enabled = isEnabled;
                        m_prefs.multiNicBindings.push_back(std::move(nb));
                    }
                    changed = true;
                }

                // Adapter name
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(nic.adapterName.c_str());
                if (ImGui::IsItemHovered() && !nic.description.empty())
                    ImGui::SetTooltip("%s", nic.description.c_str());

                // IP
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(nic.ipAddress.c_str());

                // Speed
                ImGui::TableSetColumnIndex(3);
                if (nic.speed >= 1000000000ULL)
                    ImGui::Text("%.0f Gbps", nic.speed / 1e9);
                else if (nic.speed >= 1000000ULL)
                    ImGui::Text("%.0f Mbps", nic.speed / 1e6);
                else if (nic.speed > 0)
                    ImGui::Text("%llu bps", nic.speed);
                else
                    ImGui::TextDisabled("?");

                // Test button: changes color to show result
                ImGui::TableSetColumnIndex(4);
                bool tested = i < (int)m_nicTestDone.size() && m_nicTestDone[i];
                bool passed = tested && m_nicTestResults[i];
                if (tested) {
                    if (passed) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.20f, 1));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.55f, 0.28f, 1));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.20f, 0.20f, 1));
                    }
                }
                const char* btnLabel = tested ? (passed ? " OK " : "Fail") : "Test";
                if (modernSmallButton(btnLabel)) {
                    // Try connecting to the current device through this NIC
                    bool ok = false;
                    std::string reason;
                    if (!m_device.isServerRunning()) {
                        reason = "No device connected";
                    } else {
                        // Find the device's WiFi IP from any available source
                        std::string devIp = m_device.deviceIp();
                        if (devIp.empty() || !m_device.isDirectConnection()) {
                            // Try ADB to get wlan0 IP
                            std::string serial = m_device.connectedSerial();
                            std::string wlanOut = m_device.runAdbCommand(
                                "-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
                            auto inetPos = wlanOut.find("inet ");
                            if (inetPos != std::string::npos) {
                                auto start = inetPos + 5;
                                auto slash = wlanOut.find('/', start);
                                if (slash != std::string::npos)
                                    devIp = wlanOut.substr(start, slash - start);
                            }
                            // Fallback: saved WiFi devices
                            if (devIp.empty()) {
                                for (auto& w : m_prefs.savedWifiDevices) {
                                    if (w.serial == serial && !w.wifiIp.empty()) {
                                        devIp = w.wifiIp;
                                        break;
                                    }
                                }
                            }
                        }
                        if (devIp.empty()) {
                            reason = "Could not determine device WiFi IP";
                        } else {
                            DeviceClient testCh;
                            if (!testCh.connectTcp(devIp, AFM_PORT, nic.ipAddress)) {
                                reason = "Could not reach " + devIp + " through " + nic.adapterName + " (" + nic.ipAddress + ")";
                            } else if (!testCh.verifyConnection()) {
                                reason = "Connected but server did not respond";
                                testCh.disconnectTcp();
                            } else {
                                ok = true;
                                testCh.disconnectTcp();
                            }
                        }
                    }
                    if (i < (int)m_nicTestResults.size()) {
                        m_nicTestResults[i] = ok;
                        m_nicTestDone[i] = true;
                        m_nicTestReason[i] = reason;
                    }
                }
                if (tested) ImGui::PopStyleColor(2);
                if (tested && !passed && ImGui::IsItemHovered()) {
                    std::string reason = (i < (int)m_nicTestReason.size()) ? m_nicTestReason[i] : "";
                    if (!reason.empty())
                        ImGui::SetTooltip("%s", reason.c_str());
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Count enabled NICs
        int enabledCount = 0;
        for (auto& nb : m_prefs.multiNicBindings)
            if (nb.enabled) enabledCount++;
        ImGui::Spacing();
        if (enabledCount >= 2)
            ImGui::TextColored(ImVec4(0.3f,0.9f,0.3f,1), "%d adapters selected for parallel transfers", enabledCount);
        else if (enabledCount == 1)
            ImGui::TextColored(ImVec4(0.9f,0.7f,0.2f,1), "Select at least 2 adapters for parallel transfers");
        else
            ImGui::TextDisabled("No adapters selected");
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (changed) {
        m_prefs.save();
        m_statusMessage = "Multi-NIC settings saved";
        m_statusTime = std::chrono::steady_clock::now();
    }

    float bw = 120.0f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
    if (modernButton("Close", ImVec2(bw, 0))) {
        m_showNicConfig = false;
    }

    ImGui::End();
}

void App::applyScale(float newScale) {
    m_theme.userScale = newScale;

    // Rebuild font at new size
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    float effectiveScale = m_systemDpiScale * newScale;
    ui::layoutScale = effectiveScale;
    float fontSize = 14.0f * effectiveScale * m_theme.fontScale;

    ImFontConfig fontCfg;
    fontCfg.OversampleH = 3;
    fontCfg.OversampleV = 3;
    fontCfg.PixelSnapH = true;
    fontCfg.RasterizerDensity = 1.0f;

    char winDir[MAX_PATH];
    GetWindowsDirectoryA(winDir, MAX_PATH);
    std::string segoeUIPath = std::string(winDir) + "\\Fonts\\segoeui.ttf";

    // Include Latin, Latin Extended, Greek, Cyrillic, and common symbols
    // so non-ASCII folder/file names display correctly.
    static const ImWchar glyphRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x0100, 0x024F, // Latin Extended-A + B
        0x0370, 0x03FF, // Greek and Coptic
        0x0400, 0x04FF, // Cyrillic
        0x2000, 0x206F, // General Punctuation
        0x2100, 0x214F, // Letterlike Symbols
        0,
    };

    ImFont* mainFont = nullptr;
    if (GetFileAttributesA(segoeUIPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        mainFont = io.Fonts->AddFontFromFileTTF(segoeUIPath.c_str(), fontSize, &fontCfg, glyphRanges);
    }
    if (!mainFont) {
        fontCfg.SizePixels = fontSize;
        mainFont = io.Fonts->AddFontDefault(&fontCfg);
    }

    std::string semiboldPath=std::string(winDir)+"\\Fonts\\seguisb.ttf";
    ui::semiboldFont=io.Fonts->AddFontFromFileTTF(semiboldPath.c_str(),fontSize,&fontCfg,glyphRanges);
    if(!ui::semiboldFont)ui::semiboldFont=mainFont;
    ui::connectionHeadingFont=io.Fonts->AddFontFromFileTTF(semiboldPath.c_str(),fontSize*29.0f/14.0f,&fontCfg,glyphRanges);
    if(!ui::connectionHeadingFont)ui::connectionHeadingFont=ui::semiboldFont;

    io.Fonts->Build();

    // Tell DX11 backend to rebuild font texture
    extern void ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_InvalidateDeviceObjects();

    // Save current colors before resetting style
    ImVec4 savedColors[ImGuiCol_COUNT];
    if (m_theme.customColors) {
        for (int i = 0; i < ImGuiCol_COUNT; i++)
            savedColors[i] = m_theme.colors[i];
    }

    // Reset style to base sizes, then apply setupStyle (sets both sizes and colors)
    setupStyle();
    // Scale all sizes to effective DPI * user scale
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(effectiveScale);
    // Clamp WindowMinSize to avoid ImGui assertion at low effective scales
    if (style.WindowMinSize.x < 1.0f) style.WindowMinSize.x = 1.0f;
    if (style.WindowMinSize.y < 1.0f) style.WindowMinSize.y = 1.0f;

    // Restore custom colors if active (setupStyle overwrote them)
    if (m_theme.customColors) {
        ImVec4* c = ImGui::GetStyle().Colors;
        for (int i = 0; i < ImGuiCol_COUNT; i++)
            c[i] = savedColors[i];
    }
}

void App::renderThemeWindow() {
    // After a scale change, force re-center and re-size so the window doesn't end up off-screen
    ImGuiCond posCond = ImGuiCond_Appearing;
    ImGuiCond sizeCond = ImGuiCond_FirstUseEver;
    if (m_themeNeedsReposition) {
        posCond = ImGuiCond_Always;
        sizeCond = ImGuiCond_Always;
        m_themeNeedsReposition = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    float winH = std::min(720.0f * ui::scale(), vpSize.y * 0.85f);
    float winW = std::min(std::max(560.0f * ui::scale(), 380.0f * ui::textScale()), vpSize.x * 0.9f);
    ImGui::SetNextWindowSize(ImVec2(winW, winH), sizeCond);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(400.0f * ui::scale(), vpSize.x * 0.9f),
        std::min(300.0f * ui::scale(), vpSize.y * 0.9f)), ImVec2(vpSize.x * 0.9f, vpSize.y * 0.9f));
    ImGui::SetNextWindowPos(center, posCond, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin("Appearance", &m_showThemeWindow, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    bool changed = false;

    ImGui::TextColored(m_theme.gradientPrimary, "Visual Theme");
    ImGui::Separator();
    ImGui::Spacing();
    const char* themeModes[] = { "Follow Windows", "Dark", "Light" };
    ImGui::SetNextItemWidth(210.0f);
    if (ui::combo("Theme Mode", &m_theme.mode, themeModes, 3)) {
        m_theme.customColors = false;
        m_pendingScale = m_theme.userScale;
        changed = true;
    }
    ImGui::TextDisabled(m_theme.mode == 0
        ? (m_resolvedLightTheme ? "Following Windows, light mode is active" : "Following Windows, dark mode is active")
        : (m_theme.mode == 1 ? "Dark mode is always active" : "Light mode is always active"));

    ImGui::SetNextItemWidth(210.0f);
    if (ImGui::ColorEdit3("Primary Accent", &m_theme.gradientPrimary.x,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel))
        changed = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) m_pendingScale = m_theme.userScale;
    ImGui::SetNextItemWidth(210.0f);
    if (ImGui::ColorEdit3("Secondary Accent", &m_theme.gradientSecondary.x,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel))
        changed = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) m_pendingScale = m_theme.userScale;
    int gradientPercent = (int)std::round(m_theme.gradientStrength * 100.0f);
    ImGui::SetNextItemWidth(210.0f);
    if (ImGui::SliderInt("Gradient Intensity", &gradientPercent, 0, 100, "%d%%")) {
        m_theme.gradientStrength = gradientPercent / 100.0f;
        changed = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) m_pendingScale = m_theme.userScale;
    if (modernButton("Reset Accents")) {
        m_theme.gradientPrimary = ImVec4(0.20f, 0.58f, 1.00f, 1.00f);
        m_theme.gradientSecondary = ImVec4(0.48f, 0.26f, 0.92f, 1.00f);
        m_theme.gradientStrength = 0.72f;
        m_pendingScale = m_theme.userScale;
        changed = true;
    }

    ImGui::BeginChild("##GradientPreview", ImVec2(0, ImGui::GetTextLineHeight() + 36.0f), ImGuiChildFlags_Borders);
    ImVec2 previewMin = ImGui::GetWindowPos();
    ImVec2 previewMax(previewMin.x + ImGui::GetWindowSize().x, previewMin.y + ImGui::GetWindowSize().y);
    ImDrawList* previewDraw = ImGui::GetWindowDrawList();
    ImVec4 previewBase = m_resolvedLightTheme ? ImVec4(0.94f, 0.96f, 0.99f, 1.0f)
        : ImVec4(0.025f, 0.032f, 0.050f, 1.0f);
    auto previewTint = [&](const ImVec4& accent) {
        float amount = (m_resolvedLightTheme ? 0.12f : 0.23f) * m_theme.gradientStrength;
        return ImVec4(previewBase.x + (accent.x - previewBase.x) * amount,
            previewBase.y + (accent.y - previewBase.y) * amount,
            previewBase.z + (accent.z - previewBase.z) * amount, 1.0f);
    };
    ImVec4 previewLeft = previewTint(m_theme.gradientPrimary);
    ImVec4 previewRight = previewTint(m_theme.gradientSecondary);
    previewDraw->AddRectFilledMultiColor(previewMin, previewMax,
        ImGui::ColorConvertFloat4ToU32(previewLeft), ImGui::ColorConvertFloat4ToU32(previewRight),
        ImGui::ColorConvertFloat4ToU32(previewRight), ImGui::ColorConvertFloat4ToU32(previewLeft));
    previewDraw->AddText(ImVec2(previewMin.x + 14.0f, previewMin.y + 18.0f),
        ImGui::GetColorU32(ImGuiCol_Text), "Live gradient preview");
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextColored(m_theme.gradientPrimary, "Font Size");
    ImGui::Separator();
    ImGui::TextWrapped("Adjust text separately from app scaling. Rows and controls make room for larger text.");
    if (m_fontScalePreview < 0) m_fontScalePreview = m_theme.fontScale;
    int fontStep = std::clamp((int)std::round((m_fontScalePreview * 100.0f - 75.0f) / 5.0f), 0, 25);
    char fontLabel[16];
    snprintf(fontLabel, sizeof(fontLabel), "%d%%%%", fontStep * 5 + 75);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##FontSize", &fontStep, 0, 25, fontLabel, ImGuiSliderFlags_NoInput))
        m_fontScalePreview = (fontStep * 5 + 75) / 100.0f;
    bool fontChanged = std::abs(m_fontScalePreview - m_theme.fontScale) > 0.001f;
    ImGui::BeginDisabled(!fontChanged);
    if (modernButton("Apply font size")) {
        m_theme.fontScale = m_fontScalePreview;
        m_pendingScale = m_theme.userScale;
        m_themeNeedsReposition = true;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (modernButton("Reset##fontSize")) {
        m_fontScalePreview = m_theme.fontScale = 1.0f;
        m_pendingScale = m_theme.userScale;
        m_themeNeedsReposition = true;
        changed = true;
    }
    ImGui::TextDisabled("Current font size: %.0f%%", m_theme.fontScale * 100.0f);
    ImGui::Spacing();
    ImGui::Spacing();

    // --- UI Scale ---
    ImGui::TextColored(m_theme.gradientPrimary, "UI Scale");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextDisabled("System DPI: %.0f%%", m_systemDpiScale * 100.0f);

    if (m_scalePreview < 0) m_scalePreview = m_theme.userScale;

    // Discrete 5% steps: index 0=50%, 1=55%, ..., 50=300%
    int step = ((int)(m_scalePreview * 100.0f + 0.5f) - 50) / 5;
    if (step < 0) step = 0; if (step > 50) step = 50;
    ImGui::TextUnformatted("App Scale");
    ImGui::SetNextItemWidth(-1);
    char stepLabel[16]; snprintf(stepLabel, sizeof(stepLabel), "%d%%%%", step * 5 + 50);
    if (ImGui::SliderInt("##AppScale", &step, 0, 50, stepLabel, ImGuiSliderFlags_NoInput)) {
        m_scalePreview = (step * 5 + 50) / 100.0f;
    }

    int scalePct = step * 5 + 50;
    int currentPct = ((int)(m_theme.userScale * 100.0f + 0.5f) / 5) * 5;
    bool scaleChanged = (scalePct != currentPct);

    if (scaleChanged) {
        if (modernButton("Apply", ImVec2(0, 0), ButtonTone::Primary)) {
            m_pendingScale = m_scalePreview;
            m_themeNeedsReposition = true;
            changed = true;
        }
        ImGui::SameLine();
    }
    if (modernButton("Reset##scale")) {
        m_scalePreview = 1.0f;
        if (m_theme.userScale != 1.0f) {
            m_pendingScale = 1.0f;
            m_themeNeedsReposition = true;
            changed = true;
        }
    }
    ImGui::TextDisabled("  Current: %.0f%%  %s", m_systemDpiScale * m_theme.userScale * 100.0f,
                        scaleChanged ? "(unsaved)" : "");

    ImGui::Spacing();
    ImGui::Spacing();

    // --- Colors ---
    ImGui::TextColored(m_theme.gradientSecondary, "Colors");
    ImGui::Separator();
    ImGui::Spacing();

    if (!m_theme.customColors) {
        ImGui::TextDisabled("Using the modern adaptive design system.");
        ImGui::Spacing();
        if (modernButton("Customize Colors")) {
            // Copy current colors into theme for editing
            ImVec4* c = ImGui::GetStyle().Colors;
            for (int i = 0; i < ImGuiCol_COUNT; i++)
                m_theme.colors[i] = c[i];
            m_theme.customColors = true;
            changed = true;
        }
    } else {
        if (modernButton("Reset to Default Theme")) {
            m_theme.customColors = false;
            setupStyle();
            // Re-apply scale
            ImGui::GetStyle().ScaleAllSizes(m_systemDpiScale * m_theme.userScale);
            if (ImGui::GetStyle().WindowMinSize.x < 1.0f) ImGui::GetStyle().WindowMinSize.x = 1.0f;
            if (ImGui::GetStyle().WindowMinSize.y < 1.0f) ImGui::GetStyle().WindowMinSize.y = 1.0f;
            changed = true;
        }

        ImGui::Spacing();

        // Color editor — grouped by category
        ImVec4* c = ImGui::GetStyle().Colors;

        struct ColorGroup {
            const char* name;
            std::vector<std::pair<int, const char*>> entries;
        };

        ColorGroup groups[] = {
            {"Backgrounds", {
                {ImGuiCol_WindowBg, "Window"},
                {ImGuiCol_ChildBg, "Child"},
                {ImGuiCol_PopupBg, "Popup"},
                {ImGuiCol_MenuBarBg, "Menu Bar"},
            }},
            {"Text", {
                {ImGuiCol_Text, "Text"},
                {ImGuiCol_TextDisabled, "Text Disabled"},
                {ImGuiCol_TextSelectedBg, "Text Selection"},
            }},
            {"Borders", {
                {ImGuiCol_Border, "Border"},
                {ImGuiCol_Separator, "Separator"},
                {ImGuiCol_SeparatorHovered, "Separator Hovered"},
                {ImGuiCol_SeparatorActive, "Separator Active"},
            }},
            {"Buttons", {
                {ImGuiCol_Button, "Button"},
                {ImGuiCol_ButtonHovered, "Button Hovered"},
                {ImGuiCol_ButtonActive, "Button Active"},
            }},
            {"Frames (Inputs)", {
                {ImGuiCol_FrameBg, "Frame"},
                {ImGuiCol_FrameBgHovered, "Frame Hovered"},
                {ImGuiCol_FrameBgActive, "Frame Active"},
            }},
            {"Headers (Rows)", {
                {ImGuiCol_Header, "Header"},
                {ImGuiCol_HeaderHovered, "Header Hovered"},
                {ImGuiCol_HeaderActive, "Header Active"},
            }},
            {"Tabs", {
                {ImGuiCol_Tab, "Tab"},
                {ImGuiCol_TabHovered, "Tab Hovered"},
                {ImGuiCol_TabSelected, "Tab Selected"},
            }},
            {"Title Bar", {
                {ImGuiCol_TitleBg, "Title"},
                {ImGuiCol_TitleBgActive, "Title Active"},
            }},
            {"Scrollbar", {
                {ImGuiCol_ScrollbarBg, "Background"},
                {ImGuiCol_ScrollbarGrab, "Grab"},
                {ImGuiCol_ScrollbarGrabHovered, "Grab Hovered"},
                {ImGuiCol_ScrollbarGrabActive, "Grab Active"},
            }},
            {"Tables", {
                {ImGuiCol_TableHeaderBg, "Header"},
                {ImGuiCol_TableBorderStrong, "Border Strong"},
                {ImGuiCol_TableBorderLight, "Border Light"},
                {ImGuiCol_TableRowBg, "Row"},
                {ImGuiCol_TableRowBgAlt, "Row Alt"},
            }},
            {"Accents", {
                {ImGuiCol_CheckMark, "Check Mark"},
                {ImGuiCol_SliderGrab, "Slider"},
                {ImGuiCol_SliderGrabActive, "Slider Active"},
                {ImGuiCol_NavHighlight, "Nav Highlight"},
                {ImGuiCol_ResizeGrip, "Resize Grip"},
                {ImGuiCol_ResizeGripHovered, "Resize Grip Hovered"},
                {ImGuiCol_ResizeGripActive, "Resize Grip Active"},
            }},
        };

        ImGui::BeginChild("##ColorEditor", ImVec2(0, -40), ImGuiChildFlags_Border);
        for (auto& group : groups) {
            if (ImGui::TreeNode(group.name)) {
                for (auto& [idx, label] : group.entries) {
                    if (ImGui::ColorEdit4(label, &c[idx].x,
                            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf)) {
                        m_theme.colors[idx] = c[idx];
                        changed = true;
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();

    if (changed) {
        m_theme.save();
    }

    ImGui::End();
}

void App::renderDebugWindow() {
    const float s=ui::scale();
    auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(1000*s,viewport->WorkSize.x-32*s),std::min(560*s,viewport->WorkSize.y-32*s)),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(580*s,300*s),viewport->WorkSize);
    if(!ImGui::Begin("Activity log (F12)",&m_showDebugWindow)){ImGui::End();return;}
    auto& logger=DebugLog::instance();
    auto entries=logger.snapshot();
    auto lower=[](std::string value){for(char& c:value)c=(char)std::tolower((unsigned char)c);return value;};
    std::string query=lower(m_debugTagFilter);
    std::vector<int> visible;
    int errors=0,warnings=0;
    for(int i=0;i<(int)entries.size();++i) {
        const auto& entry=entries[i];
        if(entry.level==LogLevel::Error)++errors;
        if(entry.level==LogLevel::Warn)++warnings;
        if((int)entry.level<m_debugLevelFilter)continue;
        if(!query.empty()&&lower(entry.tag+" "+entry.message).find(query)==std::string::npos)continue;
        visible.push_back(i);
    }
    auto exportText=[&](){std::string text;for(int index:visible)text+=DebugLog::format(entries[index])+"\r\n";return text;};
    if(ui::button("Copy visible",ui::Icon::Copy,ui::Appearance::Secondary))ImGui::SetClipboardText(exportText().c_str());
    ImGui::SameLine(0,8*s);
    if(ui::button("Export visible",ui::Icon::File,ui::Appearance::Secondary)) {
        wchar_t path[32768]=L"FastEnough-activity.txt";
        OPENFILENAMEW dialog{};
        dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=g_mainHwnd;
        dialog.lpstrFilter=L"Text files (*.txt)\0*.txt\0All files\0*.*\0";
        dialog.lpstrFile=path;dialog.nMaxFile=(DWORD)std::size(path);dialog.lpstrDefExt=L"txt";
        dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(GetSaveFileNameW(&dialog)) {
            std::ofstream output(std::filesystem::path(path),std::ios::binary);
            output<<exportText();output.close();
            if(!output)showNotification("Export failed","Could not write the log to the selected location.",true);
            else LOG_INFO("Activity","Exported activity log: "+pathToUtf8(std::filesystem::path(path)));
        }
    }
    ImGui::SameLine(0,8*s);
    if(ui::button("Log folder",ui::Icon::Folder,ui::Appearance::Secondary)) {
        auto folder=toFsPath(logger.filePath()).parent_path();
        auto result=ShellExecuteW(g_mainHwnd,L"explore",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        if((INT_PTR)result<=32)LOG_ERROR("Activity","Cannot open log folder, ShellExecute error "+std::to_string((INT_PTR)result));
    }
    ImGui::SameLine(0,8*s);
    if(ui::button("Clear view",ui::Icon::Trash,ui::Appearance::Quiet))logger.clear();
    ui::tooltip("Clear the in-app history. Saved log files are kept.");
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10*s,9*s));
    const char* levels[]={"All events","Info and above","Warnings and errors","Errors only"};
    ImGui::SetNextItemWidth(175*s);ui::combo("##LogLevel",&m_debugLevelFilter,levels,4);
    ImGui::SameLine(0,8*s);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-96*s);
    ImGui::InputTextWithHint("##LogSearch","Search operations, paths or errors",m_debugTagFilter,sizeof(m_debugTagFilter));
    ImGui::SameLine(0,8*s);ui::checkbox("Follow",&m_debugAutoScroll);
    ImGui::PopStyleVar();
    ImGui::TextDisabled("%d visible / %d recent events    %d warnings    %d errors",(int)visible.size(),(int)entries.size(),warnings,errors);
    std::string fileError=logger.fileError();
    if(!fileError.empty())ImGui::TextWrapped("Local log unavailable: %s",fileError.c_str());
    ui::textClipped(logger.filePath(),ImGui::GetContentRegionAvail().x,ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ImGui::Separator();
    ImGui::BeginChild("##LogScroll",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin((int)visible.size(),ImGui::GetTextLineHeight()+6*s+ImGui::GetStyle().ItemSpacing.y);
    while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row) {
        const auto& entry=entries[visible[row]];
        std::string line=DebugLog::format(entry);
        ImVec2 origin=ImGui::GetCursorScreenPos();
        ImGui::PushID(row);
        if(ImGui::Selectable("##LogEntry",false,ImGuiSelectableFlags_None,
            ImVec2(std::max(ImGui::GetContentRegionAvail().x,ImGui::CalcTextSize(line.c_str()).x),ImGui::GetTextLineHeight()+6*s)))
            ImGui::SetClipboardText(line.c_str());
        ui::tooltip("Click to copy this event");
        ImU32 color=ImGui::GetColorU32(entry.level==LogLevel::Debug?ImGuiCol_TextDisabled:ImGuiCol_Text);
        if(entry.level==LogLevel::Error)color=ImGui::GetColorU32(m_resolvedLightTheme?ImVec4(.7f,.14f,.18f,1):ImVec4(1,.55f,.57f,1));
        if(entry.level==LogLevel::Warn)color=ImGui::GetColorU32(m_resolvedLightTheme?ImVec4(.55f,.34f,.02f,1):ImVec4(.93f,.75f,.43f,1));
        ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x,origin.y+3*s),color,line.c_str());
        ImGui::PopID();
    }
    if(logger.m_scrollToBottom.exchange(false)&&m_debugAutoScroll)ImGui::SetScrollHereY(1);
    ImGui::EndChild();
    ImGui::End();
}

void App::renderStatusBar() {
    float s=ui::scale();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(18*s,5*s));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));
    ImGui::BeginChild("##StatusBar",ImVec2(0,std::max(36*s,ImGui::GetFontSize()+18*s)),ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_AlwaysUseWindowPadding);
    ImDrawList* draw=ImGui::GetWindowDrawList();
    const ImVec2 barPos=ImGui::GetWindowPos();
    const float barWidth=ImGui::GetWindowWidth(), barHeight=ImGui::GetWindowHeight();
    draw->AddLine(barPos,ImVec2(barPos.x+barWidth,barPos.y),ImGui::GetColorU32(ImGuiCol_Border));
    auto elapsed=std::chrono::steady_clock::now()-m_statusTime;
    const bool busy=m_asyncBusy.load();
    const bool recent=!m_statusMessage.empty()&&elapsed<std::chrono::seconds(15);
    std::string message=busy?m_asyncStatus:recent?m_statusMessage:std::string("Ready");
    std::string lower=message;
    std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return (char)std::tolower(c);});
    const bool problem=lower.find("fail")!=std::string::npos||lower.find("error")!=std::string::npos||lower.find("could not")!=std::string::npos;
    ui::PanelStyle theme(m_resolvedLightTheme);
    const ImVec4 dot=problem?theme.red:busy?theme.gold:theme.green;
    const float centerY=barPos.y+barHeight*0.5f;
    const float x0=ImGui::GetCursorScreenPos().x;
    if(busy) {
        const float pulse=0.55f+0.45f*std::sin((float)ImGui::GetTime()*5.0f);
        ImVec4 halo=dot;halo.w=0.30f*pulse;
        draw->AddCircleFilled(ImVec2(x0+4*s,centerY),7*s,ImGui::GetColorU32(halo));
    }
    draw->AddCircleFilled(ImVec2(x0+4*s,centerY),3.5f*s,ImGui::GetColorU32(dot));
    struct Key{const char* key;const char* label;};
    const Key keys[]={{"F5","Refresh"},{"F2","Rename"},{"Ctrl+L","Folder path"},{"F12","Activity log"}};
    float keysWidth=0;
    for(const auto& key:keys) keysWidth+=ImGui::CalcTextSize(key.key).x+14*s+6*s+ImGui::CalcTextSize(key.label).x+18*s;
    const bool showKeys=ImGui::GetContentRegionAvail().x>keysWidth+260*s;
    const float messageWidth=ImGui::GetContentRegionAvail().x-16*s-(showKeys?keysWidth:0);
    ui::drawTextClipped(ImVec2(x0+16*s,centerY-ImGui::GetTextLineHeight()*0.5f),message,messageWidth,
        ImGui::GetColorU32(problem?theme.red:ImGui::GetStyleColorVec4(ImGuiCol_Text)));
    if(showKeys) {
        float x=barPos.x+barWidth-28*s-keysWidth+18*s;
        for(const auto& key:keys) {
            const ImVec2 keySize=ImGui::CalcTextSize(key.key);
            const ImVec2 capMin(x,centerY-keySize.y*0.5f-3*s), capMax(x+keySize.x+14*s,centerY+keySize.y*0.5f+3*s);
            draw->AddRectFilled(ImVec2(capMin.x,capMin.y+2*s),ImVec2(capMax.x,capMax.y+2*s),ImGui::GetColorU32(ImGuiCol_Border),5*s);
            draw->AddRectFilled(capMin,capMax,ImGui::GetColorU32(ImGuiCol_Button),5*s);
            draw->AddRect(capMin,capMax,ImGui::GetColorU32(ImGuiCol_Border),5*s);
            draw->AddText(ImVec2(capMin.x+7*s,centerY-keySize.y*0.5f),ImGui::GetColorU32(ImGuiCol_Text),key.key);
            x=capMax.x+6*s;
            draw->AddText(ImVec2(x,centerY-ImGui::GetTextLineHeight()*0.5f),ImGui::GetColorU32(ImGuiCol_TextDisabled),key.label);
            x+=ImGui::CalcTextSize(key.label).x+18*s;
        }
    }
    ImGui::Dummy(ImVec2(1,1));
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);
}

void App::renderContextMenu(FilePanel& panel) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10*ui::scale(), 10*ui::scale()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10*ui::scale(), 9*ui::scale()));
    if (ImGui::BeginPopup("##FileContextMenu")) {
        if (m_contextPanel == &panel && panel.validIndex(m_contextIndex)) {
            std::string name = panel.entryName(m_contextIndex);
            ImGui::TextDisabled("%s", name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Open / Navigate")) {
                if (panel.entryIsDir(m_contextIndex)) {
                    std::string sep = panel.isAndroid ? "/" : "\\";
                    std::string np = panel.currentPath;
                    appendPathSeparator(np, sep[0]); np += name;
                    navigateToDirectory(panel, np);
                }
            }
            if (!panel.isAndroid && !panel.entryIsDir(m_contextIndex)) {
                if (ImGui::MenuItem("Open with Default App")) {
                    std::string fp = panel.currentPath;
                    appendPathSeparator(fp, '\\'); fp += name;
                    ShellExecuteA(nullptr, "open", fp.c_str(), nullptr, nullptr, SW_SHOW);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", "Ctrl+C")) {
                m_clipboardPaths.clear();
                std::string sep = panel.isAndroid ? "/" : "\\";
                for (int idx : panel.selectedIndices) {
                    if (!panel.validIndex(idx)) continue;
                    std::string p = panel.currentPath;
                    appendPathSeparator(p, sep[0]);
                    p += panel.entryName(idx);
                    m_clipboardPaths.push_back(p);
                }
                m_clipboardIsAndroid = panel.isAndroid;
                m_clipboardDeviceSlot = panel.deviceSlot;
                m_clipboardCut = false;
            }
            if (ImGui::MenuItem("Cut", "Ctrl+X")) {
                m_clipboardPaths.clear();
                std::string sep = panel.isAndroid ? "/" : "\\";
                for (int idx : panel.selectedIndices) {
                    if (!panel.validIndex(idx)) continue;
                    std::string p = panel.currentPath;
                    appendPathSeparator(p, sep[0]);
                    p += panel.entryName(idx);
                    m_clipboardPaths.push_back(p);
                }
                m_clipboardIsAndroid = panel.isAndroid;
                m_clipboardDeviceSlot = panel.deviceSlot;
                m_clipboardCut = true;
            }
            if (!m_clipboardPaths.empty()) {
                const char* pasteLabel = m_clipboardCut ? "Move Here" : "Paste";
                if (ImGui::MenuItem(pasteLabel, "Ctrl+V")) {
                    performClipboardPaste(panel);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy to Other Panel")) {
                if (&panel == &m_leftPanel) startTransfer(false); else startTransfer(true);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Rename...")) { strcpy_s(m_renameBuf, name.c_str()); m_showRenamePopup = true; }
            if (ImGui::MenuItem("Delete", "Del")) {
                m_deletePermanent = ImGui::GetIO().KeyShift;
                m_showDeleteConfirm = true;
            }
            if (ImGui::MenuItem("Delete Permanently", "Shift+Del")) {
                m_deletePermanent = true;
                m_showDeleteConfirm = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("New Folder...")) {
                memset(m_newFolderName, 0, sizeof(m_newFolderName));
                m_contextPanel = &panel; m_showNewFolderPopup = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void App::renderNewFolderPopup() {
    if (m_showNewFolderPopup) { ImGui::OpenPopup("New Folder"); m_showNewFolderPopup = false; m_newFolderNeedsFocus = true; }
    if (ui::beginDialog("New Folder", 460)) {
        const float s = ui::scale();
        const std::string location = m_contextPanel ? m_contextPanel->currentPath : std::string();
        bool cancel = ui::dialogHeader(ui::Icon::Folder, ui::accentColor(ui::Accent::Info), "New folder",
            location.empty() ? std::string("Create a folder here") : "Create in " + location, true);
        ui::sectionLabel("FOLDER NAME");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 8 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7 * s);
        if (m_newFolderNeedsFocus) { ImGui::SetKeyboardFocusHere(); m_newFolderNeedsFocus = false; }
        bool enter = ImGui::InputTextWithHint("##NFN", "New folder", m_newFolderName, sizeof(m_newFolderName), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleVar(2);
        const float buttonH = ui::footerButtonHeight();
        const float buttonW = 110 * s;
        ui::dialogFooter(buttonW * 2 + ImGui::GetStyle().ItemSpacing.x);
        ImGui::BeginDisabled(strlen(m_newFolderName) == 0);
        bool createPressed = modernButton("Create", ImVec2(buttonW, buttonH), ButtonTone::Primary);
        ImGui::EndDisabled();
        if ((createPressed || enter) && strlen(m_newFolderName) > 0 && m_contextPanel) {
            if (m_contextPanel->isAndroid) {
                std::string mkdirPath = m_contextPanel->currentPath + "/" + m_newFolderName;
                if (m_selectedDevice >= 0) { int slot = m_contextPanel->deviceSlot; FilePanel* target=m_contextPanel; postAsync("Creating folder: "+mkdirPath, [this, mkdirPath, slot, target]() {
                    auto& device=deviceForSlot(slot);
                    bool ok=device.createDirectory(mkdirPath);
                    DebugLog::instance().log(ok?LogLevel::Info:LogLevel::Error,"Files",
                        std::string(ok?"Created folder: ":"Create folder failed: ")+mkdirPath+(ok?"":": "+device.lastError()));
                    UiMessage message;
                    message.hasStatus=true;
                    message.status=ok?"Created folder: "+mkdirPath:"Create folder failed: "+device.lastError();
                    message.refreshFilePanel=target;
                    postUiMessage(std::move(message));
                }); }
            } else {
                std::string p = m_contextPanel->currentPath; appendPathSeparator(p, '\\'); p+=m_newFolderName;
                std::error_code error;
                bool created=std::filesystem::create_directories(toFsPath(p),error);
                DebugLog::instance().log(error?LogLevel::Error:LogLevel::Info,"Files",
                    std::string(error?"Create folder failed: ":created?"Created folder: ":"Folder already exists: ")+p+(error?": "+error.message():""));
                if (error) showNotification("Create folder failed",p+": "+error.message(),true);
            }
            m_contextPanel->needsRefresh = true; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (modernButton("Cancel##NewFolderCancel", ImVec2(buttonW, buttonH)) || cancel) ImGui::CloseCurrentPopup();
        ui::endDialog();
    }
}

void App::renderRenamePopup() {
    if (m_showRenamePopup) { ImGui::OpenPopup("Rename"); m_showRenamePopup = false; m_renameNeedsSelect = true; }
    if (ui::beginDialog("Rename", 460)) {
        const float s = ui::scale();
        const std::string currentName = m_contextPanel && m_contextPanel->validIndex(m_contextIndex)
            ? m_contextPanel->entryName(m_contextIndex) : std::string();
        bool cancel = ui::dialogHeader(ui::Icon::File, ui::accentColor(ui::Accent::Info), "Rename",
            currentName.empty() ? std::string() : "Current name: " + currentName, true);
        ui::sectionLabel("NEW NAME");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 8 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7 * s);
        if (m_renameNeedsSelect) ImGui::SetKeyboardFocusHere();
        auto renameCb = [](ImGuiInputTextCallbackData* data) -> int {
            bool* needsSelect = (bool*)data->UserData;
            if (*needsSelect) {
                *needsSelect = false;
                const char* dot = strrchr(data->Buf, '.');
                int selectEnd = dot ? (int)(dot - data->Buf) : data->BufTextLen;
                data->SelectionStart = 0;
                data->SelectionEnd = selectEnd;
                data->CursorPos = selectEnd;
            }
            return 0;
        };
        bool enter = ImGui::InputText("##RN", m_renameBuf, sizeof(m_renameBuf),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
            renameCb, &m_renameNeedsSelect);
        ImGui::PopStyleVar(2);
        const float buttonH = ui::footerButtonHeight();
        const float buttonW = 110 * s;
        ui::dialogFooter(buttonW * 2 + ImGui::GetStyle().ItemSpacing.x);
        if ((modernButton("Rename##RenameConfirm", ImVec2(buttonW, buttonH), ButtonTone::Primary) || enter) && m_contextPanel && m_contextPanel->validIndex(m_contextIndex)) {
            std::string oldN = m_contextPanel->entryName(m_contextIndex), requestedN(m_renameBuf);
            std::string newN = makeUniquePanelName(*m_contextPanel, requestedN, m_contextIndex);
            if (!newN.empty() && newN != oldN) {
                if (m_contextPanel->isAndroid && m_selectedDevice >= 0) {
                    std::string fromP = m_contextPanel->currentPath+"/"+oldN, toP = m_contextPanel->currentPath+"/"+newN;
                    int slot = m_contextPanel->deviceSlot;
                    FilePanel* targetPanel = m_contextPanel;
                    postAsync("Renaming...", [this, fromP, toP, newN, slot, targetPanel]() {
                        DeviceClient& device = deviceForSlot(slot);
                        UiMessage message;
                        if (device.renameFile(fromP, toP)) {
                            message.hasStatus = true;
                            message.status = "Renamed to " + newN;
                            LOG_INFO("Files","Renamed: "+fromP+" -> "+toP);
                        } else {
                            message.hasNotification = true;
                            message.notificationTitle = "Rename Failed";
                            LOG_ERROR("Files","Rename failed: "+fromP+" -> "+toP+": "+device.lastError());
                            message.notificationMessage = device.lastError();
                            message.notificationIsError = true;
                        }
                        message.refreshFilePanel = targetPanel;
                        postUiMessage(std::move(message));
                    });
                } else {
                    std::string bp = m_contextPanel->currentPath; appendPathSeparator(bp, '\\');
                    try {
                        std::filesystem::rename(toFsPath(bp+oldN), toFsPath(bp+newN));
                        LOG_INFO("Files","Renamed: "+bp+oldN+" -> "+bp+newN);
                        m_statusMessage = "Renamed to " + newN;
                        m_statusTime = std::chrono::steady_clock::now();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Files","Rename failed: "+bp+oldN+" -> "+bp+newN+": "+e.what());
                        m_statusMessage = std::string("Rename failed: ") + e.what(); m_statusTime = std::chrono::steady_clock::now();
                    }
                }
                m_contextPanel->needsRefresh = true;
            }
            m_contextIndex = -1; m_contextPanel = nullptr; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (modernButton("Cancel##RenameCancel", ImVec2(buttonW, buttonH)) || cancel) ImGui::CloseCurrentPopup();
        ui::endDialog();
    }
}

void App::renderDeleteConfirmPopup() {
    if (m_showDeleteConfirm) { ImGui::OpenPopup("Confirm Delete"); m_showDeleteConfirm = false; }
    if (ui::beginDialog("Confirm Delete", 500)) {
        const float s = ui::scale();
        const float buttonH = ui::footerButtonHeight();
        ui::PanelStyle theme(m_resolvedLightTheme);
        if (m_contextPanel && !m_contextPanel->selectedIndices.empty()) {
            int selCount = (int)m_contextPanel->selectedIndices.size();
            bool isAndroid = m_contextPanel->isAndroid;
            const bool permanent = m_deletePermanent || isAndroid;
            const std::string itemWord = selCount == 1 ? "1 item" : std::to_string(selCount) + " items";
            bool cancel = ui::dialogHeader(ui::Icon::Trash,
                ui::accentColor(permanent ? ui::Accent::Danger : ui::Accent::Warning),
                permanent ? "Delete permanently?" : "Move to Recycle Bin?",
                permanent ? itemWord + " will be deleted. This cannot be undone."
                          : itemWord + " will be moved to the Recycle Bin.", true);

            // Show file names (up to 10, then summarize)
            ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.background);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * s);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * s, 10 * s));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 6 * s));
            const float rowH = ImGui::GetTextLineHeightWithSpacing();
            const int listedRows = std::min(selCount, 10) + (selCount > 10 ? 1 : 0);
            ImGui::BeginChild("##DeleteList", ImVec2(0, listedRows * rowH + 20 * s), ImGuiChildFlags_Borders);
            int shown = 0;
            for (int idx : m_contextPanel->selectedIndices) {
                if (!m_contextPanel->validIndex(idx)) continue;
                if (shown < 10) {
                    std::string n = m_contextPanel->entryName(idx);
                    const ImVec2 iconPos = ImGui::GetCursorScreenPos();
                    ui::icon(m_contextPanel->entryIsDir(idx) ? ui::Icon::Folder : ui::Icon::File,
                        ImVec2(iconPos.x, iconPos.y + 1 * s), ImGui::GetTextLineHeight() - 2 * s, theme.color(theme.muted));
                    ImGui::Dummy(ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
                    ImGui::SameLine();
                    ui::textClipped(n, ImGui::GetContentRegionAvail().x, theme.color(theme.text));
                }
                shown++;
            }
            if (selCount > 10) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
                ImGui::Text("and %d more", selCount - 10);
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();

            // Hint about Shift
            if (!isAndroid && !m_deletePermanent) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
                ImGui::TextUnformatted("Tip: hold Shift while deleting to skip the Recycle Bin.");
                ImGui::PopStyleColor();
            }

            // Delete button
            const char* btnLabel = permanent ? "Delete Permanently" : "Move to Recycle Bin";
            const float deleteW = 180 * s;
            const float cancelW = 110 * s;
            ui::dialogFooter(deleteW + cancelW + ImGui::GetStyle().ItemSpacing.x);
            if (modernButton(btnLabel, ImVec2(deleteW, buttonH), ButtonTone::Danger)) {
                if (isAndroid) {
                    // Android: always permanent via server, refresh after the queued work finishes.
                    std::vector<std::string> delPaths;
                    for (int idx : m_contextPanel->selectedIndices) {
                        if (!m_contextPanel->validIndex(idx)) continue;
                        delPaths.push_back(m_contextPanel->currentPath + "/" + m_contextPanel->entryName(idx));
                    }
                    if (m_selectedDevice >= 0 && !delPaths.empty()) {
                        int slot = m_contextPanel->deviceSlot;
                        bool refreshLeft = (m_contextPanel == &m_leftPanel);
                        m_contextPanel->selectedIndices.clear();
                        postAsync("Deleting files...", [this, delPaths, slot, refreshLeft]() {
                            int failed = 0;
                            std::string lastError;
                            for (auto& p : delPaths) {
                                if (!deviceForSlot(slot).deleteFile(p)) {
                                    failed++;
                                    lastError = deviceForSlot(slot).lastError();
                                    LOG_ERROR("Files","Delete failed: "+p+": "+lastError);
                                } else LOG_INFO("Files","Deleted: "+p);
                            }
                            FilePanel& panel = refreshLeft ? m_leftPanel : m_rightPanel;
                            panel.needsRefresh = true;
                            if (failed == 0) {
                                m_statusMessage = "Deleted " + std::to_string(delPaths.size()) + " item(s)";
                            } else {
                                m_statusMessage = "Deleted " + std::to_string((int)delPaths.size() - failed) +
                                    " item(s), " + std::to_string(failed) + " failed";
                                if (!lastError.empty()) m_statusMessage += ": " + lastError;
                            }
                            m_statusTime = std::chrono::steady_clock::now();
                        });
                    }
                } else {
                    // Windows: use IFileOperation (recycle bin or permanent)
                    std::vector<std::string> paths;
                    for (int idx : m_contextPanel->selectedIndices) {
                        if (!m_contextPanel->validIndex(idx)) continue;
                        std::string p = m_contextPanel->currentPath;
                        appendPathSeparator(p, '\\');
                        p += m_contextPanel->entryName(idx);
                        paths.push_back(p);
                    }
                    for (const auto& path:paths) LOG_INFO("Files",std::string(m_deletePermanent?"Permanent delete requested: ":"Recycle requested: ")+path);
                    std::string error;
                    if (!deleteWindowsFiles(paths, m_deletePermanent, error)) {
                        LOG_ERROR("Files","Delete failed or cancelled: "+error);
                        m_statusMessage = "Delete failed: " + error;
                        m_statusTime = std::chrono::steady_clock::now();
                    } else LOG_INFO("Files","Delete completed, "+std::to_string(paths.size())+" items");
                    m_contextPanel->needsRefresh = true;
                }
                m_contextIndex = -1; m_contextPanel = nullptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (modernButton("Cancel##DeleteCancel", ImVec2(cancelW, buttonH)) || cancel) {
                m_contextIndex = -1; m_contextPanel = nullptr;
                ImGui::CloseCurrentPopup();
            }
        } else {
            bool cancel = ui::dialogHeader(ui::Icon::Trash, ui::accentColor(ui::Accent::Info),
                "Nothing to delete", "No items are selected.", true);
            const float closeW = 110 * s;
            ui::dialogFooter(closeW);
            if (modernButton("Close##DeleteClose", ImVec2(closeW, buttonH), ButtonTone::Primary) || cancel) {
                m_contextIndex = -1; m_contextPanel = nullptr;
                ImGui::CloseCurrentPopup();
            }
        }
        ui::endDialog();
    }
}

void App::performClipboardPaste(FilePanel& dstPanel) {
    if (m_clipboardPaths.empty()) return;
    if (dstPanel.isApps) {
        m_statusMessage = "App list does not accept file paste. Use APK install instead.";
        m_statusTime = std::chrono::steady_clock::now();
        return;
    }

    bool dstIsAndroid = dstPanel.isAndroid;
    bool srcIsAndroid = m_clipboardIsAndroid;
    bool isCut = m_clipboardCut;
    std::vector<std::string> paths = m_clipboardPaths;
    std::string dstDir = dstPanel.currentPath;
    int srcSlot = m_clipboardDeviceSlot;
    int dstSlot = dstPanel.deviceSlot;

    if (!srcIsAndroid && !dstIsAndroid) {
        // Windows to Windows — local filesystem copy/move
        postAsync(isCut ? "Moving files..." : "Copying files...", [this, paths, dstDir, isCut]() {
            for (auto& src : paths) {
                try {
                    auto srcPath = toFsPath(src);
                    std::string filename = pathToUtf8(srcPath.filename());
                    std::string dst = dstDir;
                    appendPathSeparator(dst, '\\');
                    dst += filename;
                    auto dstPath = toFsPath(dst);
                    if (isCut) {
                        std::filesystem::rename(srcPath, dstPath);
                    } else {
                        if (std::filesystem::is_directory(srcPath))
                            std::filesystem::copy(srcPath, dstPath, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
                        else
                            std::filesystem::copy_file(srcPath, dstPath, std::filesystem::copy_options::overwrite_existing);
                    }
                    LOG_INFO("Files",std::string(isCut?"Moved: ":"Copied: ")+src+" -> "+dst);
                } catch (const std::exception& e) {
                    LOG_ERROR("Files","Paste failed: "+src+" -> "+dstDir+": "+e.what());
                    m_statusMessage = std::string("Paste failed: ") + e.what();
                    m_statusTime = std::chrono::steady_clock::now();
                }
            }
        });
        dstPanel.needsRefresh = true;
        if (isCut) m_clipboardPaths.clear();
    } else if (srcIsAndroid && dstIsAndroid && srcSlot == dstSlot) {
        // Same Android device — move via rename, copy via adb shell cp
        postAsync(isCut ? "Moving files..." : "Copying files...", [this, paths, dstDir, isCut, srcSlot]() {
            for (auto& src : paths) {
                std::string filename = src.substr(src.rfind('/') + 1);
                std::string dst = dstDir;
                appendPathSeparator(dst, '/');
                dst += filename;
                if (isCut) {
                    deviceForSlot(srcSlot).renameFile(src, dst);
                } else {
                    // No server-side copy command; use adb shell cp -r
                    std::string cmd = "shell cp -r \"" + src + "\" \"" + dst + "\"";
                    deviceForSlot(srcSlot).runAdbCommand(cmd);
                }
            }
        });
        dstPanel.needsRefresh = true;
        if (isCut) m_clipboardPaths.clear();
    } else {
        // Cross-platform or cross-device — use the transfer system
        // Build a temporary selection in a virtual source panel and trigger transfer
        // For now, show a message directing users to use Copy to Other Panel
        m_statusMessage = "Cross-platform paste: use Copy to Other Panel or drag-and-drop";
        m_statusTime = std::chrono::steady_clock::now();
    }
}

void App::refreshWindowsPanel(FilePanel& panel) {
    std::string mountPoint;
    if (isOwnMountedDevicePath(panel.currentPath, &mountPoint)) {
        std::string blockedPath = panel.currentPath;
        panel.currentPath = "C:\\";
        strcpy_s(panel.pathInput, panel.currentPath.c_str());
        panel.navHistory.erase(std::remove_if(panel.navHistory.begin(), panel.navHistory.end(),
            [this](const std::string& value) { return isOwnMountedDevicePath(value); }), panel.navHistory.end());
        if (panel.navHistory.empty() || panel.navHistory.back() != panel.currentPath)
            panel.navHistory.push_back(panel.currentPath);
        panel.navHistoryPos = (int)panel.navHistory.size() - 1;
        m_statusMessage = "Blocked recursive access to " + mountPoint + ". Use the Android pane instead.";
        m_statusTime = std::chrono::steady_clock::now();
        showNotification("Mounted Drive Opened in PC Pane",
            "Fast Enough cannot browse " + blockedPath +
            " from its own PC pane because the drive is provided by this application. "
            "Use the Android pane inside Fast Enough, or browse the mounted drive in Windows Explorer.", false);
        LOG_WARN("Dokan", "Blocked recursive PC pane access to own mount: " + blockedPath);
        return;
    }
    if (panel.refreshInProgress) return;
    panel.refreshInProgress = true;
    panel.needsRefresh = false;
    const auto generation = ++panel.listingGeneration;
    const int col = panel.sortColumn;
    const bool desc = panel.sortDescending;
    const int slot = panel.deviceSlot;
    const std::string path = panel.currentPath;
    FilePanel* target = &panel;
    postListing(target, [this, target, generation, slot, path, col, desc] {
        auto started = std::chrono::steady_clock::now();
        auto entries = listWindowsDirectory(toFsPath(path), [this] { return m_shutdownPoll.load(); });
        std::sort(entries.begin(), entries.end(), [col, desc](const WindowsFileEntry& a, const WindowsFileEntry& b) {
            if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
            int cmp = col == 1 ? (a.size < b.size ? -1 : a.size > b.size ? 1 : 0) :
                col == 2 ? a.dateModified.compare(b.dateModified) : _stricmp(a.name.c_str(), b.name.c_str());
            return desc ? cmp > 0 : cmp < 0;
        });
        LOG_DEBUG("Perf", "Listed " + std::to_string(entries.size()) + " PC entries in " +
            std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()) + " ms");
        UiMessage message;
        message.listingPanel = target;
        message.listingGeneration = generation;
        message.listingSlot = slot;
        message.listingPath = path;
        message.listingWindows = true;
        message.listingWindowsEntries = std::move(entries);
        postUiMessage(std::move(message));
    });
}

void App::refreshAndroidPanel(FilePanel& panel) {
    panel.androidEntries.clear(); panel.selectedIndices.clear();
    DeviceClient& dev = deviceFor(panel);
    if (!dev.isServerRunning()) return;

    if (panel.insideMcraw) {
        panel.androidEntries = dev.listMcraw(panel.mcrawFilePath);
    } else {
        panel.androidEntries = dev.listDirectory(panel.currentPath);
    }
    int col = panel.sortColumn;
    bool desc = panel.sortDescending;
    std::sort(panel.androidEntries.begin(), panel.androidEntries.end(),
        [col, desc](const DeviceFileEntry& a, const DeviceFileEntry& b) {
            if (a.isDirectory() != b.isDirectory()) return a.isDirectory();
            int cmp;
            if (col == 1) cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
            else if (col == 2) cmp = (a.mtime < b.mtime) ? -1 : (a.mtime > b.mtime) ? 1 : 0;
            else cmp = _stricmp(a.name.c_str(), b.name.c_str());
            return desc ? cmp > 0 : cmp < 0;
        });
    strcpy_s(panel.pathInput, panel.currentPath.c_str());
}

void App::refreshAppsPanel(FilePanel& panel) {
    panel.appEntries.clear();
    panel.selectedIndices.clear();
    DeviceClient& dev = deviceFor(panel);
    if (!dev.isServerRunning()) return;
    std::string serial = deviceSession(panel.deviceSlot).serial;
    panel.appEntries = dev.listInstalledApps(serial, true);
    int col = panel.sortColumn;
    bool desc = panel.sortDescending;
    std::sort(panel.appEntries.begin(), panel.appEntries.end(),
        [col, desc](const InstalledAppEntry& a, const InstalledAppEntry& b) {
            int cmp = 0;
            if (col == 1) cmp = _stricmp(a.packageName.c_str(), b.packageName.c_str());
            else if (col == 2) cmp = (int)a.isSystem - (int)b.isSystem;
            else if (col == 3) cmp = _stricmp(a.apkPath.c_str(), b.apkPath.c_str());
            else {
                const std::string& aName = a.appName.empty() ? a.packageName : a.appName;
                const std::string& bName = b.appName.empty() ? b.packageName : b.appName;
                cmp = _stricmp(aName.c_str(), bName.c_str());
            }
            return desc ? cmp > 0 : cmp < 0;
        });
    strcpy_s(panel.pathInput, "Installed apps");
}

void App::navigateToDirectory(FilePanel& panel, const std::string& path) {
    std::string mountPoint;
    if (!panel.isAndroid && isOwnMountedDevicePath(path, &mountPoint)) {
        m_statusMessage = "Blocked recursive access to " + mountPoint + ". Use the Android pane instead.";
        m_statusTime = std::chrono::steady_clock::now();
        showNotification("Use the Android Pane",
            "The " + mountPoint + " drive is provided by Fast Enough. Browsing it from a PC pane would "
            "route file requests back into the same application. Use the Android pane here, or open the drive "
            "in Windows Explorer.", false);
        LOG_WARN("Dokan", "Blocked navigation to own mount from PC pane: " + path);
        return;
    }
    // Push to navigation history (truncate forward history)
    if (panel.navHistoryPos < 0 || panel.navHistory.empty() || panel.navHistory[panel.navHistoryPos] != path) {
        if (panel.navHistoryPos >= 0 && panel.navHistoryPos < (int)panel.navHistory.size() - 1)
            panel.navHistory.erase(panel.navHistory.begin() + panel.navHistoryPos + 1, panel.navHistory.end());
        panel.navHistory.push_back(path);
        panel.navHistoryPos = (int)panel.navHistory.size() - 1;
        if (panel.navHistory.size() > 50) { panel.navHistory.erase(panel.navHistory.begin()); panel.navHistoryPos--; }
    }
    ++panel.listingGeneration;
    ++panel.entriesRevision;
    panel.refreshInProgress = false;
    panel.windowsEntries.clear();
    panel.androidEntries.clear();
    panel.currentPath = path; panel.selectedIndices.clear(); panel.focusedIndex = -1;
    panel.searchFilter[0] = '\0'; panel.needsRefresh = true;

    // Detect MCRAW virtual directory
    if (endsWithCI(path, ".mcraw")) {
        panel.insideMcraw = true;
        panel.mcrawFilePath = path;
    } else if (panel.insideMcraw) {
        // Keep insideMcraw if we're still within a ProjFS mount (don't reset on sub-navigation)
        // Only clear if we navigated to the parent of the .mcraw file (exited the mount)
        std::filesystem::path mcrawParent = toFsPath(panel.mcrawFilePath).parent_path();
        std::filesystem::path curPath = toFsPath(path);
        // If current path IS the mcraw parent or above it, we've exited
        if (path == pathToUtf8(mcrawParent) || !pathToUtf8(curPath).starts_with(pathToUtf8(mcrawParent))) {
            // Check if we're still inside the mount point (ProjFS path won't contain the mcraw parent)
            // Simple heuristic: if path doesn't start with temp dir, we've left the mount
            char tmpDir[MAX_PATH]; GetTempPathA(MAX_PATH, tmpDir);
            std::string tempBase(tmpDir);
            bool inMountPath = (path.find(tempBase) == 0 || path.find("McrawMount") != std::string::npos);
            if (!inMountPath) {
                // Unmount the ProjFS mount when leaving the MCRAW
                McrawMountManager::instance().unmountAll();
                panel.insideMcraw = false;
                panel.mcrawFilePath.clear();
            }
        }
    }

    panel.navigationTransitionPending = true;
}

bool App::isOwnMountedDevicePath(const std::string& path, std::string* mountPoint) const {
    if (path.size() < 2 || path[1] != ':') return false;
    char pathDrive = (char)std::toupper((unsigned char)path[0]);
    for (int slot = 0; slot < deviceSessionCount(); ++slot) {
        const auto& manager = DeviceMountManager::instance(slot);
        if (!manager.isMounted()) continue;
        const std::string& mounted = manager.mountPoint();
        if (mounted.size() < 2 || mounted[1] != ':') continue;
        if (pathDrive != (char)std::toupper((unsigned char)mounted[0])) continue;
        if (mountPoint) *mountPoint = mounted;
        return true;
    }
    return false;
}

void App::switchPanelMode(FilePanel& panel, bool toAndroid) {
    rememberDeviceView(panel);
    ++panel.listingGeneration;
    panel.refreshInProgress = false;
    panel.pendingDeviceSerial.clear();
    panel.pendingDeviceName.clear();
    panel.deviceOpenError.clear();
    if (!panel.isApps && !panel.isConnections && panel.isAndroid == toAndroid) return;
    panel.isAndroid = toAndroid;
    panel.isApps = false;
    panel.isConnections = false;
    panel.selectedIndices.clear();
    panel.focusedIndex = -1;
    panel.searchFilter[0] = '\0';
    if (panel.insideMcraw) McrawMountManager::instance().unmountAll();
    panel.insideMcraw = false;
    panel.mcrawFilePath.clear();
    panel.navHistory.clear();
    panel.navHistoryPos = -1;
    if (toAndroid) {
        panel.windowsEntries.clear();
        panel.appEntries.clear();
        FilePanel& other = (&panel == &m_leftPanel) ? m_rightPanel : m_leftPanel;
        panel.deviceSlot = 0;
        for (int slot = 0; slot < deviceSessionCount(); ++slot) {
            if (!deviceSession(slot).connected || !deviceForSlot(slot).isServerRunning()) continue;
            panel.deviceSlot = slot;
            if (!(other.isAndroid || other.isApps) || slot != other.deviceSlot) break;
        }
        std::string root = deviceSession(panel.deviceSlot).storageRoot;
        panel.currentPath = root.empty() ? "/" : root;
        strcpy_s(panel.pathInput, panel.currentPath.c_str());
        {
            auto& session = deviceSession(panel.deviceSlot);
            std::lock_guard<std::mutex> lock(session.cacheMutex);
            panel.androidEntries = session.rootEntries;
        }
        panel.needsRefresh = true;
    } else {
        panel.androidEntries.clear();
        panel.appEntries.clear();
        panel.currentPath = "C:\\";
        strcpy_s(panel.pathInput, panel.currentPath.c_str());
        panel.needsRefresh = true;
    }
}

void App::switchPanelToApps(FilePanel& panel) {
    rememberDeviceView(panel);
    ++panel.listingGeneration;
    panel.refreshInProgress = false;
    panel.pendingDeviceSerial.clear();
    panel.pendingDeviceName.clear();
    panel.deviceOpenError.clear();
    if (panel.isApps && !panel.isConnections) return;
    panel.isAndroid = false;
    panel.isApps = true;
    panel.isConnections = false;
    panel.windowsEntries.clear();
    panel.androidEntries.clear();
    panel.selectedIndices.clear();
    panel.focusedIndex = -1;
    panel.searchFilter[0] = '\0';
    if (panel.insideMcraw) McrawMountManager::instance().unmountAll();
    panel.insideMcraw = false;
    panel.mcrawFilePath.clear();
    panel.navHistory.clear();
    panel.navHistoryPos = -1;
    FilePanel& other = (&panel == &m_leftPanel) ? m_rightPanel : m_leftPanel;
    panel.deviceSlot = 0;
    for (int slot = 0; slot < deviceSessionCount(); ++slot) {
        if (!deviceSession(slot).connected || !deviceForSlot(slot).isServerRunning()) continue;
        panel.deviceSlot = slot;
        if (!(other.isAndroid || other.isApps) || slot != other.deviceSlot) break;
    }
    panel.currentPath = "apps://installed";
    strcpy_s(panel.pathInput, "Installed apps");
    panel.needsRefresh = true;
}

void App::switchPanelToConnections(FilePanel& panel) {
    rememberDeviceView(panel);
    ++panel.listingGeneration;
    panel.refreshInProgress = false;
    panel.pendingDeviceSerial.clear();
    panel.pendingDeviceName.clear();
    panel.deviceOpenError.clear();
    if (panel.isConnections) return;
    panel.isAndroid = false;
    panel.isApps = false;
    panel.isConnections = true;
    panel.selectedIndices.clear();
    panel.focusedIndex = -1;
    panel.searchFilter[0] = '\0';
    if (panel.insideMcraw) McrawMountManager::instance().unmountAll();
    panel.insideMcraw = false;
    panel.mcrawFilePath.clear();
    panel.navHistory.clear();
    panel.navHistoryPos = -1;
    panel.needsRefresh = false;
}

void App::forEachAndroidPanel(std::function<void(FilePanel&)> fn) {
    if (m_leftPanel.isAndroid) fn(m_leftPanel);
    if (m_rightPanel.isAndroid) fn(m_rightPanel);
}

void App::forEachDevicePanel(std::function<void(FilePanel&)> fn) {
    if (m_leftPanel.isAndroid || m_leftPanel.isApps) fn(m_leftPanel);
    if (m_rightPanel.isAndroid || m_rightPanel.isApps) fn(m_rightPanel);
    if (m_appsWorkspacePanel.isApps) fn(m_appsWorkspacePanel);
}

void App::navigateUp(FilePanel& panel) {
    if (panel.insideMcraw) {
        // Exit MCRAW virtual directory: go to the parent folder containing the .mcraw file
        std::string parent;
        if (panel.isAndroid) {
            auto pos = panel.mcrawFilePath.rfind('/');
            parent = (pos != std::string::npos && pos > 0) ? panel.mcrawFilePath.substr(0, pos) : "/";
        } else {
            auto p = toFsPath(panel.mcrawFilePath);
            parent = pathToUtf8(p.parent_path());
        }
        navigateToDirectory(panel, parent);
        return;
    }

    std::string parent;
    if (panel.isAndroid) {
        auto pos = panel.currentPath.rfind('/');
        parent = (pos != std::string::npos && pos > 0) ? panel.currentPath.substr(0, pos) : "/";
    } else {
        auto p = toFsPath(panel.currentPath);
        parent = (p.has_parent_path() && p.parent_path() != p) ? pathToUtf8(p.parent_path()) : panel.currentPath;
    }
    if (parent != panel.currentPath)
        navigateToDirectory(panel, parent);
}

void App::devicePollLoop() {
    // Find ADB and clean start on background thread
    std::unique_lock<std::mutex> deviceWork(m_deviceWorkMutex);
    m_pollBusy = true;
    m_statusMessage = "Finding ADB...";
    m_statusTime = std::chrono::steady_clock::now();

    m_device.findAdb();
    // Share ADB path with secondary device client
    if (!m_device.getAdbPath().empty())
        deviceSession(1).client.setAdbPath(m_device.getAdbPath());

    if (!m_device.getAdbPath().empty()) {
        if (m_prefs.restartAdbOnLaunch) {
            m_statusMessage = "Restarting ADB daemon...";
            m_statusTime = std::chrono::steady_clock::now();
            LOG_INFO("ADB", "Restarting ADB daemon (preference enabled)");
            m_device.runAdbCommand("kill-server");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            m_device.runAdbCommand("start-server");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        m_statusMessage = "ADB ready - waiting for device...";
        m_statusTime = std::chrono::steady_clock::now();
    } else {
        m_statusMessage = "ADB not found. Extract the full ZIP or install Android SDK Platform Tools.";
        m_statusTime = std::chrono::steady_clock::now();
    }
    m_pollBusy = false;
    deviceWork.unlock();

    // Helper to check if any batch is active
    auto isBatchActive = [&]() -> bool {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        for (auto& b : m_batchQueue) {
            auto s = b->state.load();
            if (s == BatchState::Preparing || s == BatchState::Running || s == BatchState::Paused || s == BatchState::Queued || s == BatchState::Verifying) return true;
        }
        return false;
    };

    // ADB devices from the last update, merged with companion app devices on every update.
    std::vector<DeviceInfo> lastAdbDevices;
    uint64_t companionGeneration = 0;

    // Helper to process a device list update (from track-devices or getDevices)
    auto processDeviceUpdate = [&](const std::vector<DeviceInfo>& adbDevices) {
        std::lock_guard<std::mutex> deviceUpdate(m_deviceWorkMutex);
        if (m_shutdownPoll) return;
        m_pollBusy = true;
        lastAdbDevices = adbDevices;
        companionGeneration = companion::generation();
        discoverDeviceIdentities(adbDevices);
        linkCompanionDevices(adbDevices);
        std::vector<DeviceInfo> devices = applyConnectionModes(adbDevices);
        std::string curSerial;
        bool primaryServerRunning = m_device.isServerRunning();
        bool primaryListedOnline = false;
        {
            std::lock_guard<std::mutex> lk(m_deviceMutex);
            m_devices = devices;

            // Build list of online devices only
            auto findOnline = [&](bool preferUsb) -> int {
                // First pass: preferred type
                for (int i = 0; i < (int)m_devices.size(); i++) {
                    if (m_devices[i].state != "device") continue;
                    bool isWifi = isWifiSerial(m_devices[i].serial);
                    if (preferUsb ? !isWifi : isWifi) return i;
                }
                // Second pass: any online device
                for (int i = 0; i < (int)m_devices.size(); i++)
                    if (m_devices[i].state == "device") return i;
                return -1;
            };
            const auto identities = m_deviceSessions.identities();
            const std::string primaryIdentity = identities.front();
            int primaryIdx = -1;
            for (int i = 0; i < (int)m_devices.size(); ++i) {
                if (m_devices[i].state == "device" &&
                    deviceIdentity(m_devices[i].serial) == primaryIdentity) {
                    primaryIdx = i;
                    if (!isWifiSerial(m_devices[i].serial)) break;
                }
            }
            if (primaryIdentity.empty()) {
                primaryIdx = findOnline(true);
                if (primaryIdx >= 0) {
                    m_deviceSessions.ensure(deviceIdentity(m_devices[primaryIdx].serial));
                    deviceSession(0).serial = m_devices[primaryIdx].serial;
                    UiMessage message;
                    message.saveDeviceSessions = true;
                    postUiMessage(std::move(message));
                }
            }
            primaryListedOnline = primaryIdx >= 0;
            bool retainingDirectWifiPrimary = primaryIdx < 0 && primaryServerRunning &&
                deviceSession(0).connected && (m_device.isDirectConnection() ||
                    (m_dualChannelAvailable && m_secondaryChannelType == "WiFi" &&
                     m_secondaryChannel.isServerRunning() &&
                     m_secondaryChannel.connectedHost() != "127.0.0.1"));
            m_selectedDevice = primaryIdx;

            curSerial = retainingDirectWifiPrimary ? deviceSession(0).serial :
                (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size()
                                     && m_devices[m_selectedDevice].state == "device")
                ? m_devices[m_selectedDevice].serial : "";

            // If server is still running and serial just flipped USB↔WiFi, keep it
            if (!m_lastDeviceSerial.empty() && !curSerial.empty() &&
                curSerial != m_lastDeviceSerial && m_device.isServerRunning()) {
                // Check if old serial is still online
                for (int i = 0; i < (int)m_devices.size(); i++) {
                    if (m_devices[i].serial == m_lastDeviceSerial && m_devices[i].state == "device") {
                        m_selectedDevice = i;
                        curSerial = m_lastDeviceSerial;
                        break;
                    }
                }
            }
        }

        // Resolve marketing display names for new devices (outside lock, may call adb)
        for (auto& d : devices) {
            if (d.state != "device") continue;
            if (m_deviceDisplayNames.count(d.serial)) continue;
            // First check saved prefs for a cached name
            for (auto& w : m_prefs.savedWifiDevices) {
                if (!w.model.empty() && (w.serial == d.serial ||
                    (w.wifiIp + ":" + std::to_string(w.port)) == d.serial)) {
                    m_deviceDisplayNames[d.serial] = w.model;
                    break;
                }
            }
            if (m_deviceDisplayNames.count(d.serial)) continue;
            // Query the device for its marketing name
            std::string name = queryDeviceDisplayName(d.serial);
            if (!name.empty())
                m_deviceDisplayNames[d.serial] = name;
        }

        // Detect the other connection type appearing → upgrade to true dual channel
        // Case 1: On WiFi, USB cable plugged in → add USB as secondary channel (keep WiFi primary)
        // Triggers when: no dual channel yet, OR dual channel is WiFi-only (upgrade WiFi+WiFi to WiFi+USB)
        if (isWifiSerial(curSerial) && m_device.isServerRunning() &&
            (!m_dualChannelAvailable || m_secondaryChannelType != "USB")) {
            // Find a USB device in the list that belongs to the same physical device
            std::string usbSerial;
            {
                std::lock_guard<std::mutex> lk2(m_deviceMutex);
                // First: try known mapping
                if (!m_wifiToUsbSerial.empty() && curSerial == m_usbToWifiSerial) {
                    for (auto& d : m_devices) {
                        if (d.serial == m_wifiToUsbSerial && d.state == "device") {
                            usbSerial = m_wifiToUsbSerial;
                            break;
                        }
                    }
                }
                if (usbSerial.empty()) {
                    const std::string identity = deviceIdentity(curSerial);
                    for (const auto& device : m_devices) {
                        if (device.state == "device" && !isWifiSerial(device.serial) &&
                            deviceIdentity(device.serial) == identity) {
                            usbSerial = device.serial;
                            break;
                        }
                    }
                }
            }
            if (!usbSerial.empty()) {
                LOG_INFO("Poll", "USB cable connected while on WiFi - adding USB secondary channel via " + usbSerial);
                // Update mapping
                m_wifiToUsbSerial = usbSerial;
                m_usbToWifiSerial = curSerial;
                // Disconnect existing WiFi secondary if upgrading
                if (m_dualChannelAvailable && m_secondaryChannelType != "USB") {
                    m_secondaryChannel.disconnectTcp();
                    m_dualChannelAvailable = false;
                    LOG_INFO("Poll", "Disconnected WiFi secondary to upgrade to USB");
                }
                // Set up ADB forward on the USB serial and connect secondary channel through it
                std::string fwdResult = m_device.runAdbCommand("-s " + usbSerial + " forward tcp:5741 tcp:5740");
                if (fwdResult.find("error") == std::string::npos) {
                    m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                    if (m_secondaryChannel.connectTcp("127.0.0.1", 5741) && m_secondaryChannel.verifyConnection()) {
                        m_dualChannelAvailable = true;
                        m_secondaryChannelType = "USB";
                        m_statusMessage = "Dual channel active: WiFi + USB";
                        LOG_INFO("Poll", "Secondary USB channel established via ADB forward");
                    } else {
                        // Clean up the forward if connection failed
                        m_device.runAdbCommand("-s " + usbSerial + " forward --remove tcp:5741");
                        LOG_WARN("Poll", "USB secondary channel connection failed");
                        // Re-establish WiFi secondary as fallback
                        std::string wifiIp;
                        auto colon = curSerial.rfind(':');
                        if (colon != std::string::npos) wifiIp = curSerial.substr(0, colon);
                        if (!wifiIp.empty() && m_secondaryChannel.connectTcp(wifiIp, AFM_PORT) &&
                            m_secondaryChannel.verifyConnection()) {
                            m_dualChannelAvailable = true;
                            m_secondaryChannelType = "WiFi";
                            LOG_INFO("Poll", "Re-established WiFi secondary as fallback");
                        }
                    }
                } else {
                    LOG_WARN("Poll", "ADB forward for USB secondary failed: " + fwdResult);
                }
            }
        }
        // Case 1b: Secondary is USB but USB device disappeared → downgrade to WiFi secondary
        if (m_dualChannelAvailable && m_secondaryChannelType == "USB" &&
            isWifiSerial(curSerial) && !isBatchActive()) {
            bool usbStillOnline = false;
            {
                std::lock_guard<std::mutex> lk2(m_deviceMutex);
                if (!m_wifiToUsbSerial.empty()) {
                    for (auto& d : m_devices) {
                        if (d.serial == m_wifiToUsbSerial && d.state == "device") {
                            usbStillOnline = true;
                            break;
                        }
                    }
                }
            }
            if (!usbStillOnline) {
                LOG_INFO("Poll", "USB device gone - downgrading secondary from USB to WiFi");
                m_secondaryChannel.disconnectTcp();
                // Clean up ADB forward
                if (!m_wifiToUsbSerial.empty())
                    m_device.runAdbCommand("-s " + m_wifiToUsbSerial + " forward --remove tcp:5741");
                // Re-establish via WiFi
                std::string wifiIp;
                auto colon = curSerial.rfind(':');
                if (colon != std::string::npos) wifiIp = curSerial.substr(0, colon);
                if (!wifiIp.empty() && m_secondaryChannel.connectTcp(wifiIp, AFM_PORT) &&
                    m_secondaryChannel.verifyConnection()) {
                    m_secondaryChannelType = "WiFi";
                    m_statusMessage = "Dual channel: WiFi + WiFi";
                    m_statusTime = std::chrono::steady_clock::now();
                    LOG_INFO("Poll", "Re-established WiFi secondary after USB disconnect");
                } else {
                    m_dualChannelAvailable = false;
                    m_secondaryChannelType.clear();
                    LOG_WARN("Poll", "Failed to re-establish WiFi secondary after USB disconnect");
                    m_statusMessage = "USB disconnected, dual channel disabled";
                    m_statusTime = std::chrono::steady_clock::now();
                }
            }
        }

        // Case 2: On USB, WiFi becomes available → add WiFi as secondary channel
        // Only if the WiFi serial belongs to the SAME device as the current USB serial
        if (m_prefs.wifiAutoConnect && !isWifiSerial(curSerial) && !curSerial.empty() && m_device.isServerRunning() &&
            !m_dualChannelAvailable && !m_usbToWifiSerial.empty() && curSerial == m_wifiToUsbSerial) {
            std::lock_guard<std::mutex> lk2(m_deviceMutex);
            for (int i = 0; i < (int)m_devices.size(); i++) {
                if (m_devices[i].serial == m_usbToWifiSerial && m_devices[i].state == "device") {
                    // WiFi serial appeared — try to establish secondary channel directly
                    LOG_INFO("Poll", "WiFi appeared while on USB - adding WiFi secondary channel");
                    std::string wifiIp;
                    auto colon = m_usbToWifiSerial.rfind(':');
                    if (colon != std::string::npos) wifiIp = m_usbToWifiSerial.substr(0, colon);
                    if (!wifiIp.empty()) {
                        m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                        if (m_secondaryChannel.connectTcp(wifiIp, AFM_PORT) && m_secondaryChannel.verifyConnection()) {
                            m_dualChannelAvailable = true;
                            m_secondaryChannelType = "WiFi";
                            m_statusMessage = "Dual channel active: USB + WiFi";
                            LOG_INFO("Poll", "Secondary WiFi channel established: " + wifiIp);
                        }
                    }
                    break;
                }
            }
        }

        // A direct WiFi transfer channel remains independent of ADB. Promote it
        // before treating an empty ADB device list as a full device disconnect.
        if (!primaryListedOnline && !m_lastDeviceSerial.empty() && deviceSession(0).connected &&
            !m_device.isDirectConnection() && !m_wifiTransitionActive &&
            m_dualChannelAvailable && m_secondaryChannelType == "WiFi") {
            const std::string wifiIp = m_secondaryChannel.connectedHost();
            if (!wifiIp.empty() && wifiIp != "127.0.0.1") {
                LOG_INFO("Poll", "USB disconnected, promoting direct WiFi channel at " + wifiIp);
                if (m_device.connectDirectForSerial(m_lastDeviceSerial, wifiIp)) {
                    curSerial = m_lastDeviceSerial;
                    for (auto& channel : m_extraChannels) {
                        if (channel.isUsb && channel.dev)
                            channel.dev->disconnectTcp();
                    }
                    m_extraChannels.erase(
                        std::remove_if(m_extraChannels.begin(), m_extraChannels.end(),
                            [](const ExtraChannel& channel) { return channel.isUsb; }),
                        m_extraChannels.end());
                    m_activeChannelCount = 2 + (int)m_extraChannels.size();
                    m_statusMessage = "USB disconnected, continuing over WiFi";
                    m_statusTime = std::chrono::steady_clock::now();
                    LOG_INFO("Poll", "WiFi failover complete with " +
                        std::to_string(m_activeChannelCount) + " active channel(s)");
                } else {
                    LOG_WARN("Poll", "Direct WiFi promotion failed after USB disconnect");
                }
            }
        }

        if (curSerial != m_lastDeviceSerial) {
            LOG_INFO("Poll", "Serial changed: '" + m_lastDeviceSerial + "' -> '" + curSerial + "'");
            bool handledSerialChange = false;

            // A paired transport disappeared. The existing TCP client can still
            // report as running even when its ADB forward was tied to the removed
            // USB device, so reconnect it immediately through the surviving path.
            std::set<std::string> previousPrimaryTransports;
            addKnownPrimarySerials(previousPrimaryTransports, m_lastDeviceSerial);
            bool knownPartnerFlip = previousPrimaryTransports.count(curSerial) != 0 ||
                (isWifiSerial(m_lastDeviceSerial) && curSerial == m_wifiToUsbSerial) ||
                (!isWifiSerial(m_lastDeviceSerial) && curSerial == m_usbToWifiSerial);
            if (!curSerial.empty() && !m_lastDeviceSerial.empty() &&
                isWifiSerial(curSerial) != isWifiSerial(m_lastDeviceSerial) &&
                knownPartnerFlip && m_device.isServerRunning()) {
                if (isWifiSerial(curSerial)) {
                    std::string wifiIp = curSerial.substr(0, curSerial.rfind(':'));
                    LOG_INFO("Poll", "USB disconnected, failing over primary directly to " + curSerial);
                    if (m_device.connectDirectForSerial(curSerial, wifiIp)) {
                        deviceSession(0).serial = curSerial;
                        m_statusMessage = "USB disconnected, continuing over WiFi";
                        m_statusTime = std::chrono::steady_clock::now();
                        m_lastDeviceSerial = curSerial;
                        handledSerialChange = true;
                    }
                }
                if (!handledSerialChange) {
                    LOG_INFO("Poll", "USB/WiFi transport changed, reconnecting primary through " + curSerial);
                    m_device.disconnectTcp();
                }
            }

            if (!handledSerialChange && curSerial.empty() && !m_lastDeviceSerial.empty()) {
                LOG_WARN("Poll", "Device disconnected");
                m_wifiProbeAttempts = 0; // reset probe backoff for fast reconnect
                m_device.disconnectTcp(); // immediately kill TCP so reconnect doesn't think server is still up
                m_statusMessage = "Device disconnected";
                m_statusTime = std::chrono::steady_clock::now();
                m_androidStorageRoot.clear();
                m_androidVolumes.clear();
                deviceSession(0).storageRoot.clear();
                deviceSession(0).volumes.clear();
                deviceSession(0).connected = false;
                forEachAndroidPanel([](FilePanel& p) {
                    if (p.deviceSlot != 0) return;
                    p.selectedIndices.clear();
                    p.needsRefresh = true;
                });
            } else if (!handledSerialChange && !curSerial.empty()) {
                if (m_device.isServerRunning() && m_device.connectedSerial() == curSerial) {
                    LOG_INFO("Poll", "Device " + curSerial + " already connected, skipping init");
                    m_lastDeviceSerial = curSerial;
                    handledSerialChange = true;
                }
                if (!handledSerialChange) {
                    std::string model;
                    {
                        std::lock_guard<std::mutex> lk(m_deviceMutex);
                        model = (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size())
                            ? m_devices[m_selectedDevice].model : curSerial;
                    }
                    m_statusMessage = "Device detected: " + model + " - initializing...";
                    m_statusTime = std::chrono::steady_clock::now();
                    std::this_thread::sleep_for(std::chrono::seconds(1)); // brief settle
                    if (m_shutdownPoll) return;
                    // Don't restart server during active transfers - it kills existing connections
                    if (isBatchActive()) {
                        LOG_INFO("Poll", "Skipping onDeviceChanged - transfer active");
                        m_lastDeviceSerial = curSerial;
                    } else {
                        LOG_INFO("Poll", "Calling onDeviceChanged()");
                        onDeviceChanged();
                    }
                }
            }
            m_lastDeviceSerial = curSerial;
        }

        // Retry server connection if needed
        if (!curSerial.empty() && !m_device.isServerRunning() && !isBatchActive() && !m_wifiTransitionActive) {
            if (!isBatchActive()) {
                LOG_INFO("Poll", "Device present but server not running - retrying");
                m_statusMessage = "Retrying server connection...";
                m_statusTime = std::chrono::steady_clock::now();
                onDeviceChanged();
            }
        }

        maintainDeviceSessions(devices);

        // Health check
        if (m_device.isServerRunning() && !curSerial.empty() && !m_tetheringInProgress && !m_wifiTransitionActive) {
            auto sinceLast = std::chrono::steady_clock::now() - m_lastTransferActivity;
            bool cooldown = sinceLast < std::chrono::seconds(15);
            if (!isBatchActive() && !cooldown) {
                if (!m_device.verifyConnection()) {
                    LOG_WARN("Poll", "Health check PING failed - server connection lost");
                    m_statusMessage = "Server connection lost";
                    m_statusTime = std::chrono::steady_clock::now();
                    forEachAndroidPanel([](FilePanel& p) {
                        if (p.deviceSlot != 0) return;
                        p.androidEntries.clear();
                        p.selectedIndices.clear();
                        p.needsRefresh = true;
                    });
                } else {
                    forEachAndroidPanel([](FilePanel& p) {
                        if (p.deviceSlot == 0 && p.androidEntries.empty()) p.needsRefresh = true;
                    });
                }
            }
        }

        // Try to reconnect saved WiFi devices
        if (!isBatchActive() && m_prefs.wifiAutoConnect) {
            bool hasUsbDevice = !curSerial.empty() && !isWifiSerial(curSerial);
            bool noDeviceAtAll = !m_device.isServerRunning();

            for (auto& w : m_prefs.savedWifiDevices) {
                if (!w.autoConnect || w.wifiIp.empty() || w.wirelessDebugging) continue;
                if (hasUsbDevice && w.serial != curSerial) continue;
                if (!noDeviceAtAll && !hasUsbDevice) continue;
                std::string connectAddr = w.wifiIp + ":" + std::to_string(w.port);

                // Check if this WiFi device is online or offline
                bool isOnline = false;
                bool isOffline = false;
                {
                    std::lock_guard<std::mutex> lk(m_deviceMutex);
                    for (auto& d : m_devices) {
                        if (d.serial == connectAddr) {
                            if (d.state == "device") isOnline = true;
                            else isOffline = true; // "offline", "unauthorized", etc.
                            break;
                        }
                    }
                }

                // If offline or not in list at all, try to reconnect
                // Quick TCP probe first (1s timeout) to avoid 21s adb connect timeout
                if (!isOnline) {
                    if (!probeTcpEndpoint(w.wifiIp, w.port)) continue;

                    LOG_DEBUG("Poll", "WiFi device reachable, trying adb connect: " + connectAddr);
                    std::string result = m_device.runAdbCommand("connect " + connectAddr, 4000);
                    if (result.find("connected") != std::string::npos &&
                        result.find("failed") == std::string::npos) {
                        LOG_INFO("Poll", "WiFi device reconnected: " + connectAddr);
                        m_statusMessage = "WiFi reconnected: " + w.wifiIp;
                        m_statusTime = std::chrono::steady_clock::now();

                        // Re-establish secondary channel if it was lost
                        if (!m_dualChannelAvailable && m_device.isServerRunning()) {
                            m_secondaryChannel.setAdbPath(m_device.getAdbPath());
                            if (m_secondaryChannel.connectTcp(w.wifiIp, AFM_PORT) &&
                                m_secondaryChannel.verifyConnection()) {
                                m_dualChannelAvailable = true;
                                m_secondaryChannelType = "WiFi";
                                LOG_INFO("Poll", "Secondary WiFi channel re-established");
                                m_statusMessage = "Dual channel restored: USB + WiFi";
                                m_statusTime = std::chrono::steady_clock::now();
                            }
                        }
                    }
                }
            }
        }
        m_pollBusy = false;
    };

    // Main loop: use track-devices for instant notifications, fallback to polling
    while (!m_shutdownPoll) {
        if (m_device.getAdbPath().empty()) {
            {
                std::lock_guard<std::mutex> adbDiscovery(m_deviceWorkMutex);
                if (m_shutdownPoll) return;
                if (m_device.findAdb()) {
                    deviceSession(1).client.setAdbPath(m_device.getAdbPath());
                    m_statusMessage = "ADB ready - waiting for device...";
                    m_statusTime = std::chrono::steady_clock::now();
                    continue;
                }
                if (companion::devices().empty())
                    m_statusMessage = "ADB not found. Extract the full ZIP or install Android SDK Platform Tools.";
                m_statusTime = std::chrono::steady_clock::now();
            }
            for (int i = 0; i < 50 && !m_shutdownPoll; i++) {
                if (companion::generation() != companionGeneration) processDeviceUpdate(lastAdbDevices);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        // Try to open track-devices for instant device notifications
        uintptr_t trackSock = m_device.openTrackDevices();

        if (trackSock != (uintptr_t)(~(uintptr_t)0)) {
            // track-devices connected — receive instant updates
            LOG_INFO("Poll", "Using ADB track-devices (instant notifications)");

            // Set a timeout on the tracking socket so we can periodically check for shutdown
            // and run health checks
            // Wake up every second for companion device changes, and every 10s for health checks.
            DWORD trackTimeout = 1000;
            setsockopt((SOCKET)trackSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&trackTimeout, sizeof(trackTimeout));
            auto lastHealthCheck = std::chrono::steady_clock::now();

            AdbDeviceSnapshot snapshot;
            while (!m_shutdownPoll) {
                bool receivedUpdate = false;
                auto devices = m_device.readTrackDevicesUpdate(trackSock, &receivedUpdate);
                int err = WSAGetLastError();
                if (receivedUpdate) {
                    if (snapshot.update(devices)) processDeviceUpdate(devices);
                    continue;
                }
                if (err == WSAETIMEDOUT) {
                    if (companion::generation() != companionGeneration) {
                        processDeviceUpdate(lastAdbDevices);
                        continue;
                    }
                    if (std::chrono::steady_clock::now() - lastHealthCheck < std::chrono::seconds(10)) continue;
                    lastHealthCheck = std::chrono::steady_clock::now();
                    // An idle stream still needs periodic connection health checks.
                    // A busy worker owns the connections until its operation is complete.
                    std::unique_lock<std::mutex> healthCheck(m_deviceWorkMutex, std::try_to_lock);
                    if (!healthCheck.owns_lock()) continue;
                    if (m_shutdownPoll) break;
                    std::string curSerial;
                    {
                        std::lock_guard<std::mutex> lk(m_deviceMutex);
                        curSerial = (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size()
                            && m_devices[m_selectedDevice].state == "device")
                            ? m_devices[m_selectedDevice].serial : "";
                    }
                    if (curSerial.empty() && deviceSession(0).connected &&
                        m_device.isDirectConnection() && m_device.isServerRunning())
                        curSerial = deviceSession(0).serial;
                    if (m_device.isServerRunning() && !curSerial.empty() && !m_tetheringInProgress && !m_wifiTransitionActive) {
                        auto sinceLast = std::chrono::steady_clock::now() - m_lastTransferActivity;
                        if (!isBatchActive() && sinceLast >= std::chrono::seconds(15)) {
                            if (!m_device.verifyConnection()) {
                                LOG_WARN("Poll", "Health check failed");
                                m_statusMessage = "Server connection lost";
                                m_statusTime = std::chrono::steady_clock::now();
                                forEachAndroidPanel([](FilePanel& p) {
                                    if (p.deviceSlot != 0) return;
                                    p.androidEntries.clear();
                                    p.selectedIndices.clear();
                                    p.needsRefresh = true;
                                });
                            } else {
                                forEachAndroidPanel([](FilePanel& p) {
                                    if (p.deviceSlot == 0 && p.androidEntries.empty()) p.needsRefresh = true;
                                });
                            }
                        }
                    }
                    std::vector<DeviceInfo> sessionDevices;
                    {
                        std::lock_guard<std::mutex> lock(m_deviceMutex);
                        sessionDevices = m_devices;
                    }
                    maintainDeviceSessions(sessionDevices);
                    if (!curSerial.empty() && !m_device.isServerRunning() && !isBatchActive() && !m_wifiTransitionActive) {
                        if (!isBatchActive()) {
                            LOG_INFO("Poll", "Device present but server not running - retrying");
                            m_statusMessage = "Retrying server connection...";
                            m_statusTime = std::chrono::steady_clock::now();
                            onDeviceChanged();
                        }
                    }

                    // Probe saved WiFi devices on a backoff schedule
                    // Schedule: every 5s for first 10 attempts, every 30s for next 20, every 5min after that
                    if (!m_device.isServerRunning() && !isBatchActive()) {
                        auto now = std::chrono::steady_clock::now();

                        int intervalSec;
                        if (m_wifiProbeAttempts < 10) intervalSec = 5;
                        else if (m_wifiProbeAttempts < 30) intervalSec = 30;
                        else intervalSec = 300;

                        if (now - m_lastWifiProbeTime >= std::chrono::seconds(intervalSec)) {
                            m_lastWifiProbeTime = now;
                            bool anyAutoConnect = false;
                            for (auto& w : m_prefs.savedWifiDevices) {
                                if (!w.autoConnect || w.wifiIp.empty() || w.wirelessDebugging) continue;
                                anyAutoConnect = true;
                                std::string connectAddr = w.wifiIp + ":" + std::to_string(w.port);

                                // Quick TCP probe (1s timeout)
                                bool reachable = probeTcpEndpoint(w.wifiIp, w.port);

                                if (reachable) {
                                    LOG_INFO("Poll", "WiFi probe: " + connectAddr + " reachable, connecting...");
                                    std::string result = m_device.runAdbCommand("connect " + connectAddr, 4000);
                                    if (result.find("connected") != std::string::npos &&
                                        result.find("failed") == std::string::npos) {
                                        LOG_INFO("Poll", "WiFi auto-connect on launch: " + connectAddr);
                                        m_statusMessage = "WiFi connected: " + (w.model.empty() ? connectAddr : w.model);
                                        m_statusTime = std::chrono::steady_clock::now();
                                        m_wifiProbeAttempts = 0; // reset on success
                                        break; // connected, stop probing
                                    }
                                }
                            }
                            if (anyAutoConnect) {
                                m_wifiProbeAttempts++;
                                if (m_wifiProbeAttempts == 1)
                                    LOG_INFO("Poll", "WiFi auto-connect: probing saved devices...");
                                else if (m_wifiProbeAttempts == 10)
                                    LOG_INFO("Poll", "WiFi auto-connect: slowing to 30s interval");
                                else if (m_wifiProbeAttempts == 30)
                                    LOG_INFO("Poll", "WiFi auto-connect: slowing to 5min interval");
                            }
                        }
                    }
                    continue;
                }

                if (devices.empty()) {
                    // Real connection error — fall out to reconnect
                    LOG_WARN("Poll", "track-devices connection lost (WSA " + std::to_string(err) + ")");
                    break;
                }

            }

            m_device.closeTrackDevices(trackSock);

            if (m_shutdownPoll) return;
            // Brief pause before reconnecting
            for (int i = 0; i < 20 && !m_shutdownPoll; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            // track-devices failed — fallback to polling with getDevices()
            LOG_WARN("Poll", "track-devices unavailable — falling back to polling");

            for (int cycle = 0; cycle < 3 && !m_shutdownPoll; cycle++) {
                for (int i = 0; i < 30 && !m_shutdownPoll; i++) {
                    if (companion::generation() != companionGeneration) processDeviceUpdate(lastAdbDevices);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (m_shutdownPoll) return;

                auto devices = m_device.getDevices();
                processDeviceUpdate(devices);
            }
            // After a few poll cycles, try track-devices again
        }
    }
}

void App::onDeviceChanged() {
    if (deviceSession(0).rootRequestPending) return;
    std::string serial;
    {
        std::lock_guard<std::mutex> lk(m_deviceMutex);
        if (m_selectedDevice < 0 || m_selectedDevice >= (int)m_devices.size()) return;
        serial = m_devices[m_selectedDevice].serial;
    }
    if (m_deviceSessions.find(deviceIdentity(serial)) != 0) return;
    m_primaryReconnectActive = true;
    struct ReconnectFlagReset {
        std::atomic<bool>& flag;
        ~ReconnectFlagReset() { flag = false; }
    } reconnectFlagReset{m_primaryReconnectActive};

    bool useRootForDevice = m_prefs.rootEnabledForSerial(deviceIdentity(serial));
    // Start the TCP server on the device (status updates come from m_device.statusText())
    if (!m_device.startServer(serial, m_preferAdbForward, useRootForDevice)) {
        LOG_ERROR("Poll", "startServer failed: " + m_device.lastError());
        if (useRootForDevice) {
            std::string rootError = m_device.lastError();
            m_prefs.setRootEnabledForSerial(deviceIdentity(serial), false);
            m_prefs.save();
            LOG_WARN("Poll", "Root start failed, retrying without root: " + rootError);
            if (!m_device.startServer(serial, m_preferAdbForward, false)) {
                std::string msg = "Root access failed for this device, and the normal connection also failed.\n\n"
                    "Check that the device is rooted, grant ADB root access in Magisk, KernelSU, or your root manager, then reconnect the device.\n\n"
                    "Root details: " + rootError + "\n\n"
                    "Reconnect details: " + m_device.lastError();
                showNotification("Root Connection Failed", msg, true);
                m_statusMessage = "Root failed and normal reconnect failed. Check root access for this device. " + m_device.lastError();
                m_statusTime = std::chrono::steady_clock::now();
                return;
            }
            std::string msg = "Root access was not available or was denied for this device.\n\n"
                "Fast Enough connected without root and turned off Root for this device only. Check your root manager and grant ADB root access before enabling Root again.\n\n"
                "Details: " + rootError;
            showNotification("Connected Without Root", msg, true);
            m_statusMessage = "Root not available or not granted. Connected without root.";
        } else {
            m_statusMessage = "Server failed: " + m_device.lastError();
            m_statusTime = std::chrono::steady_clock::now();
            return;
        }
    }
    LOG_INFO("Poll", "startServer succeeded");

    std::string connMode = m_device.isDirectConnection()
        ? ("Direct TCP @ " + m_device.deviceIp()) : "ADB Forward";
    if (m_statusMessage.empty() || m_statusMessage.find("Root not available") == std::string::npos)
        m_statusMessage = "Connected via " + connMode + " - detecting storage...";
    m_statusTime = std::chrono::steady_clock::now();

    // Detect storage root via TCP server
    deviceSession(0).storageRoot = m_device.detectStoragePath();
    m_androidStorageRoot = deviceSession(0).storageRoot;
    deviceSession(0).volumes.clear();
    deviceSession(0).volumes.push_back(deviceSession(0).storageRoot);
    m_androidVolumes = deviceSession(0).volumes;

    deviceSession(0).serial = serial;
    cacheDeviceRoot(0);
    deviceSession(0).connected = m_device.isServerRunning();
    UiMessage readyMessage;
    readyMessage.deviceSessionReady = true;
    readyMessage.readyDeviceSlot = 0;
    postUiMessage(std::move(readyMessage));

    if (companion::isSerial(serial)) {
        m_dualChannelAvailable = false;
        m_secondaryChannelType.clear();
        m_extraChannels.clear();
        m_activeChannelCount = 1;
        std::string link = m_device.isDirectConnection() ? "WiFi" : "USB";
        m_statusMessage = "Ready | Companion app over " + link + (m_device.useRoot() ? " (root)" : "") +
            " | Storage: " + m_androidStorageRoot;
        m_statusTime = std::chrono::steady_clock::now();
        return;
    }

    // Reset the previous channel state before WiFi auto-connect begins. The
    // asynchronous connection must not be cleared after it succeeds.
    m_dualChannelAvailable = false;
    m_secondaryChannelType.clear();
    m_extraChannels.clear();
    m_activeChannelCount = 1; // primary always exists

    // Auto-connect WiFi for saved devices (enables dual-channel automatically)
    tryAutoWifiConnect(serial);

    // Establish parallel transfer channels based on pipe count settings

    // Detect WiFi IP — from wlan0 query, or from WiFi serial
    std::string wifiIp;
    bool primaryIsWifi = isWifiSerial(serial);
    if (primaryIsWifi) {
        // Extract IP from WiFi serial (192.168.x.x:5555 → 192.168.x.x)
        auto colon = serial.rfind(':');
        if (colon != std::string::npos) wifiIp = serial.substr(0, colon);
    } else {
        // USB connected — try to detect WiFi IP for dual-link
        std::string wlanOut = m_device.runAdbCommand("-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
        auto inetPos = wlanOut.find("inet ");
        if (inetPos != std::string::npos) {
            auto start = inetPos + 5;
            auto slash = wlanOut.find('/', start);
            if (slash != std::string::npos) wifiIp = wlanOut.substr(start, slash - start);
        }
    }

    // Helpers for creating channels
    int nextPort = AFM_PORT + 2; // 5742, 5743, ...
    auto makeAdbPipe = [&](DeviceClient& ch) -> bool {
        ch.setAdbPath(m_device.getAdbPath());
        ch.setLocalPort(nextPort);
        std::string fwd = m_device.runAdbCommand("-s " + serial + " forward tcp:" +
            std::to_string(nextPort) + " tcp:" + std::to_string(AFM_PORT));
        if (fwd.find("error") != std::string::npos) return false;
        bool ok = ch.connectTcp("127.0.0.1", nextPort) && ch.verifyConnection();
        nextPort++;
        return ok;
    };
    auto makeWifiDirectPipe = [&](DeviceClient& ch) -> bool {
        if (wifiIp.empty()) return false;
        ch.setAdbPath(m_device.getAdbPath());
        return ch.connectTcp(wifiIp, AFM_PORT) && ch.verifyConnection();
    };

    // Determine what pipes to create based on connection type and settings
    // USB primary: USB dual-pipe uses ADB forward, WiFi pipes use direct TCP
    // WiFi primary: WiFi dual-pipe uses direct TCP, USB pipes only if USB serial exists
    auto devicePipes = m_prefs.pipePreferencesFor(serial);
    if (primaryIsWifi) {
        // WiFi-connected device: create WiFi direct pipes for dual
        int wifiPipes = devicePipes.wifiPipeCount;
        if (wifiPipes >= 2) {
            m_secondaryChannel.disconnectTcp();
            if (makeWifiDirectPipe(m_secondaryChannel)) {
                m_dualChannelAvailable = true;
                m_secondaryChannelType = "WiFi";
                m_activeChannelCount = 2;
                LOG_INFO("Poll", "Channel 2: WiFi (Direct) — dual WiFi pipe");
            }
        }
    } else {
        // USB-connected device: prefer WiFi for secondary (true dual-link), else USB dual-pipe
        bool hasWifi = !wifiIp.empty();
        if (hasWifi && devicePipes.wifiPipeCount >= 1) {
            m_secondaryChannel.disconnectTcp();
            if (makeWifiDirectPipe(m_secondaryChannel)) {
                m_dualChannelAvailable = true;
                m_secondaryChannelType = "WiFi";
                m_activeChannelCount = 2;
                LOG_INFO("Poll", "Channel 2: WiFi (Direct)");
            }
        }
        if (!m_dualChannelAvailable && devicePipes.usbPipeCount >= 2) {
            m_secondaryChannel.disconnectTcp();
            if (makeAdbPipe(m_secondaryChannel)) {
                m_dualChannelAvailable = true;
                m_secondaryChannelType = "USB";
                m_activeChannelCount = 2;
                LOG_INFO("Poll", "Channel 2: USB (ADB forward port " + std::to_string(nextPort - 1) + ")");
            }
        }
    }

    // Extra channels (3rd, 4th) for dual-pipe combinations
    if (m_dualChannelAvailable) {
        // Extra USB ADB pipe (only useful for USB-connected devices)
        if (!primaryIsWifi && devicePipes.usbPipeCount >= 2 && m_secondaryChannelType == "WiFi") {
            auto ch = std::make_unique<DeviceClient>();
            if (makeAdbPipe(*ch)) {
                LOG_INFO("Poll", "Channel " + std::to_string(m_activeChannelCount + 1) + ": USB (ADB forward)");
                m_extraChannels.push_back({std::move(ch), true});
                m_activeChannelCount++;
            }
        }
        // Extra WiFi direct pipe (when wifiPipeCount=2 and secondary is already WiFi)
        if (!wifiIp.empty() && devicePipes.wifiPipeCount >= 2) {
            auto ch = std::make_unique<DeviceClient>();
            if (makeWifiDirectPipe(*ch)) {
                LOG_INFO("Poll", "Channel " + std::to_string(m_activeChannelCount + 1) + ": WiFi (Direct)");
                m_extraChannels.push_back({std::move(ch), false});
                m_activeChannelCount++;
            }
        }
    }

    if (m_activeChannelCount > 1) {
        std::string desc;
        if (m_secondaryChannelType == "USB" && m_extraChannels.empty())
            desc = "USB (dual)";
        else if (wifiIp.empty())
            desc = "USB (" + std::to_string(m_activeChannelCount) + " pipes)";
        else
            desc = std::to_string(m_activeChannelCount) + " channels (USB + WiFi)";
        LOG_INFO("Poll", "Multi-channel ready: " + desc);
    }

    std::string channelInfo;
    if (m_activeChannelCount >= 4)
        channelInfo = " | " + std::to_string(m_activeChannelCount) + " channels";
    else if (m_activeChannelCount >= 2)
        channelInfo = m_dualChannelAvailable ? (" + " + m_secondaryChannelType) : "";
    m_statusMessage = "Ready | " + connMode + channelInfo + " | Storage: " + m_androidStorageRoot;
    m_statusTime = std::chrono::steady_clock::now();
}

// Recursively add directory contents for pull (Android → Windows)
static void buildBatchItems(FilePanel& srcPanel, FilePanel& dstPanel, bool isPull,
                            std::vector<BatchFileItem>& items, uint64_t& totalBytes,
                            DeviceClient* dev = nullptr) {
    for (int idx : srcPanel.selectedIndices) {
        if (!srcPanel.validIndex(idx)) continue;
        BatchFileItem item;
        item.displayName = srcPanel.entryName(idx);
        item.isDirectory = srcPanel.entryIsDir(idx);
        item.fileSize = srcPanel.entrySize(idx);

        // MCRAW virtual item
        if (srcPanel.insideMcraw) {
            item.isMcrawVirtual = true;
            item.mcrawPath = srcPanel.mcrawFilePath;
            item.virtualName = item.displayName;
            item.isDirectory = false;
            if (isPull) {
                item.sourcePath = srcPanel.mcrawFilePath;
                std::string lp = dstPanel.currentPath;
                appendPathSeparator(lp, '\\'); lp += item.displayName;
                item.destPath = lp;
            } else {
                item.sourcePath = srcPanel.mcrawFilePath;
                std::string rp = dstPanel.currentPath;
                appendPathSeparator(rp, '/'); rp += item.displayName;
                item.destPath = rp;
            }
            totalBytes += item.fileSize;
            items.push_back(std::move(item));
            continue;
        }

        if (isPull) {
            std::string rp = srcPanel.currentPath;
            appendPathSeparator(rp, '/'); rp += item.displayName;
            item.sourcePath = rp;
            std::string lp = dstPanel.currentPath;
            appendPathSeparator(lp, '\\'); lp += item.displayName;
            item.destPath = lp;
        } else {
            std::string lp = srcPanel.currentPath;
            appendPathSeparator(lp, '\\'); lp += item.displayName;
            item.sourcePath = lp;
            std::string rp = dstPanel.currentPath;
            appendPathSeparator(rp, '/'); rp += item.displayName;
            item.destPath = rp;
            if (!item.isDirectory) {
                try { uint64_t ls = (uint64_t)std::filesystem::file_size(toFsPath(lp)); if (ls > 0) item.fileSize = ls; } catch (...) {}
            }
        }
        totalBytes += item.fileSize;
        items.push_back(item);


    }
}

// Build batch items for cross-device Android-to-Android transfer
static void buildCrossDeviceBatchItems(FilePanel& srcPanel, FilePanel& dstPanel,
                                        std::vector<BatchFileItem>& items, uint64_t& totalBytes,
                                        DeviceClient* srcDev = nullptr) {
    for (int idx : srcPanel.selectedIndices) {
        if (!srcPanel.validIndex(idx)) continue;
        BatchFileItem item;
        item.displayName = srcPanel.entryName(idx);
        item.isDirectory = srcPanel.entryIsDir(idx);
        item.fileSize = srcPanel.entrySize(idx);

        // Both source and dest are Android — use forward slashes
        std::string sp = srcPanel.currentPath;
        appendPathSeparator(sp, '/'); sp += item.displayName;
        item.sourcePath = sp;

        std::string dp = dstPanel.currentPath;
        appendPathSeparator(dp, '/'); dp += item.displayName;
        item.destPath = dp;

        totalBytes += item.fileSize;
        items.push_back(item);

    }
}

// Recursively add local directory contents
static void buildLocalBatchItems(FilePanel& srcPanel, FilePanel& dstPanel,
                                  std::vector<BatchFileItem>& items, uint64_t& totalBytes) {
    for (int idx : srcPanel.selectedIndices) {
        if (!srcPanel.validIndex(idx)) continue;
        BatchFileItem item;
        item.displayName = srcPanel.entryName(idx);
        item.isDirectory = srcPanel.entryIsDir(idx);
        item.fileSize = srcPanel.entrySize(idx);

        std::string sp = srcPanel.currentPath;
        appendPathSeparator(sp, '\\'); sp += item.displayName;
        item.sourcePath = sp;

        std::string dp = dstPanel.currentPath;
        appendPathSeparator(dp, '\\'); dp += item.displayName;
        item.destPath = dp;

        if (!item.isDirectory) {
            try { uint64_t s = (uint64_t)std::filesystem::file_size(toFsPath(sp)); if (s > 0) item.fileSize = s; } catch (...) {}
        }
        totalBytes += item.fileSize;
        items.push_back(item);

    }
}

void App::prepareTransferManifest(const std::shared_ptr<TransferBatch>& batch) {
    batch->state = BatchState::Preparing;
    auto roots = batch->files;
    m_manifestWork.post(0, [this, batch, roots = std::move(roots)]() mutable {
        UiMessage result;
        result.preparedBatch = batch;
        auto cancelled = [this, &batch] { return m_shutdownPoll.load() || batch->stopRequested.load(); };
        try {
            DeviceClient source;
            auto sourceCancellation = batch->cancellation.track(source);
            const bool remoteSource = !batch->isLocalCopy && (batch->isPull || batch->isCrossDevice);
            if (remoteSource && !cancelled() && !source.connectParallelFrom(deviceForSlot(batch->srcDeviceSlot)))
                throw std::runtime_error("Source device is unavailable");
            if (cancelled()) source.interruptBlocking();
            std::vector<BatchFileItem> pending(roots.rbegin(), roots.rend());
            std::unordered_set<std::wstring> visitedDirectories;
            while (!pending.empty() && !cancelled()) {
                auto item = std::move(pending.back());
                pending.pop_back();
                if (!item.isDirectory) {
                    if (!remoteSource && !item.isMcrawVirtual)
                        item.fileSize = std::filesystem::file_size(toFsPath(item.sourcePath));
                    if (item.fileSize > UINT64_MAX - result.preparedBytes)
                        throw std::runtime_error("Selected files exceed the supported total size");
                    result.preparedBytes += item.fileSize;
                    result.preparedFiles.push_back(std::move(item));
                    batch->discoveredItems = result.preparedFiles.size();
                    continue;
                }
                item.fileSize = 0;
                result.preparedFiles.push_back(item);
                batch->discoveredItems = result.preparedFiles.size();
                std::vector<BatchFileItem> children;
                if (remoteSource) {
                    bool listed = false;
                    auto entries = source.listDirectory(item.sourcePath, &listed);
                    if (!listed) throw std::runtime_error(item.sourcePath + ": " + source.lastError());
                    for (const auto& entry : entries) {
                        if (cancelled()) break;
                        if (entry.name == "." || entry.name == ".." || entry.name.find('/') != std::string::npos ||
                            entry.name.find('\\') != std::string::npos)
                            throw std::runtime_error("Invalid source entry: " + entry.name);
                        BatchFileItem child;
                        child.displayName = entry.name;
                        child.sourcePath = item.sourcePath + "/" + entry.name;
                        child.destPath = item.destPath + (batch->isCrossDevice ? "/" : "\\") + entry.name;
                        child.fileSize = entry.size;
                        child.isDirectory = entry.isDirectory();
                        children.push_back(std::move(child));
                    }
                } else {
                    auto canonical = std::filesystem::canonical(toFsPath(item.sourcePath)).wstring();
                    std::transform(canonical.begin(), canonical.end(), canonical.begin(), [](wchar_t c) { return towlower(c); });
                    if (!visitedDirectories.insert(canonical).second)
                        throw std::runtime_error("Repeated directory or link cycle: " + item.sourcePath);
                    for (const auto& entry : listWindowsDirectory(toFsPath(item.sourcePath), cancelled)) {
                        BatchFileItem child;
                        child.displayName = entry.name;
                        child.sourcePath = item.sourcePath + "\\" + entry.name;
                        child.destPath = item.destPath + (batch->isLocalCopy ? "\\" : "/") + entry.name;
                        child.fileSize = entry.size;
                        child.isDirectory = entry.isDirectory;
                        children.push_back(std::move(child));
                    }
                }
                for (auto it = children.rbegin(); it != children.rend(); ++it) pending.push_back(std::move(*it));
            }
            if (!cancelled()) batch->logEvent(LogLevel::Info, "Source scan finished, " +
                std::to_string(result.preparedFiles.size()) + " items");
        } catch (const std::exception& error) {
            result.preparationError = error.what();
        } catch (...) {
            result.preparationError = "Unexpected source scan failure";
        }
        postUiMessage(std::move(result));
    });
}

void App::startWorkspaceTransfer(bool move) {
    FilePanel& source = m_lastFocusedPanel == &m_leftPanel ? m_leftPanel : m_rightPanel;
    if (!panelAvailable(source)) return;
    startTransfer(source.isAndroid, move);
}

void App::startTransfer(bool pullFromAndroid, bool move) {
    if (m_leftPanel.isConnections || m_rightPanel.isConnections) {
        m_statusMessage = "Choose a file view in both panes before starting a transfer";
        m_statusTime = std::chrono::steady_clock::now();
        return;
    }
    if (m_leftPanel.isApps || m_rightPanel.isApps) {
        m_statusMessage = "App panels are for install and uninstall only";
        m_statusTime = std::chrono::steady_clock::now();
        return;
    }

    // Determine source and destination panels based on their modes
    FilePanel* srcPtr = nullptr;
    FilePanel* dstPtr = nullptr;

    bool bothWindows = !m_leftPanel.isAndroid && !m_rightPanel.isAndroid;
    bool bothAndroid = m_leftPanel.isAndroid && m_rightPanel.isAndroid;

    if (bothWindows) {
        // Windows-to-Windows: use focused panel as source, other as destination
        srcPtr = (m_lastFocusedPanel == &m_rightPanel) ? &m_rightPanel : &m_leftPanel;
        dstPtr = (srcPtr == &m_leftPanel) ? &m_rightPanel : &m_leftPanel;
    } else if (bothAndroid) {
        // Android-to-Android: need two different devices
        if (m_leftPanel.deviceSlot == m_rightPanel.deviceSlot) {
            m_statusMessage = "Both panels are on the same device — use different devices";
            m_statusTime = std::chrono::steady_clock::now();
            return;
        }
        srcPtr = (m_lastFocusedPanel == &m_rightPanel) ? &m_rightPanel : &m_leftPanel;
        dstPtr = (srcPtr == &m_leftPanel) ? &m_rightPanel : &m_leftPanel;
    } else {
        // Mixed: need device
        FilePanel& devicePanel = m_leftPanel.isAndroid ? m_leftPanel : m_rightPanel;
        if (!deviceSession(devicePanel.deviceSlot).connected ||
            !deviceForSlot(devicePanel.deviceSlot).isServerRunning()) return;
        if (pullFromAndroid) {
            srcPtr = m_leftPanel.isAndroid ? &m_leftPanel : &m_rightPanel;
            dstPtr = !m_leftPanel.isAndroid ? &m_leftPanel : &m_rightPanel;
        } else {
            srcPtr = !m_leftPanel.isAndroid ? &m_leftPanel : &m_rightPanel;
            dstPtr = m_leftPanel.isAndroid ? &m_leftPanel : &m_rightPanel;
        }
    }

    queuePanelTransfer(*srcPtr,*dstPtr,move);
}

void App::queuePanelTransfer(FilePanel& src, FilePanel& dst, bool move) {
    if (&src==&dst || src.isConnections || dst.isConnections || src.isApps || dst.isApps) return;
    if (!panelAvailable(src) || !panelAvailable(dst)) {
        LOG_WARN("Transfer","Copy rejected because a selected location is disconnected");
        m_statusMessage="Connect both locations before copying files";
        m_statusTime=std::chrono::steady_clock::now();
        return;
    }
    bool localCopy=!src.isAndroid && !dst.isAndroid;
    bool bothAndroid=src.isAndroid && dst.isAndroid;
    if (bothAndroid && src.deviceSlot==dst.deviceSlot) {
        LOG_WARN("Transfer","Copy rejected because both panes use the same Android device");
        m_statusMessage="Choose different devices for a transfer between Android panes";
        m_statusTime=std::chrono::steady_clock::now();
        return;
    }
    if (src.selectedIndices.empty()) {
        m_statusMessage = "No files selected for transfer";
        m_statusTime = std::chrono::steady_clock::now();
        return;
    }

    if (localCopy) {
        std::error_code error;
        auto destination=std::filesystem::weakly_canonical(toFsPath(dst.currentPath),error).wstring();
        if (!error) for (int index:src.selectedIndices) {
            if (!src.validIndex(index)) continue;
            auto source=std::filesystem::weakly_canonical(toFsPath(src.currentPath)/toFsPath(src.entryName(index)),error).wstring();
            if (error) continue;
            auto parent=std::filesystem::path(source).parent_path().wstring();
            bool sameFolder=_wcsicmp(parent.c_str(),destination.c_str())==0;
            std::wstring prefix=source+L"\\";
            bool insideSource=src.entryIsDir(index) && (_wcsicmp(source.c_str(),destination.c_str())==0 ||
                (destination.size()>=prefix.size() && _wcsnicmp(prefix.c_str(),destination.c_str(),prefix.size())==0));
            if (sameFolder || insideSource) {
                showNotification("Choose another destination","Files cannot be copied or moved into their source folder or its descendants.",true);
                return;
            }
        }
    }

    auto batch = std::make_shared<TransferBatch>();
    batch->isLocalCopy = localCopy;
    batch->isMove = move && localCopy;
    batch->srcDeviceSlot = src.deviceSlot;
    batch->dstDeviceSlot = dst.deviceSlot;

    // Cross-device Android-to-Android
    if (bothAndroid && src.deviceSlot != dst.deviceSlot) {
        batch->isCrossDevice = true;
        batch->isPull = false;
    } else {
        batch->isPull = !localCopy && src.isAndroid;
    }
    const FilePanel& transferDevicePanel = src.isAndroid ? src : dst;
    auto transferPipes = m_prefs.pipePreferencesFor(deviceSession(transferDevicePanel.deviceSlot).serial);
    batch->useParallelChannels = !localCopy &&
        (transferPipes.usbPipeCount + transferPipes.wifiPipeCount > 1);

    uint64_t total = 0;
    if (localCopy)
        buildLocalBatchItems(src, dst, batch->files, total);
    else if (batch->isCrossDevice)
        buildCrossDeviceBatchItems(src, dst, batch->files, total, &deviceFor(src));
    else
        buildBatchItems(src, dst, batch->isPull, batch->files, total, &deviceFor(src));
    batch->totalBytes = total;

    if (batch->files.empty()) return;

    if (batch->isCrossDevice) {
        m_pendingBatch = batch;
        m_showCrossDeviceDialog = true;
    } else {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        prepareTransferManifest(batch);
        m_batchQueue.push_back(batch);
        m_batchCV.notify_one();
        m_overlayWasOpen = false;
    }
}

void App::handleExternalFileDrop(const std::vector<std::string>& paths, int mouseX, int mouseY) {
    if (paths.empty()) return;

    // Determine which panel the drop landed on
    float mx = (float)mouseX, my = (float)mouseY;
    FilePanel* target = nullptr;
    if (mx >= m_leftPanelMin.x && mx <= m_leftPanelMax.x &&
        my >= m_leftPanelMin.y && my <= m_leftPanelMax.y) {
        target = &m_leftPanel;
    } else if (mx >= m_rightPanelMin.x && mx <= m_rightPanelMax.x &&
               my >= m_rightPanelMin.y && my <= m_rightPanelMax.y) {
        target = &m_rightPanel;
    }

    if (!target) {
        m_statusMessage = "Drop outside panel area";
        m_statusTime = std::chrono::steady_clock::now();
        return;
    }

    // Build batch items from dropped paths
    auto batch = std::make_shared<TransferBatch>();
    uint64_t total = 0;
    if (target->isAndroid) {
        batch->dstDeviceSlot = target->deviceSlot;
        auto transferPipes = m_prefs.pipePreferencesFor(deviceSession(target->deviceSlot).serial);
        batch->useParallelChannels = transferPipes.usbPipeCount + transferPipes.wifiPipeCount > 1;
    }

    for (auto& srcPath : paths) {
        BatchFileItem item;
        auto p = toFsPath(srcPath);
        item.displayName = pathToUtf8(p.filename());
        item.sourcePath = srcPath;
        std::error_code ec;
        item.isDirectory = std::filesystem::is_directory(p, ec);
        if (ec) item.isDirectory = false;

        if (target->isAndroid) {
            // Explorer → Android: push
            std::string rp = target->currentPath;
            appendPathSeparator(rp, '/');
            rp += item.displayName;
            item.destPath = rp;
            batch->isPull = false;
        } else {
            // Explorer → Windows panel: local copy
            std::string dp = target->currentPath;
            appendPathSeparator(dp, '\\');
            dp += item.displayName;
            item.destPath = dp;
            batch->isLocalCopy = true;
        }

        if (!item.isDirectory) {
            item.fileSize = (uint64_t)std::filesystem::file_size(p, ec);
            if (ec) item.fileSize = 0;
        }
        total += item.fileSize;
        batch->files.push_back(std::move(item));
    }

    batch->totalBytes = total;
    if (batch->files.empty()) return;

    if (!batch->isLocalCopy) {
        if (!deviceSession(target->deviceSlot).connected ||
            !deviceForSlot(target->deviceSlot).isServerRunning()) {
            m_statusMessage = "No device connected for transfer";
            m_statusTime = std::chrono::steady_clock::now();
            return;
        }
    }

    if (batch->isLocalCopy) {
        bool sameDrive = false;
        if (!batch->files.empty()) {
            auto& s = batch->files[0].sourcePath;
            auto& d = batch->files[0].destPath;
            if (s.size() >= 2 && d.size() >= 2)
                sameDrive = (toupper(s[0]) == toupper(d[0]) && s[1] == ':' && d[1] == ':');
        }
        if (sameDrive) {
            batch->isMove = true;
            std::lock_guard<std::mutex> lk(m_batchMutex);
            prepareTransferManifest(batch);
            m_batchQueue.push_back(batch);
            m_batchCV.notify_one();
            m_overlayWasOpen = false;
        } else {
            m_pendingBatch = batch;
            m_showCopyMoveDialog = true;
        }
    } else {
        std::lock_guard<std::mutex> lk(m_batchMutex);
        prepareTransferManifest(batch);
        m_batchQueue.push_back(batch);
        m_batchCV.notify_one();
        m_overlayWasOpen = false;
    }
}

void App::startTransferFromDrag(FilePanel& srcPanel, FilePanel& dstPanel) {
    LOG_INFO("Transfer","Drop accepted: "+panelDisplayName(srcPanel)+" "+srcPanel.currentPath+
        " -> "+panelDisplayName(dstPanel)+" "+dstPanel.currentPath);
    queuePanelTransfer(srcPanel,dstPanel,false);
}

void App::processBatchQueue() {
    // Lower thread priority so transfers don't starve UI and other apps
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    // Set low I/O priority so disk reads don't evict other apps' cache
    THREAD_POWER_THROTTLING_STATE throttle{};
    throttle.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttle.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttle.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttle, sizeof(throttle));

    while (!m_shutdownTransfer) {
        std::shared_ptr<TransferBatch> batch;
        {
            std::unique_lock<std::mutex> lk(m_batchMutex);
            m_batchCV.wait(lk, [&]() {
                if (m_shutdownTransfer) return true;
                for (auto& b : m_batchQueue)
                    if (b->state.load() == BatchState::Queued) return true;
                return false;
            });
            if (m_shutdownTransfer) return;
            for (auto& b : m_batchQueue) {
                if (b->state.load() == BatchState::Queued) { batch = b; break; }
            }
        }
        if (!batch) continue;
        struct OutcomeLog {
            std::shared_ptr<TransferBatch> batch;
            ~OutcomeLog() {
                auto state=batch->state.load();
                if (batch->stopRequested && state != BatchState::Completed) {
                    state = BatchState::Stopped;
                    batch->state = state;
                }
                if (state == BatchState::Completed || state == BatchState::Failed || state == BatchState::Stopped) {
                    batch->disconnected = false;
                    batch->waitingForUserRetry = false;
                    batch->finishConflict();
                }
                bool errors=state==BatchState::Failed || batch->errorSkippedFiles>0 || batch->crcFailCount()>0;
                std::string outcome=state==BatchState::Completed?(errors?"Completed with errors":"Completed"):
                    state==BatchState::Stopped?"Cancelled":state==BatchState::Failed?"Failed":"Interrupted";
                batch->logEvent(errors?LogLevel::Error:LogLevel::Info,outcome+", bytes="+
                    std::to_string(batch->totalTransferred.load())+"/"+std::to_string(batch->totalBytes.load())+
                    ", skipped="+std::to_string(batch->skippedFiles)+", file errors="+
                    std::to_string(batch->errorSkippedFiles.load())+", verification errors="+std::to_string(batch->crcFailCount()));
                if (errors && !batch->errorMessage.empty()) batch->logEvent(LogLevel::Error,batch->errorMessage);
                int index=batch->currentFileIndex.load();
                if (state==BatchState::Failed && index>=0 && index<(int)batch->files.size()) {
                    const auto& file=batch->files[index];
                    batch->logEvent(LogLevel::Error,"Source: "+file.sourcePath+" -> Destination: "+file.destPath);
                }
            }
        } outcomeLog{batch};
        try {
        if (batch->stopRequested) { batch->state=BatchState::Stopped; continue; }
        batch->logEvent(LogLevel::Info,std::string(batch->isMove?"Move":"Copy")+" started");
        std::string localPartialPath;


        auto waitForRetry = [&](std::chrono::milliseconds delay, uint64_t& seenGeneration) {
            const auto deadline = std::chrono::steady_clock::now() + delay;
            std::unique_lock<std::mutex> lk(m_batchMutex);
            while (!m_shutdownTransfer && !batch->stopRequested.load() &&
                   batch->retryGeneration.load() == seenGeneration) {
                auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;
                m_batchCV.wait_until(lk, std::min(deadline, now + std::chrono::milliseconds(100)));
            }
            seenGeneration = batch->retryGeneration.load();
            return !m_shutdownTransfer && !batch->stopRequested.load();
        };
        uint64_t singleRetryGeneration = 0;

        // Single-device transfers use whichever panel is Android:
        // pulls read from the Android source slot, pushes write to the Android destination slot.
        int batchDeviceSlot = batch->isPull ? batch->srcDeviceSlot : batch->dstDeviceSlot;
        DeviceClient& batchDev = deviceSession(batchDeviceSlot).client;
        std::unique_ptr<TransferCancellation<DeviceClient>::Registration> primaryCancellation, peerCancellation;
        if (!batch->isLocalCopy) {
            primaryCancellation = batch->cancellation.track(batchDev);
            if (batch->isCrossDevice) {
                const int peerSlot = batchDeviceSlot == batch->srcDeviceSlot ? batch->dstDeviceSlot : batch->srcDeviceSlot;
                if (peerSlot != batchDeviceSlot) peerCancellation = batch->cancellation.track(deviceForSlot(peerSlot));
            }
        }
        if (batch->stopRequested) { batch->state = BatchState::Stopped; continue; }

        if (!batch->isLocalCopy && !batch->isCrossDevice && !batchDev.isServerRunning()) {
            batch->state = BatchState::Failed;
            batch->errorMessage = "Server not connected";
            continue;
        }

        // Remember the original device serial for reconnection safety
        std::string originalSerial = batch->isLocalCopy ? "" : batchDev.connectedSerial();

        batch->state = BatchState::Running;
        batch->startTime = std::chrono::steady_clock::now();

        auto logBatchPerformance = [&](const std::string& mode, double seconds, double avgSpeed) {
            if (seconds < 0.1) seconds = 0.1;
            std::string direction;
            if (batch->isLocalCopy)
                direction = "PC to PC";
            else if (batch->isCrossDevice)
                direction = "Android to Android";
            else if (batch->isPull)
                direction = "Android to PC";
            else
                direction = "PC to Android";

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2);
            oss << "Transfer complete: direction=" << direction
                << ", mode=" << mode
                << ", files=" << batch->totalFiles()
                << ", bytes=" << batch->totalTransferred.load()
                << " (" << formatSize(batch->totalTransferred.load()) << ")"
                << ", duration=" << seconds << "s"
                << ", avg=" << formatSpeed(avgSpeed)
                << ", channels=" << batch->numChannels;
            if (batch->skippedFiles > 0)
                oss << ", skipped=" << batch->skippedFiles;
            LOG_INFO("Perf", oss.str());

            if (batch->numChannels > 1) {
                int nCh = std::min(batch->numChannels, (int)TransferBatch::MAX_CHANNELS);
                for (int ch = 0; ch < nCh; ch++) {
                    auto& cp = batch->channels[ch];
                    uint64_t bytes = cp.bytesTransferred.load();
                    if (bytes == 0 && cp.filesCompleted.load() == 0) continue;
                    double chSec = cp.elapsedSec.load();
                    if (chSec < 0.1) chSec = seconds;
                    if (chSec < 0.1) chSec = 0.1;
                    LOG_INFO("Perf", "  channel " + std::to_string(ch + 1) + "/" + std::to_string(nCh) +
                        " " + cp.channelName + ": blocks=" + std::to_string(cp.filesCompleted.load()) +
                        ", bytes=" + std::to_string(bytes) + " (" + formatSize(bytes) + ")" +
                        ", duration=" + std::to_string(chSec) + "s" +
                        ", avg=" + formatSpeed((double)bytes / chSec) +
                        (cp.relayReadSec.load() > 0 || cp.relaySendSec.load() > 0
                            ? ", srcRecv=" + std::to_string(cp.relayReadSec.load()) + "s" +
                              ", dstSend=" + std::to_string(cp.relaySendSec.load()) + "s"
                            : ""));
                }
            }
        };

        // --- Multi-NIC channel setup ---
        bool useMultiNic = false;
        std::vector<DeviceClient*> multiNicDevPtrs;
        std::vector<std::string> multiNicBindIps;
        std::vector<std::string> multiNicNames;
        std::string multiNicDeviceIp;

        if (m_prefs.enableMultiNic && !batch->isLocalCopy && !batch->isCrossDevice) {
            std::vector<NicBinding> activeNics;
            for (auto& nb : m_prefs.multiNicBindings)
                if (nb.enabled) activeNics.push_back(nb);

            if (activeNics.size() >= 2) {
                // Find the device's WiFi IP from any available source
                multiNicDeviceIp = batchDev.isDirectConnection() ? batchDev.deviceIp() : "";
                if (multiNicDeviceIp.empty()) {
                    // Try ADB to get wlan0 IP
                    std::string serial = batchDev.connectedSerial();
                    std::string wlanOut = m_device.runAdbCommand(
                        "-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
                    auto inetPos = wlanOut.find("inet ");
                    if (inetPos != std::string::npos) {
                        auto start = inetPos + 5;
                        auto slash = wlanOut.find('/', start);
                        if (slash != std::string::npos)
                            multiNicDeviceIp = wlanOut.substr(start, slash - start);
                    }
                    // Fallback: saved WiFi devices
                    if (multiNicDeviceIp.empty()) {
                        for (auto& w : m_prefs.savedWifiDevices) {
                            if (w.serial == serial && !w.wifiIp.empty()) {
                                multiNicDeviceIp = w.wifiIp;
                                break;
                            }
                        }
                    }
                }
                if (!multiNicDeviceIp.empty()) {
                    // Detect USB tethering IP to label USB channels correctly
                    std::string usbTetheringIp;
                    {
                        std::string serial = batchDev.connectedSerial();
                        usbTetheringIp = batchDev.detectDeviceIp(serial);
                    }

                    m_nicChannels.clear();
                    m_nicLocalIps.clear();
                    for (int i = 0; i < (int)activeNics.size(); i++) {
                        auto ch = std::make_unique<DeviceClient>();
                        if (ch->connectTcp(multiNicDeviceIp, batchDev.serverPort(), activeNics[i].localIp) &&
                            ch->verifyConnection()) {
                            multiNicDevPtrs.push_back(ch.get());
                            multiNicBindIps.push_back(activeNics[i].localIp);
                            // Label USB tethering adapters as "USB" (same /24 subnet as tethering IP)
                            std::string name = activeNics[i].adapterName;
                            if (!usbTetheringIp.empty()) {
                                auto lastDot1 = usbTetheringIp.rfind('.');
                                auto lastDot2 = activeNics[i].localIp.rfind('.');
                                if (lastDot1 != std::string::npos && lastDot2 != std::string::npos &&
                                    usbTetheringIp.substr(0, lastDot1) == activeNics[i].localIp.substr(0, lastDot2))
                                    name = "USB";
                            }
                            multiNicNames.push_back(name);
                            m_nicChannels.push_back(std::move(ch));
                            LOG_INFO("MultiNIC", "Channel " + std::to_string(i) + " via " +
                                     name + " (" + activeNics[i].localIp + ")");
                        } else {
                            LOG_WARN("MultiNIC", "Channel failed via " + activeNics[i].adapterName +
                                     " (" + activeNics[i].localIp + "): " + ch->lastError());
                        }
                    }
                    if ((int)multiNicDevPtrs.size() >= 2) {
                        useMultiNic = true;
                        batch->useMultiNic = true;
                        batch->numChannels = (int)multiNicDevPtrs.size();
                        LOG_INFO("MultiNIC", std::to_string(multiNicDevPtrs.size()) + " channels ready");
                    } else {
                        for (auto& ch : m_nicChannels) ch->disconnectTcp();
                        m_nicChannels.clear();
                        m_nicLocalIps.clear();
                        LOG_WARN("MultiNIC", "Not enough channels connected, falling back");
                    }
                }
            }
        }

        // --- Parallel transfer (dual-channel or multi-NIC) ---
        struct RuntimeChannel {
            DeviceClient* dev = nullptr;
            std::unique_ptr<DeviceClient> owned;
            std::string name;
            std::string reconnectIp;
            bool preferUsb = false;
            std::unique_ptr<TransferCancellation<DeviceClient>::Registration> cancellation;
        };
        std::vector<RuntimeChannel> runtimeChannels;
        std::string batchWifiIp;
        std::string batchUsbSerial;
        std::string batchHwSerial;

        if (batch->useParallelChannels && !batch->isLocalCopy && !batch->isCrossDevice && !useMultiNic &&
            companion::isSerial(batchDev.connectedSerial())) {
            auto transferPipes = m_prefs.pipePreferencesFor(batchDev.connectedSerial());
            RuntimeChannel primary;
            primary.dev = &batchDev;
            primary.name = companionPrimaryName(batchDev);
            primary.reconnectIp = batchDev.connectedHost();
            primary.preferUsb = !batchDev.isDirectConnection();
            runtimeChannels.push_back(std::move(primary));
            for (const auto& endpoint : companionChannelTargets(batchDev, transferPipes.usbPipeCount,
                    transferPipes.wifiPipeCount, TransferBatch::MAX_CHANNELS)) {
                if (batch->stopRequested) break;
                auto ch = std::make_unique<DeviceClient>();
                ch->setSerial(batchDev.connectedSerial());
                ch->setServerPort(endpoint.port);
                ch->setLocalPort(endpoint.port);
                if (!ch->connectTcp(endpoint.host, endpoint.port) || !ch->verifyConnection()) continue;
                RuntimeChannel rc;
                rc.dev = ch.get();
                rc.owned = std::move(ch);
                rc.cancellation = batch->cancellation.track(*rc.dev);
                rc.name = companionChannelName(endpoint.kind);
                rc.reconnectIp = endpoint.host;
                rc.preferUsb = endpoint.isUsb();
                runtimeChannels.push_back(std::move(rc));
            }
            if (runtimeChannels.size() > 1) {
                batch->numChannels = (int)runtimeChannels.size();
                LOG_INFO("Transfer", "Prepared " + std::to_string(runtimeChannels.size()) +
                    " companion channels for slot " + std::to_string(batchDeviceSlot));
            }
        } else if (batch->useParallelChannels && !batch->isLocalCopy && !batch->isCrossDevice && !useMultiNic) {
            std::string batchSerial = batchDev.connectedSerial();
            bool primaryUsesWifi = batchDev.isDirectConnection() || isWifiSerial(batchSerial);
            bool primaryIsUsb = !batchSerial.empty() && !primaryUsesWifi;
            if (!batchSerial.empty() && !isWifiSerial(batchSerial)) batchUsbSerial = batchSerial;
            if (batchDev.isDirectConnection()) batchWifiIp = batchDev.deviceIp();
            if (isWifiSerial(batchSerial)) {
                auto colon = batchSerial.rfind(':');
                if (colon != std::string::npos) batchWifiIp = batchSerial.substr(0, colon);
            }

            if (!batchSerial.empty()) {
                std::string hwOut = batchDev.runAdbCommand("-s " + batchSerial + " shell getprop ro.serialno");
                while (lastCharOr(hwOut) == '\n' || lastCharOr(hwOut) == '\r' || lastCharOr(hwOut) == ' ')
                    hwOut.pop_back();
                batchHwSerial = hwOut;
            }
            auto transferPipes = m_prefs.pipePreferencesFor(batchSerial);

            if (batchWifiIp.empty() && !batchSerial.empty()) {
                std::string wlanOut = batchDev.runAdbCommand("-s " + batchSerial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
                auto inetPos = wlanOut.find("inet ");
                if (inetPos != std::string::npos) {
                    auto start = inetPos + 5;
                    auto slash = wlanOut.find('/', start);
                    if (slash != std::string::npos)
                        batchWifiIp = wlanOut.substr(start, slash - start);
                }
            }
            if (batchWifiIp.empty()) {
                for (auto& w : m_prefs.savedWifiDevices) {
                    std::string wifiSerial = w.wifiIp + ":" + std::to_string(w.port);
                    if (!w.wifiIp.empty() && (w.serial == batchSerial || wifiSerial == batchSerial)) {
                        batchWifiIp = w.wifiIp;
                        break;
                    }
                }
            }

            if (batchUsbSerial.empty() && !batchHwSerial.empty()) {
                std::lock_guard<std::mutex> lk(m_deviceMutex);
                for (auto& d : m_devices) {
                    if (d.state != "device" || isWifiSerial(d.serial)) continue;
                    std::string hw = batchDev.runAdbCommand("-s " + d.serial + " shell getprop ro.serialno");
                    while (lastCharOr(hw) == '\n' || lastCharOr(hw) == '\r' || lastCharOr(hw) == ' ')
                        hw.pop_back();
                    if (hw == batchHwSerial) {
                        batchUsbSerial = d.serial;
                        break;
                    }
                }
            }

            RuntimeChannel primary;
            primary.dev = &batchDev;
            primary.name = primaryIsUsb ? "USB (ADB)" : (batchDev.isDirectConnection() ? "WiFi (Direct)" : "WiFi (ADB)");
            primary.reconnectIp = batchWifiIp;
            primary.preferUsb = primaryIsUsb;
            runtimeChannels.push_back(std::move(primary));

            int usbChannels = primaryIsUsb ? 1 : 0;
            int wifiChannels = primaryUsesWifi ? 1 : 0;
            int nextPort = 5840;
            int remoteHelperPort = batchDev.serverPort();

            auto addUsbChannel = [&]() -> bool {
                if (batch->stopRequested) return false;
                if (batchUsbSerial.empty()) return false;
                for (int attempt = 0; attempt < 32; attempt++) {
                    auto ch = std::make_unique<DeviceClient>();
                    ch->setAdbPath(batchDev.getAdbPath());
                    ch->setServerPort(batchDev.serverPort());
                    ch->setLocalPort(nextPort++);
                    int localPort = ch->localPort();
                    std::string fwd = batchDev.runAdbCommand("-s " + batchUsbSerial + " forward tcp:" +
                        std::to_string(localPort) + " tcp:" + std::to_string(remoteHelperPort));
                    if (fwd.find("offline") != std::string::npos)
                        return false;
                    if (fwd.find("error") != std::string::npos) {
                        LOG_WARN("Transfer", "USB forward port " + std::to_string(localPort) +
                            " failed, trying next port");
                        continue;
                    }
                    if (!ch->connectTcp("127.0.0.1", localPort) || !ch->verifyConnection()) {
                        LOG_WARN("Transfer", "USB forward port " + std::to_string(localPort) +
                            " connected but did not verify");
                        batchDev.runAdbCommand("-s " + batchUsbSerial + " forward --remove tcp:" +
                            std::to_string(localPort));
                        return false;
                    }
                    RuntimeChannel rc;
                    rc.dev = ch.get();
                    rc.owned = std::move(ch);
                    rc.cancellation = batch->cancellation.track(*rc.dev);
                    rc.name = "USB (ADB)";
                    rc.reconnectIp = batchWifiIp;
                    rc.preferUsb = true;
                    runtimeChannels.push_back(std::move(rc));
                    usbChannels++;
                    return true;
                }
                return false;
            };

            auto addWifiChannel = [&]() -> bool {
                if (batch->stopRequested) return false;
                if (batchWifiIp.empty()) return false;
                auto ch = std::make_unique<DeviceClient>();
                ch->setAdbPath(batchDev.getAdbPath());
                ch->setServerPort(batchDev.serverPort());
                ch->setLocalPort(nextPort++);
                if (!ch->connectTcp(batchWifiIp, remoteHelperPort) || !ch->verifyConnection())
                    return false;
                RuntimeChannel rc;
                rc.dev = ch.get();
                rc.owned = std::move(ch);
                rc.cancellation = batch->cancellation.track(*rc.dev);
                rc.name = "WiFi (Direct)";
                rc.reconnectIp = batchWifiIp;
                rc.preferUsb = false;
                runtimeChannels.push_back(std::move(rc));
                wifiChannels++;
                return true;
            };

            while (usbChannels < transferPipes.usbPipeCount && runtimeChannels.size() < TransferBatch::MAX_CHANNELS)
                if (!addUsbChannel()) break;
            while (wifiChannels < transferPipes.wifiPipeCount && runtimeChannels.size() < TransferBatch::MAX_CHANNELS)
                if (!addWifiChannel()) break;

            if (runtimeChannels.size() > 1) {
                batch->numChannels = (int)runtimeChannels.size();
                LOG_INFO("Transfer", "Prepared " + std::to_string(runtimeChannels.size()) +
                    " channels for slot " + std::to_string(batchDeviceSlot));
            }
        }

        bool doDualChannel = batch->useParallelChannels && runtimeChannels.size() > 1 && !useMultiNic;
        if (!doDualChannel && !useMultiNic)
            batch->useParallelChannels = false;
        if ((doDualChannel || useMultiNic) && !batch->isLocalCopy &&
            !batch->isCrossDevice && batch->totalFiles() >= 1) {
            LOG_INFO("Transfer", useMultiNic ?
                "Starting multi-NIC parallel transfer (" + std::to_string(batch->numChannels) +
                " channels, " + std::to_string(batch->totalFiles()) + " files)" :
                "Starting parallel dual-channel transfer (" +
                std::to_string(batch->totalFiles()) + " files)");

            std::vector<std::unique_ptr<TransferCancellation<DeviceClient>::Registration>> nicCancellation;
            if (useMultiNic) for (auto* client : multiNicDevPtrs)
                nicCancellation.push_back(batch->cancellation.track(*client));
            auto parallelStart = std::chrono::steady_clock::now();

            // Setup per-channel names
            if (useMultiNic) {
                for (int i = 0; i < (int)multiNicDevPtrs.size(); i++)
                    batch->channels[i].channelName = multiNicNames[i];
            } else {
                for (int i = 0; i < (int)runtimeChannels.size(); i++)
                    batch->channels[i].channelName = runtimeChannels[i].name;
            }

            // === Work-stealing block queue for file distribution ===
            // Small files (<200MB): go into a shared whole-file queue
            // Large files (>=200MB): split into 200MB blocks, go into a shared block queue
            // Both channels pull from the queue — faster channel naturally gets more work

            // Queue large logical ranges to keep scheduling overhead low. Workers
            // split those ranges at assignment time using their observed speed, so
            // fast links receive larger slices and slow links do not own huge ranges.
            static const uint64_t MB = 1024ULL * 1024;
            auto largeBlockSizeFor = [](uint64_t fileSize) -> uint64_t {
                const uint64_t MB = 1024ULL * 1024;
                const uint64_t GB = 1024ULL * MB;
                if (fileSize >= 10 * GB) return 2 * GB;
                if (fileSize >= 2 * GB)  return 1 * GB;
                if (fileSize >= 500 * MB) return 500 * MB;
                return 200 * MB;
            };
            auto tailBlockSizeFor = [](uint64_t fileSize) -> uint64_t {
                const uint64_t MB = 1024ULL * 1024;
                const uint64_t GB = 1024ULL * MB;
                if (fileSize >= 2 * GB) return 256 * MB;
                return 200 * MB;
            };
            auto blockSizeFor = [&](uint64_t fileSize, uint64_t remaining, int channels) -> uint64_t {
                uint64_t largeBlock = largeBlockSizeFor(fileSize);
                uint64_t tailBlock = tailBlockSizeFor(fileSize);
                uint64_t tailWindow = largeBlock * (uint64_t)std::max(2, channels * 2);
                return remaining > tailWindow ? largeBlock : tailBlock;
            };
            auto assignmentSliceFor = [&](uint64_t fileSize, uint64_t available, double channelSpeed) -> uint64_t {
                const uint64_t MB = 1024ULL * 1024;
                const uint64_t minSlice = 64 * MB;
                const uint64_t probeSlice = 128 * MB;
                const uint64_t align = 16 * MB;
                uint64_t maxSlice = largeBlockSizeFor(fileSize);
                double targetSec = 2.0;
                uint64_t desired = probeSlice;
                if (channelSpeed > 4.0 * (double)MB)
                    desired = (uint64_t)(channelSpeed * targetSec);
                desired = std::max(minSlice, std::min(desired, maxSlice));
                desired = (desired / align) * align;
                if (desired < minSlice) desired = minSlice;
                return std::min(available, desired);
            };

            struct WorkBlock {
                int fileIndex;
                uint64_t offset;    // byte offset within file (0 for whole files)
                uint64_t length;    // bytes to transfer (fileSize for whole files)
                bool isWholeFile;   // true = use pullFile, false = use readRangeStreaming
                int blockId;        // sequential ID for CRC ordering
                int retries = 0;    // how many times this block has been requeued after failure
            };

            std::vector<bool> skippedFileIndices(batch->totalFiles(), false);
            std::vector<bool> existingDestination(batch->totalFiles(), false);
            // --- File conflict check for parallel transfers ---
            for (int fi = 0; fi < batch->totalFiles() && !m_shutdownTransfer; fi++) {
                if (batch->stopRequested.load()) break;
                auto& item = batch->files[fi];
                if (item.isDirectory) continue;

                bool destExists = false;
                if (batch->isPull) {
                    destExists = std::filesystem::exists(toFsPath(item.destPath));
                } else {
                    DeviceClient& destDev = batchDev;
                    uint64_t remoteSize = destDev.getFileSize(item.destPath);
                    destExists = (remoteSize > 0);
                }

                existingDestination[fi] = destExists;
                if (destExists) {
                    ConflictAction action = batch->conflictAllDecision;
                    if (action == ConflictAction::None) {
                        batch->beginConflict(item);
                        {
                            std::unique_lock<std::mutex> lk(m_batchMutex);
                            m_batchCV.wait(lk, [&]() {
                                return m_shutdownTransfer || batch->stopRequested.load() ||
                                       batch->conflictResponse.load() != ConflictAction::None;
                            });
                        }
                        action = batch->finishConflict();
                        batch->state = BatchState::Running;

                        if (action == ConflictAction::OverwriteAll) {
                            batch->conflictAllDecision = ConflictAction::OverwriteAll;
                            action = ConflictAction::Overwrite;
                        } else if (action == ConflictAction::SkipAll) {
                            batch->conflictAllDecision = ConflictAction::SkipAll;
                            action = ConflictAction::Skip;
                        }
                    } else {
                        if (action == ConflictAction::OverwriteAll) action = ConflictAction::Overwrite;
                        else if (action == ConflictAction::SkipAll) action = ConflictAction::Skip;
                    }

                    if (batch->stopRequested.load() || m_shutdownTransfer) break;

                    if (action == ConflictAction::Skip) {
                        batch->skippedFiles++;
                        skippedFileIndices[fi] = true;
                        continue;
                    }
                }
            }
            if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; continue; }

            // Recalculate total after skips
            {
                uint64_t newTotal = 0;
                for (int fi = 0; fi < batch->totalFiles(); ++fi)
                    if (!skippedFileIndices[fi]) newTotal += batch->files[fi].fileSize;
                batch->totalBytes = newTotal;
            }

            // Build the work queue
            std::deque<WorkBlock> workQueue;
            std::mutex workMutex;
            int blockIdCounter = 0;

            // Track per-file block completion for partial cleanup on cancel
            std::set<int> completedFileIndices;

            std::vector<TransferCoverage> coverage;
            for (int fi = 0; fi < batch->totalFiles(); ++fi) {
                coverage.emplace_back(batch->files[fi].isDirectory ? 0 : batch->files[fi].fileSize);
                if (skippedFileIndices[fi]) coverage.back().skip();
            }
            auto failFile = [&](int fileIndex) {
                bool firstFailure = false;
                {
                    std::lock_guard<std::mutex> lk(workMutex);
                    firstFailure = !coverage[fileIndex].failed();
                    coverage[fileIndex].fail();
                    completedFileIndices.erase(fileIndex);
                    std::erase_if(workQueue, [&](const WorkBlock& pending) { return pending.fileIndex == fileIndex; });
                }
                if (firstFailure) {
                    ++batch->errorSkippedFiles;
                    std::lock_guard<std::mutex> lk(batch->errorSkippedMutex);
                    batch->errorSkippedNames.push_back(batch->files[fileIndex].displayName);
                }
            };

            // Pre-create output files for split files (both channels write via OVERLAPPED)
            struct SplitFileHandle {
                int fileIndex;
                HANDLE handle;
                uint64_t fileSize;
            };
            std::vector<SplitFileHandle> splitHandles;

            // Track CRC per block for combining later
            struct BlockCrc {
                int blockId;
                uint64_t offset;
                uint32_t crc;
                uint64_t length;
                int fileIndex;
            };
            std::vector<BlockCrc> allBlockCrcs;
            std::mutex crcMutex;

            // Store inline CRCs for whole-file pulls (each pullFile overwrites the previous,
            // so we capture them per-file for use during verification)
            std::unordered_map<std::string, uint32_t> wholeFileCrcs;  // destPath -> CRC

            int schedulingChannels = useMultiNic ? batch->numChannels : (int)runtimeChannels.size();
            schedulingChannels = std::max(1, schedulingChannels);
            for (int i = 0; i < batch->totalFiles(); i++) {
                auto& item = batch->files[i];
                if (skippedFileIndices[i]) continue;
                if (item.isDirectory) {
                    // Directories: just create, no transfer
                    workQueue.push_back({i, 0, 0, true, blockIdCounter++});
                } else if (item.isMcrawVirtual || item.fileSize < 200 * MB) {
                    // Small files: transfer as whole file (pushFile/pullFile streaming)
                    workQueue.push_back({i, 0, item.fileSize, true, blockIdCounter++});
                } else {
                    // Large file: split into blocks (size scales with file size)
                    uint64_t remaining = item.fileSize;
                    uint64_t offset = 0;
                    int blockCount = 0;
                    while (remaining > 0) {
                        uint64_t blkSize = blockSizeFor(item.fileSize, remaining, schedulingChannels);
                        uint64_t blockLen = std::min(remaining, blkSize);
                        workQueue.push_back({i, offset, blockLen, false, blockIdCounter++});
                        offset += blockLen;
                        remaining -= blockLen;
                        blockCount++;
                    }
                    if (batch->isPull) {
                        // Ensure parent directory exists before creating the split file
                        // (directory batch items haven't been processed by workers yet)
                        try { std::filesystem::create_directories(std::filesystem::path(toFsPath(item.destPath)).parent_path()); } catch (...) {}
                        // Pre-create local output file
                        HANDLE h = CreateFileW(toWide(item.destPath).c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (h != INVALID_HANDLE_VALUE) {
                            LARGE_INTEGER li; li.QuadPart = (LONGLONG)item.fileSize;
                            SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
                            SetEndOfFile(h);
                            splitHandles.push_back({i, h, item.fileSize});
                        }
                    } else {
                        // Pre-create remote file on device with pre-allocation
                        batchDev.createFile(item.destPath, item.fileSize);
                        splitHandles.push_back({i, INVALID_HANDLE_VALUE, item.fileSize});
                    }
                    LOG_INFO("Transfer", "Splitting " + item.displayName + " (" + formatSize(item.fileSize) +
                             ") into " + std::to_string(blockCount) + " adaptive blocks");
                }
            }

            bool hasDirectoryItems = false;
            for (auto& item : batch->files) {
                if (item.isDirectory) {
                    hasDirectoryItems = true;
                    break;
                }
            }
            if (!hasDirectoryItems && batch->totalFiles() > 1 && workQueue.size() > 1) {
                std::vector<std::deque<WorkBlock>> byFile(batch->totalFiles());
                for (auto& block : workQueue) {
                    if (block.fileIndex >= 0 && block.fileIndex < (int)byFile.size())
                        byFile[block.fileIndex].push_back(block);
                }

                std::deque<WorkBlock> interleaved;
                bool added = true;
                while (added) {
                    added = false;
                    for (auto& blocks : byFile) {
                        if (blocks.empty()) continue;
                        interleaved.push_back(blocks.front());
                        blocks.pop_front();
                        added = true;
                    }
                }
                if (interleaved.size() == workQueue.size()) {
                    workQueue.swap(interleaved);
                    LOG_INFO("Transfer", "Interleaved " + std::to_string(batch->totalFiles()) +
                        " files across " + std::to_string(workQueue.size()) + " work blocks");
                }
            }

            int totalBlocks = (int)workQueue.size();
            int numCh = useMultiNic ? batch->numChannels : (int)runtimeChannels.size();
            uint64_t perChBytes = batch->totalBytes.load() / numCh;
            for (int c = 0; c < numCh; c++) {
                batch->channels[c].totalBytes = perChBytes;
                batch->channels[c].filesAssigned = (c == 0) ? totalBlocks : 0;
            }

            // Worker: pull blocks from shared queue (work-stealing)
            // Store WiFi IP for reconnection (secondary channel has no serial set)
            // Use the IP that was actually used for the secondary channel, not a saved one
            std::string secondaryWifiIp;
            {
                secondaryWifiIp = batchWifiIp;
                // Extract IP from the actual secondary connection
                // The secondary channel was established to the WiFi IP of the current device
                std::string serial = batchDev.connectedSerial();
                // Try to get WiFi IP from the current device
                if (secondaryWifiIp.empty() && !serial.empty()) {
                    std::string wlanOut = m_device.runAdbCommand("-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
                    auto inetPos = wlanOut.find("inet ");
                    if (inetPos != std::string::npos) {
                        auto start = inetPos + 5;
                        auto slash = wlanOut.find('/', start);
                        if (slash != std::string::npos)
                            secondaryWifiIp = wlanOut.substr(start, slash - start);
                    }
                }
                // Fallback: extract from the saved device matching this serial
                if (secondaryWifiIp.empty()) {
                    for (auto& w : m_prefs.savedWifiDevices) {
                        std::string wifiSerial = w.wifiIp + ":" + std::to_string(w.port);
                        if (!w.wifiIp.empty() && (w.serial == serial || wifiSerial == serial)) {
                            secondaryWifiIp = w.wifiIp;
                            break;
                        }
                    }
                }
                if (!secondaryWifiIp.empty())
                    LOG_INFO("Transfer", "Secondary WiFi reconnect IP: " + secondaryWifiIp);
            }

            // Capture the device's hardware serial (ro.serialno) so reconnect can verify
            // it's the same device, not a different one that also has a server running
            std::string deviceHwSerial;
            {
                std::string serial = batchDev.connectedSerial();
                if (serial.empty()) {
                    // Try any available serial for the query
                    std::string devList = m_device.runAdbCommand("devices");
                    std::istringstream diss(devList);
                    std::string dline;
                    while (std::getline(diss, dline))
                        if (dline.find("\tdevice") != std::string::npos) {
                            auto tab = dline.find('\t');
                            if (tab != std::string::npos) { serial = dline.substr(0, tab); break; }
                        }
                }
                if (!serial.empty() && !companion::isSerial(serial)) {
                    std::string hwOut = m_device.runAdbCommand("-s " + serial + " shell getprop ro.serialno");
                    // Trim whitespace
                    while (lastCharOr(hwOut) == '\n' || lastCharOr(hwOut) == '\r' || lastCharOr(hwOut) == ' ')
                        hwOut.pop_back();
                    deviceHwSerial = hwOut;
                    LOG_INFO("Transfer", "Device hardware serial: " + deviceHwSerial);
                }
            }

            // (usbChannelCount removed — each channel independently restores its preferred transport)

            std::mutex aggregateProgressMutex;
            uint64_t aggregateLastBytes = 0;
            auto aggregateLastTime = parallelStart;

            auto worker = [&, deviceHwSerial](DeviceClient& dev, TransferBatch::ChannelProgress& ch,
                              const std::string& reconnectIp, const std::string& bindLocalIp,
                              bool preferUsb) {
                auto chStart = std::chrono::steady_clock::now();
                uint64_t chBytesCompleted = 0;
                uint64_t retryGeneration = 0;
                uint64_t lastSpeedBytes = 0;
                auto lastSpeedTime = chStart;
                // Capture original serial and whether this channel uses ADB forward at start,
                // so reconnect logic uses the correct transport even if serial auto-switches
                std::string origSerial = dev.connectedSerial();
                bool origIsAdbForward = !dev.isDirectConnection();
                // Channel 0 (primary) prefers USB, channel 1 (secondary) prefers WiFi
                bool origIsUsb = preferUsb;
                // Determine ACTUAL current physical link from the connection state
                bool currentIsUsb = !origSerial.empty() && !isWifiSerial(origSerial);
                if (!currentIsUsb && origIsAdbForward) {
                    // ADB forward through WiFi serial = WiFi link, through USB serial = USB link
                    currentIsUsb = !origSerial.empty() && !isWifiSerial(origSerial);
                }
                auto lastUpgradeCheck = std::chrono::steady_clock::now();

                while (!m_shutdownTransfer && !batch->stopRequested.load() &&
                       batch->state.load() != BatchState::Failed) {
                    // Every 5s, check if we can restore the original transport for true dual-channel.
                    // This catches: USB dropped → both on WiFi → USB comes back → upgrade one channel.
                    // Also: WiFi dropped → both on USB → WiFi comes back → upgrade one channel.
                    {
                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration<double>(now - lastUpgradeCheck).count() >= 5.0) {
                            lastUpgradeCheck = now;
                            // Only upgrade if we're NOT on our original physical link
                            // AND both channels are on the same link (otherwise already dual-channel)
                            if (currentIsUsb != origIsUsb && !companion::isSerial(origSerial)) {
                                bool upgraded = false;
                                if (origIsUsb) {
                                    // Originally USB, now on WiFi. Try to get back to USB.
                                    // Find USB serial for this device
                                    std::string devList = dev.runAdbCommand("devices");
                                    std::istringstream iss(devList);
                                    std::string line;
                                    while (std::getline(iss, line)) {
                                        if (line.find("\tdevice") == std::string::npos) continue;
                                        auto tab = line.find('\t');
                                        if (tab == std::string::npos) continue;
                                        std::string s = line.substr(0, tab);
                                        if (s.empty() || isWifiSerial(s)) continue;
                                        // Verify it's our device
                                        if (!deviceHwSerial.empty()) {
                                            std::string hw = dev.runAdbCommand("-s " + s + " shell getprop ro.serialno");
                                            while (lastCharOr(hw) == '\n' || lastCharOr(hw) == '\r' || lastCharOr(hw) == ' ') hw.pop_back();
                                            if (hw != deviceHwSerial) continue;
                                        }
                                        std::string fwd = dev.runAdbCommand("-s " + s + " forward tcp:" +
                                            std::to_string(dev.localPort()) + " tcp:" + std::to_string(dev.serverPort()));
                                        if (fwd.find("error") == std::string::npos && fwd.find("offline") == std::string::npos) {
                                            dev.disconnectTcp();
                                            if (dev.connectTcp("127.0.0.1", dev.localPort()) && dev.verifyConnection()) {
                                                ch.channelName = "USB (ADB)";
                                                LOG_INFO("Transfer", ch.channelName + " upgraded back to USB");
                                                upgraded = true;
                                            }
                                        }
                                        break;
                                    }
                                } else {
                                    // Originally WiFi, now on USB. Try to get back to WiFi.
                                    // IMPORTANT: probe with a throwaway socket first so we don't
                                    // kill the working USB connection if WiFi is still down.
                                    std::string ip = reconnectIp;
                                    if (ip.empty() && !origSerial.empty()) {
                                        auto colon = origSerial.rfind(':');
                                        if (colon != std::string::npos) ip = origSerial.substr(0, colon);
                                    }
                                    if (!ip.empty()) {
                                        // Quick TCP probe without disrupting current connection
                                        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                                        if (probe != INVALID_SOCKET) {
                                            u_long nonBlock = 1;
                                            ioctlsocket(probe, FIONBIO, &nonBlock);
                                            struct sockaddr_in addr = {};
                                            addr.sin_family = AF_INET;
                                            addr.sin_port = htons(dev.serverPort());
                                            inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
                                            connect(probe, (struct sockaddr*)&addr, sizeof(addr));
                                            fd_set wset;
                                            FD_ZERO(&wset);
                                            FD_SET(probe, &wset);
                                            struct timeval tv = {1, 0}; // 1 second timeout
                                            bool reachable = (select((int)probe + 1, nullptr, &wset, nullptr, &tv) > 0);
                                            closesocket(probe);
                                            if (reachable) {
                                                // WiFi is reachable, now switch for real
                                                dev.disconnectTcp();
                                                if (dev.connectTcp(ip, dev.serverPort()) && dev.verifyConnection()) {
                                                    ch.channelName = "WiFi (Direct)";
                                                    LOG_INFO("Transfer", ch.channelName + " upgraded back to WiFi");
                                                    upgraded = true;
                                                }
                                            }
                                        }
                                    }
                                }
                                if (upgraded) {
                                    bool wasUsb = currentIsUsb;
                                    currentIsUsb = origIsUsb;
                                    ch.speed = 0;
                                    lastSpeedBytes = ch.bytesTransferred.load();
                                    lastSpeedTime = std::chrono::steady_clock::now();
                                }
                            }
                        }
                    }

                    // Grab a logical range, then split off only the slice this
                    // channel should own based on its current measured speed.
                    WorkBlock block;
                    {
                        std::lock_guard<std::mutex> lk(workMutex);
                        if (workQueue.empty()) break;
                        block = workQueue.front();
                        workQueue.pop_front();
                        if (coverage[block.fileIndex].failed()) continue;
                        if (!block.isWholeFile && block.length > 0) {
                            auto& queuedItem = batch->files[block.fileIndex];
                            uint64_t assignLen = assignmentSliceFor(
                                queuedItem.fileSize, block.length, ch.speed.load());
                            if (assignLen < block.length) {
                                WorkBlock remainder = block;
                                remainder.offset += assignLen;
                                remainder.length -= assignLen;
                                workQueue.push_front(remainder);
                                block.length = assignLen;
                            }
                        }
                    }

                    auto& item = batch->files[block.fileIndex];
                    if (item.isDirectory) {
                        bool created = false;
                        if (batch->isPull) {
                            std::error_code error;
                            std::filesystem::create_directories(toFsPath(item.destPath), error);
                            created = !error;
                        } else created = dev.createDirectory(item.destPath);
                        if (!created) failFile(block.fileIndex);
                        else {
                            std::lock_guard<std::mutex> lk(workMutex);
                            coverage[block.fileIndex].record(0, 0);
                            completedFileIndices.insert(block.fileIndex);
                            ch.filesCompleted++;
                        }
                        continue;
                    }

                    ch.curBlockSize = block.length;
                    ch.curBlockTransferred = 0;
                    if (block.isWholeFile)
                        ch.currentFile = item.displayName;
                    else
                        ch.currentFile = item.displayName + " [block " + formatSize(block.offset) + "]";

                    // Progress callback (shared by whole-file and block transfers)
                    auto updateProgress = [&](uint64_t blockTransferred) {
                        ch.bytesTransferred = chBytesCompleted + blockTransferred;
                        ch.curBlockTransferred = blockTransferred;
                        // Speed
                        auto now = std::chrono::steady_clock::now();
                        double dt = std::chrono::duration<double>(now - lastSpeedTime).count();
                        if (dt >= 0.5) {
                            double delta = (double)ch.bytesTransferred.load() - (double)lastSpeedBytes;
                            double instant = delta / dt;
                            // Cap instant speed to 10 GB/s to avoid overflow spikes during recovery
                            if (instant > 10e9) instant = 0;
                            if (instant < 0) instant = 0;
                            ch.speed = (ch.speed.load() > 0) ? ch.speed.load() * 0.8 + instant * 0.2 : instant;
                            lastSpeedBytes = ch.bytesTransferred.load();
                            lastSpeedTime = now;
                        }
                        // Effective batch progress and speed across all channels.
                        std::lock_guard<std::mutex> progressLk(aggregateProgressMutex);
                        uint64_t combined = 0;
                        for (int c = 0; c < numCh; c++)
                            combined += batch->channels[c].bytesTransferred.load();
                        uint64_t tb = batch->totalBytes.load();
                        if (tb > 0 && combined > tb)
                            combined = tb;
                        batch->totalTransferred = combined;
                        if (tb > 0) batch->totalProgress = (float)((double)combined / (double)tb);

                        auto totalNow = std::chrono::steady_clock::now();
                        double totalDt = std::chrono::duration<double>(totalNow - aggregateLastTime).count();
                        if (totalDt >= 0.5) {
                            double instant = ((double)combined - (double)aggregateLastBytes) / totalDt;
                            if (instant < 0 || instant > 10e9) instant = 0;
                            double prev = batch->speedBytesPerSec.load();
                            double effectiveSpeed = prev > 0 ? prev * 0.8 + instant * 0.2 : instant;
                            batch->speedBytesPerSec = effectiveSpeed;
                            batch->etaSeconds = (effectiveSpeed > 0 && tb > combined)
                                ? (double)(tb - combined) / effectiveSpeed : -1.0;
                            aggregateLastBytes = combined;
                            aggregateLastTime = totalNow;
                        }
                    };

                    auto progressCb = [&](uint64_t transferred, uint64_t total) -> bool {
                        if (m_shutdownTransfer || batch->stopRequested.load()) return false;
                        while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                            batch->state = BatchState::Paused;
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        if (m_shutdownTransfer || batch->stopRequested.load()) return false;
                        batch->state = BatchState::Running;
                        updateProgress(transferred);
                        return true;
                    };

                    bool ok = false;

                    if (block.isWholeFile) {
                        // Whole file transfer
                        uint64_t outSize = 0;
                        if (batch->isPull) {
                            if (item.isMcrawVirtual) {
                                ok = dev.pullMcrawItem(item.mcrawPath, item.virtualName,
                                                       item.destPath, outSize, progressCb);
                            } else {
                                ok = dev.pullFile(item.sourcePath, item.destPath, outSize, progressCb);
                                // Capture inline CRC before next pullFile overwrites it
                                if (ok && !batch->stopRequested.load()) {
                                    std::lock_guard<std::mutex> lk(crcMutex);
                                    wholeFileCrcs[item.destPath] = dev.getInlineCrc();
                                }
                            }
                        } else {
                            ok = dev.pushFile(item.sourcePath, item.destPath, item.fileSize, progressCb);
                            // Capture inline CRC (computed from local source file during push)
                            if (ok && !batch->stopRequested.load()) {
                                std::lock_guard<std::mutex> lk(crcMutex);
                                wholeFileCrcs[item.sourcePath] = dev.getInlineCrc();
                            }
                        }
                    } else if (batch->isPull) {
                        // Pull block: read range from device, write to local file
                        uint32_t blockCrc = ~(uint32_t)0;
                        HANDLE fh = INVALID_HANDLE_VALUE;
                        for (auto& sh : splitHandles)
                            if (sh.fileIndex == block.fileIndex) { fh = sh.handle; break; }

                        uint64_t blockReceived = 0;
                        auto rangeCb = [&](const void* data, uint32_t len, uint64_t offset) -> bool {
                            if (m_shutdownTransfer || batch->stopRequested.load()) return false;
                            while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                                batch->state = BatchState::Paused;
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                            if (m_shutdownTransfer || batch->stopRequested.load()) return false;
                            batch->state = BatchState::Running;
                            crc32Update(blockCrc, data, len);
                            if (fh != INVALID_HANDLE_VALUE) {
                                OVERLAPPED ov = {};
                                ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
                                ov.OffsetHigh = (DWORD)(offset >> 32);
                                DWORD written;
                                WriteFile(fh, data, len, &written, &ov);
                            }
                            blockReceived += len;
                            updateProgress(blockReceived);
                            return true;
                        };

                        uint64_t got = dev.readRangeStreaming(item.sourcePath, block.offset, block.length, rangeCb);
                        ok = (got >= block.length) || batch->stopRequested.load();
                        if (ok && !batch->stopRequested.load()) {
                            std::lock_guard<std::mutex> lk(crcMutex);
                            allBlockCrcs.push_back({block.blockId, block.offset, ~blockCrc, block.length, block.fileIndex});
                        }
                    } else {
                        // Push block: stream local file range to device via single writeRangeStreaming call
                        uint64_t written = dev.writeRangeStreaming(
                            item.destPath, block.offset,
                            item.sourcePath, block.offset, block.length,
                            progressCb);
                        ok = (written >= block.length) || batch->stopRequested.load();
                        // Compute block CRC from local file for verification (avoids re-reading later)
                        if (ok && !batch->stopRequested.load()) {
                            uint32_t blockCrc = ~(uint32_t)0;
                            HANDLE hCrc = CreateFileW(toWide(item.sourcePath).c_str(), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                            if (hCrc != INVALID_HANDLE_VALUE) {
                                LARGE_INTEGER seekPos; seekPos.QuadPart = (LONGLONG)block.offset;
                                SetFilePointerEx(hCrc, seekPos, nullptr, FILE_BEGIN);
                                char crcBuf[256 * 1024];
                                uint64_t remaining = block.length;
                                while (remaining > 0) {
                                    DWORD toRead = (DWORD)std::min(remaining, (uint64_t)sizeof(crcBuf));
                                    DWORD bytesRead = 0;
                                    if (!ReadFile(hCrc, crcBuf, toRead, &bytesRead, nullptr) || bytesRead == 0) break;
                                    crc32Update(blockCrc, crcBuf, bytesRead);
                                    remaining -= bytesRead;
                                }
                                CloseHandle(hCrc);
                                std::lock_guard<std::mutex> lk(crcMutex);
                                allBlockCrcs.push_back({block.blockId, block.offset, ~blockCrc, block.length, block.fileIndex});
                            }
                        }
                    }

                    if (!ok && !batch->stopRequested.load()) {
                        std::string errMsg = dev.lastError();

                        bool isLocalWriteError = batch->isPull &&
                            (errMsg.find("Failed to create local file") != std::string::npos ||
                             errMsg.find("Failed to open local file") != std::string::npos ||
                             errMsg.find("Cannot open") != std::string::npos ||
                             errMsg.find("Access is denied") != std::string::npos);

                        if (isLocalWriteError) {
                            LOG_WARN("Transfer", ch.channelName + " local destination failed: " +
                                     item.displayName + " - " + errMsg);
                            batch->state = BatchState::Failed;
                            batch->errorMessage = errMsg +
                                "\n\nThe destination may require administrator rights or be read-only.";
                            {
                                std::lock_guard<std::mutex> lk(workMutex);
                                workQueue.clear();
                            }
                            break;
                        }

                        // Check if this is a file-level error (server responded properly)
                        // vs an actual connection failure. File errors like "No such file or
                        // directory" or "Permission denied" mean the connection is still alive,
                        // we just need to skip this file and move on.
                        bool isFileError = (errMsg.find("No such file") != std::string::npos ||
                                            errMsg.find("Permission denied") != std::string::npos ||
                                            errMsg.find("Is a directory") != std::string::npos ||
                                            errMsg.find("Not a directory") != std::string::npos);

                        if (isFileError) {
                            LOG_WARN("Transfer", ch.channelName + " failed: " + item.displayName + " - " + errMsg);
                            failFile(block.fileIndex);
                            ch.bytesTransferred = chBytesCompleted;
                            ch.curBlockTransferred = 0;
                            continue;
                        }

                        ++block.retries;
                        if (block.retries >= 3) {
                            LOG_WARN("Transfer", ch.channelName + " giving up after " +
                                std::to_string(block.retries) + " retries: " + item.displayName);
                            failFile(block.fileIndex);
                            ch.bytesTransferred = chBytesCompleted;
                            ch.curBlockTransferred = 0;
                            continue;
                        }

                        LOG_WARN("Transfer", ch.channelName + " channel lost: " + item.displayName +
                                 " - " + errMsg);
                        ch.bytesTransferred = chBytesCompleted;
                        ch.curBlockTransferred = 0;
                        ch.speed = 0;
                        ch.currentFile = "(reconnecting...)";

                        // Return failed block to queue so other channel can grab it meanwhile
                        {
                            std::lock_guard<std::mutex> lk(workMutex);
                            workQueue.push_back(block);
                        }

                        // Try to reconnect just the TCP socket (NOT restart the server)
                        // Keep trying until transfer ends or stop is requested
                        bool recovered = false;
                        // Use the serial captured at worker start to reconnect to the same transport
                        std::string serial = origSerial;
                        for (int retry = 1; !m_shutdownTransfer && !batch->stopRequested.load(); retry++) {
                            // Check if there are still blocks left to process
                            {
                                std::lock_guard<std::mutex> lk(workMutex);
                                if (workQueue.empty()) break; // no work left, no point reconnecting
                            }
                            ch.currentFile = "(reconnecting #" + std::to_string(retry) + ")";
                            LOG_INFO("Transfer", ch.channelName + " reconnect attempt " + std::to_string(retry));
                            if (!waitForRetry(std::chrono::seconds(3), retryGeneration)) break;

                            // Disconnect old socket
                            dev.disconnectTcp();

                            bool connected = false;
                            std::string actualTransport = ch.channelName; // default to current name
                            if (companion::isSerial(serial)) {
                                for (const auto& endpoint : companion::endpointsFor(companion::idFromSerial(serial))) {
                                    if (dev.connectTcp(endpoint.host, endpoint.port) && dev.verifyConnection()) {
                                        connected = true;
                                        actualTransport = companionChannelName(endpoint.kind);
                                        break;
                                    }
                                }
                            } else if (!bindLocalIp.empty()) {
                                // Multi-NIC channel: reconnect with same NIC binding
                                connected = dev.connectTcp(reconnectIp, dev.serverPort(), bindLocalIp) && dev.verifyConnection();
                            } else {
                                // Universal reconnect: try all available transports
                                // 1. Try original transport first
                                if (origIsAdbForward) {
                                    // Try ADB forward first (original transport)
                                    // ... then fall back to WiFi direct
                                } else {
                                    // Try WiFi direct first (original transport)
                                    std::string ip;
                                    if (!serial.empty()) {
                                        auto colon = serial.rfind(':');
                                        ip = (colon != std::string::npos) ? serial.substr(0, colon) : serial;
                                    } else if (!reconnectIp.empty()) {
                                        ip = reconnectIp;
                                    }
                                    if (!ip.empty()) {
                                        connected = dev.connectTcp(ip, dev.serverPort()) && dev.verifyConnection();
                                        if (connected) actualTransport = "WiFi (Direct)";
                                    }
                                }

                                // 2. Try ADB forward (works for both channel types as fallback)
                                if (!connected) {
                                    std::vector<std::string> candidates;
                                    if (!serial.empty()) candidates.push_back(serial);
                                    {
                                        std::string devList = dev.runAdbCommand("devices");
                                        std::istringstream iss(devList);
                                        std::string line;
                                        while (std::getline(iss, line)) {
                                            if (line.find("\tdevice") != std::string::npos) {
                                                auto tab = line.find('\t');
                                                if (tab != std::string::npos) {
                                                    std::string s = line.substr(0, tab);
                                                    if (!s.empty() && s != serial) candidates.push_back(s);
                                                }
                                            }
                                        }
                                    }
                                    for (auto& cand : candidates) {
                                        if (cand != serial && deviceHwSerial.empty()) continue;
                                        if (!deviceHwSerial.empty()) {
                                            std::string hwCheck = dev.runAdbCommand("-s " + cand + " shell getprop ro.serialno");
                                            while (lastCharOr(hwCheck) == '\n' || lastCharOr(hwCheck) == '\r' || lastCharOr(hwCheck) == ' ')
                                                hwCheck.pop_back();
                                            if (hwCheck != deviceHwSerial) continue;
                                        }
                                        std::string fwdResult = dev.runAdbCommand("-s " + cand + " forward tcp:" +
                                            std::to_string(dev.localPort()) + " tcp:" + std::to_string(dev.serverPort()));
                                        if (fwdResult.find("error") == std::string::npos &&
                                            fwdResult.find("offline") == std::string::npos) {
                                            connected = dev.connectTcp("127.0.0.1", dev.localPort()) && dev.verifyConnection();
                                            if (connected) {
                                                actualTransport = isWifiSerial(cand) ? "WiFi (ADB)" : "USB (ADB)";
                                                LOG_INFO("Transfer", "Reconnected via ADB forward to serial: " + cand);
                                                break;
                                            }
                                        }
                                    }
                                }

                                // 3. If ADB-forward channel, also try WiFi direct as last resort
                                if (!connected && origIsAdbForward && !reconnectIp.empty()) {
                                    connected = dev.connectTcp(reconnectIp, dev.serverPort()) && dev.verifyConnection();
                                    if (connected) actualTransport = "WiFi (Direct)";
                                }
                            }

                            if (connected) {
                                // Update channel name to reflect actual transport used
                                if (!bindLocalIp.empty()) {
                                    // Multi-NIC: keep original name
                                } else {
                                    ch.channelName = actualTransport;
                                }
                                // Track which physical link we're on for upgrade checks
                                currentIsUsb = (actualTransport == "USB (ADB)");
                                LOG_INFO("Transfer", ch.channelName + " reconnected - resuming");
                                recovered = true;
                                ch.speed = 0;
                                lastSpeedBytes = ch.bytesTransferred.load();
                                lastSpeedTime = std::chrono::steady_clock::now();
                                break;
                            }
                        }

                        if (!recovered) {
                            ch.currentFile = "(disconnected)";
                            LOG_INFO("Transfer", ch.channelName + " no more blocks - stopping reconnect");
                            break;
                        }
                        continue; // grab next block from queue
                    }

                    if (!ok || batch->stopRequested.load() || m_shutdownTransfer) break;
                    bool accepted = false;
                    {
                        std::lock_guard<std::mutex> lk(workMutex);
                        if (coverage[block.fileIndex].failed()) continue;
                        accepted = coverage[block.fileIndex].record(block.offset, block.length);
                        if (coverage[block.fileIndex].complete()) completedFileIndices.insert(block.fileIndex);
                    }
                    if (!accepted) {
                        failFile(block.fileIndex);
                        ch.bytesTransferred = chBytesCompleted;
                        continue;
                    }
                    chBytesCompleted += block.length;
                    ch.bytesTransferred = chBytesCompleted;
                    ch.filesCompleted++;

                    if (block.isWholeFile)
                        LOG_INFO("Transfer", ch.channelName + " done: " + item.displayName);
                    else
                        LOG_INFO("Transfer", ch.channelName + " block done: " + item.displayName +
                                 " [" + formatSize(block.offset) + "+" + formatSize(block.length) + "]");
                }

                ch.speed = 0; // zero speed when channel exits (so combined speed is accurate)
                ch.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - chStart).count();
            };

            if (useMultiNic) {
                std::vector<std::thread> threads;
                for (int i = 0; i < (int)multiNicDevPtrs.size(); i++) {
                    threads.emplace_back(worker, std::ref(*multiNicDevPtrs[i]),
                                         std::ref(batch->channels[i]),
                                         multiNicDeviceIp, multiNicBindIps[i], true);
                }
                for (auto& t : threads) t.join();
            } else {
                // Launch all available channels as worker threads
                std::vector<std::thread> threads;
                batch->numChannels = std::min((int)runtimeChannels.size(), (int)TransferBatch::MAX_CHANNELS);
                numCh = batch->numChannels;
                for (int i = 0; i < batch->numChannels; i++) {
                    batch->channels[i].channelName = runtimeChannels[i].name;
                    threads.emplace_back(worker, std::ref(*runtimeChannels[i].dev), std::ref(batch->channels[i]),
                                         runtimeChannels[i].reconnectIp, std::string(""), runtimeChannels[i].preferUsb);
                }
                for (auto& t : threads) t.join();
            }

            // Close all split file handles
            for (auto& sh : splitHandles) {
                if (sh.handle != INVALID_HANDLE_VALUE) CloseHandle(sh.handle);
            }

            uint64_t copiedBytes = 0;
            int copiedFiles = 0;
            int incompleteFiles = 0;
            for (int fi = 0; fi < batch->totalFiles(); ++fi) {
                if (coverage[fi].skipped()) continue;
                if (coverage[fi].complete()) {
                    copiedBytes += coverage[fi].copiedBytes();
                    if (!batch->files[fi].isDirectory) ++copiedFiles;
                } else ++incompleteFiles;
            }
            batch->totalTransferred = copiedBytes;
            batch->totalProgress = batch->totalBytes.load() > 0 ?
                (float)((double)copiedBytes / (double)batch->totalBytes.load()) :
                (incompleteFiles == 0 ? 1.0f : 0.0f);

            // --- CRC verification for parallel transfers ---
            if (batch->state.load() != BatchState::Failed && !batch->stopRequested.load() &&
                m_prefs.enableCrcVerification) {
                batch->state = BatchState::Verifying;

                // Combine block CRCs for split files in file-offset order.
                if (!allBlockCrcs.empty()) {
                    std::sort(allBlockCrcs.begin(), allBlockCrcs.end(),
                              [](const BlockCrc& a, const BlockCrc& b) {
                                  if (a.fileIndex != b.fileIndex) return a.fileIndex < b.fileIndex;
                                  return a.offset < b.offset;
                              });

                    // Group by file and combine
                    std::unordered_map<int, std::vector<BlockCrc*>> fileBlocks;
                    for (auto& bc : allBlockCrcs) fileBlocks[bc.fileIndex].push_back(&bc);

                    for (auto& [fi, blocks] : fileBlocks) {
                        if (!coverage[fi].complete()) continue;
                        if (blocks.size() <= 1) continue;
                        // Chain combine: crc = combine(crc, next_block_crc, next_block_len)
                        uint32_t combined = blocks[0]->crc;
                        for (size_t i = 1; i < blocks.size(); i++)
                            combined = crc32Combine(combined, blocks[i]->crc, blocks[i]->length);
                        auto& item = batch->files[fi];
                        std::string crcLocalPath = batch->isPull ? item.destPath : item.sourcePath;
                        wholeFileCrcs[crcLocalPath] = combined;
                        LOG_INFO("Transfer", "Block CRC combined (" + std::to_string(blocks.size()) +
                                 " blocks): " + item.displayName + " crc=" + std::to_string(combined));
                    }
                }

                for (int fi = 0; fi < batch->totalFiles(); fi++) {
                    if (batch->stopRequested.load()) break;
                    auto& item = batch->files[fi];
                    if (item.isDirectory || item.isMcrawVirtual || !coverage[fi].complete()) continue;

                    // For pulls: sourcePath=Android, destPath=Windows
                    // For pushes: sourcePath=Windows, destPath=Android
                    std::string remotePath = batch->isPull ? item.sourcePath : item.destPath;
                    std::string localPath = batch->isPull ? item.destPath : item.sourcePath;

                    batch->crcFileName = item.displayName;
                    batch->crcProgress = 0.0f;
                    batch->crcPhase = 0;
                    batch->errorMessage = "Verifying: " + item.displayName;

                    std::string crcDetail;
                    double crcRemoteMs = 0, crcLocalMs = 0;

                    // Set inline CRC from per-file map (whole files from any channel)
                    // or from block combination (split files, set above)
                    auto wfIt = wholeFileCrcs.find(localPath);
                    if (wfIt != wholeFileCrcs.end())
                        batchDev.setInlineCrc(wfIt->second, localPath);

                    bool crcOk = batchDev.verifyFileCrc(remotePath, localPath, crcDetail,
                                                        &batch->crcProgress, &batch->crcPhase,
                                                        &crcRemoteMs, &crcLocalMs);

                    TransferBatch::CrcResult crcRes;
                    crcRes.fileName = item.displayName;
                    crcRes.passed = crcOk;
                    crcRes.detail = crcDetail;
                    crcRes.remoteMs = crcRemoteMs;
                    crcRes.localMs = crcLocalMs;
                    crcRes.totalMs = crcRemoteMs + crcLocalMs;
                    crcRes.fileSize = item.fileSize;
                    crcRes.sourcePath = item.sourcePath;  // original batch paths (not CRC-rearranged)
                    crcRes.destPath = item.destPath;
                    batch->crcResults.push_back(std::move(crcRes));

                    if (!crcOk) {
                        LOG_ERROR("Transfer", "CRC FAILED (parallel): " + item.displayName + " - " + crcDetail);
                        batch->errorMessage = "CRC FAILED: " + item.displayName;
                        // Check if this was a block-split file
                        bool wasSplit = false;
                        for (auto& bc : allBlockCrcs) if (bc.fileIndex == fi) { wasSplit = true; break; }
                        if (wasSplit)
                            batch->errorMessage += " (block-split — file likely corrupted)";
                    } else {
                        LOG_INFO("Transfer", "CRC OK (parallel): " + item.displayName);
                        batch->errorMessage.clear();
                    }
                }

                batch->crcFileName.clear();
                batch->crcProgress = 0.0f;
                batch->crcPhase = 0;
            }

            // Finalize
            if (batch->state.load() != BatchState::Failed && !batch->stopRequested.load()) {
                // Check if any CRC failed
                bool anyCrcFail = false;
                for (auto& r : batch->crcResults) if (!r.passed) anyCrcFail = true;

                auto totalTime = std::chrono::steady_clock::now() - parallelStart;
                double wallSec = std::chrono::duration<double>(totalTime).count();
                if (wallSec < 0.1) wallSec = 0.1;
                double avgSpeed = (double)batch->totalTransferred.load() / wallSec;
                batch->finalAvgSpeed = avgSpeed;
                batch->finalTimeSec = wallSec;
                batch->speedBytesPerSec = 0;
                batch->etaSeconds = -1;

                if (incompleteFiles > 0) {
                    batch->state = BatchState::Failed;
                    batch->errorMessage = std::to_string(incompleteFiles) + " item(s) could not be copied. " +
                        std::to_string(copiedFiles) + " file(s) copied successfully.";
                } else if (anyCrcFail) {
                    batch->state = BatchState::Failed;
                    batch->errorMessage = "CRC verification failed — file(s) may be corrupted";
                } else {
                    batch->state = BatchState::Completed;
                    logBatchPerformance(useMultiNic ? "multi-NIC parallel" : "multi-channel parallel",
                                        wallSec, avgSpeed);
                }

                m_statusMessage = (batch->state.load() == BatchState::Completed ? "Copied: " : "Transfer failed. Copied: ") +
                    std::to_string(copiedFiles) + " files, " +
                    formatSize(batch->totalTransferred.load()) + " @ " + formatSpeed(avgSpeed) +
                    (useMultiNic ? " (" + std::to_string(numCh) + " NICs)" :
                        " (" + std::to_string(numCh) + " channels)");
                m_statusTime = std::chrono::steady_clock::now();

                m_leftPanel.needsRefresh = true;
                m_rightPanel.needsRefresh = true;
            } else if (batch->stopRequested.load()) {
                batch->state = BatchState::Stopped;
            }

            // Clean up only incomplete files on stop/cancel/error
            // Files whose blocks all finished are kept; only partially-transferred files are deleted
            if (batch->state.load() == BatchState::Stopped || batch->state.load() == BatchState::Failed) {
                for (int fi = 0; fi < batch->totalFiles(); fi++) {
                    auto& item = batch->files[fi];
                    if (item.isDirectory) continue;
                    if (skippedFileIndices[fi]) continue;
                    if (existingDestination[fi]) continue;
                    if (completedFileIndices.count(fi)) continue; // fully transferred, keep it
                    if (batch->isPull) {
                        try {
                            if (std::filesystem::exists(toFsPath(item.destPath))) {
                                std::filesystem::remove(toFsPath(item.destPath));
                                LOG_INFO("Transfer", "Deleted incomplete file: " + item.destPath);
                            }
                        } catch (...) {}
                    } else {
                        batchDev.deleteFile(item.destPath);
                        LOG_INFO("Transfer", "Deleted incomplete remote file: " + item.destPath);
                    }
                }
                if (useMultiNic) {
                    for (auto* dev : multiNicDevPtrs) {
                        dev->flushStaleData();
                        dev->restoreTimeouts();
                    }
                } else {
                    for (auto& rc : runtimeChannels) {
                        if (!rc.dev) continue;
                        rc.dev->flushStaleData();
                        rc.dev->restoreTimeouts();
                    }
                }
            }
            nicCancellation.clear();
            // Clean up multi-NIC channels after transfer completes
            if (useMultiNic) {
                for (auto& ch : m_nicChannels) ch->disconnectTcp();
                m_nicChannels.clear();
                m_nicLocalIps.clear();
            }
            for (auto& rc : runtimeChannels) {
                if (rc.owned) rc.owned->disconnectTcp();
            }
            continue;
        }

        uint64_t bytesCompletedBefore = 0;
        auto batchStart = std::chrono::steady_clock::now();
        uint64_t lastSpeedBytes = 0;
        auto lastSpeedTime = batchStart;

        std::vector<RuntimeChannel> crossSrcChannels;
        std::vector<RuntimeChannel> crossDstChannels;

        auto buildCrossDeviceChannels = [&](DeviceClient& baseDev, int slot, int portBase,
                                            std::vector<RuntimeChannel>& out) {
            out.clear();
            if (!baseDev.isServerRunning()) return;

            if (companion::isSerial(baseDev.connectedSerial())) {
                auto pipes = m_prefs.pipePreferencesFor(baseDev.connectedSerial());
                RuntimeChannel primary;
                primary.dev = &baseDev;
                primary.name = companionPrimaryName(baseDev);
                primary.reconnectIp = baseDev.connectedHost();
                primary.preferUsb = !baseDev.isDirectConnection();
                out.push_back(std::move(primary));
                for (const auto& endpoint : companionChannelTargets(baseDev, pipes.usbPipeCount, pipes.wifiPipeCount,
                        TransferBatch::MAX_CHANNELS)) {
                    if (batch->stopRequested) break;
                    auto ch = std::make_unique<DeviceClient>();
                    ch->setSerial(baseDev.connectedSerial());
                    ch->setServerPort(endpoint.port);
                    ch->setLocalPort(endpoint.port);
                    if (!ch->connectTcp(endpoint.host, endpoint.port) || !ch->verifyConnection()) continue;
                    RuntimeChannel rc;
                    rc.dev = ch.get();
                    rc.owned = std::move(ch);
                    rc.cancellation = batch->cancellation.track(*rc.dev);
                    rc.name = companionChannelName(endpoint.kind);
                    rc.reconnectIp = endpoint.host;
                    rc.preferUsb = endpoint.isUsb();
                    out.push_back(std::move(rc));
                }
                return;
            }

            std::string serial = baseDev.connectedSerial();
            std::string wifiIp;
            std::string usbSerial;
            std::string hwSerial;
            bool primaryUsesWifi = baseDev.isDirectConnection() || isWifiSerial(serial);
            bool primaryIsUsb = !serial.empty() && !primaryUsesWifi;
            if (!serial.empty() && !isWifiSerial(serial)) usbSerial = serial;
            if (baseDev.isDirectConnection()) wifiIp = baseDev.deviceIp();
            if (isWifiSerial(serial)) {
                auto colon = serial.rfind(':');
                if (colon != std::string::npos) wifiIp = serial.substr(0, colon);
            }

            bool serialAdbOnline = false;
            if (!serial.empty()) {
                std::lock_guard<std::mutex> lk(m_deviceMutex);
                serialAdbOnline = std::any_of(m_devices.begin(), m_devices.end(), [&](const DeviceInfo& device) {
                    return device.serial == serial && device.state == "device";
                });
            }
            if (!serialAdbOnline) usbSerial.clear();

            if (serialAdbOnline) {
                std::string hwOut = baseDev.runAdbCommand("-s " + serial + " shell getprop ro.serialno");
                while (lastCharOr(hwOut) == '\n' || lastCharOr(hwOut) == '\r' || lastCharOr(hwOut) == ' ')
                    hwOut.pop_back();
                hwSerial = hwOut;
            }
            auto transferPipes = m_prefs.pipePreferencesFor(serial);

            if (wifiIp.empty() && !serial.empty()) {
                std::string wlanOut = baseDev.runAdbCommand("-s " + serial + " shell \"ip -4 addr show wlan0 2>/dev/null\"");
                auto inetPos = wlanOut.find("inet ");
                if (inetPos != std::string::npos) {
                    auto start = inetPos + 5;
                    auto slash = wlanOut.find('/', start);
                    if (slash != std::string::npos)
                        wifiIp = wlanOut.substr(start, slash - start);
                }
            }
            if (wifiIp.empty()) {
                for (auto& w : m_prefs.savedWifiDevices) {
                    std::string wifiSerial = w.wifiIp + ":" + std::to_string(w.port);
                    if (!w.wifiIp.empty() && (w.serial == serial || wifiSerial == serial)) {
                        wifiIp = w.wifiIp;
                        break;
                    }
                }
            }

            if (usbSerial.empty() && !hwSerial.empty()) {
                std::lock_guard<std::mutex> lk(m_deviceMutex);
                for (auto& d : m_devices) {
                    if (d.state != "device" || isWifiSerial(d.serial)) continue;
                    std::string hw = baseDev.runAdbCommand("-s " + d.serial + " shell getprop ro.serialno");
                    while (lastCharOr(hw) == '\n' || lastCharOr(hw) == '\r' || lastCharOr(hw) == ' ')
                        hw.pop_back();
                    if (hw == hwSerial) {
                        usbSerial = d.serial;
                        break;
                    }
                }
            }

            RuntimeChannel primary;
            primary.dev = &baseDev;
            primary.name = primaryIsUsb ? "USB (ADB)" : (baseDev.isDirectConnection() ? "WiFi (Direct)" : "WiFi (ADB)");
            primary.reconnectIp = wifiIp;
            primary.preferUsb = primaryIsUsb;
            out.push_back(std::move(primary));

            int usbChannels = primaryIsUsb ? 1 : 0;
            int wifiChannels = primaryUsesWifi ? 1 : 0;
            int nextPort = portBase;
            int remoteHelperPort = baseDev.serverPort();

            auto addUsbChannel = [&]() -> bool {
                if (batch->stopRequested) return false;
                if (usbSerial.empty()) return false;
                for (int attempt = 0; attempt < 32; attempt++) {
                    auto ch = std::make_unique<DeviceClient>();
                    ch->setAdbPath(baseDev.getAdbPath());
                    ch->setServerPort(baseDev.serverPort());
                    ch->setLocalPort(nextPort++);
                    int localPort = ch->localPort();
                    std::string fwd = baseDev.runAdbCommand("-s " + usbSerial + " forward tcp:" +
                        std::to_string(localPort) + " tcp:" + std::to_string(remoteHelperPort));
                    if (fwd.find("offline") != std::string::npos)
                        return false;
                    if (fwd.find("error") != std::string::npos) {
                        LOG_WARN("Transfer", "USB forward port " + std::to_string(localPort) +
                            " failed, trying next port");
                        continue;
                    }
                    if (!ch->connectTcp("127.0.0.1", localPort) || !ch->verifyConnection()) {
                        LOG_WARN("Transfer", "USB forward port " + std::to_string(localPort) +
                            " connected but did not verify");
                        baseDev.runAdbCommand("-s " + usbSerial + " forward --remove tcp:" +
                            std::to_string(localPort));
                        return false;
                    }
                    RuntimeChannel rc;
                    rc.dev = ch.get();
                    rc.owned = std::move(ch);
                    rc.cancellation = batch->cancellation.track(*rc.dev);
                    rc.name = "USB (ADB)";
                    rc.reconnectIp = wifiIp;
                    rc.preferUsb = true;
                    out.push_back(std::move(rc));
                    usbChannels++;
                    return true;
                }
                return false;
            };

            auto addWifiChannel = [&]() -> bool {
                if (batch->stopRequested) return false;
                if (wifiIp.empty()) return false;
                auto ch = std::make_unique<DeviceClient>();
                ch->setAdbPath(baseDev.getAdbPath());
                ch->setServerPort(baseDev.serverPort());
                ch->setLocalPort(nextPort++);
                if (!ch->connectTcp(wifiIp, remoteHelperPort) || !ch->verifyConnection())
                    return false;
                RuntimeChannel rc;
                rc.dev = ch.get();
                rc.owned = std::move(ch);
                rc.cancellation = batch->cancellation.track(*rc.dev);
                rc.name = "WiFi (Direct)";
                rc.reconnectIp = wifiIp;
                rc.preferUsb = false;
                out.push_back(std::move(rc));
                wifiChannels++;
                return true;
            };

            while (usbChannels < transferPipes.usbPipeCount && out.size() < TransferBatch::MAX_CHANNELS)
                if (!addUsbChannel()) break;
            while (wifiChannels < transferPipes.wifiPipeCount && out.size() < TransferBatch::MAX_CHANNELS)
                if (!addWifiChannel()) break;
        };

        if (batch->isCrossDevice && !batch->useDokanRelay) {
            DeviceClient& srcDev = deviceSession(batch->srcDeviceSlot).client;
            DeviceClient& dstDev = deviceSession(batch->dstDeviceSlot).client;
            buildCrossDeviceChannels(srcDev, batch->srcDeviceSlot, 5920, crossSrcChannels);
            buildCrossDeviceChannels(dstDev, batch->dstDeviceSlot, 5984, crossDstChannels);

            int pairCount = std::min((int)crossSrcChannels.size(), (int)crossDstChannels.size());
            pairCount = std::min(pairCount, (int)TransferBatch::MAX_CHANNELS);
            if (pairCount > 0) {
                batch->numChannels = pairCount;
                batch->useParallelChannels = pairCount > 1;
                for (int i = 0; i < pairCount; i++) {
                    batch->channels[i].channelName = crossSrcChannels[i].name + " > " + crossDstChannels[i].name;
                    batch->channels[i].totalBytes = batch->totalBytes.load() / pairCount;
                    batch->channels[i].filesAssigned = 0;
                }
                LOG_INFO("Transfer", "Cross-device fast relay ready with " + std::to_string(pairCount) + " channel pair(s)");
            }
        }

        if (batch->isCrossDevice && !batch->useDokanRelay &&
            !crossSrcChannels.empty() && !crossDstChannels.empty()) {
            DeviceClient& srcDev = deviceSession(batch->srcDeviceSlot).client;
            DeviceClient& dstDev = deviceSession(batch->dstDeviceSlot).client;
            int pairCount = std::min((int)crossSrcChannels.size(), (int)crossDstChannels.size());
            pairCount = std::min(pairCount, (int)TransferBatch::MAX_CHANNELS);

            if (!srcDev.isServerRunning() || !dstDev.isServerRunning()) {
                batch->state = BatchState::Failed;
                batch->errorMessage = "Both devices must be connected for cross-device transfer";
                continue;
            }

            struct RelayWork {
                int fileIndex;
                uint64_t offset;
                uint64_t length;
            };
            struct RelayRangeCrc {
                uint64_t offset;
                uint64_t length;
                uint32_t srcCrc;
                uint32_t dstCrc;
            };

            static const uint64_t MB = 1024ULL * 1024;
            static const uint64_t GB = 1024ULL * MB;
            auto relayReadRangeSizeFor = [](uint64_t fileSize, int channels) -> uint64_t {
                if (channels <= 1) return fileSize;
                const uint64_t MB = 1024ULL * 1024;
                // Paired streaming relays each range immediately. Keep ranges large
                // enough to avoid overhead, but not so large that one slow pair owns
                // the tail of a multi-file batch.
                if (fileSize >= 1024 * MB) return 128 * MB;
                if (fileSize >= 500 * MB) return 96 * MB;
                return 64 * MB;
            };
            std::vector<bool> skipped(batch->totalFiles(), false);
            uint64_t newTotal = 0;

            for (int fi = 0; fi < batch->totalFiles() && !m_shutdownTransfer; fi++) {
                if (batch->stopRequested.load()) break;
                auto& item = batch->files[fi];

                if (item.isDirectory) {
                    if (!dstDev.createDirectory(item.destPath)) {
                        batch->state = BatchState::Failed;
                        batch->errorMessage = item.displayName + ": create directory failed";
                        break;
                    }
                    continue;
                }

                if (item.fileSize == 0)
                    item.fileSize = srcDev.getFileSize(item.sourcePath);

                bool destExists = dstDev.getFileSize(item.destPath) > 0;
                if (destExists) {
                    ConflictAction action = batch->conflictAllDecision;
                    if (action == ConflictAction::None) {
                        batch->beginConflict(item);
                        {
                            std::unique_lock<std::mutex> lk(m_batchMutex);
                            m_batchCV.wait(lk, [&]() {
                                return m_shutdownTransfer || batch->stopRequested.load() ||
                                    batch->conflictResponse.load() != ConflictAction::None;
                            });
                        }
                        action = batch->finishConflict();
                        batch->state = BatchState::Running;
                        if (action == ConflictAction::OverwriteAll) {
                            batch->conflictAllDecision = ConflictAction::OverwriteAll;
                            action = ConflictAction::Overwrite;
                        } else if (action == ConflictAction::SkipAll) {
                            batch->conflictAllDecision = ConflictAction::SkipAll;
                            action = ConflictAction::Skip;
                        }
                    } else {
                        if (action == ConflictAction::OverwriteAll) action = ConflictAction::Overwrite;
                        else if (action == ConflictAction::SkipAll) action = ConflictAction::Skip;
                    }

                    if (batch->stopRequested.load() || m_shutdownTransfer) break;
                    if (action == ConflictAction::Skip) {
                        skipped[fi] = true;
                        batch->skippedFiles++;
                        continue;
                    }
                }

                if (!dstDev.createFile(item.destPath, item.fileSize)) {
                    batch->state = BatchState::Failed;
                    batch->errorMessage = item.displayName + ": create destination file failed";
                    break;
                }
                newTotal += item.fileSize;
            }

            if (batch->state.load() == BatchState::Failed) {
                for (auto& rc : crossSrcChannels) if (rc.owned) rc.owned->disconnectTcp();
                for (auto& rc : crossDstChannels) if (rc.owned) rc.owned->disconnectTcp();
                continue;
            }
            if (batch->stopRequested.load() || m_shutdownTransfer) {
                batch->state = BatchState::Stopped;
                for (auto& rc : crossSrcChannels) if (rc.owned) rc.owned->disconnectTcp();
                for (auto& rc : crossDstChannels) if (rc.owned) rc.owned->disconnectTcp();
                continue;
            }

            batch->totalBytes = newTotal;
            std::vector<std::deque<RelayWork>> byFile(batch->totalFiles());
            std::vector<int> totalBlocksPerFile(batch->totalFiles(), 0);
            std::vector<int> completedBlocksPerFile(batch->totalFiles(), 0);
            std::vector<std::vector<RelayRangeCrc>> rangeCrcs(batch->totalFiles());
            std::set<int> completedFileIndices;
            int totalReadRanges = 0;

            for (int fi = 0; fi < batch->totalFiles(); fi++) {
                auto& item = batch->files[fi];
                if (item.isDirectory || skipped[fi]) continue;
                uint64_t readRangeSize = relayReadRangeSizeFor(item.fileSize, pairCount);
                uint64_t remaining = item.fileSize;
                uint64_t offset = 0;
                while (remaining > 0) {
                    uint64_t len = std::min(remaining, readRangeSize);
                    byFile[fi].push_back({fi, offset, len});
                    totalReadRanges++;
                    totalBlocksPerFile[fi]++;
                    offset += len;
                    remaining -= len;
                }
            }

            std::deque<RelayWork> workQueue;
            bool added = true;
            while (added) {
                added = false;
                for (auto& blocks : byFile) {
                    if (blocks.empty()) continue;
                    workQueue.push_back(blocks.front());
                    blocks.pop_front();
                    added = true;
                }
            }

            int totalBlocks = totalReadRanges;
            int progressChannels = std::min(pairCount, (int)TransferBatch::MAX_CHANNELS);
            for (int c = 0; c < progressChannels; c++) {
                batch->channels[c].bytesTransferred = 0;
                batch->channels[c].speed = 0;
                batch->channels[c].curBlockSize = 0;
                batch->channels[c].curBlockTransferred = 0;
                batch->channels[c].relayReadSec = 0;
                batch->channels[c].relaySendSec = 0;
                batch->channels[c].currentFile.clear();
                batch->channels[c].filesAssigned = totalBlocks;
                batch->channels[c].channelName = crossSrcChannels[c].name + " > " + crossDstChannels[c].name;
                batch->channels[c].totalBytes = progressChannels > 0 ? batch->totalBytes.load() / progressChannels : 0;
            }
            batch->numChannels = progressChannels;

            std::mutex workMutex;
            std::mutex crcMutex;
            std::atomic<bool> failed{false};
            std::atomic<int> completedBlocks{0};
            std::string relayError;
            std::mutex relayErrorMutex;
            std::atomic<uint64_t> bytesWrittenToDest{0};

            auto updateCrossProgress = [&]() {
                double combinedSpeed = 0;
                for (int c = 0; c < progressChannels; c++) {
                    combinedSpeed += batch->channels[c].speed.load();
                }
                uint64_t totalTransferred = bytesWrittenToDest.load();
                uint64_t tb = batch->totalBytes.load();
                if (tb > 0 && totalTransferred > tb)
                    totalTransferred = tb;
                uint64_t currentTotal = batch->totalTransferred.load();
                if (totalTransferred < currentTotal) totalTransferred = currentTotal;
                batch->totalTransferred = totalTransferred;
                if (tb > 0) batch->totalProgress = (float)((double)totalTransferred / (double)tb);
                batch->speedBytesPerSec = combinedSpeed;
                batch->etaSeconds = (combinedSpeed > 0 && tb > totalTransferred)
                    ? (double)(tb - totalTransferred) / combinedSpeed : -1.0;
            };

            auto relayWorker = [&](int chIdx) {
                auto& ch = batch->channels[chIdx];
                DeviceClient& srcCh = *crossSrcChannels[chIdx].dev;
                DeviceClient& dstCh = *crossDstChannels[chIdx].dev;
                uint64_t chCompleted = 0;
                uint64_t lastChBytes = 0;
                auto chStart = std::chrono::steady_clock::now();
                auto lastChTime = chStart;
                ch.channelName = crossSrcChannels[chIdx].name + " > " + crossDstChannels[chIdx].name;

                while (!m_shutdownTransfer && !batch->stopRequested.load() && !failed.load()) {
                    RelayWork work{};
                    {
                        std::lock_guard<std::mutex> lk(workMutex);
                        if (workQueue.empty()) break;
                        work = workQueue.front();
                        workQueue.pop_front();
                    }

                    auto& item = batch->files[work.fileIndex];
                    batch->currentFileIndex = work.fileIndex;
                    batch->curFileSize = item.fileSize;
                    ch.curBlockSize = work.length;
                    ch.curBlockTransferred = 0;
                    ch.currentFile = item.displayName + " [" + formatSize(work.offset) + "]";

                    while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                        batch->state = BatchState::Paused;
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (m_shutdownTransfer || batch->stopRequested.load() || failed.load())
                        break;
                    batch->state = BatchState::Running;

                    auto rangeProgress = [&](uint64_t transferred, uint64_t total) -> bool {
                        if (m_shutdownTransfer || batch->stopRequested.load() || failed.load())
                            return false;
                        while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                            batch->state = BatchState::Paused;
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        if (m_shutdownTransfer || batch->stopRequested.load()) return false;
                        batch->state = BatchState::Running;

                        ch.bytesTransferred = chCompleted + transferred;
                        ch.curBlockTransferred = transferred;
                        batch->currentFileIndex = work.fileIndex;
                        batch->curFileSize = item.fileSize;
                        batch->curFileTransferred = transferred;
                        if (total > 0)
                            batch->curFileProgress = (float)((double)transferred / (double)total);
                        auto now = std::chrono::steady_clock::now();
                        double dt = std::chrono::duration<double>(now - lastChTime).count();
                        if (dt >= 0.5) {
                            uint64_t chBytes = ch.bytesTransferred.load();
                            double instant = (double)(chBytes - lastChBytes) / dt;
                            double prev = ch.speed.load();
                            ch.speed = prev > 0 ? prev * 0.8 + instant * 0.2 : instant;
                            lastChBytes = chBytes;
                            lastChTime = now;
                        }
                        bytesWrittenToDest = 0;
                        for (int c = 0; c < progressChannels; c++)
                            bytesWrittenToDest += batch->channels[c].bytesTransferred.load();
                        updateCrossProgress();
                        m_lastTransferActivity = std::chrono::steady_clock::now();
                        return true;
                    };

                    DeviceClient::RelayRangeStats relayStats;
                    bool relayed = DeviceClient::relayRange(srcCh, dstCh,
                        item.sourcePath, item.destPath,
                        work.offset, work.offset, work.length,
                        rangeProgress, &relayStats);

                    ch.relayReadSec = ch.relayReadSec.load() + relayStats.srcRecvSec;
                    ch.relaySendSec = ch.relaySendSec.load() + relayStats.dstSendSec;

                    if (!relayed && !batch->stopRequested.load()) {
                        failed = true;
                        std::lock_guard<std::mutex> lk(relayErrorMutex);
                        relayError = item.displayName + ": " + ch.channelName;
                        if (!srcCh.lastError().empty())
                            relayError += ", " + srcCh.lastError();
                        if (!dstCh.lastError().empty())
                            relayError += ", " + dstCh.lastError();
                        break;
                    }

                    chCompleted += work.length;
                    ch.bytesTransferred = chCompleted;
                    ch.curBlockTransferred = work.length;
                    ch.filesCompleted++;
                    bytesWrittenToDest = 0;
                    for (int c = 0; c < progressChannels; c++)
                        bytesWrittenToDest += batch->channels[c].bytesTransferred.load();
                    updateCrossProgress();
                    completedBlocks++;
                    {
                        std::lock_guard<std::mutex> lk(workMutex);
                        if (relayStats.hasSrcCrc && relayStats.hasDstCrc) {
                            std::lock_guard<std::mutex> crcLk(crcMutex);
                            rangeCrcs[work.fileIndex].push_back({
                                work.offset, work.length, relayStats.srcCrc, relayStats.dstCrc
                            });
                        }
                        completedBlocksPerFile[work.fileIndex]++;
                        if (completedBlocksPerFile[work.fileIndex] >= totalBlocksPerFile[work.fileIndex])
                            completedFileIndices.insert(work.fileIndex);
                    }
                }
                ch.speed = 0;
                ch.elapsedSec = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - chStart).count();
            };

            LOG_INFO("Transfer", "Cross-device streaming relay: " + std::to_string(batch->totalFiles()) +
                " files using " + std::to_string(pairCount) + " channel pair(s), " +
                std::to_string(totalReadRanges) + " interleaved range(s)");

            std::vector<std::thread> relayThreads;
            for (int c = 0; c < pairCount; c++)
                relayThreads.emplace_back(relayWorker, c);
            for (auto& t : relayThreads) t.join();

            if (failed.load() || (!batch->stopRequested.load() && completedBlocks.load() < totalBlocks)) {
                batch->state = BatchState::Failed;
                batch->errorMessage = relayError.empty() ? "Cross-device transfer failed" : relayError;
                for (int fi = 0; fi < batch->totalFiles(); fi++) {
                    auto& item = batch->files[fi];
                    if (item.isDirectory || skipped[fi]) continue;
                    if (completedFileIndices.count(fi)) continue;
                    dstDev.deleteFile(item.destPath);
                }
            } else if (batch->stopRequested.load()) {
                batch->state = BatchState::Stopped;
                for (int fi = 0; fi < batch->totalFiles(); fi++) {
                    auto& item = batch->files[fi];
                    if (item.isDirectory || skipped[fi]) continue;
                    if (completedFileIndices.count(fi)) continue;
                    dstDev.deleteFile(item.destPath);
                }
            } else {
                bool anyCrcFail = false;
                if (m_prefs.enableCrcVerification) {
                    batch->state = BatchState::Verifying;
                    for (int fi = 0; fi < batch->totalFiles() && !batch->stopRequested.load(); fi++) {
                        auto& item = batch->files[fi];
                        if (item.isDirectory || skipped[fi]) continue;

                        batch->crcFileName = item.displayName;
                        batch->crcProgress = 1.0f;
                        batch->crcPhase = 2;
                        batch->errorMessage = "Verifying: " + item.displayName;

                        auto blocks = rangeCrcs[fi];
                        std::sort(blocks.begin(), blocks.end(),
                                  [](const RelayRangeCrc& a, const RelayRangeCrc& b) {
                                      return a.offset < b.offset;
                                  });

                        bool crcOk = !blocks.empty();
                        bool inlineCrcComplete = !blocks.empty() &&
                            blocks.size() == (size_t)totalBlocksPerFile[fi];
                        uint64_t expectedOffset = 0;
                        uint32_t srcCrc = 0;
                        uint32_t dstCrc = 0;
                        std::string crcFailReason;
                        if (blocks.empty()) {
                            crcFailReason = "inline range CRC unavailable";
                        } else if (!inlineCrcComplete) {
                            crcOk = false;
                            crcFailReason = "inline range CRC incomplete: " +
                                std::to_string(blocks.size()) + "/" +
                                std::to_string(totalBlocksPerFile[fi]) + " ranges";
                        } else {
                            srcCrc = blocks[0].srcCrc;
                            dstCrc = blocks[0].dstCrc;
                            expectedOffset = blocks[0].length;
                            crcOk = (blocks[0].offset == 0);
                            if (!crcOk)
                                crcFailReason = "first range does not start at offset 0";
                            else if (blocks[0].srcCrc != blocks[0].dstCrc) {
                                crcOk = false;
                                crcFailReason = "range CRC mismatch at offset 0";
                            }
                        }
                        for (size_t i = 1; crcOk && i < blocks.size(); i++) {
                            if (blocks[i].offset != expectedOffset) {
                                crcOk = false;
                                crcFailReason = "missing range at offset " + std::to_string(expectedOffset);
                                break;
                            }
                            if (blocks[i].srcCrc != blocks[i].dstCrc) {
                                crcOk = false;
                                crcFailReason = "range CRC mismatch at offset " + std::to_string(blocks[i].offset);
                                break;
                            }
                            srcCrc = crc32Combine(srcCrc, blocks[i].srcCrc, blocks[i].length);
                            dstCrc = crc32Combine(dstCrc, blocks[i].dstCrc, blocks[i].length);
                            expectedOffset += blocks[i].length;
                        }
                        if (crcOk && expectedOffset != item.fileSize) {
                            crcOk = false;
                            crcFailReason = "range coverage ended at " + std::to_string(expectedOffset) +
                                " of " + std::to_string(item.fileSize);
                        }
                        if (crcOk)
                            crcOk = (srcCrc == dstCrc);
                        if (!crcOk && crcFailReason.empty())
                            crcFailReason = "combined CRC mismatch";

                        bool usedFallbackCrc = false;
                        double srcRemoteMs = 0.0;
                        double dstRemoteMs = 0.0;
                        std::string fallbackDetail;
                        if (!crcOk) {
                            uint32_t srcRemoteCrc = 0;
                            uint32_t dstRemoteCrc = 0;
                            std::string srcDetail;
                            std::string dstDetail;
                            bool srcRemoteOk = srcDev.getRemoteCrc32(item.sourcePath, srcRemoteCrc,
                                srcDetail, item.fileSize, &srcRemoteMs);
                            bool dstRemoteOk = dstDev.getRemoteCrc32(item.destPath, dstRemoteCrc,
                                dstDetail, item.fileSize, &dstRemoteMs);
                            usedFallbackCrc = true;

                            char fbBuf[192];
                            snprintf(fbBuf, sizeof(fbBuf),
                                     "Source remote: %08X  Destination remote: %08X",
                                     srcRemoteCrc, dstRemoteCrc);
                            fallbackDetail = fbBuf;

                            if (srcRemoteOk && dstRemoteOk) {
                                crcOk = (srcRemoteCrc == dstRemoteCrc);
                                if (crcOk) {
                                    srcCrc = srcRemoteCrc;
                                    dstCrc = dstRemoteCrc;
                                } else {
                                    crcFailReason = "remote CRC mismatch after inline failure: " + crcFailReason;
                                }
                            } else {
                                std::string remoteReason = "remote CRC fallback failed";
                                if (!srcRemoteOk) remoteReason += " (source: " + srcDetail + ")";
                                if (!dstRemoteOk) remoteReason += " (destination: " + dstDetail + ")";
                                crcFailReason = remoteReason + " after inline failure: " + crcFailReason;
                            }
                        }

                        char detailBuf[160];
                        snprintf(detailBuf, sizeof(detailBuf),
                                 "Source inline: %08X  Destination inline: %08X",
                                 srcCrc, dstCrc);

                        TransferBatch::CrcResult crcRes;
                        crcRes.fileName = item.displayName;
                        crcRes.passed = crcOk;
                        if (usedFallbackCrc) {
                            crcRes.detail = fallbackDetail + " (inline: " + crcFailReason + ")";
                        } else {
                            crcRes.detail = crcOk ? detailBuf : std::string(detailBuf) + " (" + crcFailReason + ")";
                        }
                        crcRes.remoteMs = srcRemoteMs;
                        crcRes.localMs = dstRemoteMs;
                        crcRes.totalMs = srcRemoteMs + dstRemoteMs;
                        crcRes.fileSize = item.fileSize;
                        crcRes.sourcePath = item.sourcePath;
                        crcRes.destPath = item.destPath;
                        batch->crcResults.push_back(std::move(crcRes));

                        if (!crcOk) {
                            anyCrcFail = true;
                            LOG_ERROR("Transfer", "CRC FAILED (cross-device inline): " + item.displayName +
                                " - " + crcFailReason);
                        } else if (usedFallbackCrc) {
                            LOG_WARN("Transfer", "CRC OK (cross-device remote fallback): " + item.displayName +
                                " - inline check could not complete: " + crcFailReason);
                        } else {
                            LOG_INFO("Transfer", "CRC OK (cross-device inline): " + item.displayName);
                        }
                    }
                    batch->crcFileName.clear();
                    batch->crcProgress = 0.0f;
                    batch->crcPhase = 0;
                }

                batch->totalProgress = 1.0f;
                batch->totalTransferred = batch->totalBytes.load();
                auto totalTime = std::chrono::steady_clock::now() - batch->startTime;
                double activeSec = std::chrono::duration<double>(totalTime).count() -
                    batch->pausedSeconds.load();
                if (activeSec < 0.1) activeSec = 0.1;
                double avgSpeed = (double)batch->totalTransferred.load() / activeSec;
                batch->finalAvgSpeed = avgSpeed;
                batch->finalTimeSec = activeSec;
                batch->speedBytesPerSec = 0;
                batch->etaSeconds = -1;
                if (anyCrcFail) {
                    batch->state = BatchState::Failed;
                    batch->errorMessage = "CRC verification failed - file(s) may be corrupted";
                } else {
                    batch->state = BatchState::Completed;
                    logBatchPerformance("cross-device streaming relay", activeSec, avgSpeed);
                    batch->errorMessage.clear();
                }
                m_statusMessage = "Cross-device done: " + std::to_string(batch->totalFiles()) + " files, " +
                    formatSize(batch->totalTransferred.load()) + " @ " + formatSpeed(avgSpeed);
                m_statusTime = std::chrono::steady_clock::now();
                m_leftPanel.needsRefresh = true;
                m_rightPanel.needsRefresh = true;
            }

            for (auto& rc : crossSrcChannels) if (rc.owned) rc.owned->disconnectTcp();
            for (auto& rc : crossDstChannels) if (rc.owned) rc.owned->disconnectTcp();
            continue;
        }

        // --- Fast parallel MCRAW batch extraction ---
        // If this is a local copy and contains MCRAW virtual DNG frames from the same container,
        // extract the DNG frames in parallel, then handle non-DNG items (metadata, audio) normally.
        if (batch->isLocalCopy && !batch->files.empty()) {
            std::string mcrawContainer;
            int dngCount = 0;
            int nonDngCount = 0;
            std::string destDir;
            for (auto& item : batch->files) {
                if (item.isDirectory) continue;
                if (item.isMcrawVirtual && item.virtualName.find(".dng") != std::string::npos) {
                    if (mcrawContainer.empty()) {
                        mcrawContainer = item.mcrawPath;
                        auto dp = std::filesystem::path(item.destPath).parent_path();
                        destDir = dp.string();
                    } else if (item.mcrawPath != mcrawContainer) {
                        mcrawContainer.clear(); // mixed containers, can't batch
                        break;
                    }
                    dngCount++;
                } else {
                    nonDngCount++;
                }
            }

            if (dngCount >= 2 && !mcrawContainer.empty() && !destDir.empty()) {
                // Use parallel batch extraction for DNG frames
                LOG_INFO("Transfer", "MCRAW batch path: " + std::to_string(dngCount) + " DNG + " +
                         std::to_string(nonDngCount) + " other items from " + mcrawContainer);
                try { std::filesystem::create_directories(destDir); } catch (...) {}

                int extracted = extractLocalMcrawBatch(
                    mcrawContainer, destDir, 0, // auto thread count
                    &batch->stopRequested,
                    [&](int completed, int total, uint64_t bytesWritten) {
                        batch->currentFileIndex = completed - 1;
                        batch->totalTransferred = bytesWritten;
                        if (total > 0)
                            batch->totalProgress = (float)completed / (float)total;
                        batch->curFileProgress = 1.0f;

                        // Speed tracking
                        auto now = std::chrono::steady_clock::now();
                        double dt = std::chrono::duration<double>(now - lastSpeedTime).count();
                        if (dt >= 0.5) {
                            double delta = (double)(bytesWritten - lastSpeedBytes);
                            double instantSpeed = delta / dt;
                            double prevSpeed = batch->speedBytesPerSec.load();
                            double smoothed = (prevSpeed > 0) ? prevSpeed * 0.8 + instantSpeed * 0.2 : instantSpeed;
                            batch->speedBytesPerSec = smoothed;
                            uint64_t tb = batch->totalBytes.load();
                            if (smoothed > 0 && tb > bytesWritten)
                                batch->etaSeconds = (double)(tb - bytesWritten) / smoothed;
                            else batch->etaSeconds = -1.0;
                            lastSpeedBytes = bytesWritten;
                            lastSpeedTime = now;
                        }

                        m_lastTransferActivity = std::chrono::steady_clock::now();
                    });

                bytesCompletedBefore = batch->totalTransferred.load();

                if (batch->stopRequested.load()) {
                    batch->state = BatchState::Stopped;
                    continue;
                }

                // If there are non-DNG items (metadata.json, audio.wav), extract them now
                if (nonDngCount > 0) {
                    for (auto& item : batch->files) {
                        if (batch->stopRequested.load()) break;
                        if (item.isDirectory || !item.isMcrawVirtual) continue;
                        if (item.virtualName.find(".dng") != std::string::npos) continue; // already extracted
                        extractLocalMcrawItem(mcrawContainer, item.virtualName, item.destPath);
                    }
                }

                // Update final state
                batch->totalTransferred = batch->totalBytes.load();
                batch->totalProgress = 1.0f;
                if (extracted >= dngCount) {
                    batch->state = BatchState::Completed;
                } else if (extracted > 0) {
                    batch->state = BatchState::Completed;
                } else {
                    batch->state = BatchState::Failed;
                    batch->errorMessage = "MCRAW batch extraction failed";
                }
                if (batch->state.load() == BatchState::Completed) {
                    auto totalTime = std::chrono::steady_clock::now() - batch->startTime;
                    double activeSec = std::chrono::duration<double>(totalTime).count() -
                        batch->pausedSeconds.load();
                    if (activeSec < 0.1) activeSec = 0.1;
                    double avgSpeed = (double)batch->totalTransferred.load() / activeSec;
                    batch->finalAvgSpeed = avgSpeed;
                    batch->finalTimeSec = activeSec;
                    logBatchPerformance("MCRAW batch extraction", activeSec, avgSpeed);
                }
                continue; // skip per-item loop
            }
        }

        bool currentDestinationStarted = false;
        for (int fi = 0; fi < batch->totalFiles() && !m_shutdownTransfer; fi++) {
            currentDestinationStarted = false;
            if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; break; }

            // Pause — track paused duration for accurate avg speed
            if (batch->pauseRequested.load()) {
                auto pauseStart = std::chrono::steady_clock::now();
                while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                    batch->state = BatchState::Paused;
                    std::unique_lock<std::mutex> lk(m_batchMutex);
                    m_batchCV.wait_for(lk, std::chrono::milliseconds(200));
                }
                batch->pausedSeconds = batch->pausedSeconds.load() +
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - pauseStart).count();
            }
            if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; break; }
            batch->state = BatchState::Running;

            batch->currentFileIndex = fi;
            auto& item = batch->files[fi];

            // Get file size if unknown
            if (item.fileSize == 0 && !item.isDirectory) {
                if (batch->isPull) item.fileSize = batchDev.getFileSize(item.sourcePath);
                else { try { item.fileSize = std::filesystem::file_size(toFsPath(item.sourcePath)); } catch (...) {} }
                uint64_t newTotal = 0;
                for (auto& f : batch->files) newTotal += f.fileSize;
                batch->totalBytes = newTotal;
            }

            batch->curFileSize = item.fileSize;
            batch->curFileTransferred = 0;
            batch->curFileProgress = 0.0f;

            // --- Write permission pre-check for local destinations ---
            if (!item.isDirectory && (batch->isPull || batch->isLocalCopy)) {
                // Test if we can create a file in the destination directory
                std::filesystem::path destDir = toFsPath(item.destPath).parent_path();
                try {
                    std::filesystem::create_directories(destDir);
                } catch (...) {}
                std::string testPath = pathToUtf8(destDir) + "\\.afm_write_test";
                HANDLE hTest = CreateFileW(toWide(testPath).c_str(), GENERIC_WRITE, 0, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                if (hTest == INVALID_HANDLE_VALUE) {
                    DWORD err = GetLastError();
                    batch->state = BatchState::Failed;
                    if (err == ERROR_ACCESS_DENIED)
                        batch->errorMessage = "Access denied: " + pathToUtf8(destDir) + "\n\nThis location requires administrator rights.";
                    else
                        batch->errorMessage = "Cannot write to: " + pathToUtf8(destDir) + " (error " + std::to_string(err) + ")";
                    break;
                }
                CloseHandle(hTest); // FILE_FLAG_DELETE_ON_CLOSE auto-deletes

                // Also check if destination file specifically is read-only
                if (std::filesystem::exists(toFsPath(item.destPath))) {
                    auto perms = std::filesystem::status(toFsPath(item.destPath)).permissions();
                    if ((perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none) {
                        batch->state = BatchState::Failed;
                        batch->errorMessage = "Destination file is read-only: " + item.displayName;
                        break;
                    }
                }
            }

            // --- File conflict check ---
            if (!item.isDirectory) {
                bool destExists = false;
                if (batch->isLocalCopy || batch->isPull) {
                    // Destination is Windows — check local filesystem
                    destExists = std::filesystem::exists(toFsPath(item.destPath));
                } else {
                    // Destination is Android — use the DESTINATION device client
                    DeviceClient& destDev = batch->isCrossDevice
                        ? deviceSession(batch->dstDeviceSlot).client : batchDev;
                    uint64_t remoteSize = destDev.getFileSize(item.destPath);
                    destExists = (remoteSize > 0);
                }

                if (destExists) {
                    ConflictAction action = batch->conflictAllDecision;

                    if (action == ConflictAction::None) {
                        // No sticky decision — ask the user
                        batch->beginConflict(item);

                        // Wait for UI thread to set conflictResponse
                        {
                            std::unique_lock<std::mutex> lk(m_batchMutex);
                            m_batchCV.wait(lk, [&]() {
                                return m_shutdownTransfer || batch->stopRequested.load() ||
                                       batch->conflictResponse.load() != ConflictAction::None;
                            });
                        }
                        action = batch->finishConflict();
                        batch->state = BatchState::Running;

                        // Store sticky "All" decisions
                        if (action == ConflictAction::OverwriteAll) {
                            batch->conflictAllDecision = ConflictAction::OverwriteAll;
                            action = ConflictAction::Overwrite;
                        } else if (action == ConflictAction::SkipAll) {
                            batch->conflictAllDecision = ConflictAction::SkipAll;
                            action = ConflictAction::Skip;
                        }
                    } else {
                        // Sticky decision — map "All" variants to their single action
                        if (action == ConflictAction::OverwriteAll) action = ConflictAction::Overwrite;
                        else if (action == ConflictAction::SkipAll) action = ConflictAction::Skip;
                    }

                    if (batch->stopRequested.load() || m_shutdownTransfer) {
                        batch->state = BatchState::Stopped;
                        break;
                    }

                    if (action == ConflictAction::Skip) {
                        batch->skippedFiles++;
                        bytesCompletedBefore += item.fileSize;
                        batch->totalTransferred = bytesCompletedBefore;
                        if (batch->totalBytes.load() > 0)
                            batch->totalProgress = (float)((double)bytesCompletedBefore / (double)batch->totalBytes.load());
                        continue; // skip this file
                    }
                    // else: Overwrite — fall through to normal transfer
                }
            }

            // Progress callback - updates batch atomics, called from DeviceClient
            auto progressCb = [&](uint64_t transferred, uint64_t total) -> bool {
                // Check stop/shutdown
                if (m_shutdownTransfer || batch->stopRequested.load()) return false;

                // Handle pause inside transfer — track paused duration
                if (batch->pauseRequested.load()) {
                    auto pStart = std::chrono::steady_clock::now();
                    while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                        batch->state = BatchState::Paused;
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    batch->pausedSeconds = batch->pausedSeconds.load() +
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - pStart).count();
                }
                if (batch->stopRequested.load()) return false;
                batch->state = BatchState::Running;

                // Update per-file progress
                batch->curFileTransferred = transferred;
                if (total > 0) batch->curFileProgress = (float)((double)transferred / (double)total);

                // Update total progress
                uint64_t totalTx = bytesCompletedBefore + transferred;
                uint64_t tb = batch->totalBytes.load();
                if (tb > 0 && totalTx > tb) totalTx = tb;
                uint64_t currentTotal = batch->totalTransferred.load();
                if (totalTx < currentTotal) totalTx = currentTotal;
                batch->totalTransferred = totalTx;
                if (tb > 0) batch->totalProgress = (float)((double)totalTx / (double)tb);

                // Speed/ETA
                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - lastSpeedTime).count();
                if (dt >= 0.5) {
                    double delta = (double)(totalTx - lastSpeedBytes);
                    double instantSpeed = delta / dt;
                    // Exponential moving average — smooths out burst/pause fluctuations
                    double prevSpeed = batch->speedBytesPerSec.load();
                    double smoothed = (prevSpeed > 0) ? prevSpeed * 0.8 + instantSpeed * 0.2 : instantSpeed;
                    batch->speedBytesPerSec = smoothed;
                    if (smoothed > 0 && tb > totalTx) batch->etaSeconds = (double)(tb - totalTx) / smoothed;
                    else batch->etaSeconds = -1.0;
                    lastSpeedBytes = totalTx;
                    lastSpeedTime = now;
                }

                return true;
            };

            currentDestinationStarted = !item.isDirectory;

            // --- Local copy/move (Windows-to-Windows) ---
            if (batch->isLocalCopy) {
                bool ok = false;
                try {
                    // Same-drive move: instant rename (no data copy)
                    bool sameDrive = (item.sourcePath.size() >= 2 && item.destPath.size() >= 2 &&
                                     toupper(item.sourcePath[0]) == toupper(item.destPath[0]));

                    if (batch->isMove && sameDrive && !item.isDirectory) {
                        // Create parent directory if needed
                        auto dp = toFsPath(item.destPath);
                        if (dp.has_parent_path())
                            std::filesystem::create_directories(dp.parent_path());
                        std::filesystem::rename(toFsPath(item.sourcePath), dp);
                        progressCb(item.fileSize, item.fileSize);
                        ok = true;
                    } else if (item.isDirectory) {
                        std::filesystem::create_directories(toFsPath(item.destPath));
                        ok = true;
                    } else {
                        // Create parent directory if needed
                        auto dp = toFsPath(item.destPath);
                        if (dp.has_parent_path())
                            std::filesystem::create_directories(dp.parent_path());

                        bool touched=false;
                        try {
                            ok=copyLocalFile(toFsPath(item.sourcePath),toFsPath(item.destPath),item.fileSize,touched,progressCb);
                        } catch (...) {
                            if (touched) localPartialPath=item.destPath;
                            throw;
                        }
                        if (!ok && touched) localPartialPath=item.destPath;
                        if (ok && batch->isMove) std::filesystem::remove(toFsPath(item.sourcePath));
                    }
                } catch (const std::exception& e) {
                    batch->errorMessage = item.displayName + ": " + e.what();
                }

                if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; break; }
                if (!ok) {
                    if (batch->errorMessage.empty())
                        batch->errorMessage = item.displayName + ": " + (batch->isMove ? "move" : "copy") + " failed";
                    batch->state = BatchState::Failed;
                    break;
                }

                batch->logEvent(LogLevel::Info,std::string(item.isDirectory?"Prepared folder: ":batch->isMove?"Moved: ":"Copied: ")+item.sourcePath+" -> "+item.destPath);
                bytesCompletedBefore += item.fileSize;
                batch->totalTransferred = bytesCompletedBefore;
                batch->curFileProgress = 1.0f;
                m_lastTransferActivity = std::chrono::steady_clock::now();
                continue; // next file
            }

            // --- Cross-device Android-to-Android transfer ---
            if (batch->isCrossDevice) {
                DeviceClient& srcDev = deviceSession(batch->srcDeviceSlot).client;
                DeviceClient& dstDev = deviceSession(batch->dstDeviceSlot).client;
                bool ok = false;

                if (!srcDev.isServerRunning() || !dstDev.isServerRunning()) {
                    batch->state = BatchState::Failed;
                    batch->errorMessage = "Both devices must be connected for cross-device transfer";
                    break;
                }

                if (item.isDirectory) {
                    // Create directory on destination device
                    ok = dstDev.createDirectory(item.destPath);
                } else if (!batch->useDokanRelay) {
                    int pairCount = std::min((int)crossSrcChannels.size(), (int)crossDstChannels.size());
                    pairCount = std::min(pairCount, (int)TransferBatch::MAX_CHANNELS);

                    if (pairCount <= 0) {
                        ok = DeviceClient::relayFile(srcDev, dstDev, item.sourcePath, item.destPath,
                                                     item.fileSize, progressCb);
                    } else {
                        if (item.fileSize == 0)
                            item.fileSize = srcDev.getFileSize(item.sourcePath);

                        ok = dstDev.createFile(item.destPath, item.fileSize);
                        if (ok) {
                            static const uint64_t MB = 1024ULL * 1024;
                            static const uint64_t GB = 1024ULL * MB;
                            auto blockSizeFor = [](uint64_t fileSize, int channels) -> uint64_t {
                                if (channels <= 1) return fileSize;
                                const uint64_t MB = 1024ULL * 1024;
                                const uint64_t GB = 1024ULL * MB;
                                if (fileSize >= 2 * GB) return 128 * MB;
                                if (fileSize >= 512 * MB) return 96 * MB;
                                return 64 * MB;
                            };

                            struct RelayBlock {
                                uint64_t offset;
                                uint64_t length;
                            };

                            std::deque<RelayBlock> workQueue;
                            uint64_t blockSize = blockSizeFor(item.fileSize, pairCount);
                            uint64_t remaining = item.fileSize;
                            uint64_t offset = 0;
                            while (remaining > 0) {
                                uint64_t len = std::min(remaining, blockSize);
                                workQueue.push_back({offset, len});
                                offset += len;
                                remaining -= len;
                            }

                            std::mutex workMutex;
                            std::atomic<bool> failed{false};
                            std::atomic<int> completedBlocks{0};
                            int totalBlocks = (int)workQueue.size();
                            std::string relayError;
                            std::mutex relayErrorMutex;

                            for (int c = 0; c < pairCount; c++) {
                                batch->channels[c].bytesTransferred = 0;
                                batch->channels[c].speed = 0;
                                batch->channels[c].curBlockSize = 0;
                                batch->channels[c].curBlockTransferred = 0;
                                batch->channels[c].relayReadSec = 0;
                                batch->channels[c].relaySendSec = 0;
                                batch->channels[c].currentFile = item.displayName;
                                batch->channels[c].filesAssigned = totalBlocks;
                                batch->channels[c].channelName = crossSrcChannels[c].name + " > " + crossDstChannels[c].name;
                            }
                            batch->numChannels = pairCount;

                            auto updateCombinedProgress = [&]() {
                                uint64_t fileTransferred = 0;
                                double combinedSpeed = 0;
                                for (int c = 0; c < pairCount; c++) {
                                    fileTransferred += batch->channels[c].bytesTransferred.load();
                                    combinedSpeed += batch->channels[c].speed.load();
                                }
                                if (fileTransferred > item.fileSize) fileTransferred = item.fileSize;
                                batch->curFileTransferred = fileTransferred;
                                if (item.fileSize > 0)
                                    batch->curFileProgress = (float)((double)fileTransferred / (double)item.fileSize);
                                uint64_t totalTx = bytesCompletedBefore + fileTransferred;
                                uint64_t tb = batch->totalBytes.load();
                                if (tb > 0 && totalTx > tb) totalTx = tb;
                                uint64_t currentTotal = batch->totalTransferred.load();
                                if (totalTx < currentTotal) totalTx = currentTotal;
                                batch->totalTransferred = totalTx;
                                if (tb > 0) batch->totalProgress = (float)((double)totalTx / (double)tb);
                                batch->speedBytesPerSec = combinedSpeed;
                                batch->etaSeconds = (combinedSpeed > 0 && tb > totalTx)
                                    ? (double)(tb - totalTx) / combinedSpeed : -1.0;
                            };

                            auto relayWorker = [&](int chIdx) {
                                auto& ch = batch->channels[chIdx];
                                DeviceClient& srcCh = *crossSrcChannels[chIdx].dev;
                                DeviceClient& dstCh = *crossDstChannels[chIdx].dev;
                                uint64_t chCompleted = 0;
                                uint64_t lastChBytes = 0;
                                auto lastChTime = std::chrono::steady_clock::now();

                                while (!m_shutdownTransfer && !batch->stopRequested.load() && !failed.load()) {
                                    RelayBlock block{};
                                    {
                                        std::lock_guard<std::mutex> lk(workMutex);
                                        if (workQueue.empty()) break;
                                        block = workQueue.front();
                                        workQueue.pop_front();
                                    }

                                    ch.curBlockSize = block.length;
                                    ch.curBlockTransferred = 0;
                                    ch.currentFile = item.displayName + " [" + formatSize(block.offset) + "]";

                                    auto rangeProgress = [&](uint64_t transferred, uint64_t total) -> bool {
                                        if (m_shutdownTransfer || batch->stopRequested.load() || failed.load())
                                            return false;
                                        while (batch->pauseRequested.load() && !m_shutdownTransfer && !batch->stopRequested.load()) {
                                            batch->state = BatchState::Paused;
                                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                        }
                                        if (m_shutdownTransfer || batch->stopRequested.load()) return false;
                                        batch->state = BatchState::Running;

                                        ch.bytesTransferred = chCompleted + transferred;
                                        ch.curBlockTransferred = transferred;

                                        auto now = std::chrono::steady_clock::now();
                                        double dt = std::chrono::duration<double>(now - lastChTime).count();
                                        if (dt >= 0.5) {
                                            uint64_t chBytes = ch.bytesTransferred.load();
                                            double instant = (double)(chBytes - lastChBytes) / dt;
                                            double prev = ch.speed.load();
                                            ch.speed = prev > 0 ? prev * 0.8 + instant * 0.2 : instant;
                                            lastChBytes = chBytes;
                                            lastChTime = now;
                                        }

                                        updateCombinedProgress();
                                        m_lastTransferActivity = std::chrono::steady_clock::now();
                                        return true;
                                    };

                                    DeviceClient::RelayRangeStats relayStats;
                                    bool relayed = DeviceClient::relayRange(srcCh, dstCh,
                                        item.sourcePath, item.destPath,
                                        block.offset, block.offset, block.length,
                                        rangeProgress, &relayStats);

                                    ch.relayReadSec = ch.relayReadSec.load() + relayStats.srcRecvSec;
                                    ch.relaySendSec = ch.relaySendSec.load() + relayStats.dstSendSec;

                                    if (!relayed && !batch->stopRequested.load()) {
                                        failed = true;
                                        std::lock_guard<std::mutex> lk(relayErrorMutex);
                                        relayError = ch.channelName + ": " + srcCh.lastError();
                                        if (relayError.find(": ") == relayError.size() - 2)
                                            relayError += dstCh.lastError();
                                        break;
                                    }

                                    chCompleted += block.length;
                                    ch.bytesTransferred = chCompleted;
                                    ch.curBlockTransferred = block.length;
                                    ch.filesCompleted = completedBlocks.fetch_add(1) + 1;
                                    updateCombinedProgress();
                                }
                                ch.speed = 0;
                            };

                            LOG_INFO("Transfer", "Cross-device pipelined relay: " + item.displayName +
                                " using " + std::to_string(pairCount) + " channel pair(s), " +
                                std::to_string(totalBlocks) + " block(s)");

                            std::vector<std::thread> relayThreads;
                            for (int c = 0; c < pairCount; c++)
                                relayThreads.emplace_back(relayWorker, c);
                            for (auto& t : relayThreads) t.join();

                            ok = !failed.load() && !batch->stopRequested.load() &&
                                 completedBlocks.load() >= totalBlocks;
                            if (!ok && !batch->stopRequested.load()) {
                                if (!relayError.empty())
                                    batch->errorMessage = item.displayName + ": " + relayError;
                                dstDev.deleteFile(item.destPath);
                            }
                        }
                    }
                } else {
                    // Zero-copy relay: stream directly from device A to device B through PC memory
                    ok = DeviceClient::relayFile(srcDev, dstDev, item.sourcePath, item.destPath,
                                                  item.fileSize, progressCb);
                }

                if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; break; }
                if (!ok) {
                    batch->state = BatchState::Failed;
                    if (batch->errorMessage.empty())
                        batch->errorMessage = item.displayName + ": cross-device transfer failed";
                    break;
                }

                bytesCompletedBefore += item.fileSize;
                batch->totalTransferred = bytesCompletedBefore;
                batch->curFileProgress = 1.0f;
                m_lastTransferActivity = std::chrono::steady_clock::now();
                continue;
            }

            // Execute transfer via TCP server, with disconnect recovery
            bool ok = false;
            bool isResume = false;
            int maxRetries = 10;

            for (int attempt = 0; attempt <= maxRetries; attempt++) {
                if (batch->stopRequested.load()) break;

                if (attempt > 0) {
                    // Reconnect after disconnect — track disconnected duration
                    auto disconnectStart = std::chrono::steady_clock::now();
                    batch->disconnected = true;
                    batch->errorMessage = "Connection lost - reconnecting... (attempt " +
                        std::to_string(attempt) + "/" + std::to_string(maxRetries) + ")";
                    m_statusMessage = batch->errorMessage;
                    m_statusTime = std::chrono::steady_clock::now();

                    // Wait for device to come back and reconnect (up to 2 minutes)
                    bool reconnected = false;
                    for (int wait = 0; wait < 120 && !m_shutdownTransfer && !batch->stopRequested.load(); wait++) {
                        if (!waitForRetry(std::chrono::seconds(2), singleRetryGeneration)) break;

                        batch->errorMessage = "Waiting for device... (" + std::to_string(wait * 2) + "s)";

                        // Poll devices directly from this thread (don't rely on UI thread's m_devices)
                        auto devices = batchDev.getDevices();
                        std::string foundSerial;
                        for (auto& d : devices) {
                            // Match against original serial to avoid reconnecting to a different device
                            if (d.state == "device" && d.serial == originalSerial) { foundSerial = d.serial; break; }
                        }
                        if (foundSerial.empty()) continue;

                        batch->errorMessage = "Device found (" + foundSerial + ") - connecting...";
                        LOG_INFO("Transfer", "Device found: " + foundSerial + " - reconnecting");
                        if (!waitForRetry(std::chrono::seconds(3), singleRetryGeneration)) break;

                        // Wait for tethering to finish if poll thread is doing it
                        while (m_tetheringInProgress && !m_shutdownTransfer && !batch->stopRequested.load()) {
                            batch->errorMessage = "Waiting for tethering to complete...";
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        }
                        if (m_shutdownTransfer || batch->stopRequested.load()) break;

                        // Re-enable tethering only if user prefers Direct TCP
                        if (!m_preferAdbForward) {
                            std::string ip = batchDev.detectDeviceIp(foundSerial);
                            if (ip.empty()) {
                                batch->errorMessage = "Re-enabling USB tethering...";
                                LOG_INFO("Transfer", "No tethering IP - re-enabling tethering");
                                m_tetheringInProgress = true;
                                batchDev.tryEnableUsbTethering(foundSerial);
                                m_tetheringInProgress = false;
                            }
                        }

                        if (batchDev.startServer(foundSerial, m_preferAdbForward, m_prefs.rootEnabledForSerial(deviceIdentity(foundSerial)))) {
                            std::string connMode = batchDev.isDirectConnection()
                                ? "Direct TCP: " + batchDev.deviceIp() : "ADB Forward";
                            m_statusMessage = "Reconnected via " + connMode + " - resuming transfer";
                            m_statusTime = std::chrono::steady_clock::now();

                            // Update m_lastDeviceSerial so poll thread doesn't
                            // treat this as a "new device" and re-run tethering
                            m_lastDeviceSerial = foundSerial;

                            batch->disconnected = false;
                            batch->errorMessage.clear();
                            isResume = true;
                            reconnected = true;
                            // Accumulate disconnected time
                            batch->pausedSeconds = batch->pausedSeconds.load() +
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - disconnectStart).count();
                            // Reset speed tracking
                            lastSpeedBytes = batch->totalTransferred.load();
                            lastSpeedTime = std::chrono::steady_clock::now();
                            break;
                        } else {
                            batch->errorMessage = "Server start failed: " + batchDev.lastError();
                        }
                    }
                    if (!reconnected) continue; // retry loop
                }

                if (item.isDirectory) {
                    // Create directory on destination
                    if (batch->isPull) {
                        try { std::filesystem::create_directories(item.destPath); } catch (...) {}
                    } else {
                        batchDev.createDirectory(item.destPath);
                    }
                    ok = true;
                    break; // no retry needed for mkdir
                } else if (item.isMcrawVirtual) {
                    // MCRAW virtual item extraction
                    if (batch->isPull) {
                        // Android MCRAW → Windows: use pullMcrawItem
                        uint64_t outSize = 0;
                        ok = batchDev.pullMcrawItem(item.mcrawPath, item.virtualName,
                                                     item.destPath, outSize, progressCb);
                        if (ok && outSize > 0) item.fileSize = outSize;
                    } else {
                        // Windows MCRAW → Android: extract locally to temp, then push
                        std::string tmpPath = item.destPath + ".tmp";
                        ok = extractLocalMcrawItem(item.mcrawPath, item.virtualName, tmpPath);
                        if (ok) {
                            uint64_t tmpSize = (uint64_t)std::filesystem::file_size(toFsPath(tmpPath));
                            item.fileSize = tmpSize;
                            ok = batchDev.pushFile(tmpPath, item.destPath, tmpSize, progressCb);
                            std::filesystem::remove(toFsPath(tmpPath));
                        }
                    }
                } else if (batch->isPull) {
                    LOG_INFO("Transfer", "Batch pull: " + item.sourcePath + " -> " + item.destPath);
                    uint64_t outSize = 0;
                    if (isResume) {
                        ok = batchDev.resumePullFile(item.sourcePath, item.destPath, outSize, progressCb);
                    } else {
                        ok = batchDev.pullFile(item.sourcePath, item.destPath, outSize, progressCb);
                    }
                    LOG_INFO("Transfer", "Batch pull result: " + std::string(ok ? "OK" : "FAIL") +
                             " error=" + batchDev.lastError());
                    if (ok && outSize > 0) item.fileSize = outSize;
                } else {
                    if (isResume) {
                        ok = batchDev.resumePushFile(item.sourcePath, item.destPath, item.fileSize, progressCb);
                    } else {
                        ok = batchDev.pushFile(item.sourcePath, item.destPath, item.fileSize, progressCb);
                    }
                }

                if (ok) break; // success

                // Check if it was a user cancel vs connection error
                if (batch->stopRequested.load()) break;
                if (batchDev.lastError() == "Cancelled") break;

                // Local file errors (permission denied, read-only, etc.) — don't retry
                {
                    std::string err = batchDev.lastError();
                    if (err.find("Failed to create local file") != std::string::npos ||
                        err.find("Cannot open") != std::string::npos ||
                        err.find("Access is denied") != std::string::npos) {
                        batch->state = BatchState::Failed;
                        batch->errorMessage = err + "\n\nThe destination may require administrator rights or be read-only.";
                        break;
                    }
                }

                // If we've used all automatic retries, ask the user
                if (attempt == maxRetries) {
                    batch->errorMessage = "Connection failed after " + std::to_string(maxRetries) +
                        " retries. Click Retry or Stop.";
                    batch->waitingForUserRetry = true;
                    batch->userRetryRequested = false;

                    // Wait for user to click Retry or Stop (uses CV instead of busy-wait)
                    {
                        std::unique_lock<std::mutex> lk(m_batchMutex);
                        m_batchCV.wait(lk, [&]() {
                            return m_shutdownTransfer || batch->stopRequested.load() || batch->userRetryRequested.load();
                        });
                    }

                    batch->waitingForUserRetry = false;
                    if (batch->userRetryRequested.load()) {
                        batch->userRetryRequested = false;
                        batch->errorMessage.clear();
                        attempt = -1; // reset retry counter (will become 0 on loop increment)
                        isResume = true;
                        continue;
                    }
                    break; // user chose stop
                }

                // Connection error - will retry
                isResume = true;
            }

            if (batch->stopRequested.load()) { batch->state = BatchState::Stopped; break; }

            if (!ok) {
                batch->state = BatchState::Failed;
                batch->errorMessage = item.displayName + ": " + batchDev.lastError();
                break;
            }

            m_lastTransferActivity = std::chrono::steady_clock::now();

            // CRC32 verification with retry (can be disabled in preferences)
            // Skip CRC for MCRAW virtual items — data is generated on-the-fly, no remote file to verify against
            if (!item.isDirectory && !item.isMcrawVirtual && !batch->isLocalCopy && !batch->stopRequested.load() &&
                batchDev.isServerRunning() && m_prefs.enableCrcVerification) {
                std::string crcDetail;
                std::string remotePath = batch->isPull ? item.sourcePath : item.destPath;
                std::string localPath  = batch->isPull ? item.destPath   : item.sourcePath;
                batch->crcFileName = item.displayName;
                batch->crcProgress = 0.0f;
                batch->crcPhase = 0;
                batch->state = BatchState::Verifying;
                batch->errorMessage = "Verifying: " + item.displayName;

                bool crcOk = false;
                double crcRemoteMs = 0, crcLocalMs = 0;
                auto crcTotalStart = std::chrono::steady_clock::now();
                for (int crcAttempt = 0; crcAttempt < 3; crcAttempt++) {
                    batch->crcProgress = 0.0f;
                    batch->crcPhase = 0;
                    if (batchDev.verifyFileCrc(remotePath, localPath, crcDetail, &batch->crcProgress, &batch->crcPhase,
                                               &crcRemoteMs, &crcLocalMs)) {
                        crcOk = true;
                        break;
                    }
                    // If it's a connection error (not actual mismatch), retry after reconnect
                    if (crcDetail.find("Local:") != std::string::npos) break; // actual CRC mismatch, don't retry
                    LOG_WARN("Transfer", "CRC check failed (attempt " + std::to_string(crcAttempt+1) + "/3): " + crcDetail);
                    if (crcAttempt < 2 && !batchDev.isServerRunning()) {
                        // Try to reconnect
                        batch->errorMessage = "CRC check failed - reconnecting...";
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        batchDev.startServer(originalSerial, m_preferAdbForward, m_prefs.rootEnabledForSerial(deviceIdentity(originalSerial)));
                        if (!batchDev.isServerRunning()) break;
                        batch->errorMessage = "Retrying CRC verification: " + item.displayName;
                    }
                }
                batch->state = BatchState::Running;
                batch->crcFileName.clear();
                batch->crcProgress = 0.0f;
                batch->crcPhase = 0;

                double crcTotalMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - crcTotalStart).count();

                TransferBatch::CrcResult crcRes;
                crcRes.fileName = item.displayName;
                crcRes.passed = crcOk;
                crcRes.detail = crcDetail;
                crcRes.remoteMs = crcRemoteMs;
                crcRes.localMs = crcLocalMs;
                crcRes.totalMs = crcTotalMs;
                crcRes.fileSize = item.fileSize;
                crcRes.sourcePath = item.sourcePath;  // original batch paths (not CRC-rearranged)
                crcRes.destPath = item.destPath;
                batch->crcResults.push_back(std::move(crcRes));

                if (!crcOk) {
                    LOG_WARN("Transfer", "CRC failed for " + item.displayName + ": " + crcDetail + " — continuing batch");
                    batch->errorMessage = "CRC failed: " + item.displayName + " — continuing...";
                } else {
                    batch->errorMessage.clear();
                }
            }

            bytesCompletedBefore += item.fileSize;
            batch->totalTransferred = bytesCompletedBefore;
            batch->curFileProgress = 1.0f;
        }

        for (auto& rc : crossSrcChannels) {
            if (rc.owned) rc.owned->disconnectTcp();
        }
        for (auto& rc : crossDstChannels) {
            if (rc.owned) rc.owned->disconnectTcp();
        }

        // Clean up partial files and flush stale data on stop/cancel/error
        if (batch->state.load() == BatchState::Stopped || batch->state.load() == BatchState::Failed) {
            // Delete the partially-transferred file (the one that was in progress when stopped)
            int curIdx = batch->currentFileIndex.load();
            if (currentDestinationStarted && curIdx >= 0 && curIdx < batch->totalFiles()) {
                auto& item = batch->files[curIdx];
                if (!item.isDirectory && batch->isPull && !batch->isLocalCopy) {
                    // Pull: partial file is on Windows
                    try {
                        if (std::filesystem::exists(toFsPath(item.destPath))) {
                            std::filesystem::remove(toFsPath(item.destPath));
                            LOG_INFO("Transfer", "Deleted partial file: " + item.destPath);
                        }
                    } catch (...) {}
                } else if (!item.isDirectory && !batch->isPull && !batch->isLocalCopy) {
                    // Push: partial file is on Android
                    batchDev.deleteFile(item.destPath);
                    LOG_INFO("Transfer", "Deleted partial remote file: " + item.destPath);
                } else if (!item.isDirectory && batch->isLocalCopy && localPartialPath==item.destPath) {
                    // Local copy: partial file is on Windows
                    try {
                        if (std::filesystem::exists(toFsPath(item.destPath))) {
                            std::filesystem::remove(toFsPath(item.destPath));
                            LOG_INFO("Transfer", "Deleted partial file: " + item.destPath);
                        }
                    } catch (...) {}
                }
            }
            batchDev.flushStaleData();
            batchDev.restoreTimeouts();
        }

        if (batch->isLocalCopy && batch->isMove && batch->state==BatchState::Running) {
            for (auto item=batch->files.rbegin();item!=batch->files.rend();++item) {
                if (!item->isDirectory) continue;
                std::error_code error;
                std::filesystem::remove(toFsPath(item->sourcePath),error);
                if (error) {
                    ++batch->errorSkippedFiles;
                    batch->logEvent(LogLevel::Warn,"Source folder retained after move: "+item->sourcePath+": "+error.message());
                }
            }
        }

        // Finalize batch
        if (batch->state.load() == BatchState::Running || batch->state.load() == BatchState::Verifying) {
            batch->totalProgress = 1.0f;
            batch->totalTransferred = batch->totalBytes.load();
            auto totalTime = std::chrono::steady_clock::now() - batch->startTime;
            double wallSec = std::chrono::duration<double>(totalTime).count();
            double activeSec = wallSec - batch->pausedSeconds.load(); // exclude paused/disconnected time
            if (activeSec < 0.1) activeSec = 0.1;
            double avgSpeed = (activeSec > 0) ? (double)batch->totalTransferred.load() / activeSec : 0;
            batch->finalAvgSpeed = avgSpeed;
            batch->finalTimeSec = activeSec;
            batch->speedBytesPerSec = 0;
            batch->etaSeconds = -1;
            batch->state = BatchState::Completed;
            logBatchPerformance(batch->isCrossDevice ? "cross-device streamed" :
                                (batch->isLocalCopy ? "local copy" : "single-channel"),
                                activeSec, avgSpeed);

            std::string skippedStr = (batch->skippedFiles > 0)
                ? " (" + std::to_string(batch->skippedFiles) + " skipped)" : "";
            m_statusMessage = "Batch done: " + std::to_string(batch->totalFiles()) + " files" + skippedStr + ", " +
                formatSize(batch->totalTransferred.load()) + " @ " + formatSpeed(avgSpeed);
            m_statusTime = std::chrono::steady_clock::now();

            m_leftPanel.needsRefresh = true;
            m_rightPanel.needsRefresh = true;
        }
        } catch (const std::exception& error) {
            batch->errorMessage=error.what();
            batch->state=BatchState::Failed;
        } catch (...) {
            batch->errorMessage="Unexpected transfer exception";
            batch->state=BatchState::Failed;
        }
    }
}

std::string App::queryDeviceDisplayName(const std::string& serial) {
    if (companion::isSerial(serial)) {
        CompanionDeviceView view;
        return companion::find(companion::idFromSerial(serial), view) ? view.name : std::string();
    }
    DeviceClient query;
    query.setAdbPath(m_device.getAdbPath());
    auto result = query.runAdbCommandResult("-s " + serial +
        " shell getprop", 2500, [this] { return m_shutdownPoll.load(); });
    if (!result.succeeded()) return {};
    std::unordered_map<std::string, std::string> properties;
    std::istringstream lines(result.standardOutput);
    std::string line;
    while (std::getline(lines, line)) {
        const auto split = line.find("]: [");
        const auto end = line.rfind(']');
        if (line.empty() || line.front() != '[' || split == std::string::npos || end <= split + 3) continue;
        properties[line.substr(1, split - 1)] = sanitizeDeviceDisplayValue(line.substr(split + 4, end - split - 4));
    }
    for (const char* key : {"ro.product.marketname", "ro.product.vendor.marketname", "ro.config.marketing_name"})
        if (!properties[key].empty()) return properties[key];
    std::string brand = properties["ro.product.brand"];
    const std::string model = properties["ro.product.model"];
    if (brand.empty()) return model;
    brand[0] = (char)toupper((unsigned char)brand[0]);
    return model.empty() ? brand : brand + " " + model;
}

std::string App::formatSize(uint64_t bytes) {
    if (bytes == 0) return "0 B";
    const char* u[] = {"B","KB","MB","GB","TB"};
    int i = 0; double s = (double)bytes;
    while (s >= 1024.0 && i < 4) { s /= 1024.0; i++; }
    char buf[32];
    if (i == 0) snprintf(buf, sizeof(buf), "%llu B", bytes);
    else snprintf(buf, sizeof(buf), "%.1f %s", s, u[i]);
    return buf;
}

std::string App::formatSpeed(double bps) {
    if (bps <= 0) return "0 B/s";
    const char* u[] = {"B/s","KB/s","MB/s","GB/s"};
    int i = 0; double v = bps;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; i++; }
    char buf[32]; snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
    return buf;
}

std::string App::formatETA(double sec) {
    if (sec < 0) return "";
    if (sec < 60) { char b[16]; snprintf(b,sizeof(b),"%.0fs",sec); return b; }
    if (sec < 3600) { int m=(int)(sec/60),s=(int)sec%60; char b[16]; snprintf(b,sizeof(b),"%dm %02ds",m,s); return b; }
    int h=(int)(sec/3600),m=((int)sec%3600)/60; char b[16]; snprintf(b,sizeof(b),"%dh %02dm",h,m); return b;
}

std::string App::getFileIcon(const std::string& name, bool isDir) {
    if (isDir) return "[D]";
    auto p = name.rfind('.'); if (p == std::string::npos) return "   ";
    std::string ext = name.substr(p);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext==".jpg"||ext==".jpeg"||ext==".png"||ext==".gif"||ext==".bmp"||ext==".webp"||ext==".dng") return "[I]";
    if (ext==".mp4"||ext==".mkv"||ext==".avi"||ext==".mov"||ext==".webm") return "[V]";
    if (ext==".mp3"||ext==".wav"||ext==".ogg"||ext==".flac"||ext==".aac") return "[A]";
    if (ext==".apk"||ext==".xapk") return "[K]";
    if (ext==".zip"||ext==".rar"||ext==".7z"||ext==".tar"||ext==".gz") return "[Z]";
    if (ext==".mcraw") return "[M]";
    if (ext==".pdf") return "[P]";
    if (ext==".txt"||ext==".log"||ext==".md"||ext==".json"||ext==".xml"||ext==".csv") return "[T]";
    return "   ";
}

std::vector<std::string> App::getWindowsDrives() {
    std::vector<std::string> drives;
    DWORD mask = GetLogicalDrives();
    for (char c='A'; c<='Z'; c++) {
        std::string drive = std::string(1, c) + ":";
        if ((mask & 1) && !isOwnMountedDevicePath(drive)) drives.push_back(std::move(drive));
        mask >>= 1;
    }
    return drives;
}

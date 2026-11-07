// JNI glue between the Kotlin companion app and the native file server.

#include <jni.h>
#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "afm_host.h"

#define LOG_TAG "FastEnoughNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

JavaVM* g_vm = nullptr;
jobject g_bridge = nullptr;
jmethodID g_authorize = nullptr;
jmethodID g_appCommand = nullptr;
jmethodID g_launchRoot = nullptr;
jmethodID g_clientsChanged = nullptr;
pthread_key_t g_detachKey;

void detachThread(void*) {
    if (g_vm) g_vm->DetachCurrentThread();
}

JNIEnv* env() {
    JNIEnv* result = nullptr;
    if (g_vm->GetEnv((void**)&result, JNI_VERSION_1_6) == JNI_OK) return result;
    if (g_vm->AttachCurrentThread(&result, nullptr) != JNI_OK) return nullptr;
    pthread_setspecific(g_detachKey, result);
    return result;
}

std::string toString(JNIEnv* e, jstring value) {
    if (!value) return {};
    const char* chars = e->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars) e->ReleaseStringUTFChars(value, chars);
    e->DeleteLocalRef(value);
    return result;
}

// Builds a Java string from UTF-8 bytes without relying on modified UTF-8.
jstring fromUtf8(JNIEnv* e, const std::string& value) {
    jbyteArray bytes = e->NewByteArray((jsize)value.size());
    e->SetByteArrayRegion(bytes, 0, (jsize)value.size(), (const jbyte*)value.data());
    jclass stringClass = e->FindClass("java/lang/String");
    jmethodID constructor = e->GetMethodID(stringClass, "<init>", "([BLjava/lang/String;)V");
    jstring charset = e->NewStringUTF("UTF-8");
    auto result = (jstring)e->NewObject(stringClass, constructor, bytes, charset);
    e->DeleteLocalRef(charset);
    e->DeleteLocalRef(bytes);
    e->DeleteLocalRef(stringClass);
    return result;
}

std::string callString(jmethodID method, const std::string& argument, int command = -1) {
    JNIEnv* e = env();
    if (!e || !g_bridge) return {};
    jstring arg = fromUtf8(e, argument);
    jstring result = command >= 0
        ? (jstring)e->CallObjectMethod(g_bridge, method, (jint)command, arg)
        : (jstring)e->CallObjectMethod(g_bridge, method, arg);
    e->DeleteLocalRef(arg);
    if (e->ExceptionCheck()) {
        e->ExceptionDescribe();
        e->ExceptionClear();
        return {};
    }
    return toString(e, result);
}

// The Kotlin side answers with a single status character followed by text:
// 'Y' or 'R' (accepted, R requests root) or 'N' (rejected).
bool authorize(const std::string& request, std::string& reply, bool& useRoot) {
    std::string answer = callString(g_authorize, request);
    if (answer.empty() || (answer[0] != 'Y' && answer[0] != 'R')) {
        reply = answer.size() > 1 ? answer.substr(1) : "Not authorized";
        return false;
    }
    useRoot = answer[0] == 'R';
    reply = answer.substr(1);
    return true;
}

bool appCommand(uint32_t command, const std::string& payload, std::string& reply) {
    std::string answer = callString(g_appCommand, payload, (int)command);
    if (answer.empty()) { reply = "Request failed"; return false; }
    reply = answer.substr(1);
    return answer[0] == 'Y';
}

void clientsChanged(int count) {
    JNIEnv* e = env();
    if (!e || !g_bridge) return;
    e->CallVoidMethod(g_bridge, g_clientsChanged, (jint)count);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

// --- Root helper channel ---

std::mutex g_rootMutex;
int g_rootChannel = -1;
std::atomic<bool> g_rootExited{false};

std::string randomHex(size_t bytes) {
    std::random_device device;
    static const char* digits = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned value = device() & 0xff;
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}

bool channelAlive(int fd) {
    pollfd p{fd, POLLIN | POLLRDHUP, 0};
    if (poll(&p, 1, 0) < 0) return false;
    // The helper never writes after the handshake, so readable means closed.
    return (p.revents & (POLLIN | POLLHUP | POLLERR | POLLRDHUP | POLLNVAL)) == 0;
}

bool readWithTimeout(int fd, void* buffer, size_t length, int timeoutMs) {
    auto* p = static_cast<char*>(buffer);
    while (length > 0) {
        pollfd waitFor{fd, POLLIN, 0};
        if (poll(&waitFor, 1, timeoutMs) <= 0) return false;
        ssize_t n = recv(fd, p, length, 0);
        if (n <= 0) return false;
        p += n;
        length -= (size_t)n;
    }
    return true;
}

int rootHelperSocket() {
    std::lock_guard<std::mutex> lock(g_rootMutex);
    if (g_rootChannel >= 0) {
        if (channelAlive(g_rootChannel)) return g_rootChannel;
        close(g_rootChannel);
        g_rootChannel = -1;
    }

    const std::string name = "fastenough.root." + std::to_string(getpid()) + "." + randomHex(8);
    const std::string secret = randomHex(24);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path + 1, name.data(), name.size());
    socklen_t addrLength = (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    if (bind(listener, (sockaddr*)&addr, addrLength) < 0 || listen(listener, 1) < 0) {
        LOGW("Root channel bind failed: %s", strerror(errno));
        close(listener);
        return -1;
    }

    // Kotlin starts "su -c <helper> --root-helper <name> <secret>".
    g_rootExited = false;
    JNIEnv* e = env();
    bool launched = false;
    if (e && g_bridge) {
        jstring jname = fromUtf8(e, name);
        jstring jsecret = fromUtf8(e, secret);
        launched = e->CallBooleanMethod(g_bridge, g_launchRoot, jname, jsecret);
        if (e->ExceptionCheck()) { e->ExceptionClear(); launched = false; }
        e->DeleteLocalRef(jname);
        e->DeleteLocalRef(jsecret);
    }
    if (!launched) {
        close(listener);
        return -1;
    }

    // Allow time for the user to approve the root request, unless su exits first.
    int ready = 0;
    for (int waited = 0; waited < 90000 && ready == 0 && !g_rootExited; waited += 250) {
        pollfd waitFor{listener, POLLIN, 0};
        ready = poll(&waitFor, 1, 250);
    }
    int channel = ready > 0 ? accept4(listener, nullptr, nullptr, SOCK_CLOEXEC) : -1;
    close(listener);
    if (channel < 0) {
        LOGW("Root helper did not connect");
        return -1;
    }

    ucred peer{};
    socklen_t peerLength = sizeof(peer);
    uint32_t secretLength = 0;
    std::string received;
    bool valid = getsockopt(channel, SOL_SOCKET, SO_PEERCRED, &peer, &peerLength) == 0 && peer.uid == 0 &&
        readWithTimeout(channel, &secretLength, 4, 5000) && secretLength == secret.size();
    if (valid) {
        received.resize(secretLength);
        valid = readWithTimeout(channel, received.data(), secretLength, 5000) && received == secret;
    }
    if (!valid) {
        LOGW("Rejected root helper connection");
        close(channel);
        return -1;
    }
    LOGI("Root helper connected");
    g_rootChannel = channel;
    return channel;
}

void closeRootChannel() {
    std::lock_guard<std::mutex> lock(g_rootMutex);
    if (g_rootChannel >= 0) close(g_rootChannel);
    g_rootChannel = -1;
}

// --- Server thread ---

std::mutex g_serverMutex;
std::thread g_serverThread;
bool g_serverRunning = false;

} // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    pthread_key_create(&g_detachKey, detachThread);
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
Java_com_fastenough_companion_NativeBridge_nativeInit(JNIEnv* e, jobject thiz) {
    if (g_bridge) return;
    g_bridge = e->NewGlobalRef(thiz);
    jclass type = e->GetObjectClass(thiz);
    g_authorize = e->GetMethodID(type, "authorize", "(Ljava/lang/String;)Ljava/lang/String;");
    g_appCommand = e->GetMethodID(type, "appCommand", "(ILjava/lang/String;)Ljava/lang/String;");
    g_launchRoot = e->GetMethodID(type, "launchRootHelper", "(Ljava/lang/String;Ljava/lang/String;)Z");
    g_clientsChanged = e->GetMethodID(type, "clientsChanged", "(I)V");
    e->DeleteLocalRef(type);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_fastenough_companion_NativeBridge_nativeStartServer(JNIEnv*, jobject, jint port) {
    std::lock_guard<std::mutex> lock(g_serverMutex);
    if (g_serverRunning) return 0;
    if (g_serverThread.joinable()) g_serverThread.join();

    struct Startup {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        int error = 0;
    };
    auto startup = std::make_shared<Startup>();
    AfmHostHooks hooks;
    hooks.authorize = authorize;
    hooks.appCommand = appCommand;
    hooks.rootHelperSocket = rootHelperSocket;
    hooks.clientsChanged = clientsChanged;
    hooks.listening = [startup](bool listening, int error) {
        std::lock_guard<std::mutex> guard(startup->mutex);
        startup->done = true;
        startup->error = listening ? 0 : (error ? error : 1);
        startup->cv.notify_all();
    };
    g_serverThread = std::thread([port, hooks]() {
        int result = afm_host_run(port, hooks);
        LOGI("Server stopped (%d)", result);
    });
    std::unique_lock<std::mutex> wait(startup->mutex);
    startup->cv.wait_for(wait, std::chrono::seconds(5), [&]() { return startup->done; });
    if (!startup->done) return ETIMEDOUT;
    if (startup->error != 0) {
        wait.unlock();
        g_serverThread.join();
        return startup->error;
    }
    g_serverRunning = true;
    LOGI("Server listening on %d", port);
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_fastenough_companion_NativeBridge_nativeStopServer(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_serverMutex);
    if (!g_serverRunning) return;
    afm_host_stop();
    if (g_serverThread.joinable()) g_serverThread.join();
    g_serverRunning = false;
    closeRootChannel();
}

extern "C" JNIEXPORT void JNICALL
Java_com_fastenough_companion_NativeBridge_nativeStopRoot(JNIEnv*, jobject) {
    closeRootChannel();
}

extern "C" JNIEXPORT void JNICALL
Java_com_fastenough_companion_NativeBridge_nativeRootHelperExited(JNIEnv*, jobject) {
    g_rootExited = true;
}

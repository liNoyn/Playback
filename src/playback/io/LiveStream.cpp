// Winsock must come before anything that pulls in <windows.h>.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "LiveStream.h"

#include "playback/Playback.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace playback::io {

namespace {

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

constexpr auto CONNECT_TIMEOUT        = std::chrono::milliseconds(500);
constexpr auto LAUNCH_CONNECT_TIMEOUT = std::chrono::seconds(30);
constexpr auto LAUNCH_RETRY_INTERVAL  = std::chrono::milliseconds(250);

bool ensureWinsock() {
    static std::once_flag once;
    static bool           ready = false;
    std::call_once(once, [] {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ready;
}

void closeSocket(uintptr_t& socket) {
    if (socket != INVALID_SOCKET) {
        closesocket(static_cast<SOCKET>(socket));
        socket = INVALID_SOCKET;
    }
}

// Non-blocking connect with a timeout; returns INVALID_SOCKET on failure.
uintptr_t connectTo(std::string const& host, int port, std::chrono::milliseconds timeout) {
    if (!ensureWinsock()) return INVALID_SOCKET;

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses) != 0) return INVALID_SOCKET;

    SOCKET result = INVALID_SOCKET;
    for (auto* address = addresses; address && result == INVALID_SOCKET; address = address->ai_next) {
        SOCKET s = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        if (::connect(s, address->ai_addr, static_cast<int>(address->ai_addrlen)) != 0
            && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            fd_set failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval wait{};
            wait.tv_sec  = static_cast<long>(timeout.count() / 1000);
            wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
            if (select(0, nullptr, &writable, &failed, &wait) != 1 || !FD_ISSET(s, &writable)) {
                closesocket(s);
                continue;
            }
        }
        int       error  = 0;
        int       length = sizeof(error);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
        if (error != 0) {
            closesocket(s);
            continue;
        }

        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
        BOOL noDelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&noDelay), sizeof(noDelay));
        result = s;
    }
    freeaddrinfo(addresses);
    return static_cast<uintptr_t>(result);
}

bool sendAll(uintptr_t socket, std::string_view data) {
    while (!data.empty()) {
        int sent = ::send(static_cast<SOCKET>(socket), data.data(), static_cast<int>(std::min<size_t>(data.size(), 1 << 20)), 0);
        if (sent <= 0) return false;
        data.remove_prefix(static_cast<size_t>(sent));
    }
    return true;
}

std::string frame(LiveStream::FrameType type, std::string_view head, std::string_view body) {
    auto const  length = static_cast<uint32_t>(head.size() + body.size());
    std::string out;
    out.reserve(5 + length);
    out.push_back(static_cast<char>(type));
    out.push_back(static_cast<char>((length >> 24) & 0xFF));
    out.push_back(static_cast<char>((length >> 16) & 0xFF));
    out.push_back(static_cast<char>((length >> 8) & 0xFF));
    out.push_back(static_cast<char>(length & 0xFF));
    out.append(head);
    out.append(body);
    return out;
}

std::wstring widen(std::string const& utf8) {
    if (utf8.empty()) return {};
    int          size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

// Starts the converter hidden; the command runs through cmd so batch launchers work.
bool launchConverter(std::string const& command) {
    std::wstring cmdLine = L"cmd.exe /d /s /c \"" + widen(command) + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr,
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            nullptr,
            nullptr,
            &startup,
            &process
        )) {
        getLogger().warn("Live stream: unable to launch the converter (error {})", GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    getLogger().info("Live stream: launched the converter");
    return true;
}

} // namespace

LiveStreamConfig LiveStreamConfig::load(std::filesystem::path const& file) {
    LiveStreamConfig config;
    std::error_code  ec;
    if (!std::filesystem::exists(file, ec)) {
        nlohmann::ordered_json defaults{
            {"enabled",            config.enabled           },
            {"host",               config.host              },
            {"port",               config.port              },
            {"launchCommand",      config.launchCommand     },
            {"maxQueuedMegabytes", config.maxQueuedMegabytes}
        };
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << defaults.dump(4);
        return config;
    }

    try {
        std::ifstream in(file, std::ios::binary);
        auto          json        = nlohmann::json::parse(in);
        config.enabled            = json.value("enabled", config.enabled);
        config.host               = json.value("host", config.host);
        config.port               = json.value("port", config.port);
        config.launchCommand      = json.value("launchCommand", config.launchCommand);
        config.maxQueuedMegabytes = json.value("maxQueuedMegabytes", config.maxQueuedMegabytes);
    } catch (std::exception const& exception) {
        getLogger().warn("Live stream: ignoring invalid {}: {}", file.string(), exception.what());
        return LiveStreamConfig{};
    }
    return config;
}

std::shared_ptr<LiveStream> LiveStream::start(LiveStreamConfig config, std::string helloJson) {
    if (!config.enabled) return nullptr;
    auto stream = std::make_shared<LiveStream>(Private{}, std::move(config), std::move(helloJson));
    stream->enqueue(FrameType::Hello, stream->mHelloJson);
    // The thread keeps the stream alive until everything is sent, even after the recorder lets go of it.
    std::thread([self = stream] { self->run(); }).detach();
    return stream;
}

LiveStream::LiveStream(Private, LiveStreamConfig config, std::string helloJson)
: mConfig(std::move(config)),
  mHelloJson(std::move(helloJson)),
  mSocket(INVALID_SOCKET) {}

LiveStream::~LiveStream() { closeSocket(mSocket); }

void LiveStream::sendChunkData(std::string_view bytes) {
    if (!bytes.empty()) enqueue(FrameType::ChunkData, bytes);
}

void LiveStream::sendCacheEntry(int32_t index, std::string_view body) {
    auto const value = static_cast<uint32_t>(index);
    char const head[4]{
        static_cast<char>((value >> 24) & 0xFF),
        static_cast<char>((value >> 16) & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
        static_cast<char>(value & 0xFF)
    };
    enqueue(FrameType::CacheEntry, std::string_view(head, sizeof(head)), body);
}

void LiveStream::sendChunkEnd(std::string json) { enqueue(FrameType::ChunkEnd, json); }

void LiveStream::finish(std::string finishJson) {
    bool broken;
    {
        std::scoped_lock lock(mMutex);
        broken = mBroken;
    }
    if (broken) {
        // The converter missed part of the recording; let it convert the exported file instead.
        auto hello = nlohmann::json::parse(mHelloJson, nullptr, false);
        if (hello.is_discarded()) hello = nlohmann::json::object();
        hello["streamComplete"] = false;
        std::thread(reportExportedFile, mConfig, hello.dump(), std::move(finishJson)).detach();
        return;
    }
    enqueue(FrameType::Finish, finishJson);
    std::scoped_lock lock(mMutex);
    mClosing = true;
    mCondition.notify_all();
}

void LiveStream::cancel(std::string cancelJson) {
    enqueue(FrameType::Cancel, cancelJson);
    std::scoped_lock lock(mMutex);
    mClosing = true;
    mCondition.notify_all();
}

bool LiveStream::isBroken() const {
    std::scoped_lock lock(mMutex);
    return mBroken;
}

void LiveStream::enqueue(FrameType type, std::string_view head, std::string_view body) {
    std::string data = frame(type, head, body);
    {
        std::scoped_lock lock(mMutex);
        if (mBroken || mClosing) return;
        auto const limit = static_cast<size_t>(std::max(1, mConfig.maxQueuedMegabytes)) * 1024 * 1024;
        if (mQueuedBytes + data.size() > limit) {
            mBroken     = true;
            mOverflowed = true;
            mQueue.clear();
            mQueuedBytes = 0;
            mCondition.notify_all();
            getLogger().warn("Live stream: converter is not keeping up ({} MB queued), streaming stopped", limit >> 20);
            return;
        }
        mQueuedBytes += data.size();
        mQueue.push_back(std::move(data));
    }
    mCondition.notify_one();
}

void LiveStream::markBroken(std::string_view reason) {
    {
        std::scoped_lock lock(mMutex);
        if (mBroken) return;
        mBroken = true;
        mQueue.clear();
        mQueuedBytes = 0;
    }
    getLogger().info("Live stream: {}", reason);
}

bool LiveStream::connectToConverter() {
    mSocket = connectTo(mConfig.host, mConfig.port, CONNECT_TIMEOUT);
    if (mSocket != INVALID_SOCKET) return true;
    if (mConfig.launchCommand.empty() || !launchConverter(mConfig.launchCommand)) return false;

    auto const deadline = std::chrono::steady_clock::now() + LAUNCH_CONNECT_TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::scoped_lock lock(mMutex);
            if (mBroken) return false;
        }
        std::this_thread::sleep_for(LAUNCH_RETRY_INTERVAL);
        mSocket = connectTo(mConfig.host, mConfig.port, CONNECT_TIMEOUT);
        if (mSocket != INVALID_SOCKET) return true;
    }
    return false;
}

void LiveStream::run() {
    if (!connectToConverter()) {
        markBroken("no converter listening on " + mConfig.host + ":" + std::to_string(mConfig.port) + ", not streaming");
        return;
    }
    getLogger().info("Live stream: connected to the converter on {}:{}", mConfig.host, mConfig.port);

    while (true) {
        std::string data;
        {
            std::unique_lock lock(mMutex);
            mCondition.wait(lock, [this] { return mBroken || mClosing || !mQueue.empty(); });
            if (mBroken) break;
            if (mQueue.empty()) break; // closing and everything was sent
            data = std::move(mQueue.front());
            mQueue.pop_front();
            mQueuedBytes -= data.size();
        }
        if (!sendAll(mSocket, data)) {
            markBroken("connection to the converter lost (error " + std::to_string(WSAGetLastError()) + ")");
            break;
        }
    }
    bool overflowed;
    {
        std::scoped_lock lock(mMutex);
        overflowed = mOverflowed;
    }
    if (overflowed) {
        // The exported file is reported on a new connection at finish(); this partial stream is useless.
        sendAll(mSocket, frame(FrameType::Cancel, R"({"reason":"live stream dropped: converter fell behind"})", {}));
    }
    shutdown(static_cast<SOCKET>(mSocket), SD_SEND);
    closeSocket(mSocket);
}

void LiveStream::reportExportedFile(LiveStreamConfig config, std::string helloJson, std::string finishJson) {
    auto socket = connectTo(config.host, config.port, CONNECT_TIMEOUT);
    if (socket == INVALID_SOCKET) {
        getLogger().info("Live stream: converter unavailable; convert the exported replay manually");
        return;
    }
    bool sent = sendAll(socket, frame(FrameType::Hello, helloJson, {}))
             && sendAll(socket, frame(FrameType::Finish, finishJson, {}));
    if (sent) getLogger().info("Live stream: asked the converter to convert the exported replay");
    shutdown(static_cast<SOCKET>(socket), SD_SEND);
    closeSocket(socket);
}

} // namespace playback::io

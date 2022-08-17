#include <system_error>
#include <array>
#include <string_view>
#include <vector>
#include <algorithm>
#include <memory>
#include <optional>
#include <iterator>

#include <cstddef>
#include <cstring>
#include <cassert>
#include <cstdio>

#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <unistd.h>

/// AF_UNIX / local socket specific address
using sockaddr_un_t = struct sockaddr_un;
/// generic address, usually pointer argument
using sockaddr_t = struct sockaddr;
/// used as list for poll()
using pollfd_t = struct pollfd;
/// file descriptor
using Descriptor = int;

/// std::errc or int
/// Unwrap will either return the int, or throw the errc as a system_error
class SysReturn {
    static constexpr std::errc sNotAnError = std::errc();
public:
    constexpr SysReturn(int value) noexcept : mCode(sNotAnError), mValue(value) {}
    constexpr SysReturn(std::errc code) noexcept : mCode(code), mValue() {}
    constexpr bool IsError() const { return mCode != sNotAnError; }
    [[noreturn]] void ThrowCode() const { throw std::system_error(std::make_error_code(mCode)); }
    constexpr int Unwrap() const { if (IsError()) ThrowCode(); return mValue; }
    constexpr std::errc GetCode() const { return mCode; }
private:
    std::errc mCode;
    int mValue;
};

/// call a system function and return a errc_or<ReturnT> (usually int)
template <typename Fn, typename... Args>
[[nodiscard]] inline SysReturn SysCall(Fn&& func, Args&&... args) noexcept {
    const int result = static_cast<int>(func(std::forward<Args>(args)...));
    if (result != -1) return result;
    return std::errc(errno);
}

/// wrap a blocking syscall and return nullopt if it would block
template <typename Fn, typename... Args>
[[nodiscard]] inline std::optional<SysReturn> SysCallBlocking(Fn&& func, Args&&... args) noexcept {
    const int result = static_cast<int>(func(std::forward<Args>(args)...));
    if (result != -1) return std::optional<SysReturn>(result);
    const auto code = static_cast<std::errc>(errno);
    if (code == std::errc::operation_would_block || code == std::errc::resource_unavailable_try_again) {
        return std::nullopt;
    }
    return std::optional<SysReturn>(code);
}

namespace event {

/// bitmask for which events to return
using Mask = short;
inline constexpr Mask Readable = POLLIN; /// enable Readable events
inline constexpr Mask Priority = POLLPRI; /// enable Priority events
inline constexpr Mask Writable = POLLOUT; /// enable Writable events

class Result {
public:
    explicit Result(short events) : v(events) {}
    bool IsReadable() const { return (v & POLLIN) != 0; } /// without blocking, connector can call read or acceptor can call accept
    bool IsPriority() const { return (v & POLLPRI) != 0; } /// some exceptional condition, for tcp this is OOB data
    bool IsWritable() const { return (v & POLLOUT) != 0; } /// can call write without blocking
    bool IsErrored() const { return (v & POLLERR) != 0; } /// error to be checked with Socket::GetError(), or write pipe's target read pipe was closed
    bool IsClosed() const { return (v & POLLHUP) != 0; } /// socket closed, however for connector, subsequent reads must be called until returns 0
    bool IsInvalid() const { return (v & POLLNVAL) != 0; } /// not an open descriptor and shouldn't be polled
private:
    short v;
};
/// poll an acceptor and its connections
class Poller {
public:
    void Poll(int timeoutMs) {
        SysCall(::poll, mPollList.data(), mPollList.size(), timeoutMs).Unwrap();
    }
    void AddConnector(Descriptor descriptor) {
        mPollList.push_back({descriptor, Readable | Writable, 0});
    }
    void AddAcceptor(Descriptor descriptor) {
        mPollList.push_back({descriptor, Readable, 0});
    }
    Result At(int idx) const {
        return Result(mPollList.at(idx).revents);
    }
    void Remove(int idx) {
        mPollList.erase(mPollList.begin() + idx);
    }
    void Clear() { mPollList.clear(); }
    int GetSize() const { return static_cast<int>(mPollList.size()); }
private:
    std::vector<pollfd_t> mPollList{};
};

}

/// owned socket file descriptor
class Socket {
    static constexpr Descriptor sInvalidSocket = -1;
public:
    /// open a new socket
    Socket(int domain, int type, int protocol) : mDescriptor(SysCall(::socket, domain, type, protocol).Unwrap()) {
        SetNonBlocking();
    }
    /// using file descriptor returned from system call
    explicit Socket(Descriptor descriptor) : mDescriptor(descriptor) {
        if (descriptor == sInvalidSocket) throw std::invalid_argument("invalid socket descriptor");
        SetNonBlocking();
    }
    ~Socket() {
        // owns resource and must close it, descriptor will be set invalid if moved from
        // discard any errors thrown by close, most mean it never owned it or didn't exist
        if (mDescriptor != sInvalidSocket) (void)SysCall(::close, mDescriptor);
    }
    // manage descriptor like never null unique_ptr
    Socket(Socket&& other) noexcept : mDescriptor(other.mDescriptor) {
        other.mDescriptor = sInvalidSocket;
    }
    Socket& operator=(Socket&& rhs) noexcept {
        std::swap(mDescriptor, rhs.mDescriptor);
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    /// get underlying file descriptor
    Descriptor GetDescriptor() const { return mDescriptor; }
    /// get an error on the socket, indicated by errored poll event
    std::errc GetError() const {
        return static_cast<std::errc>(GetSockOpt<int>(SOL_SOCKET, SO_ERROR).first);
    }
    void SetBlocking() { mIsNonBlocking = false; SetStatusFlags(GetStatusFlags() & ~(O_NONBLOCK)); }
    bool GetAndResetIsReadable() { const bool temp = mIsReadable; mIsReadable = false; return temp || !mIsNonBlocking; }
    bool GetAndResetIsWritable() { const bool temp = mIsWritable; mIsWritable = false; return temp || !mIsNonBlocking; }
    /// @return false if socket should close
    bool Update(event::Result res) {
        if (res.IsErrored()) {
            throw std::system_error(std::make_error_code(GetError()));
        }
        if (res.IsInvalid() || res.IsClosed()) {
            return false;
        }
        if (res.IsReadable()) {
            mIsReadable = true;
        }
        if (res.IsWritable()) {
            mIsWritable = true;
        }
        return true;
    }

private:
    void SetNonBlocking() { mIsNonBlocking = true; SetStatusFlags(GetStatusFlags() | O_NONBLOCK); }
    int GetStatusFlags() const { return SysCall(::fcntl, mDescriptor, F_GETFL, 0).Unwrap(); }
    void SetStatusFlags(int flags) { SysCall(::fcntl, mDescriptor, F_SETFL, flags).Unwrap(); }

    /// get or set socket option, most are ints, non default length is only for strings
    template <typename T>
    std::pair<T, socklen_t> GetSockOpt(int level, int optname, T inputValue = T(), socklen_t inputSize = sizeof(T)) const {
        T outValue = inputValue;
        socklen_t outSize = inputSize;
        SysCall(::getsockopt, mDescriptor, level, optname, &outValue, &outSize).Unwrap();
        return std::make_pair(outValue, outSize);
    }
    template <typename T>
    void SetSockOpt(int level, int optname, const T& inputValue, socklen_t inputSize = sizeof(T)) {
        SysCall(::setsockopt, level, optname, &inputValue, inputSize).Unwrap();
    }

    Descriptor mDescriptor;
    bool mIsReadable = false;
    bool mIsWritable = false;
    bool mIsNonBlocking = false;
};

/// address for unix sockets
class LocalAddress {
    static constexpr sa_family_t sFamily = AF_UNIX; // always AF_UNIX
    /// max returned by Size()
    static constexpr socklen_t sMaxSize = sizeof(sockaddr_un_t);
    /// offset of sun_path within the address object, sun_path is a char array
    static constexpr socklen_t sPathOffset = sMaxSize - sizeof(sockaddr_un_t::sun_path);
public:
    /// empty address
    LocalAddress() noexcept : mSize(sMaxSize) {}
    /// almost always bind before use
    explicit LocalAddress(std::string_view path)
        : mSize(sPathOffset + path.size() + 1) { // beginning of object + length of path + null terminator
        if (path.empty()) throw std::invalid_argument("path empty");
        if (mSize > sMaxSize) throw std::length_error("path too long");
        // copy and null terminate path
        std::strncpy(&mAddress.sun_path[0], path.data(), path.size());
        mAddress.sun_path[path.size()] = '\0';
        mAddress.sun_family = sFamily;
    }

    /// deletes the path on the filesystem, usually called before bind
    void Unlink() const {
        // TODO: keep important errors (permissions, logic, ...)
        (void)SysCall(::unlink, GetPath());
    }

    /// system calls with address
    const sockaddr_t* GetPtr() const { return reinterpret_cast<const sockaddr_t*>(&mAddress); }
    /// system calls with address out
    sockaddr_t* GetPtr() { return reinterpret_cast<sockaddr_t*>(&mAddress); }
    /// system calls with addrLen
    socklen_t GetSize() const { return mSize; }
    /// system calls with addrLen out
    socklen_t* GetSizePtr() {
        mSize = sMaxSize;
        return &mSize;
    }
    const char* GetPath() const { return &mAddress.sun_path[0]; }
    bool IsValid() const {
        return mAddress.sun_family == sFamily; // not used with wrong socket type
    }
private:
    socklen_t mSize;
    sockaddr_un_t mAddress{};
};

class LocalSocket : public Socket {
    static constexpr int sDomain = AF_UNIX, // unix domain socket
                         sType = SOCK_SEQPACKET, // message boundaries and connection oriented
                         sProtocol = 0; // auto selected
public:
    explicit LocalSocket(std::string_view path) : Socket(sDomain, sType, sProtocol), mAddress(path) {}
    LocalSocket(Descriptor descriptor, LocalAddress address) : Socket(descriptor), mAddress(address) {
        if (!mAddress.IsValid()) throw std::invalid_argument("invalid local socket address");
    }

protected:
    void UnlinkAddress() { mAddress.Unlink(); }
    void Bind() const { SysCall(::bind, GetDescriptor(), mAddress.GetPtr(), mAddress.GetSize()).Unwrap(); }
    void Listen(int backlog) const { SysCall(::listen, GetDescriptor(), backlog).Unwrap(); }
    void Connect() const { SysCall(::connect, GetDescriptor(), mAddress.GetPtr(), mAddress.GetSize()).Unwrap(); }

private:
    LocalAddress mAddress;
};

class LocalConnectorSocket : public LocalSocket {
public:
    /// open as outbound connector to path
    explicit LocalConnectorSocket(std::string_view path) : LocalSocket(path) {
        Connect();
    }
    /// open as inbound connector from accept
    LocalConnectorSocket(Descriptor descriptor, LocalAddress address) : LocalSocket(descriptor, address) {}
    /// send a byte buffer
    /// @tparam TBufIt iterator to contiguous memory
    /// @return number of bytes sent or nullopt if blocking
    template <typename TBufIt>
    std::optional<int> TrySend(TBufIt bufBegin, int bytesToSend) {
        if (!GetAndResetIsWritable()) return std::nullopt;
        constexpr int flags = 0;
        if (auto bytesSent = SysCallBlocking(::send, GetDescriptor(), &(*bufBegin), bytesToSend, flags)) {
            return (*bytesSent).Unwrap();
        }
        return std::nullopt;
    }
    /// receive a byte buffer
    /// @tparam TBufIt iterator to contiguous memory
    /// @return number of bytes written to buffer or nullopt if blocking
    template <typename TBufIt>
    std::optional<int> TryRecv(TBufIt bufBegin, int bufSize) {
        if (!GetAndResetIsReadable()) return std::nullopt;
        constexpr int flags = 0;
        if (auto bytesRecv = SysCallBlocking(::recv, GetDescriptor(), &(*bufBegin), bufSize, flags)) {
            return (*bytesRecv).Unwrap();
        }
        return std::nullopt;
    }
};

class LocalAcceptorSocket : public LocalSocket {
public:
    /// open as acceptor on path, backlog is accept queue size
    LocalAcceptorSocket(std::string_view path, int backlog) : LocalSocket(path) {
        UnlinkAddress();
        Bind();
        Listen(backlog);
    }
    /// accept an inbound connector or nullopt if blocking
    std::optional<LocalConnectorSocket> Accept() {
        if (!GetAndResetIsReadable()) return std::nullopt;
        LocalAddress address;
        if (auto desc = SysCallBlocking(::accept, GetDescriptor(), address.GetPtr(), address.GetSizePtr())) {
            return LocalConnectorSocket((*desc).Unwrap(), address);
        }
        return std::nullopt;
    }
};
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

namespace {

constexpr int kDefaultPort = 8080;
constexpr int kBacklog = 16;

volatile std::sig_atomic_t was_sighup = 0;
volatile std::sig_atomic_t was_stop = 0;

void signal_handler(int signal_number)
{
    if (signal_number == SIGHUP)
        was_sighup = 1;
    else
        was_stop = 1;
}

class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    FileDescriptor(FileDescriptor &&other) noexcept : fd_(other.release()) {}
    FileDescriptor &operator=(FileDescriptor &&other) noexcept
    {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ != -1; }

    int release()
    {
        int old = fd_;
        fd_ = -1;
        return old;
    }

    void reset(int fd = -1)
    {
        if (fd_ != -1)
            ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

void print_error(const char *what)
{
    const int saved_errno = errno;
    std::cerr << what << ": " << std::strerror(saved_errno) << std::endl;
}

}

int main(int argc, char *argv[])
{
    int port = kDefaultPort;
    if (argc > 1) {
        char *end = nullptr;
        const long value = std::strtol(argv[1], &end, 10);
        if (*end != '\0' || value < 1 || value > 65535) {
            std::cerr << "Usage: " << argv[0] << " [port]" << std::endl;
            return EXIT_FAILURE;
        }
        port = static_cast<int>(value);
    }

    FileDescriptor server(::socket(AF_INET, SOCK_STREAM, 0));
    if (!server.valid()) {
        print_error("socket");
        return EXIT_FAILURE;
    }

    const int enable = 1;
    if (::setsockopt(server.get(), SOL_SOCKET, SO_REUSEADDR,
                     &enable, sizeof(enable)) == -1) {
        print_error("setsockopt");
        return EXIT_FAILURE;
    }

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(server.get(), reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) == -1) {
        print_error("bind");
        return EXIT_FAILURE;
    }

    if (::listen(server.get(), kBacklog) == -1) {
        print_error("listen");
        return EXIT_FAILURE;
    }

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGHUP, &action, nullptr) == -1 ||
        sigaction(SIGINT, &action, nullptr) == -1 ||
        sigaction(SIGTERM, &action, nullptr) == -1) {
        print_error("sigaction");
        return EXIT_FAILURE;
    }

    sigset_t blocked_mask;
    sigset_t original_mask;
    sigemptyset(&blocked_mask);
    sigaddset(&blocked_mask, SIGHUP);
    sigaddset(&blocked_mask, SIGINT);
    sigaddset(&blocked_mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &blocked_mask, &original_mask) == -1) {
        print_error("sigprocmask");
        return EXIT_FAILURE;
    }

    std::cout << "Listening on port " << port
              << " (PID " << ::getpid() << ")" << std::endl;

    FileDescriptor client;

    for (;;) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server.get(), &read_fds);
        int max_fd = server.get();
        if (client.valid()) {
            FD_SET(client.get(), &read_fds);
            if (client.get() > max_fd)
                max_fd = client.get();
        }

        const int ready = ::pselect(max_fd + 1, &read_fds, nullptr, nullptr,
                                    nullptr, &original_mask);

        if (ready == -1 && errno != EINTR) {
            print_error("pselect");
            break;
        }

        if (was_stop)
            break;

        if (was_sighup) {
            was_sighup = 0;
            std::cout << "Received SIGHUP" << std::endl;
        }

        if (ready == -1)
            continue;

        if (FD_ISSET(server.get(), &read_fds)) {
            sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            FileDescriptor accepted(::accept(
                server.get(), reinterpret_cast<sockaddr *>(&peer), &peer_len));

            if (!accepted.valid()) {
                if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED)
                    print_error("accept");
            } else {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                std::cout << "New connection from " << ip << ":"
                          << ntohs(peer.sin_port)
                          << " (fd " << accepted.get() << ")" << std::endl;

                if (client.valid()) {
                    std::cout << "Connection fd " << accepted.get()
                              << " rejected (slot is busy)" << std::endl;
                } else {
                    client = std::move(accepted);
                }
            }
        }

        if (client.valid() && FD_ISSET(client.get(), &read_fds)) {
            std::array<char, 4096> buffer;
            const ssize_t received =
                ::recv(client.get(), buffer.data(), buffer.size(), 0);
            if (received > 0) {
                std::cout << "Received " << received << " bytes" << std::endl;
            } else {
                if (received == 0)
                    std::cout << "Client (fd " << client.get()
                              << ") closed the connection" << std::endl;
                else
                    print_error("recv");
                client.reset();
            }
        }
    }

    std::cout << "Shutting down" << std::endl;
    sigprocmask(SIG_SETMASK, &original_mask, nullptr);
    return EXIT_SUCCESS;
}

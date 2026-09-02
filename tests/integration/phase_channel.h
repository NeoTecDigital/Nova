// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <poll.h>
#include <unistd.h>

/**
 * A one-byte token channel between the harness's server half and its forked
 * client half.
 *
 * Phases are sequenced by tokens, never by sleeps. A sleep-paced protocol test
 * is a race that passes on the developer's machine: the compositor's work is
 * event-driven, so the only correct barrier is "the other side said it got
 * there". Both halves must keep servicing their own event source while waiting,
 * so the wait is a poll over two descriptors rather than a blocking read - a
 * server that blocks on the pipe stops dispatching Wayland and the client it is
 * waiting for deadlocks against it.
 */
namespace VazioTest {

class PhaseChannel {
public:
    PhaseChannel() = default;

    PhaseChannel(int read_fd, int write_fd) : read_fd_(read_fd), write_fd_(write_fd) {}

    PhaseChannel(const PhaseChannel&) = delete;
    PhaseChannel& operator=(const PhaseChannel&) = delete;

    // Movable, not copyable: two objects owning one fd pair is a double close.
    PhaseChannel(PhaseChannel&& other) noexcept
        : read_fd_(other.read_fd_), write_fd_(other.write_fd_) {
        other.read_fd_ = -1;
        other.write_fd_ = -1;
    }

    PhaseChannel& operator=(PhaseChannel&& other) noexcept {
        if (this != &other) {
            close();
            read_fd_ = other.read_fd_;
            write_fd_ = other.write_fd_;
            other.read_fd_ = -1;
            other.write_fd_ = -1;
        }
        return *this;
    }

    ~PhaseChannel() { close(); }

    void close() {
        if (read_fd_ >= 0) { ::close(read_fd_); read_fd_ = -1; }
        if (write_fd_ >= 0) { ::close(write_fd_); write_fd_ = -1; }
    }

    int readFd() const { return read_fd_; }

    bool send(char token) {
        if (write_fd_ < 0) return false;
        ssize_t written = 0;
        do {
            written = ::write(write_fd_, &token, 1);
        } while (written < 0 && errno == EINTR);
        return written == 1;
    }

    /**
     * Wait for one token while `pump` keeps the caller's own event source alive.
     *
     * @param pump Called every poll wakeup and every timeout slice. The server
     *        half dispatches Wayland here; the client half flushes and reads.
     * @return the token, or '\0' on timeout / peer death.
     */
    template <typename Pump>
    char await(Pump&& pump, int timeout_ms = 15000) {
        const int64_t deadline = nowMs() + timeout_ms;
        while (nowMs() < deadline) {
            pump();

            struct pollfd pfd = { read_fd_, POLLIN | POLLHUP, 0 };
            const int slice = 20;
            int ready = ::poll(&pfd, 1, slice);
            if (ready < 0) {
                if (errno == EINTR) continue;
                return '\0';
            }
            if (ready == 0) continue;

            char token = '\0';
            ssize_t got = ::read(read_fd_, &token, 1);
            if (got == 1) return token;
            if (got == 0) return '\0';   // peer closed: treat as timeout, caller reports
            if (got < 0 && errno == EINTR) continue;
            return '\0';
        }
        return '\0';
    }

    static int64_t nowMs() {
        struct timespec ts = {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    }

private:
    int read_fd_ = -1;
    int write_fd_ = -1;
};

/**
 * The two half-duplex pipes a fork needs, created before the fork and narrowed
 * to one direction in each process.
 */
struct PhasePipes {
    int server_to_client[2] = { -1, -1 };
    int client_to_server[2] = { -1, -1 };

    bool create() {
        return ::pipe(server_to_client) == 0 && ::pipe(client_to_server) == 0;
    }

    // Server keeps: read(client_to_server), write(server_to_client).
    void adoptServer(PhaseChannel& channel) {
        ::close(server_to_client[0]);
        ::close(client_to_server[1]);
        channel = PhaseChannel(client_to_server[0], server_to_client[1]);
    }

    // Client keeps the mirror image.
    void adoptClient(PhaseChannel& channel) {
        ::close(server_to_client[1]);
        ::close(client_to_server[0]);
        channel = PhaseChannel(server_to_client[0], client_to_server[1]);
    }
};

}  // namespace VazioTest

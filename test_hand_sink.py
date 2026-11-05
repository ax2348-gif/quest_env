#!/usr/bin/env python3
"""Compile the actual hand TCP sink on the host and test it never blocks XR."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root / 'native/telemetry.cpp').read_text()
sink = source[source.index('class TcpHandSink'):source.index('class TcpControllerServer')]
headers = '''
#include <string>
#include <cerrno>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
int64_t monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
constexpr const char* kHandHost = "127.0.0.1";
int kHandPort = 0;
constexpr int64_t kReconnectDelayNs = 20LL * 1000LL * 1000LL;
'''
# Use an OS-assigned test port and inspect the fd, without changing production code.
sink = sink.replace('private:', 'public:')
harness = '''
int listener() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0 && listen(fd, 4) == 0);
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr*)&addr, &len);
    kHandPort = ntohs(addr.sin_port);
    return fd;
}
// Each call stands in for one XR frame and must return promptly.
void frame(TcpHandSink& sink, const std::string& line) {
    const int64_t start = monotonicNs();
    sink.sendLine(line);
    assert(monotonicNs() - start < 50LL * 1000LL * 1000LL);
}
void connectSink(TcpHandSink& sink) {
    for (int i = 0; i < 200 && (sink.fd_ < 0 || sink.connecting_); ++i) {
        frame(sink, "probe");
        usleep(1000);
    }
    assert(sink.fd_ >= 0 && !sink.connecting_);
}
int main() {
    // No receiver: attempts fail without blocking or crashing.
    { int fd = listener(); close(fd); }
    TcpHandSink absent;
    for (int i = 0; i < 50; ++i) frame(absent, "nobody");

    // Normal receiver gets whole lines.
    int server = listener();
    TcpHandSink sink;
    connectSink(sink);
    int reader = accept(server, nullptr, nullptr);
    timeval timeout{2, 0};
    setsockopt(reader, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char buffer[8];
    while (recv(reader, buffer, 1, MSG_DONTWAIT) > 0) {}  // drain probes
    frame(sink, "hello");
    assert(recv(reader, buffer, 6, MSG_WAITALL) == 6 && memcmp(buffer, "hello\\n", 6) == 0);

    // Stalled receiver: frames stay fast and the sink drops the connection.
    int small = 4096;
    setsockopt(sink.fd_, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    std::string payload(4000, 'x');
    for (int i = 0; i < 2000 && sink.fd_ >= 0; ++i) frame(sink, payload);
    assert(sink.fd_ == -1);

    // It reconnects after the retry delay.
    usleep(30000);
    connectSink(sink);
    close(reader); close(server);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'hand_sink.cpp'
    exe = Path(tmp) / 'hand_sink'
    src.write_text(headers + sink + harness)
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
print('PASS: absent receiver, whole lines, stalled-reader drop without blocking, reconnect')

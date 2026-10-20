#!/usr/bin/env python3
"""Compile the actual TCP fanout class on the host and test reader isolation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root / 'native/telemetry.cpp').read_text()
server = source[source.index('class TcpControllerServer'):source.index('\nstd::string fixed')]
headers = '''
#include <array>
#include <string>
#include <cerrno>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
constexpr int kControllerPort = 0;
'''
# Use an OS-assigned test port and inspect bound fd, without changing production code.
server = server.replace('private:', 'public:')
harness = '''
int main() {
    TcpControllerServer server;
    assert(server.start());
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    assert(getsockname(server.listenFd_, (sockaddr*)&addr, &len) == 0);
    auto connectReader = [&]() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        timeval timeout{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        assert(connect(fd, (sockaddr*)&addr, len) == 0);
        return fd;
    };
    int slow = connectReader(), fast = connectReader();
    server.sendLine("hello");
    char buffer[4096];
    assert(recv(slow, buffer, 6, MSG_WAITALL) == 6);
    assert(recv(fast, buffer, 6, MSG_WAITALL) == 6);
    // Shrink the slow reader's server-side buffer to force backpressure quickly.
    int small = 4096;
    setsockopt(server.clients_[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    std::string payload(1023, 'x');
    for (int i = 0; i < 2000; ++i) {
        server.sendLine(payload);
        assert(recv(fast, buffer, 1024, MSG_WAITALL) == 1024);
        assert(buffer[1023] == '\\n');
    }
    assert(server.clients_[0] == -1);
    assert(server.connected());
    int replacement = connectReader();
    server.sendLine("again");
    assert(recv(replacement, buffer, 6, MSG_WAITALL) == 6);
    assert(recv(fast, buffer, 6, MSG_WAITALL) == 6);
    server.shutdown();
    assert(!server.connected());
    close(slow); close(fast); close(replacement);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'fanout.cpp'
    exe = Path(tmp) / 'fanout'
    src.write_text(headers + server + harness)
    subprocess.run(['c++', '-std=c++17', '-O2', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
print('PASS: concurrent readers, slow-reader isolation, replacement reader, shutdown')

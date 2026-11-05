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
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <cstdio>
#include <cstdint>
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
    assert(addr.sin_addr.s_addr == htonl(INADDR_ANY));
    const sockaddr_in any = addr;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

    // Optional tailnet-only check: by local (arrival) address, not peer address.
    assert(TcpControllerServer::allowedLocalAddress(0x7F000001u));     // 127.0.0.1
    assert(TcpControllerServer::allowedLocalAddress(0x64487753u));     // 100.72.77.83
    assert(TcpControllerServer::allowedLocalAddress(0x647FFFFFu));     // 100.127.255.255
    assert(!TcpControllerServer::allowedLocalAddress(0x643FFFFFu));    // 100.63.255.255
    assert(!TcpControllerServer::allowedLocalAddress(0x64800000u));    // 100.128.0.0
    assert(!TcpControllerServer::allowedLocalAddress(0xC0A80102u));    // 192.168.1.2
    // A non-loopback, non-Tailscale address on this host stands in for Wi-Fi.
    in_addr lan{};
    ifaddrs* list = nullptr;
    assert(getifaddrs(&list) == 0);
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        const uint32_t host = ntohl(((sockaddr_in*)i->ifa_addr)->sin_addr.s_addr);
        if (!TcpControllerServer::allowedLocalAddress(host)) {
            lan = ((sockaddr_in*)i->ifa_addr)->sin_addr;
            break;
        }
    }
    freeifaddrs(list);
    auto connectTo = [&](in_addr ip) {
        sockaddr_in target = addr;
        target.sin_addr = ip;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        timeval timeout{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        assert(connect(fd, (sockaddr*)&target, sizeof(target)) == 0);
        return fd;
    };
    for (bool tailnetOnly : {false, true}) {
        TcpControllerServer filtered;
        filtered.tailnetOnly = tailnetOnly;
        assert(filtered.start());
        len = sizeof(addr);
        getsockname(filtered.listenFd_, (sockaddr*)&addr, &len);
        assert(addr.sin_addr.s_addr == any.sin_addr.s_addr);
        int local = connectTo(in_addr{htonl(INADDR_LOOPBACK)});
        filtered.sendLine("loop");
        assert(recv(local, buffer, 5, MSG_WAITALL) == 5);
        if (lan.s_addr) {
            int remote = connectTo(lan);
            filtered.sendLine("lan");
            // Rejected readers see EOF; accepted ones get the line.
            ssize_t got = recv(remote, buffer, 4, MSG_WAITALL);
            assert(tailnetOnly ? got == 0 : got == 4);
            close(remote);
        }
        filtered.shutdown();
        close(local);
    }
    if (!lan.s_addr) puts("SKIP: no non-loopback IPv4 address for the interface check");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'fanout.cpp'
    exe = Path(tmp) / 'fanout'
    src.write_text(headers + server + harness)
    subprocess.run(['c++', '-std=c++17', '-O2', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
print('PASS: all-interface bind, concurrent readers, slow-reader isolation, replacement reader, shutdown, optional tailnet-only check')

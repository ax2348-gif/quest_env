#pragma once
// Background HTTP client for the robot AR view. One thread reads the
// robot_state.v1 line stream and (re)fetches the description when it connects
// and when model_version changes; an optional second thread reads a separate
// objects stream. The XR frame loop only takes a mutex-guarded snapshot of
// shared pointers: it never waits on the network.
// Plain POSIX sockets (Android and host); http:// only.
#include "robot_ar.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <errno.h>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
namespace robot_ar {
inline int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Url {
    std::string host, port = "80", path = "/";
};
inline bool parseUrl(const std::string &url, Url &out) {
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0)
        return false;
    std::string rest = url.substr(scheme.size());
    size_t slash = rest.find_first_of("/?");
    std::string authority = rest.substr(0, slash);
    Url u;
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (u.path[0] == '?')
        u.path = "/" + u.path;
    if (authority.find('@') != std::string::npos)
        return false; // No credentials in URLs.
    if (!authority.empty() && authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos)
            return false;
        u.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':')
                return false;
            u.port = authority.substr(close + 2);
        }
    } else {
        size_t colon = authority.rfind(':');
        u.host = authority.substr(0, colon);
        if (colon != std::string::npos)
            u.port = authority.substr(colon + 1);
    }
    if (u.host.empty() || u.port.empty() || u.port.find_first_not_of("0123456789") != std::string::npos)
        return false;
    for (char c : u.path)
        if (c == ' ' || c == '\r' || c == '\n')
            return false;
    out = u;
    return true;
}
struct ResponseHead {
    int status = 0;
    bool chunked = false;
    long long length = -1;
};
inline bool parseHead(const std::string &head, ResponseHead &out) {
    ResponseHead h;
    size_t eol = head.find("\r\n");
    std::string first = head.substr(0, eol);
    if (first.compare(0, 5, "HTTP/") != 0)
        return false;
    size_t sp = first.find(' ');
    if (sp == std::string::npos)
        return false;
    h.status = std::atoi(first.c_str() + sp + 1);
    size_t pos = eol == std::string::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        if (end == std::string::npos)
            end = head.size();
        std::string line = head.substr(pos, end - pos);
        pos = end + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string k = lower(trim(line.substr(0, colon))), v = lower(trim(line.substr(colon + 1)));
        if (k == "transfer-encoding" && v.find("chunked") != std::string::npos)
            h.chunked = true;
        else if (k == "content-length")
            h.length = std::atoll(v.c_str());
    }
    out = h;
    return true;
}
// Incremental body decoding: identity (length-delimited or until close) or chunked.
struct BodyDecoder {
    bool chunked = false;
    long long remaining = -1; // Identity with Content-Length; -1 = until close.
    enum Phase { Size, Data, DataEnd, Trailer, Done } phase = Size;
    std::string line;
    void reset(const ResponseHead &h) {
        chunked = h.chunked;
        remaining = h.chunked ? 0 : h.length;
        phase = h.chunked ? Size : (h.length == 0 ? Done : Data);
        line.clear();
    }
    bool done() const { return phase == Done; }
    // Appends decoded bytes to out. False on a malformed chunk framing.
    bool feed(const char *p, size_t n, std::string &out) {
        size_t i = 0;
        while (i < n && phase != Done) {
            if (!chunked) {
                size_t take = remaining < 0 ? n - i : std::min<size_t>(n - i, size_t(remaining));
                out.append(p + i, take);
                i += take;
                if (remaining >= 0 && (remaining -= take) == 0)
                    phase = Done;
                continue;
            }
            if (phase == Data) {
                size_t take = std::min<size_t>(n - i, size_t(remaining));
                out.append(p + i, take);
                i += take;
                if ((remaining -= take) == 0) {
                    phase = DataEnd;
                    line.clear();
                }
                continue;
            }
            // Size, DataEnd and Trailer read CRLF-terminated lines.
            char c = p[i++];
            if (c != '\n') {
                if (line.size() > 1024)
                    return false;
                line += c;
                continue;
            }
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (phase == DataEnd) {
                if (!line.empty())
                    return false;
                phase = Size;
            } else if (phase == Size) {
                char *end = nullptr;
                long long size = std::strtoll(line.c_str(), &end, 16);
                if (end == line.c_str() || size < 0)
                    return false;
                remaining = size;
                phase = size == 0 ? Trailer : Data;
            } else if (phase == Trailer && line.empty())
                phase = Done;
            line.clear();
        }
        return true;
    }
};
// Splits a byte stream into lines (LF or CRLF). Accepts SSE "data:" framing too.
struct LineSplitter {
    std::string buffer;
    static constexpr size_t kMaxLine = 8u << 20;
    template <class F> bool feed(const std::string &bytes, F &&onLine) {
        buffer += bytes;
        size_t start = 0, nl;
        while ((nl = buffer.find('\n', start)) != std::string::npos) {
            std::string line = buffer.substr(start, nl - start);
            start = nl + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.compare(0, 5, "data:") == 0)
                line = line.substr(5);
            line = trim(line);
            if (!line.empty() && line[0] != ':')
                onLine(line);
        }
        buffer.erase(0, start);
        return buffer.size() <= kMaxLine;
    }
};

class Connection {
    int fd = -1;

  public:
    std::string error;
    ~Connection() { close(); }
    void close() {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
    }
    // Connect and send a GET. Polls in short slices so `running` can abort it.
    bool open(const Url &u, const std::atomic<bool> &running, int timeoutMs = 2000) {
        close();
        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res) {
            error = "DNS FAILED";
            return false;
        }
        bool ok = false;
        for (addrinfo *a = res; a && !ok && running; a = a->ai_next) {
            fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0)
                continue;
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            int r = connect(fd, a->ai_addr, a->ai_addrlen);
            if (r == 0)
                ok = true;
            else if (errno == EINPROGRESS) {
                for (int waited = 0; waited < timeoutMs && running && !ok; waited += 100) {
                    pollfd p{fd, POLLOUT, 0};
                    int n = poll(&p, 1, 100);
                    if (n > 0) {
                        int err = 0;
                        socklen_t len = sizeof(err);
                        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                        if (err) {
                            error = err == ECONNREFUSED ? "CONNECTION REFUSED" : "CONNECT FAILED";
                            break;
                        }
                        ok = true;
                    }
                }
                if (!ok && error.empty())
                    error = "CONNECT TIMEOUT";
            } else
                error = errno == ECONNREFUSED ? "CONNECTION REFUSED" : "CONNECT FAILED";
            if (!ok)
                close();
        }
        freeaddrinfo(res);
        if (!ok)
            return false;
        std::string host = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
        std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + host + ":" + u.port +
                          "\r\nAccept: application/json, application/x-ndjson, text/plain\r\n"
                          "User-Agent: quest-robot-ar/1\r\nConnection: close\r\n\r\n";
        size_t sent = 0;
        for (int waited = 0; sent < req.size() && running && waited < timeoutMs;) {
#ifdef MSG_NOSIGNAL
            ssize_t n = send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
#else
            ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
#endif
            if (n > 0)
                sent += size_t(n);
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd p{fd, POLLOUT, 0};
                poll(&p, 1, 100);
                waited += 100;
            } else
                break;
        }
        if (sent != req.size()) {
            error = "SEND FAILED";
            close();
            return false;
        }
        return true;
    }
    // >0 bytes, 0 = closed by peer, -1 = error, -2 = nothing within waitMs.
    ssize_t read(char *buf, size_t cap, int waitMs) {
        if (fd < 0)
            return -1;
        pollfd p{fd, POLLIN, 0};
        int n = poll(&p, 1, waitMs);
        if (n == 0)
            return -2;
        if (n < 0)
            return errno == EINTR ? -2 : -1;
        ssize_t r = recv(fd, buf, cap, 0);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return -2;
        return r;
    }
};

// GET a whole (small) document. Gives up after timeoutMs without progress.
inline bool httpGet(const std::string &url, const std::atomic<bool> &running, std::string &body,
                    std::string &error, int timeoutMs = 3000) {
    Url u;
    if (!parseUrl(url, u)) {
        error = "BAD URL";
        return false;
    }
    Connection c;
    if (!c.open(u, running, timeoutMs)) {
        error = c.error;
        return false;
    }
    std::string head;
    ResponseHead h;
    BodyDecoder d;
    bool inBody = false;
    char buf[16384];
    int idle = 0;
    body.clear();
    while (running) {
        ssize_t n = c.read(buf, sizeof(buf), 100);
        if (n == -2) {
            if ((idle += 100) >= timeoutMs) {
                error = "READ TIMEOUT";
                return false;
            }
            continue;
        }
        idle = 0;
        if (n <= 0)
            break;
        const char *p = buf;
        size_t len = size_t(n);
        if (!inBody) {
            head.append(p, len);
            size_t end = head.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (head.size() > 65536) {
                    error = "BAD RESPONSE";
                    return false;
                }
                continue;
            }
            std::string rest = head.substr(end + 4);
            head.resize(end);
            if (!parseHead(head, h)) {
                error = "BAD RESPONSE";
                return false;
            }
            if (h.status != 200) {
                error = "HTTP " + std::to_string(h.status);
                return false;
            }
            d.reset(h);
            inBody = true;
            if (!d.feed(rest.data(), rest.size(), body)) {
                error = "BAD CHUNKING";
                return false;
            }
        } else if (!d.feed(p, len, body)) {
            error = "BAD CHUNKING";
            return false;
        }
        if (body.size() > (32u << 20)) {
            error = "RESPONSE TOO LARGE";
            return false;
        }
        if (inBody && d.done())
            return true;
    }
    if (!inBody) {
        error = running ? "NO RESPONSE" : "STOPPED";
        return false;
    }
    // Close-delimited bodies end at EOF; framed ones must have completed.
    if (!d.chunked && d.remaining < 0)
        return true;
    error = "TRUNCATED";
    return false;
}

struct Snapshot {
    std::shared_ptr<const Model> model;
    std::shared_ptr<const State> state;
    std::shared_ptr<const std::vector<Object>> objects; // From a separate objects stream.
    int64_t lastStateNs = 0;                           // Monotonic receipt time of the newest state.
    double fps = 0;                                    // State lines per second (receipt).
    bool connected = false;
    std::string error;
    uint64_t states = 0, modelFetches = 0;
    std::string modelNote, modelUrl; // Set when the description fell back to variant=simple.
};
// variant=boxes -> variant=simple in place; false when the URL asks for no boxes.
inline bool boxesFallback(std::string &url) {
    const std::string from = "variant=boxes";
    size_t at = url.find(from);
    if (at == std::string::npos)
        return false;
    url.replace(at, from.size(), "variant=simple");
    return true;
}

class ClientCore {
    Config config_;
    std::atomic<bool> running_{false};
    std::thread state_, objects_;
    mutable std::mutex mutex_;
    Snapshot snap_;
    int64_t lastModelAttempt_ = 0, connectedNs_ = 0;
    std::deque<int64_t> recent_; // Receipt times within the last second (state thread only).
    static constexpr int kStallMs = 2000;          // No bytes this long: reconnect.
    static constexpr int64_t kModelRetryNs = 2000000000LL;

    void sleepMs(int ms) {
        for (int t = 0; t < ms && running_; t += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    void setError(const std::string &e) {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.error = e;
    }
    // Fetch the description if missing, forced (new connection) or out of date.
    void refreshModel(const std::string &wanted, bool force) {
        if (config_.modelUrl.empty())
            return;
        std::shared_ptr<const Model> current;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current = snap_.model;
        }
        bool stale = !current || (!wanted.empty() && wanted != current->version);
        int64_t now = nowNs();
        if (!force && (!stale || now - lastModelAttempt_ < kModelRetryNs))
            return;
        lastModelAttempt_ = now;
        std::string body, error;
        bool ok = httpGet(config_.modelUrl, running_, body, error);
        // A description node without the boxes variant rejects it (4xx): use simple, once.
        if (!ok && error.compare(0, 6, "HTTP 4") == 0 && boxesFallback(config_.modelUrl)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.modelNote = "SIMPLE MODEL - NO BOXES";
                snap_.modelUrl = config_.modelUrl;
            }
            ok = httpGet(config_.modelUrl, running_, body, error);
        }
        if (!ok) {
            setError("MODEL " + error);
            return;
        }
        Json j;
        auto m = std::make_shared<Model>();
        std::string why;
        if (!parseJson(body, j) || !parseModel(j, *m, &why)) {
            setError("MODEL PARSE");
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.model = m;
        snap_.modelFetches++;
    }
    // One streaming connection: returns when it ends, stalls or errors.
    template <class F> void streamOnce(const std::string &url, F &&onLine) {
        Url u;
        if (!parseUrl(url, u)) {
            setError("BAD URL");
            return;
        }
        Connection c;
        if (!c.open(u, running_)) {
            setError(c.error);
            return;
        }
        std::string head, decoded;
        ResponseHead h;
        BodyDecoder d;
        LineSplitter lines;
        bool inBody = false;
        char buf[16384];
        int idle = 0;
        while (running_) {
            ssize_t n = c.read(buf, sizeof(buf), 100);
            if (n == -2) {
                if ((idle += 100) >= kStallMs) {
                    setError("STREAM STALLED");
                    return;
                }
                continue;
            }
            idle = 0;
            if (n <= 0) {
                setError(n == 0 ? "STREAM CLOSED" : "STREAM ERROR");
                return;
            }
            decoded.clear();
            if (!inBody) {
                head.append(buf, size_t(n));
                size_t end = head.find("\r\n\r\n");
                if (end == std::string::npos) {
                    if (head.size() > 65536)
                        return setError("BAD RESPONSE");
                    continue;
                }
                std::string rest = head.substr(end + 4);
                head.resize(end);
                if (!parseHead(head, h))
                    return setError("BAD RESPONSE");
                if (h.status != 200)
                    return setError("STATE HTTP " + std::to_string(h.status));
                d.reset(h);
                inBody = true;
                if (!d.feed(rest.data(), rest.size(), decoded))
                    return setError("BAD CHUNKING");
            } else if (!d.feed(buf, size_t(n), decoded))
                return setError("BAD CHUNKING");
            if (!lines.feed(decoded, onLine))
                return setError("LINE TOO LONG");
            if (d.done())
                return setError("STREAM ENDED");
        }
    }
    void runState() {
        int backoff = 250;
        while (running_) {
            refreshModel("", true); // Fetch at every (re)connect.
            bool got = false;
            streamOnce(config_.stateUrl, [&](const std::string &line) {
                Json j;
                auto st = std::make_shared<State>();
                if (!parseJson(line, j) || !parseState(j, *st))
                    return;
                refreshModel(st->modelVersion, false);
                got = true;
                int64_t now = nowNs();
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.state = st;
                snap_.lastStateNs = now;
                snap_.states++;
                snap_.connected = true;
                if (snap_.error.compare(0, 5, "MODEL") != 0)
                    snap_.error.clear();
                // Receipt rate over the last second (or since connecting, if shorter).
                if (!connectedNs_)
                    connectedNs_ = now;
                recent_.push_back(now);
                while (!recent_.empty() && now - recent_.front() > 1000000000LL)
                    recent_.pop_front();
                double span = std::min(1e9, std::max(2.5e8, double(now - connectedNs_)));
                snap_.fps = recent_.size() * 1e9 / span;
            });
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.connected = false;
                snap_.fps = 0;
                recent_.clear();
                connectedNs_ = 0;
            }
            backoff = got ? 250 : std::min(2000, backoff * 2);
            sleepMs(backoff);
        }
    }
    void runObjects() {
        int backoff = 250;
        while (running_) {
            streamOnce(config_.objectsUrl, [&](const std::string &line) {
                Json j;
                auto objects = std::make_shared<std::vector<Object>>();
                if (!parseJson(line, j))
                    return;
                const Json *o = j.get("objects");
                if (!parseObjects(o ? o : &j, *objects))
                    return;
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.objects = objects;
            });
            sleepMs(backoff);
            backoff = std::min(2000, backoff * 2);
        }
    }

    friend class Client;
    std::atomic<int> active_{0};

  public:
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snap_;
    }
};

// Owns the threads. The shared core outlives them, so stop() can give up
// waiting (a DNS lookup cannot be interrupted) and detach without risk.
class Client {
    std::shared_ptr<ClientCore> core_;
    std::thread state_, objects_;

  public:
    ~Client() { stop(); }
    void start(const Config &c) {
        stop();
        if (!c.enabled)
            return;
        core_ = std::make_shared<ClientCore>();
        core_->config_ = c;
        core_->running_ = true;
        auto core = core_;
        core->active_ = c.objectsUrl.empty() ? 1 : 2;
        state_ = std::thread([core] {
            core->runState();
            core->active_--;
        });
        if (!c.objectsUrl.empty())
            objects_ = std::thread([core] {
                core->runObjects();
                core->active_--;
            });
    }
    // Returns within ~timeoutMs even if a thread is stuck in name resolution.
    void stop(int timeoutMs = 1000) {
        if (!core_)
            return;
        core_->running_ = false;
        for (int t = 0; t < timeoutMs && core_->active_ > 0; t += 10)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        bool done = core_->active_ == 0;
        for (std::thread *th : {&state_, &objects_})
            if (th->joinable()) {
                if (done)
                    th->join();
                else
                    th->detach();
            }
        core_.reset();
    }
    bool running() const { return core_ && core_->running_; }
    Snapshot snapshot() const { return core_ ? core_->snapshot() : Snapshot{}; }
};

// Receipt age in seconds (infinite before the first state).
inline double ageSeconds(const Snapshot &s, int64_t now = nowNs()) {
    return s.lastStateNs ? (now - s.lastStateNs) / 1e9 : 1e9;
}
inline bool isStale(const Snapshot &s, int64_t now = nowNs()) { return ageSeconds(s, now) > kStaleAfterS; }
// Two short HUD lines.
inline std::vector<std::string> hudLines(const Snapshot &s, int64_t now = nowNs()) {
    std::vector<std::string> out;
    char buf[96];
    double age = ageSeconds(s, now);
    if (!s.state)
        out.push_back(s.error.empty() ? "ROBOT AR CONNECTING" : hudText("ROBOT AR " + s.error));
    else if (age > kStaleAfterS) {
        snprintf(buf, sizeof(buf), "ROBOT AR STALE %.1f S", std::min(age, 999.9));
        out.push_back(buf);
        if (!s.error.empty())
            out.push_back(hudText(s.error));
    } else {
        snprintf(buf, sizeof(buf), "ROBOT AR OK %.0f HZ %.0f MS%s", s.fps, age * 1000, s.model ? "" : " NO MODEL");
        out.push_back(buf);
    }
    if (!s.modelNote.empty())
        out.push_back(s.modelNote);
    if (s.state && s.state->limited)
        out.push_back(hudText("LIMITED " + s.state->reason));
    else if (s.state && !s.state->baseMode.empty())
        out.push_back(hudText("BASE " + s.state->baseMode));
    return out;
}
} // namespace robot_ar

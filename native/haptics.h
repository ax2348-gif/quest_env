#pragma once
// Controller vibration from the teleop node (HAPTICS.md; robot_utils docs/quest_teleop_node.md "Haptics").
// One long GET of the node's `/haptics?stream=1` (NDJSON, schema teleop.quest_haptics.v1: per hand a level and
// pulses with increasing ids) feeds a Player; the XR frame loop ticks it and turns its commands into
// xrApplyHapticFeedback / xrStopHapticFeedback (telemetry.cpp). This header has no OpenXR: host tests cover the
// parsing and the scheduling. robot_utils' fake headset has a port of Player (tools/quest_teleop/fake_headset.py).
#include "robot_ar_client.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace haptics {
using robot_ar::Json;

constexpr const char *kSchema = "teleop.quest_haptics.v1";
constexpr double kMaxAgeS = 0.5;       // a pulse older than this on arrival is skipped (late = meaningless)
constexpr double kMaxTtlS = 1.0;       // a level's ttl_s is capped here (a lost stream stops the buzz)
constexpr double kLevelChunkS = 0.1;   // a level plays in pieces this long ...
constexpr double kLevelRefreshS = 0.05; // ... re-applied this often (so it ends within kLevelChunkS of its TTL)
constexpr double kMaxDurationS = 2.0;  // one pulse at most this long
constexpr int kStallMs = 2000;         // no byte this long: reconnect (the node writes a line at least every 1 s)

struct Pulse {
    long long id = 0, group = 0;
    float amplitude = 0, frequencyHz = 0;
    double durationS = 0, delayS = 0, ageS = 0;
};
struct Hand {
    bool present = false;
    float level = 0, frequencyHz = 0;
    std::vector<Pulse> pulses;
};
struct Message {
    std::string epoch;
    double ttlS = 0;
    Hand hands[2]; // 0 left, 1 right
};
// One vibration command for the frame loop: apply (amplitude, duration, frequency 0 = the runtime's) or stop.
struct Command {
    int hand = 0;
    bool stop = false;
    float amplitude = 0;
    int64_t durationNs = 0;
    float frequencyHz = 0;
    long long id = 0; // the pulse's; 0 for a level
};

inline float clamp01(double v) { return std::isfinite(v) ? float(std::min(1.0, std::max(0.0, v))) : 0.0f; }
inline double fnum(const Json *j, double fallback = 0) { return j && j->isNum() && std::isfinite(j->n) ? j->n : fallback; }
// The node's epoch as sent back in the frames: [A-Za-z0-9._-], at most 32 characters.
inline std::string cleanEpoch(const std::string &s) {
    std::string out;
    for (char c : s)
        if (out.size() < 32 && (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'))
            out += c;
    return out;
}

inline bool parseMessage(const std::string &text, Message &out) {
    Json j;
    if (!robot_ar::parseJson(text, j) || !j.isObj())
        return false;
    const Json *schema = j.get("schema"), *hands = j.get("hands"), *epoch = j.get("epoch");
    if (!schema || !schema->isStr() || schema->s != kSchema || !hands || !hands->isObj())
        return false;
    Message m;
    m.epoch = epoch && epoch->isStr() ? cleanEpoch(epoch->s) : "";
    m.ttlS = std::min(kMaxTtlS, std::max(0.0, fnum(j.get("ttl_s"))));
    const char *names[2] = {"left", "right"};
    for (int h = 0; h < 2; h++) {
        const Json *o = hands->get(names[h]);
        if (!o || !o->isObj())
            continue;
        Hand &hd = m.hands[h];
        hd.present = true;
        hd.level = clamp01(fnum(o->get("level")));
        hd.frequencyHz = float(std::max(0.0, fnum(o->get("frequency_hz"))));
        const Json *ps = o->get("pulses");
        if (!ps || !ps->isArr())
            continue;
        for (const Json &p : ps->a) {
            const Json *id = p.get("id");
            if (!id || !id->isNum() || id->n < 1)
                continue;
            Pulse q;
            q.id = (long long)id->n;
            q.group = (long long)fnum(p.get("group"), id->n);
            q.amplitude = clamp01(fnum(p.get("amplitude")));
            q.durationS = std::min(kMaxDurationS, std::max(0.0, fnum(p.get("duration_s"))));
            q.frequencyHz = float(std::max(0.0, fnum(p.get("frequency_hz"))));
            q.delayS = std::min(kMaxDurationS, std::max(0.0, fnum(p.get("delay_s"))));
            q.ageS = std::max(0.0, fnum(p.get("age_s")));
            hd.pulses.push_back(q);
        }
        std::sort(hd.pulses.begin(), hd.pulses.end(), [](const Pulse &a, const Pulse &b) { return a.id < b.id; });
    }
    out = m;
    return true;
}

// Plays each pulse once (by id, per epoch), delay_s after it first arrives; a newer group on a hand replaces that
// hand's pulses still waiting; a level plays (in kLevelChunkS pieces) until its line's ttl runs out, pulses over it.
// receive() runs on the stream thread, tick() on the frame loop.
class Player {
    struct Pending {
        int64_t startNs;
        Pulse p;
    };
    struct Playing {
        enum Kind { None, PulseOn, Level } kind = None;
        int64_t untilNs = 0; // a pulse's end, a level's next refresh
        float amplitude = 0;
    };
    mutable std::mutex mu_;
    std::string epoch_;
    long long lastId_ = 0;
    bool connected_ = false;
    std::vector<Pending> pending_[2];
    float level_[2] = {0, 0}, levelFreq_[2] = {0, 0};
    int64_t levelUntilNs_[2] = {0, 0};
    Playing playing_[2];
    static int64_t ns(double s) { return std::llround(s * 1e9); }
    void resetHands() {
        for (int h = 0; h < 2; h++) {
            pending_[h].clear();
            level_[h] = 0;
            levelUntilNs_[h] = 0;
        }
    }

  public:
    uint64_t lines = 0, bad = 0, played = 0, skippedLate = 0;

    bool receive(const std::string &line, int64_t nowNs) {
        Message m;
        std::lock_guard<std::mutex> g(mu_);
        if (!parseMessage(line, m)) {
            bad++;
            return false;
        }
        lines++;
        if (m.epoch != epoch_) { // a new node life: its ids start over
            epoch_ = m.epoch;
            lastId_ = 0;
            resetHands();
        }
        long long top = lastId_;
        for (int h = 0; h < 2; h++) {
            const Hand &hd = m.hands[h];
            if (!hd.present)
                continue;
            level_[h] = hd.level;
            levelFreq_[h] = hd.frequencyHz;
            levelUntilNs_[h] = nowNs + ns(m.ttlS);
            for (const Pulse &p : hd.pulses) {
                if (p.id <= lastId_)
                    continue;
                top = std::max(top, p.id);
                if (p.ageS > kMaxAgeS) {
                    skippedLate++;
                    continue;
                }
                auto &q = pending_[h];
                q.erase(std::remove_if(q.begin(), q.end(), [&](const Pending &x) { return x.p.group != p.group; }),
                        q.end());
                q.push_back({nowNs + ns(p.delayS), p});
            }
        }
        lastId_ = top;
        return true;
    }

    std::vector<Command> tick(int64_t nowNs) {
        std::vector<Command> out;
        std::lock_guard<std::mutex> g(mu_);
        for (int h = 0; h < 2; h++) {
            auto &q = pending_[h];
            Playing &cur = playing_[h];
            const Pulse *due = nullptr;
            Pulse last;
            for (const auto &x : q)
                if (x.startNs <= nowNs) {
                    last = x.p; // pending is in arrival (id) order: the newest due one wins
                    due = &last;
                }
            if (due) {
                q.erase(std::remove_if(q.begin(), q.end(), [&](const Pending &x) { return x.startNs <= nowNs; }),
                        q.end());
                out.push_back({h, false, last.amplitude, ns(last.durationS), last.frequencyHz, last.id});
                cur = {Playing::PulseOn, nowNs + ns(last.durationS), last.amplitude};
                played++;
                continue;
            }
            if (cur.kind == Playing::PulseOn && nowNs < cur.untilNs)
                continue;
            if (level_[h] > 0 && nowNs < levelUntilNs_[h]) {
                if (cur.kind != Playing::Level || cur.amplitude != level_[h] || nowNs >= cur.untilNs) {
                    out.push_back({h, false, level_[h], ns(kLevelChunkS), levelFreq_[h], 0});
                    cur = {Playing::Level, nowNs + ns(kLevelRefreshS), level_[h]};
                }
            } else if (cur.kind != Playing::None) {
                if (cur.kind == Playing::Level)
                    out.push_back({h, true, 0, 0, 0, 0});
                cur = Playing{};
            }
        }
        return out;
    }

    // The stream is up / lost. Lost: nothing more plays (a level stops at the next tick; a pulse ends itself).
    void setConnected(bool up) {
        std::lock_guard<std::mutex> g(mu_);
        connected_ = up;
        if (!up)
            resetHands();
    }

    // The `haptics` block of each telemetry record: the node's capability check and ack.
    std::string blockJson() const {
        std::lock_guard<std::mutex> g(mu_);
        std::string s = std::string("{\"schema\":\"") + kSchema + "\",\"epoch\":";
        s += epoch_.empty() ? "null" : "\"" + epoch_ + "\"";
        s += ",\"last_id\":" + std::to_string(lastId_) + ",\"connected\":" + (connected_ ? "true" : "false") + "}";
        return s;
    }
    long long lastId() const {
        std::lock_guard<std::mutex> g(mu_);
        return lastId_;
    }
};

// The stream reader: one thread, reconnecting (250 ms doubling to 2 s; 10 s after a 404: no teleop node there).
class Client {
  public:
    ~Client() { stop(); }
    void start(const std::string &url, Player *player) {
        stop();
        url_ = url;
        player_ = player;
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        running_ = false;
        if (thread_.joinable())
            thread_.join();
    }
    std::string error() const {
        std::lock_guard<std::mutex> g(mu_);
        return error_;
    }
    uint64_t connects() const { return connects_; }

  private:
    void setError(const std::string &e) {
        std::lock_guard<std::mutex> g(mu_);
        error_ = e;
    }
    // One connection: false when it never answered 200 (the status in `status`).
    bool once(int &status) {
        robot_ar::Url u;
        if (!robot_ar::parseUrl(url_, u)) {
            setError("HAPTICS BAD URL");
            return false;
        }
        robot_ar::Connection c;
        if (!c.open(u, running_)) {
            setError("HAPTICS " + c.error);
            return false;
        }
        std::string head, decoded;
        robot_ar::ResponseHead h;
        robot_ar::BodyDecoder d;
        robot_ar::LineSplitter lines;
        bool inBody = false;
        char buf[8192];
        int idle = 0;
        while (running_) {
            ssize_t n = c.read(buf, sizeof(buf), 50);
            if (n == -2) {
                if ((idle += 50) >= kStallMs) {
                    setError("HAPTICS STREAM STALLED");
                    break;
                }
                continue;
            }
            idle = 0;
            if (n <= 0) {
                setError(n == 0 ? "HAPTICS STREAM CLOSED" : "HAPTICS STREAM ERROR");
                break;
            }
            decoded.clear();
            if (!inBody) {
                head.append(buf, size_t(n));
                size_t end = head.find("\r\n\r\n");
                if (end == std::string::npos) {
                    if (head.size() > 65536) {
                        setError("HAPTICS BAD RESPONSE");
                        break;
                    }
                    continue;
                }
                std::string rest = head.substr(end + 4);
                head.resize(end);
                if (!robot_ar::parseHead(head, h)) {
                    setError("HAPTICS BAD RESPONSE");
                    break;
                }
                status = h.status;
                if (h.status != 200) {
                    setError("HAPTICS HTTP " + std::to_string(h.status));
                    return false;
                }
                d.reset(h);
                inBody = true;
                player_->setConnected(true);
                connects_++;
                if (!d.feed(rest.data(), rest.size(), decoded)) {
                    setError("HAPTICS BAD CHUNKING");
                    break;
                }
            } else if (!d.feed(buf, size_t(n), decoded)) {
                setError("HAPTICS BAD CHUNKING");
                break;
            }
            const int64_t now = robot_ar::nowNs();
            if (!lines.feed(decoded, [&](const std::string &line) { player_->receive(line, now); })) {
                setError("HAPTICS LINE TOO LONG");
                break;
            }
            if (d.done()) {
                setError("HAPTICS STREAM ENDED");
                break;
            }
        }
        if (inBody)
            player_->setConnected(false);
        return inBody;
    }
    void run() {
        int backoff = 250;
        while (running_) {
            int status = 0;
            bool answered = once(status);
            backoff = answered ? 250 : status == 404 ? 10000 : std::min(2000, backoff * 2);
            for (int waited = 0; running_ && waited < backoff; waited += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    std::string url_;
    Player *player_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> connects_{0};
    std::thread thread_;
    mutable std::mutex mu_;
    std::string error_;
};
} // namespace haptics

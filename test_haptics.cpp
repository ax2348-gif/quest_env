// Host test for native/haptics.h (HAPTICS.md). Two modes:
//   test_haptics            scripted Player cases, one JSON record per line ({"case", ...});
//   test_haptics URL SEC    the Client against a live stream for SEC seconds: every command and the final block.
// test_haptics.py runs both (the second against its own little HTTP server) and checks the records.
#include "haptics.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

using namespace haptics;

static const int64_t MS = 1000000;

static std::string cmdJson(const Command &c, int64_t tMs) {
    std::ostringstream s;
    s << "{\"t_ms\":" << tMs << ",\"hand\":\"" << (c.hand ? "right" : "left") << "\",\"action\":\""
      << (c.stop ? "stop" : "apply") << "\",\"amplitude\":" << c.amplitude << ",\"duration_ms\":"
      << c.durationNs / MS << ",\"frequency_hz\":" << c.frequencyHz << ",\"id\":" << c.id << "}";
    return s.str();
}

// Ticks the player at `times` (ms) and records every command with its tick time.
static std::string run(Player &p, const std::vector<int64_t> &times) {
    std::ostringstream s;
    s << "[";
    bool first = true;
    for (int64_t t : times)
        for (const Command &c : p.tick(t * MS)) {
            s << (first ? "" : ",") << cmdJson(c, t);
            first = false;
        }
    s << "]";
    return s.str();
}

static std::string line(const std::string &epoch, const std::string &left, const std::string &right,
                        double ttl = 0.5) {
    std::ostringstream s;
    s << "{\"schema\":\"teleop.quest_haptics.v1\",\"epoch\":\"" << epoch << "\",\"seq\":1,\"ttl_s\":" << ttl
      << ",\"hands\":{\"left\":" << left << ",\"right\":" << right << "}}";
    return s.str();
}
static std::string hand(double level, const std::string &pulses = "") {
    return "{\"level\":" + std::to_string(level) + ",\"frequency_hz\":0,\"pulses\":[" + pulses + "]}";
}
static std::string pulse(int id, int group, double amp, double dur, double delay, double age = 0) {
    std::ostringstream s;
    s << "{\"id\":" << id << ",\"group\":" << group << ",\"amplitude\":" << amp << ",\"duration_s\":" << dur
      << ",\"frequency_hz\":0,\"delay_s\":" << delay << ",\"age_s\":" << age << "}";
    return s.str();
}
static void record(const std::string &name, const std::string &body) {
    std::cout << "{\"case\":\"" << name << "\"," << body << "}\n";
}

static void scripted() {
    {   // armed: one short on both hands, once; the same line again plays nothing
        Player p;
        std::string l = line("e1", hand(0, pulse(1, 1, 0.6, 0.06, 0)), hand(0, pulse(2, 2, 0.6, 0.06, 0)));
        bool ok = p.receive(l, 0);
        std::string a = run(p, {0, 10, 70});
        p.receive(l, 80 * MS);
        std::string b = run(p, {80, 200});
        record("armed", "\"ok\":" + std::string(ok ? "true" : "false") + ",\"first\":" + a + ",\"again\":" + b +
                            ",\"block\":" + p.blockJson());
    }
    {   // disarmed: two short, 140 ms apart; a newer group (disabled) replaces the waiting second
        Player p;
        p.receive(line("e1", hand(0, pulse(1, 1, 0.6, 0.06, 0) + "," + pulse(2, 1, 0.6, 0.06, 0.14)), hand(0)), 0);
        std::string two = run(p, {0, 100, 140, 300});
        Player q;
        q.receive(line("e1", hand(0, pulse(1, 1, 0.6, 0.06, 0) + "," + pulse(2, 1, 0.6, 0.06, 0.14)), hand(0)), 0);
        std::string a = run(q, {0});
        q.receive(line("e1", hand(0, pulse(1, 1, 0.6, 0.06, 0) + "," + pulse(2, 1, 0.6, 0.06, 0.14) + "," +
                                         pulse(3, 3, 0.8, 0.4, 0)), hand(0)), 20 * MS);
        std::string b = run(q, {20, 140, 500});
        record("two_and_replace", "\"two\":" + two + ",\"replaced_first\":" + a + ",\"replaced_then\":" + b);
    }
    {   // a pulse too old on arrival: skipped but acknowledged
        Player p;
        p.receive(line("e1", hand(0, pulse(5, 5, 0.6, 0.06, 0, 0.9)), hand(0)), 0);
        record("late", "\"cmds\":" + run(p, {0, 100}) + ",\"last_id\":" + std::to_string(p.lastId()) +
                           ",\"skipped\":" + std::to_string(p.skippedLate));
    }
    {   // a level: applied in 100 ms pieces every 50 ms, stopped at its TTL; a pulse plays over it
        Player p;
        p.receive(line("e1", hand(0), hand(0.2), 0.3), 0);
        std::string a = run(p, {0, 20, 50, 60, 100});
        p.receive(line("e1", hand(0), hand(0.2, pulse(7, 7, 0.9, 0.04, 0)), 0.3), 110 * MS);
        std::string b = run(p, {110, 130, 160, 200, 400, 420});
        record("level", "\"before\":" + a + ",\"pulse_and_ttl\":" + b);
    }
    {   // a new epoch: ids start over; disconnect stops a level
        Player p;
        p.receive(line("e1", hand(0, pulse(9, 9, 0.5, 0.05, 0)), hand(0)), 0);
        run(p, {0});
        p.receive(line("e2", hand(0.3, pulse(1, 1, 0.5, 0.05, 0)), hand(0)), 100 * MS);
        std::string a = run(p, {100, 160});
        p.setConnected(false);
        std::string b = run(p, {170, 300});
        record("epoch_and_disconnect", "\"new_epoch\":" + a + ",\"after_disconnect\":" + b + ",\"block\":" +
                                           p.blockJson());
    }
    {   // parsing: rejected lines, clamping
        Player p;
        bool bad1 = p.receive("not json", 0), bad2 = p.receive("{\"schema\":\"other\",\"hands\":{}}", 0);
        bool bad3 = p.receive("{\"schema\":\"teleop.quest_haptics.v1\"}", 0);
        Message m;
        m.hands[0].pulses.resize(1);
        bool ok = parseMessage(line("we\\\"ird epoch!", hand(7, pulse(3, 3, 4, 99, -1)), "null", 50), m);
        record("parse", std::string("\"rejected\":[") + (bad1 ? "true" : "false") + "," + (bad2 ? "true" : "false") +
                            "," + (bad3 ? "true" : "false") + "],\"bad\":" + std::to_string(p.bad) +
                            ",\"ok\":" + (ok ? "true" : "false") + ",\"epoch\":\"" + m.epoch + "\",\"ttl\":" +
                            std::to_string(m.ttlS) + ",\"level\":" + std::to_string(m.hands[0].level) +
                            ",\"right_present\":" + (m.hands[1].present ? "true" : "false") + ",\"amp\":" +
                            std::to_string(m.hands[0].pulses[0].amplitude) + ",\"dur\":" +
                            std::to_string(m.hands[0].pulses[0].durationS) + ",\"delay\":" +
                            std::to_string(m.hands[0].pulses[0].delayS));
    }
}

static void live(const std::string &url, double seconds) {
    Player p;
    Client c;
    const int64_t t0 = robot_ar::nowNs();
    c.start(url, &p);
    while (robot_ar::nowNs() - t0 < int64_t(seconds * 1e9)) {
        const int64_t now = robot_ar::nowNs();
        for (const Command &cmd : p.tick(now))
            std::cout << "{\"case\":\"cmd\",\"cmd\":" << cmdJson(cmd, (now - t0) / MS) << "}\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cout << "{\"case\":\"end\",\"block\":" << p.blockJson() << ",\"connects\":" << c.connects()
              << ",\"lines\":" << p.lines << "}\n";
    c.stop();
}

int main(int argc, char **argv) {
    if (argc == 3)
        live(argv[1], std::atof(argv[2]));
    else
        scripted();
    return 0;
}

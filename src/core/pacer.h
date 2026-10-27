#pragma once
#include <cstdint>
#include <deque>

namespace ff {
struct PaceItem {
    double due = 0;      // when to present (seconds, same clock as 'now')
    float t = 1.0f;      // interpolation phase; 1 = real frame
    bool real = true;
    double arrival = 0;  // arrival time of the real frame that triggered this item
    uint64_t frameId = 0;
};

// Pure CPU scheduler. On each real frame N it schedules M-1 generated frames between N-1 and N
// followed by N itself, spaced by the measured capture cadence / M. The first generated frame is
// due immediately, so the real frame is held for at most (M-1)/M of one capture interval.
class Pacer {
public:
    void reset();
    void setMultiplier(int m);
    int multiplier() const { return m_mult; }
    void onRealFrame(double now, double captureTime, uint64_t id);
    bool popDue(double now, PaceItem& out);
    double nextDue() const;
    bool empty() const { return m_q.empty(); }
    double interval() const { return m_interval; }
    uint64_t emitted() const { return m_emitted; }
    uint64_t dropped() const { return m_dropped; }
private:
    std::deque<PaceItem> m_q;
    int m_mult = 2;
    bool m_hasPrev = false, m_haveInterval = false;
    double m_interval = 1.0 / 60.0, m_lastCap = -1;
    uint64_t m_emitted = 0, m_dropped = 0;
};
}

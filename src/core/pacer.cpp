#include "core/pacer.h"
#include <algorithm>
#include <limits>

namespace ff {
void Pacer::reset() {
    m_q.clear(); m_hasPrev = false; m_haveInterval = false;
    m_interval = 1.0 / 60.0; m_lastCap = -1; m_emitted = m_dropped = 0;
}

void Pacer::setMultiplier(int m) { m_mult = std::clamp(m, 1, 4); }

void Pacer::onRealFrame(double now, double cap, uint64_t id) {
    if (m_lastCap >= 0) {
        double d = cap - m_lastCap;
        if (d > 0.001 && d < 0.25) {
            m_interval = m_haveInterval ? m_interval * 0.9 + d * 0.1 : d;
            m_haveInterval = true;
        }
    }
    m_lastCap = cap;
    // Anything still pending belongs to the previous pair, whose data is about to be replaced.
    m_dropped += m_q.size();
    m_q.clear();
    if (!m_hasPrev || m_mult == 1) {
        m_q.push_back({now, 1.0f, true, now, id});
        m_hasPrev = true;
        return;
    }
    double s = m_interval / m_mult;
    for (int k = 1; k < m_mult; ++k)
        m_q.push_back({now + (k - 1) * s, (float)k / (float)m_mult, false, now, id});
    m_q.push_back({now + (m_mult - 1) * s, 1.0f, true, now, id});
}

bool Pacer::popDue(double now, PaceItem& out) {
    // If we are late, skip generated frames whose successor is already due (never skip real ones).
    while (m_q.size() >= 2 && !m_q[0].real && m_q[1].due <= now) { m_q.pop_front(); ++m_dropped; }
    if (m_q.empty() || m_q.front().due > now) return false;
    out = m_q.front(); m_q.pop_front(); ++m_emitted;
    return true;
}

double Pacer::nextDue() const {
    return m_q.empty() ? std::numeric_limits<double>::infinity() : m_q.front().due;
}
}

#pragma once
#include <QHash>
#include <QString>
#include <QSet>
#include <algorithm>

// Monotonic deadlines and revisions keep late TCP probes from overriding live frames.
class CameraHealthSchedule {
public:
    static constexpr qint64 onlineIntervalMs = 15000;
    static constexpr qint64 captureGraceMs = 5000;
    static constexpr qint64 offlineIntervalMs = 30000;
    struct State {
        QString endpoint;
        qint64 due{0};
        qint64 lastLiveAt{-1}, captureBusyUntil{0};
        quint64 revision{0};
        int failures{0};
        bool known{false}, online{false}, inFlight{false};
    };
    QHash<QString, State> states;
    State& configure(const QString& id, const QString& endpoint) {
        auto& state = states[id];
        if (state.endpoint != endpoint) {
            const auto revision = state.revision + 1;
            state = State(); state.endpoint = endpoint; state.revision = revision;
        }
        return state;
    }
    bool deferForCapture(const QString& id, qint64 now) {
        auto& state = states[id];
        // Grant only one short grace period until acquisition supplies a result.
        // Repeated captures must not postpone heartbeat checks indefinitely.
        if (state.captureBusyUntil != 0) return false;
        state.captureBusyUntil = now + captureGraceMs;
        state.due = state.due > now ? std::min(state.due, state.captureBusyUntil) : state.captureBusyUntil;
        ++state.revision; state.inFlight = false;
        return true;
    }
    quint64 begin(const QString& id) {
        auto& state = states[id]; state.inFlight = true; return ++state.revision;
    }
    bool update(const QString& id, bool online, qint64 now, bool live = false) {
        auto& state = states[id];
        const bool changed = !state.known || state.online != online;
        if (live) {
            ++state.revision; state.inFlight = false; state.captureBusyUntil = 0;
            state.lastLiveAt = online ? now : -1;
        }
        if (!live || online || changed) {
            state.failures = online ? 0 : std::min(state.failures + 1, 4);
            // Keep checking registered offline devices promptly. Exponential
            // backoff up to five minutes made recovery appear to need discovery.
            const auto nextDue = now + (online ? onlineIntervalMs : offlineIntervalMs);
            // Live frames prove availability, but must not endlessly push out periodic probes.
            state.due = live && online && state.known && state.online && state.due > 0
                ? std::min(state.due, nextDue) : nextDue;
        }
        state.known = true; state.online = online;
        return changed;
    }
    bool complete(const QString& id, quint64 revision, bool online, qint64 now, bool& changed) {
        auto it = states.find(id);
        if (it == states.end() || it->revision != revision) return false;
        it->inFlight = false;
        // A completed probe already exhausted every registered service endpoint.
        // Publish the first confirmed failure so startup rows do not remain pending.
        changed = update(id, online, now);
        return true;
    }
    void prune(const QSet<QString>& ids) {
        for (auto it = states.begin(); it != states.end();)
            if (!ids.contains(it.key())) it = states.erase(it); else ++it;
    }
};

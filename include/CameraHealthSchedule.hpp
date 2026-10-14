#pragma once
#include <QHash>
#include <QString>
#include <QSet>
#include <algorithm>

// Monotonic deadlines and revisions keep late TCP probes from overriding live frames.
class CameraHealthSchedule {
public:
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
            state.due = now + (online ? 30000 : std::min(300000, 30000 * (1 << state.failures)));
        }
        state.known = true; state.online = online;
        return changed;
    }
    bool complete(const QString& id, quint64 revision, bool online, qint64 now, bool& changed) {
        auto it = states.find(id);
        if (it == states.end() || it->revision != revision) return false;
        it->inFlight = false; changed = update(id, online, now); return true;
    }
    void prune(const QSet<QString>& ids) {
        for (auto it = states.begin(); it != states.end();)
            if (!ids.contains(it.key())) it = states.erase(it); else ++it;
    }
};

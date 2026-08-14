#ifndef AMBIENT_FACE_STATE_H
#define AMBIENT_FACE_STATE_H

#include <cstdint>

namespace ambient::face {

using ConnectionEpoch = uint64_t;

enum class Pipeline {
    kIdle,
    kHeard,
    kReceived,
    kThinking,
    kTool,
    kGenerating,
    kReply,
    kDone,
};

enum class MetaState {
    kListening,
    kThinking,
    kSpeaking,
    /* No live capture seat. Not the same as offline. */
    kUnowned,
};

struct State {
    ConnectionEpoch connection_epoch = 0;
    bool online = false;
    bool speaker_active = false;
    bool muted = false;
    bool senses_enabled = true;
    /* Server-reported: this device holds no capture seat (or the room has
     * none). Distinct from offline — the WS is up, the room is just deaf. */
    bool seat_unowned = false;
    /* Local mic energy above the speech band (driven by FeedPcm, zero round
     * trips). Purely cosmetic: the resting face "perks up" when there is
     * sound, which is the truth-panel's cheapest honest signal. */
    bool hearing = false;
    Pipeline pipeline = Pipeline::kIdle;
};

inline const char* Expression(const State& state) {
    if (!state.online) return "sad";
    if (!state.senses_enabled) return "sleepy";
    if (state.muted) return "icon_speaker_zzz";
    /* Unowned outranks the pipeline, not mute/senses. */
    if (state.seat_unowned) return "confused";
    if (state.speaker_active) return "laughing";

    switch (state.pipeline) {
        case Pipeline::kHeard: return "surprised";
        case Pipeline::kReceived: return "winking";
        case Pipeline::kThinking: return "thinking";
        case Pipeline::kTool: return "cool";
        case Pipeline::kGenerating: return "confident";
        case Pipeline::kReply: return "happy";
        case Pipeline::kIdle:
        case Pipeline::kDone:
        default:
            /* listen only while local energy says there is speech. */
            return state.hearing ? "listen" : "neutral";
    }
}

inline bool Accepts(const State& state, ConnectionEpoch epoch) {
    return epoch != 0 && epoch == state.connection_epoch;
}

inline bool BeginConnection(State& state, ConnectionEpoch epoch) {
    if (epoch == 0 || epoch <= state.connection_epoch) return false;
    state.connection_epoch = epoch;
    state.online = false;
    state.muted = false;
    state.senses_enabled = true;
    state.seat_unowned = false;
    state.pipeline = Pipeline::kIdle;
    return true;
}

inline bool SetConnected(State& state, ConnectionEpoch epoch) {
    if (!Accepts(state, epoch) || state.online) return false;
    state.online = true;
    return true;
}

inline bool SetDisconnected(State& state, ConnectionEpoch epoch) {
    if (!Accepts(state, epoch)) return false;
    const bool changed = state.online || state.muted || !state.senses_enabled ||
                         state.seat_unowned || state.pipeline != Pipeline::kIdle;
    state.online = false;
    state.muted = false;
    state.senses_enabled = true;
    state.seat_unowned = false;
    state.pipeline = Pipeline::kIdle;
    return changed;
}

inline bool SetSpeakerActive(State& state, bool active) {
    if (state.speaker_active == active) return false;
    state.speaker_active = active;
    return true;
}

/* Local signal, no epoch gate — mic energy is a fact about this device, not
 * about any particular server connection (same footing as SetSpeakerActive). */
inline bool SetHearing(State& state, bool hearing) {
    if (state.hearing == hearing) return false;
    state.hearing = hearing;
    return true;
}

inline bool SetMuted(State& state, ConnectionEpoch epoch, bool muted) {
    if (!Accepts(state, epoch) || state.muted == muted) return false;
    state.muted = muted;
    return true;
}

inline bool SetSensesEnabled(State& state, ConnectionEpoch epoch, bool enabled) {
    if (!Accepts(state, epoch) || state.senses_enabled == enabled) return false;
    state.senses_enabled = enabled;
    return true;
}

inline bool SetPipeline(State& state, ConnectionEpoch epoch, Pipeline pipeline) {
    if (!Accepts(state, epoch) || state.pipeline == pipeline) return false;
    state.pipeline = pipeline;
    return true;
}

inline bool ApplyTranscript(State& state, ConnectionEpoch epoch) {
    if (!Accepts(state, epoch)) return false;
    if (state.pipeline != Pipeline::kIdle && state.pipeline != Pipeline::kDone) return false;
    state.pipeline = Pipeline::kHeard;
    return true;
}

inline bool ApplyMetaState(State& state, ConnectionEpoch epoch, MetaState meta) {
    if (!Accepts(state, epoch)) return false;

    /* Seat honesty rides the same frame: `unowned` raises the flag, any other
     * state is the channel saying the room has ears again, which clears it. */
    bool changed = false;
    const bool unowned = meta == MetaState::kUnowned;
    if (state.seat_unowned != unowned) {
        state.seat_unowned = unowned;
        changed = true;
    }
    if (unowned) return changed;

    Pipeline pipeline = state.pipeline;
    switch (meta) {
        case MetaState::kThinking:
            if (pipeline != Pipeline::kTool && pipeline != Pipeline::kReply &&
                pipeline != Pipeline::kDone) {
                pipeline = Pipeline::kThinking;
            }
            break;
        case MetaState::kSpeaking:
            if (pipeline != Pipeline::kReply && pipeline != Pipeline::kDone) {
                pipeline = Pipeline::kGenerating;
            }
            break;
        case MetaState::kListening:
            if (pipeline == Pipeline::kThinking) pipeline = Pipeline::kIdle;
            break;
        case MetaState::kUnowned:
            break; // handled above
    }

    if (SetPipeline(state, epoch, pipeline)) changed = true;
    return changed;
}

inline bool IsReactionSpeechId(const char* speech_id) {
    return speech_id != nullptr && speech_id[0] == 's';
}

}  // namespace ambient::face

#endif  // AMBIENT_FACE_STATE_H

#include "ambient_face_state.h"

#include <cassert>
#include <cstring>
#include <cstdio>

namespace {

using ambient::face::BeginConnection;
using ambient::face::ConnectionEpoch;
using ambient::face::Expression;
using ambient::face::MetaState;
using ambient::face::Pipeline;
using ambient::face::State;

void ExpectExpression(const State& state, const char* expected) {
    assert(std::strcmp(Expression(state), expected) == 0);
}

void TestPriorityAndMappings() {
    State state;
    ExpectExpression(state, "sad");

    assert(BeginConnection(state, 1));
    assert(ambient::face::SetConnected(state, 1));
    ExpectExpression(state, "neutral");
    assert(ambient::face::SetHearing(state, true));
    ExpectExpression(state, "listen");
    assert(ambient::face::SetHearing(state, false));

    const struct {
        Pipeline pipeline;
        const char* expression;
    } mappings[] = {
        {Pipeline::kIdle, "neutral"},
        {Pipeline::kHeard, "surprised"},
        {Pipeline::kReceived, "winking"},
        {Pipeline::kThinking, "thinking"},
        {Pipeline::kTool, "cool"},
        {Pipeline::kGenerating, "confident"},
        {Pipeline::kReply, "happy"},
        {Pipeline::kDone, "neutral"},
    };
    for (const auto& mapping : mappings) {
        ambient::face::SetPipeline(state, 1, mapping.pipeline);
        ExpectExpression(state, mapping.expression);
    }

    assert(ambient::face::SetSpeakerActive(state, true));
    ExpectExpression(state, "laughing");
    assert(ambient::face::SetMuted(state, 1, true));
    ExpectExpression(state, "icon_speaker_zzz");
    assert(ambient::face::SetSensesEnabled(state, 1, false));
    ExpectExpression(state, "sleepy");
    assert(ambient::face::SetDisconnected(state, 1));
    ExpectExpression(state, "sad");
}

void TestConnectionEpochAndLocalSpeaker() {
    State state;
    assert(BeginConnection(state, 1));
    assert(ambient::face::SetConnected(state, 1));
    assert(ambient::face::SetSpeakerActive(state, true));
    assert(ambient::face::SetDisconnected(state, 1));
    assert(state.speaker_active);

    assert(BeginConnection(state, 2));
    assert(!ambient::face::SetPipeline(state, 1, Pipeline::kReply));
    assert(!ambient::face::SetConnected(state, 1));
    assert(ambient::face::SetConnected(state, 2));
    ExpectExpression(state, "laughing");

    assert(ambient::face::SetSpeakerActive(state, false));
    ExpectExpression(state, "neutral");
    assert(!BeginConnection(state, 2));
    assert(!BeginConnection(state, 1));
}

void TestSeatHonesty() {
    State state;
    assert(BeginConnection(state, 1));
    assert(ambient::face::SetConnected(state, 1));

    assert(ambient::face::ApplyMetaState(state, 1, MetaState::kUnowned));
    ExpectExpression(state, "confused");
    assert(ambient::face::SetPipeline(state, 1, Pipeline::kThinking));
    ExpectExpression(state, "confused");
    assert(ambient::face::SetMuted(state, 1, true));
    ExpectExpression(state, "icon_speaker_zzz");
    assert(ambient::face::SetSensesEnabled(state, 1, false));
    ExpectExpression(state, "sleepy");
    assert(ambient::face::SetSensesEnabled(state, 1, true));
    assert(ambient::face::SetMuted(state, 1, false));
    ExpectExpression(state, "confused");

    assert(ambient::face::ApplyMetaState(state, 1, MetaState::kListening));
    assert(!state.seat_unowned);

    assert(ambient::face::ApplyMetaState(state, 1, MetaState::kUnowned));
    assert(BeginConnection(state, 2));
    assert(!state.seat_unowned); // new epoch starts clean
    assert(!ambient::face::ApplyMetaState(state, 1, MetaState::kUnowned));
    assert(!state.seat_unowned);

    assert(ambient::face::SetConnected(state, 2));
    assert(ambient::face::ApplyMetaState(state, 2, MetaState::kUnowned));
    assert(ambient::face::SetDisconnected(state, 2));
    ExpectExpression(state, "sad");
    assert(!state.seat_unowned);
}

void TestFineAndCoarsePipelineRules() {
    State state;
    assert(BeginConnection(state, 1));
    assert(ambient::face::SetConnected(state, 1));

    assert(ambient::face::ApplyTranscript(state, 1));
    assert(state.pipeline == Pipeline::kHeard);
    assert(ambient::face::SetPipeline(state, 1, Pipeline::kThinking));
    assert(!ambient::face::ApplyTranscript(state, 1));

    assert(ambient::face::SetPipeline(state, 1, Pipeline::kDone));
    assert(ambient::face::ApplyTranscript(state, 1));
    assert(state.pipeline == Pipeline::kHeard);

    assert(ambient::face::SetPipeline(state, 1, Pipeline::kTool));
    assert(!ambient::face::ApplyMetaState(state, 1, MetaState::kThinking));
    assert(state.pipeline == Pipeline::kTool);

    assert(ambient::face::SetPipeline(state, 1, Pipeline::kReply));
    assert(!ambient::face::ApplyMetaState(state, 1, MetaState::kSpeaking));
    assert(state.pipeline == Pipeline::kReply);

    assert(ambient::face::SetPipeline(state, 1, Pipeline::kDone));
    assert(!ambient::face::ApplyMetaState(state, 1, MetaState::kSpeaking));
    assert(state.pipeline == Pipeline::kDone);

    assert(ambient::face::SetPipeline(state, 1, Pipeline::kIdle));
    assert(ambient::face::SetSpeakerActive(state, true));
    assert(ambient::face::ApplyMetaState(state, 1, MetaState::kSpeaking));
    ExpectExpression(state, "laughing");
    assert(ambient::face::SetSpeakerActive(state, false));
    ExpectExpression(state, "confident");

    assert(ambient::face::ApplyMetaState(state, 1, MetaState::kThinking));
    assert(ambient::face::ApplyMetaState(state, 1, MetaState::kListening));
    assert(state.pipeline == Pipeline::kIdle);
}

void TestSpeechOwnership() {
    assert(ambient::face::IsReactionSpeechId("s000001"));
    assert(!ambient::face::IsReactionSpeechId("c-u000001"));
    assert(!ambient::face::IsReactionSpeechId(""));
    assert(!ambient::face::IsReactionSpeechId(nullptr));
}

}  // namespace

int main() {
    TestPriorityAndMappings();
    TestConnectionEpochAndLocalSpeaker();
    TestFineAndCoarsePipelineRules();
    TestSpeechOwnership();
    TestSeatHonesty();
    std::puts("ambient face state tests passed");
    return 0;
}

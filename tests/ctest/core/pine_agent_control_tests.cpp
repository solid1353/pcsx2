// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "PINE.h"
#include "Recording/PadData.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

namespace
{
	using namespace PINEServer::AgentControl;

	PadStateBytes MakeState(const u8 seed)
	{
		PadStateBytes state;
		for (size_t i = 0; i < state.size(); i++)
			state[i] = static_cast<u8>(seed + i);
		return state;
	}

	void AppendState(std::vector<u8>* payload, const u8 slot, const PadStateBytes& state)
	{
		payload->push_back(slot);
		payload->insert(payload->end(), state.begin(), state.end());
	}
} // namespace

TEST(PINEAgentControlProtocol, ParsesVersionedMultiSlotStateRequest)
{
	EXPECT_EQ(static_cast<u8>(Opcode::SetStates), 0x16);
	EXPECT_EQ(static_cast<u8>(Opcode::Step), 0x17);
	EXPECT_EQ(static_cast<u8>(Opcode::GetStates), 0x18);
	EXPECT_EQ(static_cast<u8>(Opcode::Release), 0x19);

	const PadStateBytes first_state = MakeState(0x10);
	const PadStateBytes second_state = MakeState(0x40);
	std::vector<u8> payload = {PROTOCOL_VERSION, 2};
	AppendState(&payload, 0, first_state);
	AppendState(&payload, 7, second_state);

	const std::optional<ParsedStateRequest> request = ParseStateRequest(payload);
	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(request->bytes_consumed, payload.size());
	ASSERT_EQ(request->states.size(), 2u);
	EXPECT_EQ(request->states[0].slot, 0);
	EXPECT_EQ(request->states[0].state, first_state);
	EXPECT_EQ(request->states[1].slot, 7);
	EXPECT_EQ(request->states[1].state, second_state);
}

TEST(PINEAgentControlProtocol, RejectsInvalidStateRequests)
{
	const PadStateBytes state = MakeState(0x20);
	std::vector<u8> wrong_version = {PROTOCOL_VERSION + 1, 1};
	AppendState(&wrong_version, 0, state);
	EXPECT_FALSE(ParseStateRequest(wrong_version).has_value());

	std::vector<u8> duplicate_slots = {PROTOCOL_VERSION, 2};
	AppendState(&duplicate_slots, 1, state);
	AppendState(&duplicate_slots, 1, state);
	EXPECT_FALSE(ParseStateRequest(duplicate_slots).has_value());

	std::vector<u8> truncated = {PROTOCOL_VERSION, 1, 0};
	truncated.insert(truncated.end(), state.begin(), state.end() - 1);
	EXPECT_FALSE(ParseStateRequest(truncated).has_value());
}

TEST(PINEAgentControlProtocol, ParsesPositiveFrameStepRequest)
{
	constexpr u32 frame_count = 37;
	const PadStateBytes state = MakeState(0x30);
	std::vector<u8> payload = {PROTOCOL_VERSION};
	const u8* const frame_bytes = reinterpret_cast<const u8*>(&frame_count);
	payload.insert(payload.end(), frame_bytes, frame_bytes + sizeof(frame_count));
	payload.push_back(1);
	AppendState(&payload, 2, state);

	const std::optional<ParsedStepRequest> request = ParseStepRequest(payload);
	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(request->frame_count, frame_count);
	EXPECT_EQ(request->bytes_consumed, payload.size());
	ASSERT_EQ(request->states.size(), 1u);
	EXPECT_EQ(request->states[0].slot, 2);
	EXPECT_EQ(request->states[0].state, state);

	std::fill(payload.begin() + 1, payload.begin() + 1 + sizeof(frame_count), 0);
	EXPECT_FALSE(ParseStepRequest(payload).has_value());
}

TEST(PINEAgentControlProtocol, DistinguishesReadAndReleaseSlotLists)
{
	const std::array<u8, 4> slots = {PROTOCOL_VERSION, 2, 0, 7};
	const std::optional<ParsedSlotRequest> request = ParseSlotRequest(slots, false);
	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(request->slots, (std::vector<u8>{0, 7}));
	EXPECT_EQ(request->bytes_consumed, slots.size());

	const std::array<u8, 2> all_slots = {PROTOCOL_VERSION, 0};
	EXPECT_FALSE(ParseSlotRequest(all_slots, false).has_value());
	const std::optional<ParsedSlotRequest> release_all = ParseSlotRequest(all_slots, true);
	ASSERT_TRUE(release_all.has_value());
	EXPECT_TRUE(release_all->slots.empty());
}

TEST(PINEAgentControlState, ControlsOnlySubmittedSlotsAndReleasesExplicitly)
{
	OverrideState state;
	EXPECT_FALSE(state.HasAny());
	const PadStateRecord initial[] = {{1, MakeState(0x10)}, {3, MakeState(0x30)}};
	ASSERT_TRUE(state.SetStates(initial));
	EXPECT_TRUE(state.HasAny());
	EXPECT_FALSE(state.IsControlled(0));
	EXPECT_TRUE(state.IsControlled(1));
	EXPECT_TRUE(state.IsControlled(3));

	const PadStateRecord replacement[] = {{1, MakeState(0x50)}};
	ASSERT_TRUE(state.SetStates(replacement));
	EXPECT_EQ(state.GetState(1), replacement[0].state);
	EXPECT_EQ(state.GetState(3), initial[1].state);

	const u8 released_slot[] = {1};
	EXPECT_EQ(state.Release(released_slot), (std::vector<u8>{1}));
	EXPECT_FALSE(state.IsControlled(1));
	EXPECT_TRUE(state.IsControlled(3));
	EXPECT_EQ(state.Release(std::span<const u8>()), (std::vector<u8>{3}));
	EXPECT_FALSE(state.HasAny());
	EXPECT_FALSE(state.IsControlled(3));
}

TEST(PINEAgentControlState, RestoresPersistentStateAfterStepScopedInput)
{
	OverrideState state;
	const PadStateRecord persistent[] = {{1, MakeState(0x10)}};
	ASSERT_TRUE(state.SetStates(persistent));

	const PadStateRecord stepped[] = {{1, MakeState(0x40)}, {3, MakeState(0x60)}};
	const std::vector<PadStateSnapshot> snapshot = state.Capture(stepped);
	ASSERT_TRUE(state.SetStates(stepped));
	EXPECT_EQ(state.GetState(1), stepped[0].state);
	EXPECT_EQ(state.GetState(3), stepped[1].state);

	EXPECT_EQ(state.Restore(snapshot), (std::vector<u8>{3}));
	EXPECT_EQ(state.GetState(1), persistent[0].state);
	EXPECT_FALSE(state.IsControlled(3));
}

TEST(PINEAgentControlState, RunningStepUsesExactlyOneCapturedInputFrameWithoutFrameAdvance)
{
	RunningStepFrameSequence frames(1);
	EXPECT_FALSE(StepExecutionRequiresFrameAdvance(StepExecutionMode::Running));
	EXPECT_TRUE(StepExecutionRequiresFrameAdvance(StepExecutionMode::PausedFrameAdvance));
	EXPECT_FALSE(frames.IsAwaitingCapture());
	EXPECT_FALSE(frames.IsComplete());

	ASSERT_TRUE(frames.BeginInputFrame());
	EXPECT_TRUE(frames.IsAwaitingCapture());
	EXPECT_FALSE(frames.IsComplete());
	EXPECT_FALSE(frames.BeginInputFrame());

	ASSERT_TRUE(frames.FinishInputFrame());
	EXPECT_FALSE(frames.IsAwaitingCapture());
	EXPECT_TRUE(frames.IsComplete());
	EXPECT_FALSE(frames.BeginInputFrame());
}

TEST(PINEAgentControlState, RestoresRunningStepStateOnlyAfterFinalRecordedFrame)
{
	OverrideState state;
	const PadStateRecord persistent[] = {{1, MakeState(0x10)}};
	const PadStateRecord stepped[] = {{1, MakeState(0x40)}, {3, MakeState(0x60)}};
	ASSERT_TRUE(state.SetStates(persistent));
	const std::vector<PadStateSnapshot> snapshot = state.Capture(stepped);
	ASSERT_TRUE(state.SetStates(stepped));

	RunningStepFrameSequence frames(2);
	std::vector<PadStateBytes> recorded_states;
	for (u32 frame = 0; frame < 2; frame++)
	{
		ASSERT_TRUE(frames.BeginInputFrame());
		recorded_states.push_back(state.GetState(1).value());
		ASSERT_TRUE(frames.FinishInputFrame());
		if (!frames.IsComplete())
			EXPECT_EQ(state.GetState(1), stepped[0].state);
	}

	ASSERT_TRUE(frames.IsComplete());
	EXPECT_EQ(recorded_states, (std::vector<PadStateBytes>{stepped[0].state, stepped[0].state}));
	EXPECT_EQ(state.Restore(snapshot), (std::vector<u8>{3}));
	EXPECT_EQ(state.GetState(1), persistent[0].state);
	EXPECT_FALSE(state.IsControlled(3));
}

TEST(PINEAgentControlState, AllowsRecordingAndRejectsReplay)
{
	EXPECT_TRUE(IsAgentControlAllowedForInputRecording(false, false, true));
	EXPECT_TRUE(IsAgentControlAllowedForInputRecording(true, true, false));
	EXPECT_FALSE(IsAgentControlAllowedForInputRecording(true, false, true));
	EXPECT_FALSE(IsAgentControlAllowedForInputRecording(true, false, false));
	EXPECT_FALSE(IsAgentControlAllowedForInputRecording(true, true, true));
}

TEST(PINEAgentControlState, UsesRecordingCompatibleFullPadLayout)
{
	const PadStateBytes state = MakeState(0x70);
	EXPECT_EQ(PadData(0, 0, state).ToArray(), state);

	PadStateBytes neutral = {};
	neutral[0] = 0xFF;
	neutral[1] = 0xFF;
	std::fill(neutral.begin() + 2, neutral.begin() + 6, PadData::ANALOG_VECTOR_NEUTRAL);
	EXPECT_EQ(GetNeutralPadState(), neutral);
}

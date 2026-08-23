// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

/* A reference client implementation for interfacing with PINE is available
 * here: https://code.govanify.com/govanify/pine/ */

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// PINE uses a concept of "slot" to be able to communicate with multiple
// emulators at the same time, each slot should be unique to each emulator to
// allow PnP and configurable by the end user so that several runs don't
// conflict with each others
#define PINE_DEFAULT_SLOT 28011

namespace PINEServer
{
	bool IsInitialized();
	int GetSlot();

	bool Initialize(int slot = PINE_DEFAULT_SLOT);
	void Deinitialize();

	namespace AgentControl
	{
		constexpr u8 PROTOCOL_VERSION = 1;
		constexpr size_t PAD_STATE_SIZE = 18;
		constexpr size_t MAX_UNIFIED_SLOTS = 8;

		enum class Opcode : u8
		{
			SetStates = 0x16,
			Step = 0x17,
			GetStates = 0x18,
			Release = 0x19,
		};

		using PadStateBytes = std::array<u8, PAD_STATE_SIZE>;

		// Recording-compatible DS2 layout: active-low button bytes; RX, RY, LX, LY; then twelve pressure bytes.
		struct PadStateRecord
		{
			u8 slot;
			PadStateBytes state;
		};

		struct PadStateSnapshot
		{
			u8 slot;
			std::optional<PadStateBytes> state;
		};

		struct ParsedStateRequest
		{
			std::vector<PadStateRecord> states;
			size_t bytes_consumed;
		};

		struct ParsedStepRequest
		{
			u32 frame_count;
			std::vector<PadStateRecord> states;
			size_t bytes_consumed;
		};

		struct ParsedSlotRequest
		{
			std::vector<u8> slots;
			size_t bytes_consumed;
		};

		struct PadStateReadback
		{
			u8 slot;
			bool controlled;
			PadStateBytes state;
		};

		enum class StepExecutionMode : u8
		{
			Running,
			PausedFrameAdvance,
		};

		class RunningStepFrameSequence
		{
		public:
			explicit RunningStepFrameSequence(u32 frame_count);
			bool BeginInputFrame();
			bool FinishInputFrame();
			bool FinishRestoreBoundary();
			bool IsAwaitingCapture() const;
			bool IsAwaitingRestore() const;
			bool IsComplete() const;

		private:
			u32 m_remaining_frames;
			bool m_awaiting_capture = false;
			bool m_awaiting_restore = false;
			bool m_complete = false;
		};

		class OverrideState
		{
		public:
			bool SetStates(std::span<const PadStateRecord> states);
			std::vector<PadStateSnapshot> Capture(std::span<const PadStateRecord> states) const;
			std::vector<u8> Restore(std::span<const PadStateSnapshot> states);
			std::vector<u8> Release(std::span<const u8> slots);
			bool HasAny() const;
			bool IsControlled(u8 slot) const;
			std::optional<PadStateBytes> GetState(u8 slot) const;

		private:
			std::array<std::optional<PadStateBytes>, MAX_UNIFIED_SLOTS> m_states;
		};

		const PadStateBytes& GetNeutralPadState();
		u32 GetRunningStepExclusiveEndFrame(u32 captured_frame);
		bool StepExecutionRequiresFrameAdvance(StepExecutionMode mode);
		bool IsAgentControlAllowedForInputRecording(bool active, bool recording, bool replaying);
		std::optional<ParsedStateRequest> ParseStateRequest(std::span<const u8> payload);
		std::optional<ParsedStepRequest> ParseStepRequest(std::span<const u8> payload);
		std::optional<ParsedSlotRequest> ParseSlotRequest(std::span<const u8> payload, bool allow_empty);

		void ApplyOverridesAfterInputPoll();
		void OnClientDisconnected();
		void OnVMReset();
		void OnVMShutdown();
		void OnVMPaused(u32 frame_count, bool frame_advance_completed);
		void OnInputFrameProcessed();
	} // namespace AgentControl

	namespace ReplayAnalysis
	{
		constexpr u8 PROTOCOL_VERSION = 1;
		constexpr size_t MAX_SCREENSHOT_PATH_SIZE = 32768;

		enum class Opcode : u8
		{
			Status = 0x1A,
			Step = 0x1B,
			Screenshot = 0x1C,
		};

		struct ReplayStatus
		{
			u32 replay_frame;
			u32 total_frames;
			u32 vblank;
		};

		struct ParsedStepRequest
		{
			u32 vblank_count;
			size_t bytes_consumed;
		};

		struct ParsedScreenshotRequest
		{
			std::string path;
			size_t bytes_consumed;
		};

		struct ReplayStepResult
		{
			u32 start_replay_frame;
			u32 end_replay_frame;
			u32 start_vblank;
			u32 end_vblank;
		};

		struct StepTicket
		{
			u64 sequence;
		};

		bool IsReadOnlyReplay(bool active, bool recording, bool replaying);
		bool ReplayStepFits(u32 replay_frame, u32 total_frames, u32 vblank_count);
		bool ReplayStepIntervalsMatch(const ReplayStepResult& result, u32 vblank_count);
		bool IsScreenshotPathValid(std::string_view path);
		std::optional<ParsedStepRequest> ParseStepRequest(std::span<const u8> payload);
		std::optional<ParsedScreenshotRequest> ParseScreenshotRequest(std::span<const u8> payload);
		bool QueryStatusFromServer(ReplayStatus* status);
		bool StartStepFromServer(const ParsedStepRequest& request, StepTicket* ticket);
		bool WaitForStepFromServer(const StepTicket& ticket, ReplayStepResult* result);
		bool SaveScreenshotFromServer(std::string path);
		void OnClientDisconnected();
		void OnVMReset();
		void OnVMShutdown();
		void OnVMPaused(u32 vblank, bool frame_advance_completed);
		void OnInputFrameProcessed();
	} // namespace ReplayAnalysis
} // namespace PINEServer

// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "BuildVersion.h"
#include "Common.h"
#include "Counters.h"
#include "GS/GS.h"
#include "Host.h"
#include "MTGS.h"
#include "Elfheader.h"
#include "Recording/InputRecording.h"
#include "Recording/PadData.h"
#include "SaveState.h"
#include "PINE.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "SIO/Sio.h"
#include "VMManager.h"
#include "vtlb.h"
#include "common/Error.h"
#include "common/Threading.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <span>
#include <sys/types.h>
#include <thread>

#include "fmt/format.h"

#if defined(_WIN32)
#define read_portable(a, b, c) (recv(a, (char*)b, c, 0))
#define write_portable(a, b, c) (send(a, (const char*)b, c, 0))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			closesocket((a)); \
			(a) = INVALID_SOCKET; \
		} \
	} while (0)
#include "common/RedtapeWindows.h"
#include <WinSock2.h>
#elif defined(__linux__) || defined(__FreeBSD__)
#define read_portable(a, b, c) (read(a, b, c))
#define write_portable(a, b, c) (send(a, b, c, MSG_NOSIGNAL))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			close((a)); \
			(a) = -1; \
		} \
	} while (0)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#else
#define read_portable(a, b, c) (read(a, b, c))
#define write_portable(a, b, c) (write(a, b, c))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			close((a)); \
			(a) = -1; \
		} \
	} while (0)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define PINE_EMULATOR_NAME "pcsx2"

#ifdef _WIN32

static bool InitializeWinsock()
{
	static bool initialized = false;
	if (initialized)
		return true;

	WSADATA wsa = {};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;

	initialized = true;
	std::atexit([]() { WSACleanup(); });
	return true;
}

#endif

namespace PINEServer
{
	namespace AgentControl
	{
		static_assert(MAX_UNIFIED_SLOTS == Pad::NUM_CONTROLLER_PORTS);

		static std::mutex s_mutex;
		static std::condition_variable s_step_condition;
		static OverrideState s_overrides;
		static std::atomic_bool s_has_overrides{false};

		enum class StepStatus
		{
			Idle,
			Pending,
			Complete,
			Aborted,
		};

		struct StepState
		{
			u64 sequence = 0;
			StepStatus status = StepStatus::Idle;
			u32 start_frame = 0;
			u32 end_frame = 0;
		};

		static StepState s_step;

		const PadStateBytes& GetNeutralPadState()
		{
			static constexpr PadStateBytes state = {
				0xFF,
				0xFF,
				PadData::ANALOG_VECTOR_NEUTRAL,
				PadData::ANALOG_VECTOR_NEUTRAL,
				PadData::ANALOG_VECTOR_NEUTRAL,
				PadData::ANALOG_VECTOR_NEUTRAL,
			};
			return state;
		}

		bool OverrideState::SetStates(const std::span<const PadStateRecord> states)
		{
			if (states.empty() || states.size() > MAX_UNIFIED_SLOTS)
				return false;

			std::array<bool, MAX_UNIFIED_SLOTS> seen = {};
			for (const PadStateRecord& record : states)
			{
				if (record.slot >= MAX_UNIFIED_SLOTS || seen[record.slot])
					return false;
				seen[record.slot] = true;
			}

			for (const PadStateRecord& record : states)
				m_states[record.slot] = record.state;
			return true;
		}

		std::vector<u8> OverrideState::Release(const std::span<const u8> slots)
		{
			std::vector<u8> released;
			if (slots.empty())
			{
				for (u8 slot = 0; slot < m_states.size(); slot++)
				{
					if (m_states[slot].has_value())
					{
						released.push_back(slot);
						m_states[slot].reset();
					}
				}
				return released;
			}

			for (const u8 slot : slots)
			{
				if (slot < m_states.size() && m_states[slot].has_value())
				{
					released.push_back(slot);
					m_states[slot].reset();
				}
			}
			return released;
		}

		bool OverrideState::IsControlled(const u8 slot) const
		{
			return slot < m_states.size() && m_states[slot].has_value();
		}

		bool OverrideState::HasAny() const
		{
			return std::ranges::any_of(m_states, [](const std::optional<PadStateBytes>& state) {
				return state.has_value();
			});
		}

		std::optional<PadStateBytes> OverrideState::GetState(const u8 slot) const
		{
			return slot < m_states.size() ? m_states[slot] : std::nullopt;
		}

		static bool ParseRecords(const std::span<const u8> payload, const size_t count_offset,
			const size_t records_offset, const bool allow_empty, std::vector<PadStateRecord>* records,
			size_t* bytes_consumed)
		{
			if (payload.size() <= count_offset)
				return false;

			const size_t count = payload[count_offset];
			if ((!allow_empty && count == 0) || count > MAX_UNIFIED_SLOTS ||
				payload.size() < records_offset + count * (1 + PAD_STATE_SIZE))
			{
				return false;
			}

			std::array<bool, MAX_UNIFIED_SLOTS> seen = {};
			records->clear();
			records->reserve(count);
			for (size_t i = 0; i < count; i++)
			{
				const size_t offset = records_offset + i * (1 + PAD_STATE_SIZE);
				const u8 slot = payload[offset];
				if (slot >= MAX_UNIFIED_SLOTS || seen[slot])
					return false;

				seen[slot] = true;
				PadStateRecord& record = records->emplace_back();
				record.slot = slot;
				std::copy_n(payload.begin() + offset + 1, PAD_STATE_SIZE, record.state.begin());
			}

			*bytes_consumed = records_offset + count * (1 + PAD_STATE_SIZE);
			return true;
		}

		std::optional<ParsedStateRequest> ParseStateRequest(const std::span<const u8> payload)
		{
			if (payload.empty() || payload[0] != PROTOCOL_VERSION)
				return std::nullopt;

			ParsedStateRequest request;
			if (!ParseRecords(payload, 1, 2, false, &request.states, &request.bytes_consumed))
				return std::nullopt;
			return request;
		}

		std::optional<ParsedStepRequest> ParseStepRequest(const std::span<const u8> payload)
		{
			if (payload.size() < 6 || payload[0] != PROTOCOL_VERSION)
				return std::nullopt;

			ParsedStepRequest request;
			std::memcpy(&request.frame_count, payload.data() + 1, sizeof(request.frame_count));
			if (request.frame_count == 0 ||
				!ParseRecords(payload, 5, 6, false, &request.states, &request.bytes_consumed))
			{
				return std::nullopt;
			}
			return request;
		}

		std::optional<ParsedSlotRequest> ParseSlotRequest(const std::span<const u8> payload, const bool allow_empty)
		{
			if (payload.size() < 2 || payload[0] != PROTOCOL_VERSION)
				return std::nullopt;

			const size_t count = payload[1];
			if ((!allow_empty && count == 0) || count > MAX_UNIFIED_SLOTS || payload.size() < 2 + count)
				return std::nullopt;

			ParsedSlotRequest request;
			request.bytes_consumed = 2 + count;
			request.slots.reserve(count);
			std::array<bool, MAX_UNIFIED_SLOTS> seen = {};
			for (size_t i = 0; i < count; i++)
			{
				const u8 slot = payload[2 + i];
				if (slot >= MAX_UNIFIED_SLOTS || seen[slot])
					return std::nullopt;
				seen[slot] = true;
				request.slots.push_back(slot);
			}
			return request;
		}

		static bool IsUsablePadSlot(const u8 slot)
		{
			PadBase* const pad = (slot < Pad::NUM_CONTROLLER_PORTS) ? Pad::GetPad(slot) : nullptr;
			return pad && pad->GetType() == Pad::ControllerType::DualShock2;
		}

		static void ApplyPadState(const u8 slot, const PadStateBytes& state)
		{
			const auto [port, pad_slot] = sioConvertPadToPortAndSlot(slot);
			PadData(static_cast<int>(port), static_cast<int>(pad_slot), state).OverrideActualController();
		}

		static void NeutralizeSlots(const std::span<const u8> slots)
		{
			for (const u8 slot : slots)
			{
				if (IsUsablePadSlot(slot))
					ApplyPadState(slot, GetNeutralPadState());
			}
		}

		static std::vector<u8> ClearOverridesAndAbortStep()
		{
			std::vector<u8> released;
			{
				std::unique_lock lock(s_mutex);
				released = s_overrides.Release(std::span<const u8>());
				s_has_overrides.store(false, std::memory_order_release);
				if (s_step.status == StepStatus::Pending)
				{
					s_step.status = StepStatus::Aborted;
					s_step_condition.notify_all();
				}
			}
			return released;
		}

		static void ClearOverridesAndNeutralizeOnCPUThread()
		{
			const std::vector<u8> released = ClearOverridesAndAbortStep();
			NeutralizeSlots(released);
		}

		static bool ValidateStateOperation(const std::span<const PadStateRecord> states, const bool require_paused)
		{
			if (!VMManager::HasValidVM() || (require_paused && VMManager::GetState() != VMState::Paused) ||
				g_InputRecording.isActive())
			{
				return false;
			}

			return std::ranges::all_of(states, [](const PadStateRecord& record) { return IsUsablePadSlot(record.slot); });
		}

		static bool InstallStatesOnCPUThread(const std::span<const PadStateRecord> states, const bool require_paused)
		{
			if (!ValidateStateOperation(states, require_paused))
			{
				ClearOverridesAndNeutralizeOnCPUThread();
				return false;
			}

			{
				std::unique_lock lock(s_mutex);
				if (!s_overrides.SetStates(states))
				{
					lock.unlock();
					ClearOverridesAndNeutralizeOnCPUThread();
					return false;
				}
				s_has_overrides.store(true, std::memory_order_release);
			}

			for (const PadStateRecord& record : states)
				ApplyPadState(record.slot, record.state);
			return true;
		}

		static bool SetStatesFromServer(const std::span<const PadStateRecord> states)
		{
			bool success = false;
			Host::RunOnCPUThread([&success, states]() { success = InstallStatesOnCPUThread(states, false); }, true);
			return success;
		}

		struct StepTicket
		{
			u64 sequence;
			u32 start_frame;
		};

		static bool StartStepFromServer(const ParsedStepRequest& request, StepTicket* ticket)
		{
			bool success = false;
			Host::RunOnCPUThread(
				[&request, ticket, &success]() {
					if (!InstallStatesOnCPUThread(request.states, true))
						return;

					{
						std::unique_lock lock(s_mutex);
						if (s_step.status == StepStatus::Pending)
						{
							lock.unlock();
							ClearOverridesAndNeutralizeOnCPUThread();
							return;
						}
						s_step.sequence++;
						s_step.status = StepStatus::Pending;
						s_step.start_frame = g_FrameCount;
						s_step.end_frame = g_FrameCount;
						ticket->sequence = s_step.sequence;
						ticket->start_frame = s_step.start_frame;
					}

					VMManager::FrameAdvance(request.frame_count);
					if (VMManager::GetState() != VMState::Running)
					{
						ClearOverridesAndNeutralizeOnCPUThread();
						return;
					}
					success = true;
				},
				true);
			return success;
		}

		static bool WaitForStepFromServer(const StepTicket& ticket, u32* end_frame)
		{
			// Waiting must remain off the CPU thread so frame advance can reach auto-pause; this needs proper client runtime testing.
			std::unique_lock lock(s_mutex);
			s_step_condition.wait(lock, [&ticket]() {
				return s_step.sequence != ticket.sequence || s_step.status != StepStatus::Pending;
			});

			const bool success =
				(s_step.sequence == ticket.sequence && s_step.status == StepStatus::Complete);
			if (success)
				*end_frame = s_step.end_frame;
			if (s_step.sequence == ticket.sequence)
				s_step.status = StepStatus::Idle;
			return success;
		}

		static bool ReadStatesFromServer(const std::span<const u8> slots, std::vector<PadStateReadback>* readback)
		{
			bool success = false;
			Host::RunOnCPUThread(
				[slots, readback, &success]() {
					if (!VMManager::HasValidVM() || g_InputRecording.isActive() ||
						!std::ranges::all_of(slots, [](const u8 slot) { return IsUsablePadSlot(slot); }))
					{
						ClearOverridesAndNeutralizeOnCPUThread();
						return;
					}

					ApplyOverridesAfterInputPoll();
					readback->clear();
					readback->reserve(slots.size());
					for (const u8 slot : slots)
					{
						const auto [port, pad_slot] = sioConvertPadToPortAndSlot(slot);
						bool controlled;
						{
							std::unique_lock lock(s_mutex);
							controlled = s_overrides.IsControlled(slot);
						}
						readback->push_back({slot, controlled,
							PadData(static_cast<int>(port), static_cast<int>(pad_slot)).ToArray()});
					}
					success = true;
				},
				true);
			return success;
		}

		static bool ReleaseFromServer(const std::span<const u8> slots)
		{
			bool success = false;
			Host::RunOnCPUThread(
				[slots, &success]() {
					if (g_InputRecording.isActive())
					{
						ClearOverridesAndNeutralizeOnCPUThread();
						return;
					}

					std::vector<u8> released;
					{
						std::unique_lock lock(s_mutex);
						released = s_overrides.Release(slots);
						s_has_overrides.store(s_overrides.HasAny(), std::memory_order_release);
					}
					NeutralizeSlots(released);
					success = true;
				},
				true);
			return success;
		}

		static void HandleCommandError()
		{
			Host::RunOnCPUThread([]() { ClearOverridesAndNeutralizeOnCPUThread(); }, true);
		}

		void ApplyOverridesAfterInputPoll()
		{
			if (!s_has_overrides.load(std::memory_order_acquire))
				return;

			if (g_InputRecording.isActive())
			{
				ClearOverridesAndNeutralizeOnCPUThread();
				return;
			}

			std::vector<PadStateRecord> states;
			{
				std::unique_lock lock(s_mutex);
				for (u8 slot = 0; slot < MAX_UNIFIED_SLOTS; slot++)
				{
					if (std::optional<PadStateBytes> state = s_overrides.GetState(slot); state.has_value())
						states.push_back({slot, std::move(state.value())});
				}
			}

			if (!std::ranges::all_of(states, [](const PadStateRecord& record) { return IsUsablePadSlot(record.slot); }))
			{
				ClearOverridesAndNeutralizeOnCPUThread();
				return;
			}
			for (const PadStateRecord& record : states)
				ApplyPadState(record.slot, record.state);
		}

		void OnClientDisconnected()
		{
			std::vector<u8> released = ClearOverridesAndAbortStep();
			if (!released.empty())
			{
				Host::RunOnCPUThread([released = std::move(released)]() { NeutralizeSlots(released); });
			}
		}

		void OnVMReset()
		{
			ClearOverridesAndNeutralizeOnCPUThread();
		}

		void OnVMShutdown()
		{
			ClearOverridesAndNeutralizeOnCPUThread();
		}

		void OnVMPaused(const u32 frame_count, const bool frame_advance_completed)
		{
			std::vector<u8> released;
			{
				std::unique_lock lock(s_mutex);
				if (s_step.status != StepStatus::Pending)
					return;

				if (frame_advance_completed)
				{
					s_step.end_frame = frame_count;
					s_step.status = StepStatus::Complete;
				}
				else
				{
					s_step.status = StepStatus::Aborted;
					released = s_overrides.Release(std::span<const u8>());
					s_has_overrides.store(false, std::memory_order_release);
				}
				s_step_condition.notify_all();
			}
			NeutralizeSlots(released);
		}
	} // namespace AgentControl

	static std::thread s_thread;
	static int s_slot;

#ifdef _WIN32
	// windows claim to have support for AF_UNIX sockets but that is a blatant lie,
	// their SDK won't even run their own examples, so we go on TCP sockets.
	static SOCKET s_sock = INVALID_SOCKET;
	// the message socket used in thread's accept().
	static SOCKET s_msgsock = INVALID_SOCKET;
#else
	// absolute path of the socket. Stored in XDG_RUNTIME_DIR, if unset /tmp
	static std::string s_socket_name;
	static int s_sock = -1;
	// the message socket used in thread's accept().
	static int s_msgsock = -1;
#endif

	// Whether the socket processing thread should stop executing/is stopped.
	static std::atomic_bool s_end{true};

	/**
	 * Maximum memory used by an IPC message request.
	 * Equivalent to 50,000 Write64 requests.
	 */
#define MAX_IPC_SIZE 650000

	/**
	 * Maximum memory used by an IPC message reply.
	 * Equivalent to 50,000 Read64 replies.
	 */
#define MAX_IPC_RETURN_SIZE 450000

	/**
	 * IPC return buffer.
	 * A preallocated buffer used to store all IPC replies.
	 * to the size of 50.000 MsgWrite64 IPC calls.
	 */
	static std::vector<u8> s_ret_buffer;

	/**
	 * IPC messages buffer.
	 * A preallocated buffer used to store all IPC messages.
	 */
	static std::vector<u8> s_ipc_buffer;

	/**
	 * IPC Command messages opcodes.
	 * A list of possible operations possible by the IPC.
	 * Each one of them is what we call an "opcode" and is the first
	 * byte sent by the IPC to differentiate between commands.
	 */
	enum IPCCommand : unsigned char
	{
		MsgRead8 = 0, /**< Read 8 bit value to memory. */
		MsgRead16 = 1, /**< Read 16 bit value to memory. */
		MsgRead32 = 2, /**< Read 32 bit value to memory. */
		MsgRead64 = 3, /**< Read 64 bit value to memory. */
		MsgWrite8 = 4, /**< Write 8 bit value to memory. */
		MsgWrite16 = 5, /**< Write 16 bit value to memory. */
		MsgWrite32 = 6, /**< Write 32 bit value to memory. */
		MsgWrite64 = 7, /**< Write 64 bit value to memory. */
		MsgVersion = 8, /**< Returns PCSX2 version. */
		MsgSaveState = 9, /**< Saves a savestate. */
		MsgLoadState = 0xA, /**< Loads a savestate. */
		MsgTitle = 0xB, /**< Returns the game title. */
		MsgID = 0xC, /**< Returns the game ID. */
		MsgUUID = 0xD, /**< Returns the game UUID. */
		MsgGameVersion = 0xE, /**< Returns the game verion. */
		MsgStatus = 0xF, /**< Returns the emulator status. */
		MsgReloadPatches = 0x10, /**< Reloads patches from disk. */
		MsgScreenshot = 0x11, /**< Queues a native screenshot. */
		MsgPause = 0x12, /**< Pauses the virtual machine. */
		MsgResume = 0x13, /**< Resumes the virtual machine. */
		MsgClearExecutionCaches = 0x14, /**< Clears CPU execution caches. */
		MsgPadPulse = 0x15, /**< Pulses one DualShock 2 binding. */
		MsgAgentSetStates = static_cast<u8>(AgentControl::Opcode::SetStates), /**< Installs persistent full-pad overrides. */
		MsgAgentStep = static_cast<u8>(AgentControl::Opcode::Step), /**< Atomically installs pad states and advances frames. */
		MsgAgentGetStates = static_cast<u8>(AgentControl::Opcode::GetStates), /**< Reads effective full-pad states. */
		MsgAgentRelease = static_cast<u8>(AgentControl::Opcode::Release), /**< Releases and neutralizes pad overrides. */
		MsgUnimplemented = 0xFF /**< Unimplemented IPC message. */
	};

	/**
	 * Emulator status enum.
	 * A list of possible emulator statuses.
	 */
	enum EmuStatus : uint32_t
	{
		Running = 0, /**< Game is running */
		Paused = 1, /**< Game is paused */
		Shutdown = 2 /**< Game is shutdown */
	};

	/**
	 * IPC message buffer.
	 * A list of all needed fields to store an IPC message.
	 */
	struct IPCBuffer
	{
		int size; /**< Size of the buffer. */
		std::vector<u8> buffer; /**< Buffer. */
	};

	/**
	 * IPC result codes.
	 * A list of possible result codes the IPC can send back.
	 * Each one of them is what we call an "opcode" or "tag" and is the
	 * first byte sent by the IPC to differentiate between results.
	 */
	enum IPCResult : unsigned char
	{
		IPC_OK = 0, /**< IPC command successfully completed. */
		IPC_FAIL = 0xFF /**< IPC command failed to complete. */
	};

	// Thread used to relay IPC commands.
	void MainLoop();
	void ClientLoop();

	/**
	 * Internal function, Parses an IPC command.
	 * buf: buffer containing the IPC command.
	 * buf_size: size of the buffer announced.
	 * ret_buffer: buffer that will be used to send the reply.
	 * return value: IPCBuffer containing a buffer with the result
	 *               of the command and its size.
	 */
	static IPCBuffer ParseCommand(std::span<u8> buf, std::vector<u8>& ret_buffer, u32 buf_size);

	/**
	 * Formats an IPC buffer
	 * ret_buffer: return buffer to use.
	 * size: size of the IPC buffer.
	 * return value: buffer containing the status code allocated of size
	 */
	static std::vector<u8>& MakeOkIPC(std::vector<u8>& ret_buffer, uint32_t size);
	static std::vector<u8>& MakeFailIPC(std::vector<u8>& ret_buffer, uint32_t size);

	/**
	 * Initializes an open socket for IPC communication.
	 */
	bool AcceptClient();

	/**
	 * Converts a primitive value to bytes in little endian
	 * res_vector: the vector to modify
	 * res: the value to convert
	 * i: where to insert it into the vector
	 * NB: implicitely inlined
	 */
	template <typename T>
	static void ToResultVector(std::vector<u8>& res_vector, T res, int i)
	{
		memcpy(&res_vector[i], (char*)&res, sizeof(T));
	}

	/**
	 * Converts bytes in little endian to a primitive value
	 * span: the span to convert
	 * i: where to load it from the span
	 * return value: the converted value
	 * NB: implicitely inlined
	 */
	template <typename T>
	static T FromSpan(std::span<u8> span, int i)
	{
		return *(T*)(&span[i]);
	}

	/**
	 * Ensures an IPC message isn't too big.
	 * return value: false if checks failed, true otherwise.
	 */
	static inline bool SafetyChecks(u32 command_len, int command_size, u32 reply_len, int reply_size = 0, u32 buf_size = MAX_IPC_SIZE - 1)
	{
		return !((command_len + command_size) > buf_size ||
				 (reply_len + reply_size) >= MAX_IPC_RETURN_SIZE);
	}
} // namespace PINEServer

bool PINEServer::Initialize(int slot)
{
	s_end.store(false, std::memory_order_release);
	s_slot = slot;

#ifdef _WIN32
	if (!InitializeWinsock())
	{
		Console.WriteLn(Color_Red, "PINE: Cannot initialize winsock! Shutting down...");
		Deinitialize();
		return false;
	}

	s_sock = socket(AF_INET, SOCK_STREAM, 0);
	if ((s_sock == INVALID_SOCKET) || slot > 65536)
	{
		Console.WriteLn(Color_Red, "PINE: Cannot open socket! Shutting down...");
		Deinitialize();
		return false;
	}

	sockaddr_in server = {};
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
	server.sin_port = htons(slot);

	if (bind(s_sock, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
	{
		Console.WriteLn(Color_Red, "PINE: Error while binding to socket! Shutting down...");
		Deinitialize();
		return false;
	}

#else
	char* runtime_dir = nullptr;
#ifdef __APPLE__
	runtime_dir = std::getenv("TMPDIR");
#else
	runtime_dir = std::getenv("XDG_RUNTIME_DIR");
#endif
	// fallback in case macOS or other OSes don't implement the XDG base
	// spec
	if (runtime_dir == nullptr)
		s_socket_name = "/tmp/" PINE_EMULATOR_NAME ".sock";
	else
	{
		s_socket_name = runtime_dir;
		s_socket_name += "/" PINE_EMULATOR_NAME ".sock";
	}

	if (slot != PINE_DEFAULT_SLOT)
		s_socket_name += "." + std::to_string(slot);

	struct sockaddr_un server;

	s_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s_sock < 0)
	{
		Console.WriteLn(Color_Red, "PINE: Cannot open socket! Shutting down...");
		Deinitialize();
		return false;
	}
	server.sun_family = AF_UNIX;
	StringUtil::Strlcpy(server.sun_path, s_socket_name, sizeof(server.sun_path));

	// we unlink the socket so that when releasing this thread the socket gets
	// freed even if we didn't close correctly the loop
	unlink(s_socket_name.c_str());
	if (bind(s_sock, (struct sockaddr*)&server, sizeof(struct sockaddr_un)))
	{
		Console.WriteLn(Color_Red, "PINE: Error while binding to socket! Shutting down...");
		Deinitialize();
		return false;
	}
#endif

	// maximum queue of 4096 commands before refusing, approximated to the
	// nearest legal value. We do not use SOMAXCONN as windows have this idea
	// that a "reasonable" value is 5, which is not.
	if (listen(s_sock, 4096))
	{
		Console.WriteLn(Color_Red, "PINE: Cannot listen for connections! Shutting down...");
		Deinitialize();
		return false;
	}

	// we allocate once buffers to not have to do mallocs for each IPC
	// request, as malloc is expansive when we optimize for µs.
	s_ret_buffer.resize(MAX_IPC_RETURN_SIZE);
	s_ipc_buffer.resize(MAX_IPC_SIZE);

	// we start the thread
	s_thread = std::thread(&PINEServer::MainLoop);

	return true;
}

bool PINEServer::IsInitialized()
{
	return !s_end.load(std::memory_order_acquire);
}

int PINEServer::GetSlot()
{
	return s_slot;
}

std::vector<u8>& PINEServer::MakeOkIPC(std::vector<u8>& ret_buffer, uint32_t size = 5)
{
	ToResultVector<uint32_t>(ret_buffer, size, 0);
	ret_buffer[4] = IPC_OK;
	return ret_buffer;
}

std::vector<u8>& PINEServer::MakeFailIPC(std::vector<u8>& ret_buffer, uint32_t size = 5)
{
	ToResultVector<uint32_t>(ret_buffer, size, 0);
	ret_buffer[4] = IPC_FAIL;
	return ret_buffer;
}

bool PINEServer::AcceptClient()
{
	s_msgsock = accept(s_sock, 0, 0);
	if (s_msgsock < 0)
	{
		// everything else is non recoverable in our scope
		// we also mark as recoverable socket errors where it would block a
		// non blocking socket, even though our socket is blocking, in case
		// we ever have to implement a non blocking socket.
#ifdef _WIN32
		const int errno_w = WSAGetLastError();
		if (!(errno_w == WSAECONNRESET || errno_w == WSAEINTR || errno_w == WSAEINPROGRESS || errno_w == WSAEMFILE || errno_w == WSAEWOULDBLOCK) && s_sock != INVALID_SOCKET)
			Console.Error("PINE: accept() returned error %d", errno_w);
#else
		if (!(errno == ECONNABORTED || errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) && s_sock >= 0)
			Console.Error("PINE: accept() returned error %d", errno);
#endif

		return false;
	}

#ifdef __APPLE__
	int nosigpipe = 1;
	setsockopt(s_msgsock, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

	// Gross C-style cast, but SOCKET is a handle on Windows.
	Console.WriteLn("PINE: New client with FD %d connected.", (int)s_msgsock);
	return true;
}

void PINEServer::MainLoop()
{
	Threading::SetNameOfCurrentThread("PINE Server");

	while (!s_end.load(std::memory_order_acquire))
	{
		if (!AcceptClient())
			continue;

		ClientLoop();
		AgentControl::OnClientDisconnected();

		Console.WriteLn("PINE: Client disconnected.");
		safe_close_portable(s_msgsock);
	}
}

void PINEServer::ClientLoop()
{
	while (!s_end.load(std::memory_order_acquire))
	{
		// either int or ssize_t depending on the platform, so we have to
		// use a bunch of auto
		auto receive_length = 0;
		auto end_length = 4;
		const std::span<u8> ipc_buffer_span(s_ipc_buffer);

		// while we haven't received the entire packet, maybe due to
		// socket datagram splittage, we continue to read
		while (receive_length < end_length)
		{
			const auto tmp_length = read_portable(s_msgsock, &ipc_buffer_span[receive_length], MAX_IPC_SIZE - receive_length);

			// we recreate the socket if an error happens
			if (tmp_length <= 0)
				return;

			receive_length += tmp_length;

			// if we got at least the final size then update
			if (end_length == 4 && receive_length >= 4)
			{
				end_length = FromSpan<u32>(ipc_buffer_span, 0);
				// we'd like to avoid a client trying to do OOB
				if (end_length > MAX_IPC_SIZE || end_length < 4)
				{
					receive_length = 0;
					break;
				}
			}
		}
		PINEServer::IPCBuffer res;

		// we remove 4 bytes to get the message size out of the IPC command
		// size in ParseCommand.
		// also, if we got a failed command, let's reset the state so we don't
		// end up deadlocking by getting out of sync, eg when a client
		// disconnects
		if (receive_length != 0)
		{
			res = ParseCommand(ipc_buffer_span.subspan(4), s_ret_buffer, (u32)end_length - 4);

			// if we cannot send back our answer restart the socket
			if (write_portable(s_msgsock, res.buffer.data(), res.size) < 0)
				return;
		}
	}
}

void PINEServer::Deinitialize()
{
	s_end.store(true, std::memory_order_release);

#ifndef _WIN32
	if (!s_socket_name.empty())
	{
		unlink(s_socket_name.c_str());
		s_socket_name = {};
	}
#endif

	// shutdown() is needed, otherwise accept() will still block.
#ifdef _WIN32
	if (s_sock != INVALID_SOCKET)
		shutdown(s_sock, SD_BOTH);
#else
	if (s_sock >= 0)
		shutdown(s_sock, SHUT_RDWR);
#endif

	safe_close_portable(s_sock);
	safe_close_portable(s_msgsock);

	if (s_thread.joinable())
		s_thread.join();
}

PINEServer::IPCBuffer PINEServer::ParseCommand(std::span<u8> buf, std::vector<u8>& ret_buffer, u32 buf_size)
{
	u32 ret_cnt = 5;
	u32 buf_cnt = 0;

	while (buf_cnt < buf_size)
	{
		if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
			return IPCBuffer{5, MakeFailIPC(ret_buffer)};
		buf_cnt++;
		// example IPC messages: MsgRead/Write
		// refer to the client doc for more info on the format
		//         IPC Message event (1 byte)
		//         |  Memory address (4 byte)
		//         |  |           argument (VLE)
		//         |  |           |
		// format: XX YY YY YY YY ZZ ZZ ZZ ZZ
		//        reply code: 00 = OK, FF = NOT OK
		//        |  return value (VLE)
		//        |  |
		// reply: XX ZZ ZZ ZZ ZZ
		switch ((IPCCommand)buf[buf_cnt - 1])
		{
			case MsgRead8:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 1, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u8 res = vtlb_ramRead<mem8_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 1;
				buf_cnt += 4;
				break;
			}
			case MsgRead16:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 2, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u16 res = vtlb_ramRead<mem16_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 2;
				buf_cnt += 4;
				break;
			}
			case MsgRead32:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 4, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u32 res = vtlb_ramRead<mem32_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 4;
				buf_cnt += 4;
				break;
			}
			case MsgRead64:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 8, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u64 res = vtlb_ramRead<mem64_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 8;
				buf_cnt += 4;
				break;
			}
			case MsgWrite8:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem8_t>(a, FromSpan<u8>(buf, buf_cnt + 4));
				buf_cnt += 5;
				break;
			}
			case MsgWrite16:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 2 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem16_t>(a, FromSpan<u16>(buf, buf_cnt + 4));
				buf_cnt += 6;
				break;
			}
			case MsgWrite32:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem32_t>(a, FromSpan<u32>(buf, buf_cnt + 4));
				buf_cnt += 8;
				break;
			}
			case MsgWrite64:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 8 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem64_t>(a, FromSpan<u64>(buf, buf_cnt + 4));
				buf_cnt += 12;
				break;
			}
			case MsgVersion:
			{
				u32 size = strlen(BuildVersion::GitRev) + 7;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				snprintf(reinterpret_cast<char*>(&ret_buffer[ret_cnt]), size, "PCSX2 %s", BuildVersion::GitRev);
				ret_cnt += size;
				break;
			}
			case MsgSaveState:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				Host::RunOnCPUThread([slot = FromSpan<u8>(buf, buf_cnt)] {
					VMManager::SaveStateToSlot(slot, true, [slot](const std::string& error) {
						SaveState_ReportSaveErrorOSD(error, slot);
					});
				});
				buf_cnt += 1;
				break;
			}
			case MsgLoadState:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				Host::RunOnCPUThread([slot = FromSpan<u8>(buf, buf_cnt)] {
					Error state_error;
					if (!VMManager::LoadStateFromSlot(slot, false, &state_error))
						SaveState_ReportLoadErrorOSD(state_error.GetDescription(), slot, false);
				});
				buf_cnt += 1;
				break;
			}
			case MsgTitle:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string gameName = VMManager::GetTitle(false);
				const u32 size = gameName.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], gameName.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgID:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string gameSerial = VMManager::GetDiscSerial();
				const u32 size = gameSerial.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], gameSerial.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgUUID:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string crc = fmt::format("{:08x}", VMManager::GetDiscCRC());
				const u32 size = crc.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], crc.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgGameVersion:
			{
				if (!VMManager::HasValidVM())
					goto error;

				const std::string ElfVersion = VMManager::GetDiscVersion();
				const u32 size = ElfVersion.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], ElfVersion.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgStatus:
			{
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, 4, buf_size)) [[unlikely]]
					goto error;
				EmuStatus status;

				switch (VMManager::GetState())
				{
					case VMState::Running:
						status = EmuStatus::Running;
						break;
					case VMState::Paused:
						status = EmuStatus::Paused;
						break;
					default:
						status = EmuStatus::Shutdown;
						break;
				}

				ToResultVector(ret_buffer, status, ret_cnt);
				ret_cnt += 4;
				break;
			}
			case MsgReloadPatches:
			{
				if (!VMManager::HasValidVM())
					goto error;

				Host::RunOnCPUThread([]() {
					VMManager::ReloadPatches(true, false, true, true);
					VMManager::Internal::ClearCPUExecutionCaches();
				},
					true);
				break;
			}
			case MsgScreenshot:
			{
				if (!VMManager::HasValidVM())
					goto error;

				Host::RunOnCPUThread([]() {
					MTGS::RunOnGSThread([]() { GSQueueSnapshot(std::string(), 0); });
				},
					true);
				break;
			}
			case MsgPause:
			{
				if (!VMManager::HasValidVM())
					goto error;

				Host::RunOnCPUThread([]() { VMManager::SetPaused(true); }, true);
				break;
			}
			case MsgResume:
			{
				if (!VMManager::HasValidVM())
					goto error;

				Host::RunOnCPUThread([]() { VMManager::SetPaused(false); }, true);
				break;
			}
			case MsgClearExecutionCaches:
			{
				if (!VMManager::HasValidVM())
					goto error;

				Host::RunOnCPUThread([]() { VMManager::Internal::ClearCPUExecutionCaches(); }, true);
				break;
			}
			case MsgPadPulse:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;

				const u8 controller = FromSpan<u8>(buf, buf_cnt);
				const u8 binding = FromSpan<u8>(buf, buf_cnt + 1);
				const u16 duration_ms = FromSpan<u16>(buf, buf_cnt + 2);
				if (controller >= Pad::NUM_CONTROLLER_PORTS)
					goto error;

				PadBase* const pad = Pad::GetPad(controller);
				if (!pad ||
					pad->GetType() != Pad::ControllerType::DualShock2 ||
					binding >= PadDualshock2::Inputs::PAD_ANALOG ||
					duration_ms == 0 ||
					duration_ms > 1000)
				{
					goto error;
				}

				Host::RunOnCPUThread([controller, binding]() {
					Pad::SetControllerState(controller, binding, 1.0f);
				},
					true);
				std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
				Host::RunOnCPUThread([controller, binding]() {
					Pad::SetControllerState(controller, binding, 0.0f);
				},
					true);
				buf_cnt += 4;
				break;
			}
			case MsgAgentSetStates:
			{
				const std::optional<AgentControl::ParsedStateRequest> request =
					AgentControl::ParseStateRequest(buf.subspan(buf_cnt, buf_size - buf_cnt));
				if (!request.has_value() || !SafetyChecks(buf_cnt, static_cast<int>(request->bytes_consumed), ret_cnt, 1, buf_size) ||
					!AgentControl::SetStatesFromServer(request->states)) [[unlikely]]
				{
					AgentControl::HandleCommandError();
					goto error;
				}

				buf_cnt += static_cast<u32>(request->bytes_consumed);
				ToResultVector(ret_buffer, AgentControl::PROTOCOL_VERSION, ret_cnt);
				ret_cnt++;
				break;
			}
			case MsgAgentStep:
			{
				const std::optional<AgentControl::ParsedStepRequest> request =
					AgentControl::ParseStepRequest(buf.subspan(buf_cnt, buf_size - buf_cnt));
				if (!request.has_value() || !SafetyChecks(buf_cnt, static_cast<int>(request->bytes_consumed),
												ret_cnt, 9, buf_size)) [[unlikely]]
				{
					AgentControl::HandleCommandError();
					goto error;
				}

				AgentControl::StepTicket ticket;
				if (!AgentControl::StartStepFromServer(request.value(), &ticket)) [[unlikely]]
				{
					AgentControl::HandleCommandError();
					goto error;
				}

				u32 end_frame;
				if (!AgentControl::WaitForStepFromServer(ticket, &end_frame)) [[unlikely]]
				{
					AgentControl::HandleCommandError();
					goto error;
				}

				buf_cnt += static_cast<u32>(request->bytes_consumed);
				ToResultVector(ret_buffer, AgentControl::PROTOCOL_VERSION, ret_cnt);
				ret_cnt++;
				ToResultVector(ret_buffer, ticket.start_frame, ret_cnt);
				ret_cnt += sizeof(ticket.start_frame);
				ToResultVector(ret_buffer, end_frame, ret_cnt);
				ret_cnt += sizeof(end_frame);
				break;
			}
			case MsgAgentGetStates:
			{
				const std::optional<AgentControl::ParsedSlotRequest> request =
					AgentControl::ParseSlotRequest(buf.subspan(buf_cnt, buf_size - buf_cnt), false);
				const u32 reply_size = request.has_value() ?
				                           static_cast<u32>(2 + request->slots.size() * (2 + AgentControl::PAD_STATE_SIZE)) :
				                           0;
				if (!request.has_value() || !SafetyChecks(buf_cnt, static_cast<int>(request->bytes_consumed),
												ret_cnt, reply_size, buf_size)) [[unlikely]]
				{
					AgentControl::HandleCommandError();
					goto error;
				}

				std::vector<AgentControl::PadStateReadback> readback;
				if (!AgentControl::ReadStatesFromServer(request->slots, &readback)) [[unlikely]]
				{
					AgentControl::HandleCommandError();
					goto error;
				}

				buf_cnt += static_cast<u32>(request->bytes_consumed);
				ToResultVector(ret_buffer, AgentControl::PROTOCOL_VERSION, ret_cnt);
				ret_cnt++;
				ToResultVector(ret_buffer, static_cast<u8>(readback.size()), ret_cnt);
				ret_cnt++;
				for (const AgentControl::PadStateReadback& state : readback)
				{
					ToResultVector(ret_buffer, state.slot, ret_cnt);
					ret_cnt++;
					ToResultVector(ret_buffer, static_cast<u8>(state.controlled), ret_cnt);
					ret_cnt++;
					std::copy(state.state.begin(), state.state.end(), ret_buffer.begin() + ret_cnt);
					ret_cnt += state.state.size();
				}
				break;
			}
			case MsgAgentRelease:
			{
				const std::optional<AgentControl::ParsedSlotRequest> request =
					AgentControl::ParseSlotRequest(buf.subspan(buf_cnt, buf_size - buf_cnt), true);
				if (!request.has_value() || !SafetyChecks(buf_cnt, static_cast<int>(request->bytes_consumed), ret_cnt, 1, buf_size) ||
					!AgentControl::ReleaseFromServer(request->slots)) [[unlikely]]
				{
					AgentControl::HandleCommandError();
					goto error;
				}

				buf_cnt += static_cast<u32>(request->bytes_consumed);
				ToResultVector(ret_buffer, AgentControl::PROTOCOL_VERSION, ret_cnt);
				ret_cnt++;
				break;
			}
			default:
			{
			error:
				return IPCBuffer{5, MakeFailIPC(ret_buffer)};
			}
		}
	}
	return IPCBuffer{(int)ret_cnt, MakeOkIPC(ret_buffer, ret_cnt)};
}

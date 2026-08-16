// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <string_view>

#include "Recording/InputRecordingFile.h"
#include "Recording/InputRecordingControls.h"

enum class InputRecordingCaptureMode
{
	Full,
	Screenshots,
};

struct InputRecordingCaptureDirectories
{
	std::string savestates;
	std::string screenshots;
};

std::optional<InputRecordingCaptureMode> ParseInputRecordingCaptureMode(std::string_view value);
InputRecordingCaptureDirectories GetInputRecordingCaptureDirectories(std::string_view recording_path,
	std::string_view capture_directory, InputRecordingCaptureMode mode);

class InputRecording
{
public:
	enum class Type
	{
		POWER_ON,
		FROM_SAVESTATE
	};

	bool create(const std::string& filename, const bool fromSaveState, const std::string& authorName);
	bool play(const std::string& path, bool capture_markers = false, const std::string& capture_directory = {},
		InputRecordingCaptureMode capture_mode = InputRecordingCaptureMode::Full);
	void stop();

	static void InformGSThread();
	void handleControllerDataUpdate();
	void saveControllerData(const PadData& data, const int port, const int slot);
	std::optional<PadData> updateControllerData(const int port, const int slot);
	void incFrameCounter();
	u32 getFrameCounter() const;
	u32 getFrameCounterStateless() const;
	bool isActive() const;
	void processRecordQueue();

	void setStartingFrame(u32 startingFrame);
	u32 getStartingFrame();

	void handleExceededFrameCounter();
	void handleReset();
	void handleLoadingSavestate();
	bool isTypeSavestate() const;
	void adjustFrameCounterOnReRecord(u32 newFrameCounter);

	InputRecordingControls& getControls();
	const InputRecordingFile& getData() const;

private:
	InputRecordingControls m_controls;
	InputRecordingFile m_file;

	Type m_type;

	bool m_initial_load_complete = false;
	bool m_is_active = false;
	bool m_watching_for_rerecords = false;
	bool m_capture_markers = false;
	bool m_capture_marker_down = false;
	InputRecordingCaptureMode m_capture_mode = InputRecordingCaptureMode::Full;
	bool m_exit_on_replay_completion = false;
	u32 m_capture_index = 0;
	std::string m_capture_savestate_directory;
	std::string m_capture_snapshot_directory;

	// A consistent way to run actions at the end of the each frame (ie. stop the recording)
	std::queue<std::function<void()>> m_recordingQueue;

	u32 m_frame_counter = 0;
	u32 m_frame_counter_stateless = 0;
	// Either 0 for a power-on movie, or the g_FrameCount that is stored on the starting frame
	u32 m_starting_frame = 0;

	void initializeState();
	void closeActiveFile();
	void captureReplayMarker();
};

extern InputRecording g_InputRecording;

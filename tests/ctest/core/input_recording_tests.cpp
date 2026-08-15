// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Recording/InputRecordingControls.h"
#include "Recording/InputRecordingFile.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>

namespace
{
	class InputRecordingFileTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			m_directory = std::filesystem::current_path() /
			              ("input-recording-file-test-" +
							  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			ASSERT_TRUE(std::filesystem::create_directories(m_directory));
			m_path = (m_directory / "recording.p2m2").string();
		}

		void TearDown() override
		{
			std::error_code error;
			std::filesystem::remove_all(m_directory, error);
		}

		std::filesystem::path m_directory;
		std::string m_path;
	};
} // namespace

TEST_F(InputRecordingFileTest, AllowsConcurrentReadOnlyPlayback)
{
	const PadData pad_data(0, 0, std::array<u8, 18>{});
	{
		InputRecordingFile recording;
		ASSERT_TRUE(recording.openNew(m_path, false));
		ASSERT_TRUE(recording.writeHeader());
		ASSERT_TRUE(recording.writePadData(0, pad_data));
	}

	InputRecordingFile first_playback;
	InputRecordingFile second_playback;
	ASSERT_TRUE(first_playback.openExisting(m_path));
	EXPECT_TRUE(second_playback.openExisting(m_path));
	EXPECT_FALSE(first_playback.writePadData(0, pad_data));
}

TEST(InputRecordingControls, PreventsRecordModeForReadOnlyPlayback)
{
	InputRecordingControls controls;
	controls.setRecordModeEnabled(false);
	controls.setRecordMode(false);
	EXPECT_TRUE(controls.isReplaying());

	controls.setRecordModeEnabled(true);
	controls.setRecordMode(false);
	EXPECT_TRUE(controls.isRecording());
}

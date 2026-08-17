// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Recording/InputRecording.h"
#include "Recording/InputRecordingControls.h"
#include "Recording/InputRecordingFile.h"

#include "common/Path.h"

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

TEST(InputRecordingCapture, ParsesSupportedModes)
{
	const std::optional<InputRecordingCaptureMode> full = ParseInputRecordingCaptureMode("full");
	const std::optional<InputRecordingCaptureMode> screenshots = ParseInputRecordingCaptureMode("screenshots");
	const std::optional<InputRecordingCaptureMode> savestates = ParseInputRecordingCaptureMode("savestates");
	ASSERT_TRUE(full.has_value());
	ASSERT_TRUE(screenshots.has_value());
	ASSERT_TRUE(savestates.has_value());
	EXPECT_EQ(full.value(), InputRecordingCaptureMode::Full);
	EXPECT_EQ(screenshots.value(), InputRecordingCaptureMode::Screenshots);
	EXPECT_EQ(savestates.value(), InputRecordingCaptureMode::Savestates);
	EXPECT_FALSE(ParseInputRecordingCaptureMode("invalid").has_value());
}

TEST(InputRecordingCapture, ParsesAndNormalizesMarkerSelection)
{
	const std::optional<InputRecordingCaptureMarkerRanges> markers =
		ParseInputRecordingCaptureMarkers("9,2-5,1,4-7");
	ASSERT_TRUE(markers.has_value());
	ASSERT_EQ(markers->size(), 2u);
	EXPECT_EQ(markers->at(0).first, 1u);
	EXPECT_EQ(markers->at(0).last, 7u);
	EXPECT_EQ(markers->at(1).first, 9u);
	EXPECT_EQ(markers->at(1).last, 9u);
}

TEST(InputRecordingCapture, RejectsInvalidMarkerSelection)
{
	for (const std::string_view value : {"", "0", "1,", ",1", "1,,2", "1--2", "5-3", "a", "4294967296"})
		EXPECT_FALSE(ParseInputRecordingCaptureMarkers(value).has_value()) << value;
}

TEST(InputRecordingCapture, SelectsRequestedMarkerOrdinals)
{
	const InputRecordingCaptureMarkerRanges all_markers;
	EXPECT_TRUE(IsInputRecordingCaptureMarkerSelected(all_markers, 1));

	const std::optional<InputRecordingCaptureMarkerRanges> markers = ParseInputRecordingCaptureMarkers("2-4,7");
	ASSERT_TRUE(markers.has_value());
	EXPECT_FALSE(IsInputRecordingCaptureMarkerSelected(markers.value(), 1));
	EXPECT_TRUE(IsInputRecordingCaptureMarkerSelected(markers.value(), 2));
	EXPECT_TRUE(IsInputRecordingCaptureMarkerSelected(markers.value(), 4));
	EXPECT_FALSE(IsInputRecordingCaptureMarkerSelected(markers.value(), 5));
	EXPECT_TRUE(IsInputRecordingCaptureMarkerSelected(markers.value(), 7));
	EXPECT_FALSE(IsInputRecordingCaptureMarkerSelected(markers.value(), 8));
}

TEST(InputRecordingCapture, SelectsDirectoriesForEachMode)
{
	const std::string capture_root = Path::Combine("capture", "case");
	const InputRecordingCaptureDirectories full =
		GetInputRecordingCaptureDirectories("recording.p2m2", capture_root, InputRecordingCaptureMode::Full);
	const InputRecordingCaptureDirectories screenshots =
		GetInputRecordingCaptureDirectories("recording.p2m2", capture_root, InputRecordingCaptureMode::Screenshots);
	const InputRecordingCaptureDirectories savestates =
		GetInputRecordingCaptureDirectories("recording.p2m2", capture_root, InputRecordingCaptureMode::Savestates);

	EXPECT_EQ(full.savestates, Path::Combine(capture_root, "sstates"));
	EXPECT_EQ(full.screenshots, Path::Combine(capture_root, "screenshots"));
	EXPECT_TRUE(screenshots.savestates.empty());
	EXPECT_EQ(screenshots.screenshots, Path::Combine(capture_root, "screenshots"));
	EXPECT_EQ(savestates.savestates, Path::Combine(capture_root, "sstates"));
	EXPECT_TRUE(savestates.screenshots.empty());
}

// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "PINE.h"

#include <gtest/gtest.h>

#include <cstring>

namespace
{
	using namespace PINEServer::ReplayAnalysis;

	TEST(PINEReplayAnalysisProtocol, ParsesVersionedPhysicalVBlankStep)
	{
		std::array<u8, 6> payload = {PROTOCOL_VERSION};
		const u32 count = 37;
		std::memcpy(payload.data() + 1, &count, sizeof(count));
		payload[5] = 0xFF;

		const std::optional<ParsedStepRequest> request = ParseStepRequest(payload);
		ASSERT_TRUE(request.has_value());
		EXPECT_EQ(request->vblank_count, count);
		EXPECT_EQ(request->bytes_consumed, 5u);
	}

	TEST(PINEReplayAnalysisProtocol, RejectsInvalidPhysicalVBlankSteps)
	{
		EXPECT_FALSE(ParseStepRequest(std::array<u8, 4>{PROTOCOL_VERSION}).has_value());
		EXPECT_FALSE(ParseStepRequest(std::array<u8, 5>{PROTOCOL_VERSION}).has_value());
		EXPECT_FALSE(ParseStepRequest(std::array<u8, 5>{PROTOCOL_VERSION + 1, 1, 0, 0, 0}).has_value());
	}

	TEST(PINEReplayAnalysisProtocol, ParsesExactAbsoluteScreenshotPath)
	{
#ifdef _WIN32
		const std::string path = "C:\\captures\\frame.png";
#else
		const std::string path = "/captures/frame.png";
#endif
		std::vector<u8> payload(5 + path.size() + 1, 0xFF);
		payload[0] = PROTOCOL_VERSION;
		const u32 path_size = static_cast<u32>(path.size());
		std::memcpy(payload.data() + 1, &path_size, sizeof(path_size));
		std::copy(path.begin(), path.end(), payload.begin() + 5);

		const std::optional<ParsedScreenshotRequest> request = ParseScreenshotRequest(payload);
		ASSERT_TRUE(request.has_value());
		EXPECT_EQ(request->path, path);
		EXPECT_EQ(request->bytes_consumed, 5 + path.size());
	}

	TEST(PINEReplayAnalysisProtocol, RejectsInvalidScreenshotPaths)
	{
		EXPECT_FALSE(IsScreenshotPathValid("relative/frame.png"));
		EXPECT_FALSE(ParseScreenshotRequest(std::array<u8, 5>{PROTOCOL_VERSION}).has_value());

		std::array<u8, 8> relative = {PROTOCOL_VERSION, 3, 0, 0, 0, 'f', 'o', 'o'};
		EXPECT_FALSE(ParseScreenshotRequest(relative).has_value());
	}

	TEST(PINEReplayAnalysisState, RequiresReadOnlyReplayAndBoundedAdvance)
	{
		EXPECT_TRUE(IsReadOnlyReplay(true, false, true));
		EXPECT_FALSE(IsReadOnlyReplay(false, false, true));
		EXPECT_FALSE(IsReadOnlyReplay(true, true, false));
		EXPECT_FALSE(IsReadOnlyReplay(true, true, true));

		EXPECT_TRUE(ReplayStepFits(10, 20, 1));
		EXPECT_TRUE(ReplayStepFits(10, 20, 10));
		EXPECT_FALSE(ReplayStepFits(10, 20, 0));
		EXPECT_FALSE(ReplayStepFits(10, 20, 11));
		EXPECT_FALSE(ReplayStepFits(21, 20, 1));
	}

	TEST(PINEReplayAnalysisState, MatchesExactReplayAndPhysicalVBlankIntervals)
	{
		EXPECT_TRUE(ReplayStepIntervalsMatch({10, 13, 20, 23}, 3));
		EXPECT_FALSE(ReplayStepIntervalsMatch({10, 12, 20, 23}, 3));
		EXPECT_FALSE(ReplayStepIntervalsMatch({10, 13, 20, 22}, 3));
		EXPECT_TRUE(ReplayStepIntervalsMatch({0xFFFFFFFEu, 1, 0xFFFFFFFFu, 2}, 3));
	}
} // namespace

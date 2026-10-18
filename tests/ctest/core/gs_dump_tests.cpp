// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSDump.h"

#include <gtest/gtest.h>
#include <zstd.h>

#include <cstring>

TEST(SaveStateGSDump, FinalizesStreamWhenLastFrameFillsInputBuffer)
{
	// Exactly fill the compressor's input buffer so finalization has no input left.
	constexpr size_t raw_size = 1024 * 1024;
	constexpr size_t frame_size = sizeof(GSPrivRegSet) + 3;
	constexpr size_t state_size = raw_size - 2 * sizeof(u32) - sizeof(GSDumpHeader) - sizeof(GSPrivRegSet) - 4 * frame_size;
	std::vector<u8> state(state_size, 0x5a);
	const freezeData fd = {static_cast<int>(state.size()), state.data()};
	const GSPrivRegSet regs = {};
	std::vector<u8> compressed;
	int completions = 0;
	auto dump = GSDumpBase::CreateZstDump("GS", "", 0x12345678, 0, 0, nullptr, fd, &regs,
		[&](std::vector<u8> data) {
			completions++;
			compressed = std::move(data);
		});
	for (int frame = 0; frame < 4; frame++)
		EXPECT_FALSE(dump->VSync(frame & 1, false, &regs));
	EXPECT_EQ(completions, 0);
	dump->Complete();
	dump.reset();
	ASSERT_EQ(completions, 1);
	ASSERT_FALSE(compressed.empty());
	const size_t frame_compressed_size = ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
	ASSERT_FALSE(ZSTD_isError(frame_compressed_size)) << ZSTD_getErrorName(frame_compressed_size);
	EXPECT_EQ(frame_compressed_size, compressed.size());

	std::vector<u8> raw(raw_size);
	const size_t decompressed_size = ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
	ASSERT_FALSE(ZSTD_isError(decompressed_size)) << ZSTD_getErrorName(decompressed_size);
	ASSERT_EQ(decompressed_size, raw_size);
	const size_t state_offset = 2 * sizeof(u32) + sizeof(GSDumpHeader);
	EXPECT_EQ(std::memcmp(raw.data() + state_offset, state.data(), state.size()), 0);
	for (size_t frame = 0; frame < 4; frame++)
	{
		const size_t offset = raw_size - (4 - frame) * frame_size;
		EXPECT_EQ(raw[offset], 3);
		EXPECT_EQ(raw[offset + sizeof(regs) + 1], 1);
		EXPECT_EQ(raw[offset + sizeof(regs) + 2], frame & 1);
	}
}

TEST(SaveStateGSDump, DiscardsUnfinishedCapture)
{
	u8 state = 0;
	const freezeData fd = {1, &state};
	const GSPrivRegSet regs = {};
	bool completed = false;
	auto dump = GSDumpBase::CreateZstDump("GS", "", 0, 0, 0, nullptr, fd, &regs,
		[&](std::vector<u8>) { completed = true; });
	EXPECT_FALSE(dump->VSync(0, false, &regs));
	dump.reset();
	EXPECT_FALSE(completed);
}

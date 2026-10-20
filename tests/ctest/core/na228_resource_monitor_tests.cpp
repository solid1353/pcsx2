// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228ResourceMonitor.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace
{
	using namespace NA228ResourceMonitor;

	class NA228MonitorTest : public testing::Test
	{
	protected:
		static constexpr uint32_t DESCRIPTOR = 0x1000;
		static constexpr uint32_t BACKING = 0x2000;
		std::vector<uint8_t> ram = std::vector<uint8_t>(EE_RAM_SIZE);
		History history;

		void Write32(uint32_t address, uint32_t value)
		{
			for (uint32_t i = 0; i < 4; i++)
				ram.at(address + i) = static_cast<uint8_t>(value >> (i * 8));
		}

		void SetUp() override
		{
			Write32(0x8f3d00, 0x336f574d);
			Write32(0x8f3d08, 0x008f3d00);
			Write32(0x8f3d20, 0x2e383232);
			Write32(0x8f3d24, 0x006e6962);
			Write32(0x607388, 3 * 1024 * 1024);
			Write32(0x60738c, 4 * 1024 * 1024);
			Write32(0x607390, 123);
			Write32(0x607398, 0x1000000);
			Write32(0x60739c, 0x1800000);
			Write32(0x6073a4, 2 * 1024 * 1024);
		}

		void SetPool(uint32_t size = 64, uint32_t descriptor = DESCRIPTOR, uint32_t backing = BACKING)
		{
			Write32(0x6073b4, DESCRIPTOR);
			Write32(descriptor, 0);
			Write32(descriptor + 4, backing);
			Write32(descriptor + 8, backing);
			Write32(descriptor + 12, backing + size);
		}

		void SetBlock(uint32_t address, uint32_t size, bool used)
		{
			// Physical traversal must ignore the allocation-list link.
			Write32(address, 0xdeadbeef);
			Write32(address + 4, used ? 0x80001000 : 0);
			Write32(address + 8, size);
		}

		Pool SamplePool()
		{
			const Snapshot snapshot = Sample(ram, history);
			EXPECT_TRUE(snapshot.detected);
			EXPECT_EQ(snapshot.pools.size(), 1u);
			return snapshot.pools.empty() ? Pool{} : snapshot.pools.front();
		}
	};

	TEST_F(NA228MonitorTest, DetectsOnlyCompleteHeaderAndTerminatedName)
	{
		EXPECT_TRUE(Detect(ram));
		for (uint32_t address : {0x8f3d00u, 0x8f3d01u, 0x8f3d02u, 0x8f3d03u,
				 0x8f3d08u, 0x8f3d09u, 0x8f3d0au, 0x8f3d0bu,
				 0x8f3d20u, 0x8f3d21u, 0x8f3d22u, 0x8f3d23u,
				 0x8f3d24u, 0x8f3d25u, 0x8f3d26u, 0x8f3d27u})
		{
			ram[address] ^= 1;
			EXPECT_FALSE(Detect(ram)) << address;
			ram[address] ^= 1;
		}
		ram[0x8f3d28] = 'x';
		EXPECT_TRUE(Detect(ram));
	}

	TEST_F(NA228MonitorTest, RejectsAbsentAndTruncatedMemory)
	{
		EXPECT_FALSE(Detect({}));
		EXPECT_FALSE(Detect(std::span(ram).first(0x8f3d27)));
		EXPECT_TRUE(Detect(std::span(ram).first(0x8f3d28)));
		EXPECT_FALSE(Sample({}, history).detected);
		EXPECT_TRUE(Format(Sample({}, history)).empty());
		EXPECT_FALSE(history.lowest_largest_gap.has_value());
	}

	TEST_F(NA228MonitorTest, ReadsAndFormatsExactHeapCounters)
	{
		const Snapshot snapshot = Sample(ram, history);
		ASSERT_TRUE(snapshot.heap.readable);
		EXPECT_EQ(snapshot.heap.used, 3 * 1024 * 1024u);
		EXPECT_EQ(snapshot.heap.peak, 4 * 1024 * 1024u);
		EXPECT_EQ(snapshot.heap.allocations, 123u);
		EXPECT_EQ(snapshot.heap.arena, 8 * 1024 * 1024u);
		EXPECT_EQ(snapshot.heap.largest_gap, 2 * 1024 * 1024u);
		const auto lines = Format(snapshot);
		ASSERT_GE(lines.size(), 4u);
		EXPECT_EQ(lines[0].text, "NA228 Resource Monitor");
		EXPECT_EQ(lines[1].text, "Heap: 3.00 / 8.00 MiB | Peak: 4.00 MiB | Allocs: 123");
		EXPECT_EQ(lines[2].text, "Largest gap: 2.00 MiB");
		EXPECT_EQ(lines[3].text, "Lowest largest gap: 2.00 MiB");
	}

	TEST_F(NA228MonitorTest, TracksLowestGapAndResetsHistory)
	{
		EXPECT_EQ(Sample(ram, history).heap.lowest_largest_gap, 2 * 1024 * 1024u);
		Write32(0x6073a4, 12345);
		EXPECT_EQ(Sample(ram, history).heap.lowest_largest_gap, 12345u);
		Write32(0x6073a4, 1024 * 1024);
		EXPECT_EQ(Sample(ram, history).heap.lowest_largest_gap, 12345u);
		history = {};
		EXPECT_EQ(Sample(ram, history).heap.lowest_largest_gap, 1024 * 1024u);
	}

	TEST_F(NA228MonitorTest, ValidatesHeapArenaAndMasksPhysicalAliases)
	{
		Write32(0x607398, 0x81000000);
		Write32(0x60739c, 0xa1800000);
		EXPECT_EQ(Sample(ram, history).heap.arena, 8 * 1024 * 1024u);
		Write32(0x60739c, 0x00ffffff);
		EXPECT_FALSE(Sample(ram, history).heap.readable);
		Write32(0x60739c, EE_RAM_SIZE + 1);
		EXPECT_FALSE(Sample(ram, history).heap.readable);
		Write32(0x60739c, EE_RAM_SIZE);
		EXPECT_TRUE(Sample(ram, history).heap.readable);
		Write32(0x607398, EE_RAM_SIZE);
		EXPECT_FALSE(Sample(ram, history).heap.readable);
	}

	TEST_F(NA228MonitorTest, UsesExactPressureThresholds)
	{
		EXPECT_EQ(GapPressure(0), Pressure::Critical);
		EXPECT_EQ(GapPressure(512 * 1024 - 1), Pressure::Critical);
		EXPECT_EQ(GapPressure(512 * 1024), Pressure::Warning);
		EXPECT_EQ(GapPressure(2 * 1024 * 1024 - 1), Pressure::Warning);
		EXPECT_EQ(GapPressure(2 * 1024 * 1024), Pressure::Normal);
		EXPECT_EQ(PoolPressure(84.999), Pressure::Normal);
		EXPECT_EQ(PoolPressure(85.0), Pressure::Warning);
		EXPECT_EQ(PoolPressure(94.999), Pressure::Warning);
		EXPECT_EQ(PoolPressure(95.0), Pressure::Critical);
		Write32(0x6073a4, 512 * 1024 - 1);
		const auto lines = Format(Sample(ram, history));
		EXPECT_EQ(lines[2].pressure, Pressure::Critical);
		EXPECT_EQ(lines[3].pressure, Pressure::Critical);
	}

	TEST_F(NA228MonitorTest, WalksPhysicalBlocksWithoutAlignmentRequirement)
	{
		SetPool(51);
		SetBlock(BACKING, 17, true);
		SetBlock(BACKING + 17, 17, false);
		SetBlock(BACKING + 34, 17, true);
		const Pool pool = SamplePool();
		ASSERT_TRUE(pool.readable);
		EXPECT_EQ(pool.size, 51u);
		EXPECT_EQ(pool.used, 34u);
		EXPECT_NEAR(pool.percent, 200.0 / 3.0, 0.0001);
	}

	TEST_F(NA228MonitorTest, TracksPoolPeaksAndResetsHistory)
	{
		SetPool();
		SetBlock(BACKING, 48, true);
		SetBlock(BACKING + 48, 16, false);
		EXPECT_EQ(SamplePool().peak_percent, 75.0);
		SetBlock(BACKING, 48, false);
		EXPECT_EQ(SamplePool().percent, 0.0);
		EXPECT_EQ(SamplePool().peak_percent, 75.0);
		history = {};
		EXPECT_EQ(SamplePool().peak_percent, 0.0);
	}

	TEST_F(NA228MonitorTest, FormatsSampledPoolAndItsPressure)
	{
		SetPool(1024);
		SetBlock(BACKING, 1024, true);
		const auto lines = Format(Sample(ram, history));
		ASSERT_EQ(lines.size(), 6u);
		EXPECT_EQ(lines[4].text, "Render pools (sampled; frame-end resets can hide peaks)");
		EXPECT_EQ(lines[5].text, "Pool 1: 100.0% | 1.0 / 1.0 KiB | Peak: 100.0%");
		EXPECT_EQ(lines[5].pressure, Pressure::Critical);
	}

	TEST_F(NA228MonitorTest, WalksAllPoolsAndKeepsTheirPeaksSeparate)
	{
		SetPool();
		SetBlock(BACKING, 64, true);
		SetPool(32, DESCRIPTOR + 0x20, BACKING + 64);
		SetBlock(BACKING + 64, 32, false);
		Write32(DESCRIPTOR, DESCRIPTOR + 0x20);
		const Snapshot snapshot = Sample(ram, history);
		ASSERT_TRUE(snapshot.pool_list_readable);
		ASSERT_EQ(snapshot.pools.size(), 2u);
		EXPECT_EQ(snapshot.pools[0].used, 64u);
		EXPECT_EQ(snapshot.pools[0].peak_percent, 100.0);
		EXPECT_EQ(snapshot.pools[1].used, 0u);
		EXPECT_EQ(snapshot.pools[1].peak_percent, 0.0);
	}

	TEST_F(NA228MonitorTest, MasksPoolPointerAliases)
	{
		SetPool();
		SetBlock(BACKING, 64, true);
		Write32(0x6073b4, DESCRIPTOR | 0x80000000);
		Write32(DESCRIPTOR + 8, BACKING | 0xa0000000);
		Write32(DESCRIPTOR + 12, (BACKING + 64) | 0x80000000);
		EXPECT_TRUE(SamplePool().readable);
		EXPECT_EQ(SamplePool().used, 64u);
	}

	TEST_F(NA228MonitorTest, StopsOnUndersizedOrOversizedBlocksWithoutUpdatingPeak)
	{
		SetPool();
		for (uint32_t size : {0u, 1u, 15u, 65u, 0xffffffffu})
		{
			SetBlock(BACKING, size, true);
			EXPECT_FALSE(SamplePool().readable) << size;
			EXPECT_TRUE(history.pool_peak_percent.empty());
		}
		const auto lines = Format(Sample(ram, history));
		EXPECT_EQ(lines.back().text, "Pool 1: unreadable");
	}

	TEST_F(NA228MonitorTest, RejectsTrailingPartialHeader)
	{
		SetPool(31);
		SetBlock(BACKING, 16, false);
		EXPECT_FALSE(SamplePool().readable);
	}

	TEST_F(NA228MonitorTest, RejectsInvalidBackingAndEnd)
	{
		SetPool();
		for (uint32_t backing : {EE_RAM_SIZE, EE_RAM_SIZE + 1, 0x1fffffffu})
		{
			Write32(DESCRIPTOR + 8, backing);
			EXPECT_FALSE(SamplePool().readable);
		}
		Write32(DESCRIPTOR + 8, BACKING);
		for (uint32_t end : {BACKING - 1, EE_RAM_SIZE + 1, 0x1fffffffu})
		{
			Write32(DESCRIPTOR + 12, end);
			EXPECT_FALSE(SamplePool().readable);
		}
	}

	TEST_F(NA228MonitorTest, AcceptsExclusiveEndAtRamBoundary)
	{
		SetPool(16, DESCRIPTOR, EE_RAM_SIZE - 16);
		SetBlock(EE_RAM_SIZE - 16, 16, true);
		EXPECT_TRUE(SamplePool().readable);
		EXPECT_EQ(SamplePool().used, 16u);
		SetBlock(EE_RAM_SIZE - 16, 17, true);
		EXPECT_FALSE(SamplePool().readable);
	}

	TEST_F(NA228MonitorTest, RejectsInvalidOrTruncatedDescriptors)
	{
		for (uint32_t descriptor : {EE_RAM_SIZE, EE_RAM_SIZE - 16, 0x1fffffffu})
		{
			Write32(0x6073b4, descriptor);
			const Snapshot snapshot = Sample(ram, history);
			ASSERT_EQ(snapshot.pools.size(), 1u);
			EXPECT_FALSE(snapshot.pools[0].readable);
			EXPECT_FALSE(snapshot.pool_list_readable);
		}
	}

	TEST_F(NA228MonitorTest, StopsCyclicPoolList)
	{
		SetPool();
		SetBlock(BACKING, 64, true);
		Write32(DESCRIPTOR, DESCRIPTOR | 0x80000000);
		const Snapshot snapshot = Sample(ram, history);
		EXPECT_FALSE(snapshot.pool_list_readable);
		ASSERT_EQ(snapshot.pools.size(), 1u);
		EXPECT_TRUE(snapshot.pools[0].readable);
	}

	TEST_F(NA228MonitorTest, ContinuesToNextPoolAfterUnreadableBlocks)
	{
		SetPool();
		SetBlock(BACKING, 0, true);
		SetPool(32, DESCRIPTOR + 0x20, BACKING + 64);
		SetBlock(BACKING + 64, 32, true);
		Write32(DESCRIPTOR, DESCRIPTOR + 0x20);
		const Snapshot snapshot = Sample(ram, history);
		ASSERT_EQ(snapshot.pools.size(), 2u);
		EXPECT_FALSE(snapshot.pools[0].readable);
		EXPECT_TRUE(snapshot.pools[1].readable);
	}

	TEST_F(NA228MonitorTest, BoundsEvenAnAcyclicPoolList)
	{
		Write32(0x6073b4, DESCRIPTOR);
		for (uint32_t i = 0; i < 65537; i++)
		{
			const uint32_t address = DESCRIPTOR + i * 0x20;
			Write32(address, i == 65536 ? 0 : address + 0x20);
			Write32(address + 8, 0x400000);
			Write32(address + 12, 0x400000);
		}
		const Snapshot snapshot = Sample(ram, history);
		EXPECT_FALSE(snapshot.pool_list_readable);
		EXPECT_EQ(snapshot.pools.size(), 65536u);
	}

	TEST_F(NA228MonitorTest, Accepts65536BlocksAndStopsBeforeBlock65537)
	{
		SetPool(65536 * 16);
		for (uint32_t i = 0; i < 65537; i++)
			SetBlock(BACKING + i * 16, 16, true);
		EXPECT_TRUE(SamplePool().readable);
		EXPECT_EQ(SamplePool().used, 65536 * 16u);
		Write32(DESCRIPTOR + 12, BACKING + 65537 * 16);
		EXPECT_FALSE(SamplePool().readable);
	}

	TEST_F(NA228MonitorTest, HandlesNoPoolsAndEmptyPool)
	{
		EXPECT_TRUE(Sample(ram, history).pools.empty());
		SetPool(0);
		EXPECT_TRUE(SamplePool().readable);
		EXPECT_EQ(SamplePool().percent, 0.0);
	}

	TEST_F(NA228MonitorTest, DoesNotReadBeyond32MiBEvenWithLargerBuffer)
	{
		ram.resize(EE_RAM_SIZE + 4096);
		Write32(0x6073b4, EE_RAM_SIZE);
		const Snapshot snapshot = Sample(ram, history);
		ASSERT_EQ(snapshot.pools.size(), 1u);
		EXPECT_FALSE(snapshot.pools[0].readable);
	}

	TEST_F(NA228MonitorTest, DoesNotModifyMemoryOrHistoryForOtherBuilds)
	{
		SetPool();
		SetBlock(BACKING, 64, true);
		const auto original = ram;
		Sample(ram, history);
		EXPECT_EQ(ram, original);
		Write32(0x8f3d00, 0);
		Write32(0x6073a4, 0);
		SetBlock(BACKING, 0, true);
		const auto snapshot = Sample(ram, history);
		EXPECT_FALSE(snapshot.detected);
		EXPECT_TRUE(snapshot.pools.empty());
		EXPECT_TRUE(Format(snapshot).empty());
		EXPECT_EQ(history.lowest_largest_gap, 2 * 1024 * 1024u);
		EXPECT_EQ(history.pool_peak_percent.at(DESCRIPTOR), 100.0);
	}
} // namespace

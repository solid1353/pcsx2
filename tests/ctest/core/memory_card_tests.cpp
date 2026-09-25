// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "SIO/Memcard/MemoryCardFile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
	class VolatileMemoryCardTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			m_old_config = EmuConfig;
			m_old_mode = FileMcd_IsVolatileMode();
			m_path = std::filesystem::current_path() /
				("volatile-memory-card-test-" +
					std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ps2");
			ASSERT_FALSE(std::filesystem::exists(m_path));
			m_path_is_test_owned = true;
			FileMcd_SetVolatileMode(true);
			Select(m_path);
		}

		void TearDown() override
		{
			FileMcd_EmuClose();
			FileMcd_SetVolatileMode(m_old_mode);
			EmuConfig = std::move(m_old_config);
			std::error_code error;
			if (m_path_is_test_owned)
				std::filesystem::remove(m_path, error);
			if (!m_intermediate.empty())
				std::filesystem::remove(m_intermediate, error);
		}

		void Select(const std::filesystem::path& path)
		{
			for (Pcsx2Config::McdOptions& card : EmuConfig.Mcd)
				card.Enabled = false;
			EmuConfig.Mcd[0].Enabled = true;
			EmuConfig.Mcd[0].Filename = path.filename().string();
			EmuConfig.CurrentMemoryCardPath = path.string();
		}

		std::filesystem::path m_path;
		std::filesystem::path m_intermediate;
		Pcsx2Config m_old_config;
		bool m_old_mode = false;
		bool m_path_is_test_owned = false;
	};
} // namespace

TEST_F(VolatileMemoryCardTest, LoadsSourceAndKeepsWritesAndErasesInRAMAcrossReopens)
{
	std::vector<u8> original(2 * 528 * 16, 0xff);
	std::fill(original.begin() + 0x210, original.begin() + 0x218, 0);
	{
		std::ofstream source(m_path, std::ios::binary);
		ASSERT_TRUE(source.is_open());
		source.write(reinterpret_cast<const char*>(original.data()), original.size());
		ASSERT_TRUE(source.good());
	}

	FileMcd_SetType();
	FileMcd_EmuOpen();
	ASSERT_TRUE(FileMcd_IsPresent(0, 0));
	std::array<u8, 8> saved = {0x0f, 0x1f, 0x2f, 0x3f, 0x4f, 0x5f, 0x6f, 0x7f};
	EXPECT_EQ(FileMcd_Save(0, 0, saved.data(), 0x100, saved.size()), 1);
	std::array<u8, 8> actual = {};
	EXPECT_EQ(FileMcd_Read(0, 0, actual.data(), 0x100, actual.size()), 1);
	EXPECT_EQ(actual, saved);
	const u64 saved_crc = FileMcd_GetCRC(0, 0);
	EXPECT_NE(saved_crc, 0u);

	FileMcd_EmuClose();
	FileMcd_EmuOpen();
	EXPECT_EQ(FileMcd_Read(0, 0, actual.data(), 0x100, actual.size()), 1);
	EXPECT_EQ(actual, saved);
	EXPECT_EQ(FileMcd_GetCRC(0, 0), saved_crc);

	EXPECT_EQ(FileMcd_EraseBlock(0, 0, 0), 1);
	EXPECT_EQ(FileMcd_Read(0, 0, actual.data(), 0x100, actual.size()), 1);
	EXPECT_EQ(actual, (std::array<u8, 8>{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}));
	FileMcd_EmuClose();

	std::ifstream source(m_path, std::ios::binary);
	ASSERT_TRUE(source.is_open());
	const std::vector<u8> after((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
	EXPECT_EQ(after, original);
}

TEST_F(VolatileMemoryCardTest, CreatesMissingCardOnlyInRAM)
{
	FileMcd_SetType();
	FileMcd_EmuOpen();
	ASSERT_TRUE(FileMcd_IsPresent(0, 0));
	std::array<u8, 8> blank = {};
	EXPECT_EQ(FileMcd_Read(0, 0, blank.data(), 0, blank.size()), 1);
	EXPECT_EQ(blank, (std::array<u8, 8>{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}));
	FileMcd_EmuClose();
	EXPECT_FALSE(std::filesystem::exists(m_path));
}

TEST_F(VolatileMemoryCardTest, ConvertsNoECCCardInRAMWithoutCreatingAnIntermediateFile)
{
	m_path.replace_extension(".bin");
	m_path_is_test_owned = false;
	ASSERT_FALSE(std::filesystem::exists(m_path));
	m_path_is_test_owned = true;
	Select(m_path);
	std::vector<u8> original(1024, 0xff);
	original[0] = 0x42;
	{
		std::ofstream source(m_path, std::ios::binary);
		ASSERT_TRUE(source.is_open());
		source.write(reinterpret_cast<const char*>(original.data()), original.size());
		ASSERT_TRUE(source.good());
	}
	const std::filesystem::path intermediate = m_path.string() + "x";
	ASSERT_FALSE(std::filesystem::exists(intermediate));
	m_intermediate = intermediate;

	FileMcd_SetType();
	FileMcd_EmuOpen();
	ASSERT_TRUE(FileMcd_IsPresent(0, 0));
	u8 value = 0;
	EXPECT_EQ(FileMcd_Read(0, 0, &value, 0, 1), 1);
	EXPECT_EQ(value, 0x42);
	EXPECT_EQ(FileMcd_Read(0, 0, &value, 1055, 1), 1);
	EXPECT_EQ(FileMcd_Read(0, 0, &value, 1056, 1), 0);
	FileMcd_EmuClose();

	EXPECT_FALSE(std::filesystem::exists(intermediate));
	std::ifstream source(m_path, std::ios::binary);
	ASSERT_TRUE(source.is_open());
	const std::vector<u8> after((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
	EXPECT_EQ(after, original);
}

TEST_F(VolatileMemoryCardTest, DisconnectsFolderCards)
{
	Select(std::filesystem::current_path());
	FileMcd_SetType();
	EXPECT_FALSE(EmuConfig.Mcd[0].Enabled);
	EXPECT_EQ(EmuConfig.Mcd[0].Type, MemoryCardType::Empty);
}

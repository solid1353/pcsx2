// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Patch.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "VMManager.h"

#include "common/MemorySettingsInterface.h"
#include "common/Path.h"

#include "MockMemoryInterface.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <tuple>

// Create a test that makes sure applying a given list of patch commands results
// in a certain sequence of memory reads/writes.
#define PATCH_TEST(name, ...) \
	static void patch_test_setup_expected_calls_##name(MockMemoryInterface& ee, MockMemoryInterface& iop); \
	TEST(Patch, name) \
	{ \
		testing::StrictMock<MockMemoryInterface> ee; \
		testing::StrictMock<MockMemoryInterface> iop; \
		{ \
			testing::InSequence seq; \
			patch_test_setup_expected_calls_##name(ee, iop); \
		} \
		Patch::PatchCommand commands[]{__VA_ARGS__}; \
		std::vector<const Patch::PatchCommand*> pointers; \
		pointers.reserve(std::size(commands)); \
		for (Patch::PatchCommand& command : commands) \
			pointers.push_back(&command); \
		Patch::ApplyPatches(pointers, Patch::PPT_ONCE_ON_LOAD, ee, iop); \
		Patch::ApplyPatches(pointers, Patch::PPT_CONTINUOUSLY, ee, iop); \
		Patch::ApplyPatches(pointers, Patch::PPT_COMBINED_0_1, ee, iop); \
		Patch::ApplyPatches(pointers, Patch::PPT_ON_LOAD_OR_WHEN_ENABLED, ee, iop); \
	} \
	static void patch_test_setup_expected_calls_##name(MockMemoryInterface& ee, MockMemoryInterface& iop)

static Patch::PatchCommand BuildPatchCommand(
	Patch::patch_place_type place,
	Patch::patch_cpu_type cpu,
	u32 address,
	Patch::patch_data_type type,
	u64 data)
{
	Patch::PatchCommand command;
	command.placetopatch = place;
	command.cpu = cpu;
	command.addr = address;
	command.type = type;
	command.data = data;
	return command;
}

TEST(Patch, ParsesCheatSectionActivationMarkers)
{
	const Patch::ParsedPatchSection default_section = Patch::ParsePatchSectionName("Gameplay\\Difficulty", true);
	EXPECT_EQ(default_section.name, "Gameplay\\Difficulty");
	EXPECT_EQ(default_section.activation_mode, Patch::PatchActivationMode::GameSettings);

	const Patch::ParsedPatchSection enabled_section = Patch::ParsePatchSectionName("+Gameplay\\Skip Intro", true);
	EXPECT_EQ(enabled_section.name, "Gameplay\\Skip Intro");
	EXPECT_EQ(enabled_section.activation_mode, Patch::PatchActivationMode::ForcedEnabled);

	const Patch::ParsedPatchSection disabled_section = Patch::ParsePatchSectionName("-Gameplay\\Attract Mode", true);
	EXPECT_EQ(disabled_section.name, "Gameplay\\Attract Mode");
	EXPECT_EQ(disabled_section.activation_mode, Patch::PatchActivationMode::ForcedDisabled);
}

TEST(Patch, DoesNotParseActivationMarkersOutsideCheatsOrWithoutAName)
{
	const Patch::ParsedPatchSection patch_section = Patch::ParsePatchSectionName("+Widescreen 16:9", false);
	EXPECT_EQ(patch_section.name, "+Widescreen 16:9");
	EXPECT_EQ(patch_section.activation_mode, Patch::PatchActivationMode::GameSettings);

	const Patch::ParsedPatchSection empty_enabled_section = Patch::ParsePatchSectionName("+", true);
	EXPECT_EQ(empty_enabled_section.name, "+");
	EXPECT_EQ(empty_enabled_section.activation_mode, Patch::PatchActivationMode::GameSettings);

	const Patch::ParsedPatchSection empty_disabled_section = Patch::ParsePatchSectionName("-", true);
	EXPECT_EQ(empty_disabled_section.name, "-");
	EXPECT_EQ(empty_disabled_section.activation_mode, Patch::PatchActivationMode::GameSettings);
}

TEST(Patch, ResolvesActivationModeForRuntimeAndUI)
{
	EXPECT_FALSE(Patch::IsPatchEnabled(Patch::PatchActivationMode::GameSettings, false));
	EXPECT_TRUE(Patch::IsPatchEnabled(Patch::PatchActivationMode::GameSettings, true));
	EXPECT_TRUE(Patch::IsPatchEnabled(Patch::PatchActivationMode::ForcedEnabled, false));
	EXPECT_TRUE(Patch::IsPatchEnabled(Patch::PatchActivationMode::ForcedEnabled, true));
	EXPECT_FALSE(Patch::IsPatchEnabled(Patch::PatchActivationMode::ForcedDisabled, false));
	EXPECT_FALSE(Patch::IsPatchEnabled(Patch::PatchActivationMode::ForcedDisabled, true));

	EXPECT_TRUE(Patch::IsPatchToggleable(Patch::PatchActivationMode::GameSettings));
	EXPECT_FALSE(Patch::IsPatchToggleable(Patch::PatchActivationMode::ForcedEnabled));
	EXPECT_FALSE(Patch::IsPatchToggleable(Patch::PatchActivationMode::ForcedDisabled));
}

TEST(Patch, UsesCanonicalSectionNameForHierarchyAndSettings)
{
	const Patch::ParsedPatchSection section = Patch::ParsePatchSectionName("+Gameplay\\Skip Intro", true);
	Patch::PatchInfo info;
	info.name = section.name;
	info.activation_mode = section.activation_mode;

	EXPECT_EQ(info.name, "Gameplay\\Skip Intro");
	EXPECT_EQ(info.GetNameParentPart(), "Gameplay");
	EXPECT_EQ(info.GetNamePart(), "Skip Intro");
}

TEST(Patch, CustomPnachReplacesAutomaticPnachLoading)
{
	const std::filesystem::path test_directory = std::filesystem::current_path() /
	                                             ("patch-pnach-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	ASSERT_TRUE(std::filesystem::create_directory(test_directory));

	const std::string old_cheats_directory = EmuFolders::Cheats;
	const std::vector<std::string> old_additional_content_folders = EmuFolders::AdditionalContentFolders;
	struct Cleanup
	{
		std::filesystem::path directory;
		std::string cheats_directory;
		std::vector<std::string> additional_content_folders;
		~Cleanup()
		{
			Patch::ClearPnachOverridePath();
			EmuFolders::Cheats = std::move(cheats_directory);
			EmuFolders::AdditionalContentFolders = std::move(additional_content_folders);
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
	} cleanup{test_directory, old_cheats_directory, old_additional_content_folders};

	const std::filesystem::path automatic_path = test_directory / "SLUS-00000_12345678.pnach";
	const std::filesystem::path custom_path = test_directory / "arbitrary custom file.txt";
	{
		std::ofstream automatic(automatic_path);
		automatic << "[Automatic]\npatch=1,EE,00100000,word,00000001\n";
		std::ofstream custom(custom_path);
		custom << "[+Custom\\Always]\npatch=1,EE,00100004,word,00000002\n";
	}
	ASSERT_TRUE(std::filesystem::is_regular_file(automatic_path));
	ASSERT_TRUE(std::filesystem::is_regular_file(custom_path));

	EmuFolders::Cheats = test_directory.string();
	EmuFolders::AdditionalContentFolders.clear();
	std::vector<Patch::PatchInfo> info = Patch::GetPatchInfo("SLUS-00000", 0x12345678, true, false, nullptr);
	ASSERT_EQ(info.size(), 1u);
	EXPECT_EQ(info[0].name, "Automatic");

	EXPECT_FALSE(Patch::SetPnachOverridePath((test_directory / "missing.pnach").string()));
	ASSERT_TRUE(Patch::SetPnachOverridePath(custom_path.string()));
	EXPECT_FALSE(Patch::SetPnachOverridePath(automatic_path.string()));
	ASSERT_TRUE(Patch::GetPnachOverridePath().has_value());
	EXPECT_EQ(*Patch::GetPnachOverridePath(), custom_path.string());

	info = Patch::GetPatchInfo("SLUS-00000", 0x12345678, true, false, nullptr);
	ASSERT_EQ(info.size(), 1u);
	EXPECT_EQ(info[0].name, "Custom\\Always");
	EXPECT_EQ(info[0].activation_mode, Patch::PatchActivationMode::ForcedEnabled);
	EXPECT_TRUE(Patch::GetPatchInfo("SLUS-00000", 0x12345678, false, false, nullptr).empty());
}

TEST(Patch, DiscoversCheatsInOrderedAdditionalContentFolders)
{
	const std::filesystem::path test_directory = std::filesystem::current_path() /
	                                             ("patch-content-folders-test-" +
													 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const std::filesystem::path primary_directory = test_directory / "primary";
	const std::filesystem::path first_directory = test_directory / "first";
	const std::filesystem::path second_directory = test_directory / "second";
	ASSERT_TRUE(std::filesystem::create_directories(primary_directory));
	ASSERT_TRUE(std::filesystem::create_directories(first_directory));
	ASSERT_TRUE(std::filesystem::create_directories(second_directory));

	const std::string old_cheats_directory = EmuFolders::Cheats;
	const std::vector<std::string> old_additional_content_folders = EmuFolders::AdditionalContentFolders;
	struct Cleanup
	{
		std::filesystem::path directory;
		std::string cheats_directory;
		std::vector<std::string> additional_content_folders;
		~Cleanup()
		{
			EmuFolders::Cheats = std::move(cheats_directory);
			EmuFolders::AdditionalContentFolders = std::move(additional_content_folders);
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
	} cleanup{test_directory, old_cheats_directory, old_additional_content_folders};

	for (const auto& [directory, name, address] :
		{std::tuple{primary_directory, "Primary", "00100000"}, std::tuple{first_directory, "First", "00100004"},
			std::tuple{second_directory, "Second", "00100008"}})
	{
		std::ofstream file(directory / "SLUS-00001_12345678.pnach");
		file << '[' << name << "]\npatch=1,EE," << address << ",word,00000001\n";
	}

	EmuFolders::Cheats = primary_directory.string();
	EmuFolders::AdditionalContentFolders = {first_directory.string(), second_directory.string(), first_directory.string()};
	const std::vector<std::string> search_folders = EmuFolders::GetContentSearchFolders(EmuFolders::Cheats);
	ASSERT_EQ(search_folders.size(), 3u);
	EXPECT_EQ(search_folders[0], primary_directory.string());
	EXPECT_EQ(search_folders[1], first_directory.string());
	EXPECT_EQ(search_folders[2], second_directory.string());

	const std::vector<Patch::PatchInfo> info = Patch::GetPatchInfo("SLUS-00001", 0x12345678, true, false, nullptr);
	ASSERT_EQ(info.size(), 3u);
	EXPECT_EQ(info[0].name, "Primary");
	EXPECT_EQ(info[1].name, "First");
	EXPECT_EQ(info[2].name, "Second");
}

TEST(Patch, ResolvesCheatsAndGameSettingsByConfiguredContentAlias)
{
	const std::filesystem::path test_directory = std::filesystem::current_path() /
	                                             ("content-alias-test-" +
													 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const std::filesystem::path primary_directory = test_directory / "primary";
	const std::filesystem::path additional_directory = test_directory / "additional";
	const std::filesystem::path nested_directory = additional_directory / "NUN5";
	ASSERT_TRUE(std::filesystem::create_directories(primary_directory));
	ASSERT_TRUE(std::filesystem::create_directories(nested_directory));

	const std::string old_cheats_directory = EmuFolders::Cheats;
	const std::string old_game_settings_directory = EmuFolders::GameSettings;
	const std::vector<std::string> old_additional_content_folders = EmuFolders::AdditionalContentFolders;
	const std::vector<std::pair<std::string, std::string>> old_content_aliases = EmuFolders::ContentAliases;
	struct Cleanup
	{
		std::filesystem::path directory;
		std::string cheats_directory;
		std::string game_settings_directory;
		std::vector<std::string> additional_content_folders;
		std::vector<std::pair<std::string, std::string>> content_aliases;
		~Cleanup()
		{
			EmuFolders::Cheats = std::move(cheats_directory);
			EmuFolders::GameSettings = std::move(game_settings_directory);
			EmuFolders::AdditionalContentFolders = std::move(additional_content_folders);
			EmuFolders::ContentAliases = std::move(content_aliases);
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
	} cleanup{test_directory, old_cheats_directory, old_game_settings_directory,
		old_additional_content_folders, old_content_aliases};

	MemorySettingsInterface settings;
	settings.SetStringValue("ContentAliases", "SLES-55605", "NUN5_Fallback");
	settings.SetStringValue("ContentAliases", "SLES-55605_C071D4C1", "NUN5");
	settings.SetStringValue("ContentAliases", "SLUS-INVALID", "../invalid");
	EmuFolders::LoadContentAliases(settings);
	EXPECT_EQ(EmuFolders::GetContentAlias("sles-55605", 0xC071D4C1), "NUN5");
	EXPECT_EQ(EmuFolders::GetContentAlias("SLES-55605", 0x12345678), "NUN5_Fallback");
	EXPECT_TRUE(EmuFolders::GetContentAlias("SLUS-INVALID", 0).empty());

	const std::filesystem::path alias_cheat = nested_directory / "NUN5.pnach";
	const std::filesystem::path serial_cheat = primary_directory / "SLES-55605_C071D4C1.pnach";
	std::ofstream(alias_cheat) << "[Alias]\npatch=1,EE,00100000,word,00000001\n";
	std::ofstream(serial_cheat) << "[Serial]\npatch=1,EE,00100004,word,00000002\n";
	const std::filesystem::path alias_settings = nested_directory / "NUN5.ini";
	const std::filesystem::path serial_settings = primary_directory / "SLES-55605_C071D4C1.ini";
	std::ofstream(alias_settings) << "[EmuCore]\nEnableCheats = true\n";
	std::ofstream(serial_settings) << "[EmuCore]\nEnableCheats = false\n";

	EmuFolders::Cheats = primary_directory.string();
	EmuFolders::GameSettings = primary_directory.string();
	EmuFolders::AdditionalContentFolders = {additional_directory.string()};
	const std::vector<Patch::PatchInfo> info = Patch::GetPatchInfo("SLES-55605", 0xC071D4C1, true, false, nullptr);
	ASSERT_EQ(info.size(), 1u);
	EXPECT_EQ(info[0].name, "Alias");
	EXPECT_EQ(Patch::GetPnachFilename("SLES-55605", 0xC071D4C1, true),
		(primary_directory / "NUN5.pnach").string());
	EXPECT_EQ(VMManager::GetGameSettingsPath("SLES-55605", 0xC071D4C1), alias_settings.string());
	EXPECT_TRUE(VMManager::GetGameSettingsSectionPrefix("SLES-55605", 0xC071D4C1).empty());

	std::filesystem::remove(alias_settings);
	EXPECT_EQ(VMManager::GetGameSettingsPath("SLES-55605", 0xC071D4C1),
		(primary_directory / "NUN5.ini").string());
}

TEST(Patch, ResolvesGameSettingsAndRecordingPlaybackAcrossContentFolders)
{
	const std::filesystem::path test_directory = std::filesystem::current_path() /
	                                             ("shared-content-folders-test-" +
													 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const std::filesystem::path primary_directory = test_directory / "primary";
	const std::filesystem::path first_directory = test_directory / "first";
	const std::filesystem::path second_directory = test_directory / "second";
	ASSERT_TRUE(std::filesystem::create_directories(primary_directory));
	ASSERT_TRUE(std::filesystem::create_directories(first_directory));
	ASSERT_TRUE(std::filesystem::create_directories(second_directory / "nested"));

	const std::string old_game_settings_directory = EmuFolders::GameSettings;
	const std::string old_input_recordings_directory = EmuFolders::InputRecordings;
	const std::vector<std::string> old_additional_content_folders = EmuFolders::AdditionalContentFolders;
	struct Cleanup
	{
		std::filesystem::path directory;
		std::string game_settings_directory;
		std::string input_recordings_directory;
		std::vector<std::string> additional_content_folders;
		~Cleanup()
		{
			EmuFolders::GameSettings = std::move(game_settings_directory);
			EmuFolders::InputRecordings = std::move(input_recordings_directory);
			EmuFolders::AdditionalContentFolders = std::move(additional_content_folders);
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
	} cleanup{test_directory, old_game_settings_directory, old_input_recordings_directory, old_additional_content_folders};

	const std::filesystem::path first_settings = first_directory / "SLUS-00002.ini";
	const std::filesystem::path second_settings = second_directory / "nested" / "SLUS-00002_12345678.ini";
	std::ofstream(first_settings) << "[EmuCore]\nEnableCheats = true\n";
	std::ofstream(second_settings) << "[EmuCore]\nEnableCheats = false\n";
	const std::filesystem::path first_recording = first_directory / "replay.p2m2";
	const std::filesystem::path second_recording = second_directory / "replay.p2m2";
	std::ofstream(first_recording) << "first";
	std::ofstream(second_recording) << "second";

	EmuFolders::GameSettings = primary_directory.string();
	EmuFolders::InputRecordings = primary_directory.string();
	EmuFolders::AdditionalContentFolders = {first_directory.string(), second_directory.string()};
	EXPECT_EQ(VMManager::GetGameSettingsPath("SLUS-00002", 0x12345678), first_settings.string());
	EXPECT_EQ(EmuFolders::FindFileInContentFolders(EmuFolders::InputRecordings, "replay.p2m2"), first_recording.string());

	const std::filesystem::path primary_settings = primary_directory / "SLUS-00002_12345678.ini";
	const std::filesystem::path primary_recording = primary_directory / "replay.p2m2";
	std::ofstream(primary_settings) << "[EmuCore]\nEnableCheats = true\n";
	std::ofstream(primary_recording) << "primary";
	EXPECT_EQ(VMManager::GetGameSettingsPath("SLUS-00002", 0x12345678), primary_settings.string());
	EXPECT_EQ(EmuFolders::FindFileInContentFolders(EmuFolders::InputRecordings, "replay.p2m2"), primary_recording.string());

	std::filesystem::remove(primary_settings);
	std::filesystem::remove(first_settings);
	EXPECT_EQ(VMManager::GetGameSettingsPath("SLUS-00002", 0x12345678), second_settings.string());
	std::filesystem::remove(second_settings);
	EXPECT_EQ(VMManager::GetGameSettingsPath("SLUS-00002", 0x12345678), primary_settings.string());
	EXPECT_EQ(EmuFolders::FindFileInContentFolders(EmuFolders::InputRecordings, "missing.p2m2"),
		Path::Combine(EmuFolders::InputRecordings, "missing.p2m2"));
}

TEST(MemoryCard, ResolvesAndManagesCardsAcrossContentFolders)
{
	const std::filesystem::path test_directory = std::filesystem::current_path() /
	                                             ("memory-card-content-folders-test-" +
													 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const std::filesystem::path primary_directory = test_directory / "primary";
	const std::filesystem::path first_directory = test_directory / "first";
	const std::filesystem::path second_directory = test_directory / "second";
	ASSERT_TRUE(std::filesystem::create_directories(primary_directory));
	ASSERT_TRUE(std::filesystem::create_directories(first_directory));
	ASSERT_TRUE(std::filesystem::create_directories(second_directory));

	const std::string old_memory_cards_directory = EmuFolders::MemoryCards;
	const std::vector<std::string> old_additional_content_folders = EmuFolders::AdditionalContentFolders;
	struct Cleanup
	{
		std::filesystem::path directory;
		std::string memory_cards_directory;
		std::vector<std::string> additional_content_folders;
		~Cleanup()
		{
			EmuFolders::MemoryCards = std::move(memory_cards_directory);
			EmuFolders::AdditionalContentFolders = std::move(additional_content_folders);
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
	} cleanup{test_directory, old_memory_cards_directory, old_additional_content_folders};

	const auto create_card = [](const std::filesystem::path& path) {
		std::ofstream file(path, std::ios::binary);
		file.seekp((1024 * 1024) - 1);
		file.put('\0');
	};
	const std::filesystem::path primary_duplicate = primary_directory / "duplicate.ps2";
	const std::filesystem::path first_duplicate = first_directory / "duplicate.ps2";
	const std::filesystem::path external_card = first_directory / "external.ps2";
	create_card(primary_duplicate);
	create_card(first_duplicate);
	create_card(external_card);
	ASSERT_TRUE(std::filesystem::is_regular_file(primary_duplicate));
	ASSERT_TRUE(std::filesystem::is_regular_file(first_duplicate));
	ASSERT_TRUE(std::filesystem::is_regular_file(external_card));

	EmuFolders::MemoryCards = primary_directory.string();
	EmuFolders::AdditionalContentFolders = {first_directory.string(), second_directory.string()};

	Pcsx2Config config;
	config.Mcd[0].Filename = "external.ps2";
	EXPECT_EQ(config.FullpathToMcd(0), external_card.string());
	config.Mcd[0].Filename = "missing.ps2";
	EXPECT_EQ(config.FullpathToMcd(0), (primary_directory / "missing.ps2").string());

	const std::vector<AvailableMcdInfo> cards = FileMcd_GetAvailableCards(true);
	ASSERT_EQ(cards.size(), 2u);
	const auto duplicate = std::find_if(cards.begin(), cards.end(),
		[](const AvailableMcdInfo& card) { return card.name == "duplicate.ps2"; });
	ASSERT_NE(duplicate, cards.end());
	EXPECT_EQ(duplicate->path, primary_duplicate.string());
	const auto external = std::find_if(cards.begin(), cards.end(),
		[](const AvailableMcdInfo& card) { return card.name == "external.ps2"; });
	ASSERT_NE(external, cards.end());
	EXPECT_EQ(external->path, external_card.string());

	const std::optional<AvailableMcdInfo> external_info = FileMcd_GetCardInfo("external.ps2");
	ASSERT_TRUE(external_info.has_value());
	EXPECT_EQ(external_info->path, external_card.string());
	EXPECT_FALSE(FileMcd_RenameCard("external.ps2", "duplicate.ps2"));
	EXPECT_TRUE(FileMcd_RenameCard("external.ps2", "renamed.ps2"));
	const std::filesystem::path renamed_card = first_directory / "renamed.ps2";
	EXPECT_TRUE(std::filesystem::is_regular_file(renamed_card));
	EXPECT_TRUE(FileMcd_DeleteCard("renamed.ps2"));
	EXPECT_FALSE(std::filesystem::exists(renamed_card));

	const std::filesystem::path folder_card = second_directory / "folder.ps2";
	ASSERT_TRUE(std::filesystem::create_directory(folder_card));
	config.Mcd[0].Filename = "folder.ps2";
	EXPECT_EQ(config.FullpathToMcd(0), folder_card.string());
}

TEST(Patch, ValidatesAndPreservesCommandLinePnachLines)
{
	struct Cleanup
	{
		~Cleanup() { Patch::ClearPnachLines(); }
	} cleanup;

	Patch::ClearPnachLines();
	EXPECT_TRUE(Patch::AddPnachLine(" patch=1,EE,00100000,word,00000001 // first "));
	EXPECT_TRUE(Patch::AddPnachLine("patch=1,EE,00100000,word,00000002"));
	EXPECT_TRUE(Patch::AddPnachLine("gsaspectratio=16:9"));

	const std::vector<std::string>& lines = Patch::GetPnachLines();
	ASSERT_EQ(lines.size(), 3u);
	EXPECT_EQ(lines[0], "patch=1,EE,00100000,word,00000001 ");
	EXPECT_EQ(lines[1], "patch=1,EE,00100000,word,00000002");
	EXPECT_EQ(lines[2], "gsaspectratio=16:9");

	EXPECT_FALSE(Patch::AddPnachLine(""));
	EXPECT_FALSE(Patch::AddPnachLine("// comment"));
	EXPECT_FALSE(Patch::AddPnachLine("[Section]"));
	EXPECT_FALSE(Patch::AddPnachLine("crc=12345678"));
	EXPECT_FALSE(Patch::AddPnachLine("patch=1,EE,not-an-address,word,00000001"));
	EXPECT_FALSE(Patch::AddPnachLine("unknown=1"));
	EXPECT_FALSE(Patch::AddPnachLine("patch=1,EE,00100004,word,00000003\npatch=1,EE,00100008,word,00000004"));
	EXPECT_EQ(Patch::GetPnachLines().size(), 3u);
}

// *****************************************************************************
// Writes
// *****************************************************************************

PATCH_TEST(Byte,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::BYTE_T, 0x12))
{
	ee.ExpectIdempotentWrite8(0x00100000, 0, 0x12);
}

PATCH_TEST(Short,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::SHORT_T, 0x1234))
{
	ee.ExpectIdempotentWrite16(0x00100000, 0, 0x1234);
}

PATCH_TEST(Word,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::WORD_T, 0x12345678))
{
	ee.ExpectIdempotentWrite32(0x00100000, 0, 0x12345678);
}

PATCH_TEST(Double,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::DOUBLE_T, 0x123456789acdef12))
{
	ee.ExpectIdempotentWrite64(0x00100000, 0, 0x123456789acdef12);
}

PATCH_TEST(BigEndianShort,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::SHORT_BE_T, 0x1234))
{
	ee.ExpectIdempotentWrite16(0x00100000, 0, 0x3412);
}

PATCH_TEST(BigEndianWord,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::WORD_BE_T, 0x12345678))
{
	ee.ExpectIdempotentWrite32(0x00100000, 0, 0x78563412);
}

PATCH_TEST(BigEndianDouble,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::DOUBLE_BE_T, 0xabcdef0123456789))
{
	ee.ExpectIdempotentWrite64(0x00100000, 0, 0x8967452301efcdab);
}

PATCH_TEST(IOPByte,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_IOP, 0x00100000, Patch::BYTE_T, 0x12))
{
	iop.ExpectIdempotentWrite8(0x00100000, 0, 0x12);
}

PATCH_TEST(IOPShort,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_IOP, 0x00100000, Patch::SHORT_T, 0x1234))
{
	iop.ExpectIdempotentWrite16(0x00100000, 0, 0x1234);
}

PATCH_TEST(IOPWord,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_IOP, 0x00100000, Patch::WORD_T, 0x12345678))
{
	iop.ExpectIdempotentWrite32(0x00100000, 0, 0x12345678);
}

// *****************************************************************************
// Writes (Extended)
// *****************************************************************************

PATCH_TEST(Extended8BitWrite,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00100000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead8(0x00100000, 0);
	ee.ExpectWrite8(0x00100000, 0x12);
}

PATCH_TEST(Extended16BitWrite,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x10100000, Patch::EXTENDED_T, 0x00001234))
{
	ee.ExpectRead16(0x00100000, 0);
	ee.ExpectWrite16(0x00100000, 0x1234);
}

PATCH_TEST(Extended32BitWrite,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20100000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectWrite32(0x00100000, 0x12345678);
}

// *****************************************************************************
// Increments/Decrements (Extended)
// *****************************************************************************

PATCH_TEST(Extended8BitIncrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30000012, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30000012, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead8(0x00100000, 0x00);
	ee.ExpectWrite8(0x00100000, 0x12);
	ee.ExpectRead8(0x00100000, 0x12);
	ee.ExpectWrite8(0x00100000, 0x24);
}

PATCH_TEST(Extended8BitIncrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30000012, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30000012, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead8(0x00100000, 0xee);
	ee.ExpectWrite8(0x00100000, 0x00);
	ee.ExpectRead8(0x00100000, 0x00);
	ee.ExpectWrite8(0x00100000, 0x12);
}

PATCH_TEST(Extended8BitDecrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30100012, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30100012, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead8(0x00100000, 0x24);
	ee.ExpectWrite8(0x00100000, 0x12);
	ee.ExpectRead8(0x00100000, 0x12);
	ee.ExpectWrite8(0x00100000, 0x00);
}

PATCH_TEST(Extended8BitDecrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30100012, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30100012, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead8(0x00100000, 0x12);
	ee.ExpectWrite8(0x00100000, 0x00);
	ee.ExpectRead8(0x00100000, 0x00);
	ee.ExpectWrite8(0x00100000, 0xee);
}

PATCH_TEST(Extended16BitIncrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30201234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30201234, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead16(0x00100000, 0x0000);
	ee.ExpectWrite16(0x00100000, 0x1234);
	ee.ExpectRead16(0x00100000, 0x1234);
	ee.ExpectWrite16(0x00100000, 0x2468);
}

PATCH_TEST(Extended16BitIncrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30201234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30201234, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead16(0x00100000, 0xedcc);
	ee.ExpectWrite16(0x00100000, 0x0000);
	ee.ExpectRead16(0x00100000, 0x0000);
	ee.ExpectWrite16(0x00100000, 0x1234);
}

PATCH_TEST(Extended16BitDecrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30301234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30301234, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead16(0x00100000, 0x2468);
	ee.ExpectWrite16(0x00100000, 0x1234);
	ee.ExpectRead16(0x00100000, 0x1234);
	ee.ExpectWrite16(0x00100000, 0x0000);
}

PATCH_TEST(Extended16BitDecrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30301234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30301234, Patch::EXTENDED_T, 0x00100000))
{
	ee.ExpectRead16(0x00100000, 0x1234);
	ee.ExpectWrite16(0x00100000, 0x0000);
	ee.ExpectRead16(0x00100000, 0x0000);
	ee.ExpectWrite16(0x00100000, 0xedcc);
}

PATCH_TEST(Extended32BitIncrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30400000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30400000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead32(0x00100000, 0x00000000);
	ee.ExpectWrite32(0x00100000, 0x12345678);
	ee.ExpectRead32(0x00100000, 0x12345678);
	ee.ExpectWrite32(0x00100000, 0x2468acf0);
}

PATCH_TEST(Extended32BitIncrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30400000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30400000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead32(0x00100000, 0xedcba988);
	ee.ExpectWrite32(0x00100000, 0x00000000);
	ee.ExpectRead32(0x00100000, 0x00000000);
	ee.ExpectWrite32(0x00100000, 0x12345678);
}

PATCH_TEST(Extended32BitDecrement,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30500000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30500000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead32(0x00100000, 0x2468acf0);
	ee.ExpectWrite32(0x00100000, 0x12345678);
	ee.ExpectRead32(0x00100000, 0x12345678);
	ee.ExpectWrite32(0x00100000, 0x00000000);
}

PATCH_TEST(Extended32BitDecrementWrapping,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30500000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x30500000, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead32(0x00100000, 0x12345678);
	ee.ExpectWrite32(0x00100000, 0x00000000);
	ee.ExpectRead32(0x00100000, 0x00000000);
	ee.ExpectWrite32(0x00100000, 0xedcba988);
}

// *****************************************************************************
// Serial Write (Extended)
// *****************************************************************************

PATCH_TEST(ExtendedSerialWriteZero,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x40100000, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000000, Patch::EXTENDED_T, 0x00000000))
{
}

PATCH_TEST(ExtendedSerialWriteOnce,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x40100000, Patch::EXTENDED_T, 0x00010000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x11111111))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectWrite32(0x00100000, 0x12345678);
}

PATCH_TEST(ExtendedSerialWriteContiguous,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x40100000, Patch::EXTENDED_T, 0x00020001),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x11111111))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectWrite32(0x00100000, 0x12345678);
	ee.ExpectRead32(0x00100004, 0);
	ee.ExpectWrite32(0x00100004, 0x23456789);
}

PATCH_TEST(ExtendedSerialWriteStrided,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x40100000, Patch::EXTENDED_T, 0x00020002),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x12345678, Patch::EXTENDED_T, 0x11111111))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectWrite32(0x00100000, 0x12345678);
	ee.ExpectRead32(0x00100008, 0);
	ee.ExpectWrite32(0x00100008, 0x23456789);
}

// *****************************************************************************
// Copy bytes (Extended)
// *****************************************************************************

PATCH_TEST(ExtendedCopyBytes0,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x50100000, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000000))
{
}

PATCH_TEST(ExtendedCopyBytes2,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x50100000, Patch::EXTENDED_T, 0x00000002),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead8(0x00100000, 0x12);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
	ee.ExpectRead8(0x00100001, 0x12);
	ee.ExpectRead8(0x00200001, 0);
	ee.ExpectWrite8(0x00200001, 0x12);
}

// *****************************************************************************
// Pointer write (Extended)
// *****************************************************************************

PATCH_TEST(ExtendedPointerWrite8,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000001, Patch::EXTENDED_T, 0x00000004))
{
	ee.ExpectRead32(0x00100000, 0x00200000);
	ee.ExpectRead8(0x00200004, 0);
	ee.ExpectWrite8(0x00200004, 0x12);
}

PATCH_TEST(ExtendedPointerWrite16,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x00001234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00010001, Patch::EXTENDED_T, 0x00000004))
{
	ee.ExpectRead32(0x00100000, 0x00200000);
	ee.ExpectRead16(0x00200004, 0);
	ee.ExpectWrite16(0x00200004, 0x1234);
}

PATCH_TEST(ExtendedPointerWrite32,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020001, Patch::EXTENDED_T, 0x00000004))
{
	ee.ExpectRead32(0x00100000, 0x00200000);
	ee.ExpectRead32(0x00200004, 0);
	ee.ExpectWrite32(0x00200004, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteMultiEven,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020002, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x00000000))
{
	ee.ExpectRead32(0x00100000, 0x00200000);
	ee.ExpectRead32(0x00200004, 0x00300000);
	ee.ExpectRead32(0x00300008, 0);
	ee.ExpectWrite32(0x00300008, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteMultiOdd,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020003, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x0000000c))
{
	ee.ExpectRead32(0x00100000, 0x00200000);
	ee.ExpectRead32(0x00200004, 0x00300000);
	ee.ExpectRead32(0x00300008, 0x00400000);
	ee.ExpectRead32(0x0040000c, 0);
	ee.ExpectWrite32(0x0040000c, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsNullSingle,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020001, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

// There was previously a bug where if the pointer write command was split over
// three lines or more, if the first pointer was null it would interpret the
// middle of the pointer write command as the start of a new command.
PATCH_TEST(ExtendedPointerWriteSkipsFirstNullEven,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020002, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x0fffffff, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsFirstNullOdd,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020003, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsMiddleNullEven,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020004, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x0000000c),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x0fffffff, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0x00110000);
	ee.ExpectRead32(0x00110004, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsMiddleNullOdd,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020005, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x0000000c),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000010, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0x00110000);
	ee.ExpectRead32(0x00110004, 0x00120000);
	ee.ExpectRead32(0x00120008, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsLastNullEven,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020002, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x0fffffff, Patch::EXTENDED_T, 0x00000000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0x00110000);
	ee.ExpectRead32(0x00110004, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

PATCH_TEST(ExtendedPointerWriteSkipsLastNullOdd,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x60100000, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00020003, Patch::EXTENDED_T, 0x00000004),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00000008, Patch::EXTENDED_T, 0x0fffffff),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20200000, Patch::EXTENDED_T, 0x12345678),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x20300000, Patch::EXTENDED_T, 0x12345678))
{
	ee.ExpectRead32(0x00100000, 0x00110000);
	ee.ExpectRead32(0x00110004, 0x00120000);
	ee.ExpectRead32(0x00120008, 0);
	ee.ExpectIdempotentWrite32(0x00200000, 0, 0x12345678);
	ee.ExpectIdempotentWrite32(0x00300000, 0, 0x12345678);
}

// *****************************************************************************
// Boolean operation (Extended)
// *****************************************************************************

PATCH_TEST(ExtendedBooleanOr8,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead8(0x00100000, 0x78);
	ee.ExpectWrite8(0x00100000, 0x7a);
}

PATCH_TEST(ExtendedBooleanOr16,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00101234))
{
	ee.ExpectRead16(0x00100000, 0x89ab);
	ee.ExpectWrite16(0x00100000, 0x9bbf);
}

PATCH_TEST(ExtendedBooleanAnd8,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00200012))
{
	ee.ExpectRead8(0x00100000, 0x34);
	ee.ExpectWrite8(0x00100000, 0x10);
}

PATCH_TEST(ExtendedBooleanAnd16,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00301234))
{
	ee.ExpectRead16(0x00100000, 0x5678);
	ee.ExpectWrite16(0x00100000, 0x1230);
}

PATCH_TEST(ExtendedBooleanXor8,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00400012))
{
	ee.ExpectRead8(0x00100000, 0x89);
	ee.ExpectWrite8(0x00100000, 0x9b);
}

PATCH_TEST(ExtendedBooleanXor16,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x70100000, Patch::EXTENDED_T, 0x00501234))
{
	ee.ExpectRead16(0x00100000, 0x89ab);
	ee.ExpectWrite16(0x00100000, 0x9b9f);
}

// *****************************************************************************
// Do multi-lines if conditional (Extended)
// *****************************************************************************

PATCH_TEST(ExtendedConditional8BitEqualTrue,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01010012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead8(0x00100000, 0x12);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditional8BitEqualFalse,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01010012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead8(0x00100000, 0x21);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditionalEqualTrue,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01001234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x1234);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditionalEqualFalse,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01001234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x4321);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditionalNotEqualTrue,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01101234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x4321);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
}

PATCH_TEST(ExtendedConditionalNotEqualFalse,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01101234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x1234);
}

PATCH_TEST(ExtendedConditionalLessThanTrue,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01101234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x4321);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
}

PATCH_TEST(ExtendedConditionalLessThanFalse,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0x01101234),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x1234);
}

PATCH_TEST(ExtendedConditionalECodeEqualTrue,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xe0011234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x1234);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditionalECodeEqualFalse,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xe0011234, Patch::EXTENDED_T, 0x00100000),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012),
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0x00300000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead16(0x00100000, 0x4321);
	ee.ExpectRead8(0x00300000, 0);
	ee.ExpectWrite8(0x00300000, 0x12);
}

PATCH_TEST(ExtendedConditionalResetSkipCount,
	BuildPatchCommand(Patch::PPT_ONCE_ON_LOAD, Patch::CPU_EE, 0xd0100000, Patch::EXTENDED_T, 0xff010012),
	BuildPatchCommand(Patch::PPT_CONTINUOUSLY, Patch::CPU_EE, 0x00200000, Patch::EXTENDED_T, 0x00000012))
{
	ee.ExpectRead8(0x00100000, 0xab);
	ee.ExpectRead8(0x00200000, 0);
	ee.ExpectWrite8(0x00200000, 0x12);
}

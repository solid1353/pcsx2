// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "Input/InputManager.h"
#include "LayeredSettingsInterface.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "common/MemorySettingsInterface.h"

#include <gtest/gtest.h>

#include <optional>

namespace
{
	class KeyboardPadChordTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			m_old_config = EmuConfig;
			for (auto& port : EmuConfig.Pad.Ports)
				port.Type = Pad::ControllerType::NotConnected;
			EmuConfig.Pad.Ports[0].Type = Pad::ControllerType::DualShock2;
			EmuConfig.Pad.Ports[1].Type = Pad::ControllerType::DualShock2;
			ConfigureBindings();
			m_layered_settings.SetLayer(LayeredSettingsInterface::LAYER_BASE, &m_base_settings);
			Pad::LoadConfig(m_settings);
			InputManager::ReloadBindings(m_layered_settings, m_settings, m_settings, false, false);
			const auto* info = Pad::GetControllerInfo(Pad::ControllerType::DualShock2);
			m_up_bind = info->bindings[*info->GetBindIndex("Up")].bind_index;
		}

		void TearDown() override
		{
			MemorySettingsInterface empty_settings;
			LayeredSettingsInterface layered_settings;
			layered_settings.SetLayer(LayeredSettingsInterface::LAYER_BASE, &empty_settings);
			InputManager::ReloadBindings(layered_settings, empty_settings, empty_settings, false, false);
			Pad::Shutdown();
			EmuConfig = std::move(m_old_config);
		}

		virtual void ConfigureBindings()
		{
			m_settings.SetStringList("Pad1", "Up", {"Keyboard/W"});
			m_settings.SetStringList("Pad2", "Up", {"Keyboard/Shift & Keyboard/W"});
		}

		void Key(u32 code, float value)
		{
			InputBindingKey key = {};
			key.source_type = InputSourceType::Keyboard;
			key.data = code;
			ASSERT_TRUE(InputManager::InvokeEvents(key, value));
		}

		void Press(const char* binding, float value)
		{
			const std::optional<InputBindingKey> key = InputManager::ParseInputBindingKey(binding);
			ASSERT_TRUE(key.has_value());
			ASSERT_TRUE(InputManager::InvokeEvents(key.value(), value));
		}

		float PadUp(u8 pad) const { return Pad::GetPad(pad)->GetEffectiveInput(m_up_bind); }

		Pcsx2Config m_old_config;
		MemorySettingsInterface m_base_settings;
		MemorySettingsInterface m_settings;
		LayeredSettingsInterface m_layered_settings;
		u32 m_up_bind = 0;
	};

	// Pointer buttons stand in for controller buttons: both reach pad bindings
	// through the non-keyboard path. HoldTurbo does nothing without a VM.
	class ControllerHotkeyChordTest : public KeyboardPadChordTest
	{
	protected:
		void ConfigureBindings() override
		{
			m_settings.SetStringList("Pad1", "Up", {"Pointer-0/Button0"});
			m_settings.SetStringList("Hotkeys", "HoldTurbo", {"Pointer-0/Button1 & Pointer-0/Button0"});
		}
	};
} // namespace

TEST_F(KeyboardPadChordTest, ShiftAloneDoesNotPressPadTwoAndCompletedChordSuppressesPadOne)
{
	Key(16, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 0.0f);

	Key(87, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 255.0f);
	Key(87, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);

	Key(16, 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 255.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 0.0f);
	Key(87, 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
}

TEST_F(KeyboardPadChordTest, ChordCancelsAnAlreadyHeldPlainKeyAndReleasesWhenEitherKeyLifts)
{
	Key(87, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 255.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 0.0f);

	Key(16, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 255.0f);

	Key(87, 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 0.0f);
	Key(87, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 255.0f);
	Key(16, 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 255.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 0.0f);
	Key(87, 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
}

TEST_F(ControllerHotkeyChordTest, HeldHotkeyChordKeepsItsPadInputReleased)
{
	Press("Pointer-0/Button1", 1.0f);
	Press("Pointer-0/Button0", 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	Press("Pointer-0/Button0", 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	Press("Pointer-0/Button0", 0.0f);
	Press("Pointer-0/Button1", 0.0f);

	Press("Pointer-0/Button0", 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 255.0f);
	Press("Pointer-0/Button0", 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
}

TEST_F(ControllerHotkeyChordTest, CompletedHotkeyChordReleasesAnAlreadyHeldPadInput)
{
	Press("Pointer-0/Button0", 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 255.0f);
	Press("Pointer-0/Button1", 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	Press("Pointer-0/Button0", 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
}
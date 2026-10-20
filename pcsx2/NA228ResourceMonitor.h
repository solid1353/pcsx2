// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace NA228ResourceMonitor
{
	inline constexpr uint32_t EE_RAM_SIZE = 32 * 1024 * 1024;

	enum class Pressure
	{
		Normal,
		Warning,
		Critical
	};

	struct Heap
	{
		bool readable = false;
		uint32_t used = 0;
		uint32_t peak = 0;
		uint32_t allocations = 0;
		uint32_t arena = 0;
		uint32_t largest_gap = 0;
		uint32_t lowest_largest_gap = 0;
	};

	struct Pool
	{
		uint32_t address = 0;
		bool readable = false;
		uint32_t used = 0;
		uint32_t size = 0;
		double percent = 0.0;
		double peak_percent = 0.0;
	};

	// Owned by the GS thread, cleared on every toggle-on.
	struct History
	{
		std::optional<uint32_t> lowest_largest_gap;
		std::unordered_map<uint32_t, double> pool_peak_percent;
	};

	struct Snapshot
	{
		bool detected = false;
		Heap heap;
		bool pool_list_readable = true;
		std::vector<Pool> pools;
	};

	struct Line
	{
		std::string text;
		Pressure pressure = Pressure::Normal;
	};

	// The span must refer directly to EE RAM and remain valid throughout the call.
	// Callers must check VM lifetime before supplying live memory. No guest accessors are used.
	bool Detect(std::span<const uint8_t> ram);
	Snapshot Sample(std::span<const uint8_t> ram, History& history);
	std::vector<Line> Format(const Snapshot& snapshot);
	Pressure GapPressure(uint32_t bytes);
	Pressure PoolPressure(double percent);
} // namespace NA228ResourceMonitor

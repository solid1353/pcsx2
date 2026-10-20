// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NA228ResourceMonitor.h"

#include "fmt/format.h"

#include <algorithm>
#include <unordered_set>

namespace NA228ResourceMonitor
{
	static constexpr uint32_t PHYSICAL_MASK = 0x1fffffff;
	static constexpr uint32_t MAX_BLOCKS = 65536;
	static constexpr uint32_t MAX_POOLS = 65536;

	class Memory
	{
	public:
		explicit Memory(std::span<const uint8_t> ram)
			: m_ram(ram.first(std::min<size_t>(ram.size(), EE_RAM_SIZE)))
		{
		}

		bool Range(uint32_t address, uint32_t size) const
		{
			const uint32_t physical = address & PHYSICAL_MASK;
			return physical <= m_ram.size() && size <= m_ram.size() - physical;
		}

		bool Read32(uint32_t address, uint32_t& value) const
		{
			if (!Range(address, 4))
				return false;

			// Live EE memory can change between reads. Volatile keeps each sample's loads
			// explicit; it does not turn the sample into a synchronized guest snapshot.
			const volatile uint8_t* bytes = m_ram.data() + (address & PHYSICAL_MASK);
			value = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
			return true;
		}

	private:
		std::span<const uint8_t> m_ram;
	};

	static bool Detect(const Memory& memory)
	{
		uint32_t magic, load_address, name_start, name_end;
		return memory.Read32(0x008f3d00, magic) && magic == 0x336f574d &&
		       memory.Read32(0x008f3d08, load_address) && load_address == 0x008f3d00 &&
		       memory.Read32(0x008f3d20, name_start) && name_start == 0x2e383232 &&
		       memory.Read32(0x008f3d24, name_end) && name_end == 0x006e6962;
	}

	bool Detect(std::span<const uint8_t> ram)
	{
		return Detect(Memory(ram));
	}

	static Heap ReadHeap(const Memory& memory, History& history)
	{
		Heap heap;
		uint32_t base, end;
		if (!memory.Read32(0x00607388, heap.used) || !memory.Read32(0x0060738c, heap.peak) ||
			!memory.Read32(0x00607390, heap.allocations) || !memory.Read32(0x00607398, base) ||
			!memory.Read32(0x0060739c, end) || !memory.Read32(0x006073a4, heap.largest_gap))
		{
			return heap;
		}

		base &= PHYSICAL_MASK;
		end &= PHYSICAL_MASK;
		if (!memory.Range(base, 1) || !memory.Range(end, 0) || end < base)
			return heap;

		heap.readable = true;
		heap.arena = end - base;
		history.lowest_largest_gap = std::min(history.lowest_largest_gap.value_or(heap.largest_gap), heap.largest_gap);
		heap.lowest_largest_gap = *history.lowest_largest_gap;
		return heap;
	}

	static Pool ReadPool(const Memory& memory, uint32_t address, History& history)
	{
		Pool pool;
		pool.address = address;
		uint32_t backing, end;
		if (!memory.Range(address, 0x11) || !memory.Read32(address + 8, backing) || !memory.Read32(address + 12, end))
			return pool;

		backing &= PHYSICAL_MASK;
		end &= PHYSICAL_MASK;
		if (!memory.Range(backing, 1) || !memory.Range(end, 0) || end < backing)
			return pool;

		pool.size = end - backing;
		uint32_t blocks = 0;
		for (uint32_t block = backing; block < end;)
		{
			uint32_t owner, total;
			if (++blocks > MAX_BLOCKS || end - block < 0x10 || !memory.Range(block, 0x10) ||
				!memory.Read32(block + 4, owner) || !memory.Read32(block + 8, total) ||
				total < 0x10 || total > end - block)
			{
				return pool;
			}
			if (owner != 0)
				pool.used += total;
			block += total;
		}

		pool.readable = true;
		pool.percent = pool.size ? 100.0 * pool.used / pool.size : 0.0;
		double& peak = history.pool_peak_percent[address];
		peak = std::max(peak, pool.percent);
		pool.peak_percent = peak;
		return pool;
	}

	Snapshot Sample(std::span<const uint8_t> ram, History& history)
	{
		const Memory memory(ram);
		Snapshot snapshot;
		snapshot.detected = Detect(memory);
		if (!snapshot.detected)
			return snapshot;

		snapshot.heap = ReadHeap(memory, history);
		uint32_t address;
		if (!memory.Read32(0x006073b4, address))
		{
			snapshot.pool_list_readable = false;
			return snapshot;
		}

		std::unordered_set<uint32_t> visited;
		while (address != 0)
		{
			address &= PHYSICAL_MASK;
			if (visited.size() >= MAX_POOLS || !visited.insert(address).second)
			{
				snapshot.pool_list_readable = false;
				break;
			}

			snapshot.pools.push_back(ReadPool(memory, address, history));
			if (!memory.Range(address, 0x11) || !memory.Read32(address, address))
			{
				snapshot.pool_list_readable = false;
				break;
			}
		}
		return snapshot;
	}

	Pressure GapPressure(uint32_t bytes)
	{
		return bytes < 512 * 1024 ? Pressure::Critical : (bytes < 2 * 1024 * 1024 ? Pressure::Warning : Pressure::Normal);
	}

	Pressure PoolPressure(double percent)
	{
		return percent >= 95.0 ? Pressure::Critical : (percent >= 85.0 ? Pressure::Warning : Pressure::Normal);
	}

	std::vector<Line> Format(const Snapshot& snapshot)
	{
		if (!snapshot.detected)
			return {};

		std::vector<Line> lines = {{"NA228 Resource Monitor"}};
		const Heap& heap = snapshot.heap;
		if (heap.readable)
		{
			constexpr double mib = 1024.0 * 1024.0;
			lines.push_back({fmt::format("Heap: {:.2f} / {:.2f} MiB | Peak: {:.2f} MiB | Allocs: {}", heap.used / mib, heap.arena / mib, heap.peak / mib, heap.allocations)});
			lines.push_back({fmt::format("Largest gap: {:.2f} MiB", heap.largest_gap / mib), GapPressure(heap.largest_gap)});
			lines.push_back({fmt::format("Lowest largest gap: {:.2f} MiB", heap.lowest_largest_gap / mib), GapPressure(heap.lowest_largest_gap)});
		}
		else
		{
			lines.push_back({"Heap: unreadable"});
		}

		lines.push_back({"Render pools (sampled; frame-end resets can hide peaks)"});
		for (size_t i = 0; i < snapshot.pools.size(); i++)
		{
			const Pool& pool = snapshot.pools[i];
			if (pool.readable)
			{
				lines.push_back({fmt::format("Pool {}: {:.1f}% | {:.1f} / {:.1f} KiB | Peak: {:.1f}%", i + 1, pool.percent, pool.used / 1024.0, pool.size / 1024.0, pool.peak_percent), PoolPressure(pool.percent)});
			}
			else
			{
				lines.push_back({fmt::format("Pool {}: unreadable", i + 1)});
			}
		}
		if (!snapshot.pool_list_readable)
			lines.push_back({"Render pool list: unreadable"});
		else if (snapshot.pools.empty())
			lines.push_back({"No render pools"});
		return lines;
	}
} // namespace NA228ResourceMonitor

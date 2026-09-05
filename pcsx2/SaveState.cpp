// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Achievements.h"
#include "BuildVersion.h"
#include "CDVD/CDVD.h"
#include "COP0.h"
#include "Cache.h"
#include "Config.h"
#include "Counters.h"
#include "DebugTools/Breakpoints.h"
#include "DebugTools/SymbolImporter.h"
#include "Elfheader.h"
#include "GS.h"
#include "GS/GS.h"
#include "Host.h"
#include "MTGS.h"
#include "MTVU.h"
#include "Patch.h"
#include "R3000A.h"
#include "SIO/Multitap/MultitapProtocol.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Sio.h"
#include "SIO/Sio0.h"
#include "SIO/Sio2.h"
#include "SPU2/spu2.h"
#include "SaveState.h"
#include "StateWrapper.h"
#include "USB/USB.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "ps2/BiosTools.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"

#include "IconsFontAwesome.h"
#include "fmt/format.h"

#include <csetjmp>
#include <png.h>

using namespace R5900;

static tlbs s_tlb_backup[std::size(tlb)];

static void PreLoadPrep()
{
	// ensure everything is in sync before we start overwriting stuff.
	if (THREAD_VU1)
		vu1Thread.WaitVU();
	MTGS::WaitGS(false);

	// backup current TLBs, since we're going to overwrite them all
	std::memcpy(s_tlb_backup, tlb, sizeof(s_tlb_backup));

	// clear protected pages, since we don't want to fault loading EE memory
	mmap_ResetBlockTracking();

	VMManager::Internal::ClearCPUExecutionCaches();
}

static void PostLoadPrep()
{
	resetCache();
	//	WriteCP0Status(cpuRegs.CP0.n.Status.val);
	for (int i = 0; i < 48; i++)
	{
		if (std::memcmp(&s_tlb_backup[i], &tlb[i], sizeof(tlbs)) != 0)
		{
			UnmapTLB(s_tlb_backup[i], i);
			MapTLB(tlb[i], i);
		}
	}

	if (EmuConfig.Gamefixes.GoemonTlbHack)
		GoemonPreloadTlb();
	CBreakPoints::SetSkipFirst(BREAKPOINT_EE, 0);
	CBreakPoints::SetSkipFirst(BREAKPOINT_IOP, 0);

	UpdateVSyncRate(true);

	if (VMManager::Internal::HasBootedELF())
		R5900SymbolImporter.OnElfLoadedInMemory();
}

// --------------------------------------------------------------------------------------
//  SaveStateBase  (implementations)
// --------------------------------------------------------------------------------------
SaveStateBase::SaveStateBase(VmStateBuffer& memblock)
	: m_memory(memblock)
	, m_version(g_SaveVersion)
{
}

void SaveStateBase::PrepBlock(int size)
{
	if (m_error)
		return;

	const int end = m_idx + size;
	if (IsSaving())
	{
		if (static_cast<u32>(end) >= m_memory.size())
			m_memory.resize(static_cast<u32>(end));
	}
	else
	{
		if (m_memory.size() < static_cast<u32>(end))
		{
			Console.Error("(SaveStateBase) Buffer overflow in PrepBlock(), expected %d got %zu", end, m_memory.size());
			m_error = true;
		}
	}
}

bool SaveStateBase::FreezeTag(const char* src)
{
	if (m_error)
		return false;

	char tagspace[32];
	pxAssertMsg(std::strlen(src) < (sizeof(tagspace) - 1), "Tag name exceeds the allowed length");

	std::memset(tagspace, 0, sizeof(tagspace));
	StringUtil::Strlcpy(tagspace, src, sizeof(tagspace));
	Freeze(tagspace);

	if (std::strcmp(tagspace, src) != 0)
	{
		Console.Error(fmt::format("Savestate data corruption detected while reading tag: {}", src));
		m_error = true;
		return false;
	}

	return true;
}

bool SaveStateBase::FreezeBios()
{
	if (!FreezeTag("BIOS"))
		return false;

	// Check the BIOS, and issue a warning if the bios for this state
	// doesn't match the bios currently being used (chances are it'll still
	// work fine, but some games are very picky).

	u32 bioscheck = BiosChecksum;
	char biosdesc[256];
	std::memset(biosdesc, 0, sizeof(biosdesc));
	StringUtil::Strlcpy(biosdesc, BiosDescription, sizeof(biosdesc));

	Freeze(bioscheck);
	Freeze(biosdesc);

	if (bioscheck != BiosChecksum)
	{
		Console.Error("\n  Warning: BIOS Version Mismatch, savestate may be unstable!");
		Console.Error(
			"    Current BIOS:   %s (crc=0x%08x)\n"
			"    Savestate BIOS: %s (crc=0x%08x)\n",
			BiosDescription.c_str(), BiosChecksum,
			biosdesc, bioscheck);
	}

	return IsOkay();
}

bool SaveStateBase::FreezeInternals(Error* error)
{
	// Print this until the MTVU problem in gifPathFreeze is taken care of (rama)
	if (THREAD_VU1)
		Console.Warning("MTVU speedhack is enabled, saved states may not be stable");

	if (!vmFreeze())
		return false;

	// Second Block - Various CPU Registers and States
	// -----------------------------------------------
	if (!FreezeTag("cpuRegs"))
		return false;

	Freeze(cpuRegs); // cpu regs + COP0
	Freeze(psxRegs); // iop regs
	Freeze(fpuRegs);
	Freeze(tlb); // tlbs
	Freeze(cachedTlbs); // cached tlbs
	Freeze(AllowParams1); //OSDConfig written (Fast Boot)
	Freeze(AllowParams2);

	// Third Block - Cycle Timers and Events
	// -------------------------------------
	if (!FreezeTag("Cycles"))
		return false;

	Freeze(EEsCycle);
	Freeze(EEoCycle);
	Freeze(nextDeltaCounter);
	Freeze(nextStartCounter);
	Freeze(psxNextStartCounter);
	Freeze(psxNextDeltaCounter);

	// Fourth Block - EE-related systems
	// ---------------------------------
	if (!FreezeTag("EE-Subsystems"))
		return false;

	bool okay = rcntFreeze();
	okay = okay && memFreeze(error);
	okay = okay && gsFreeze();
	okay = okay && vuMicroFreeze();
	okay = okay && vuJITFreeze();
	okay = okay && vif0Freeze();
	okay = okay && vif1Freeze();
	okay = okay && sifFreeze();
	okay = okay && ipuFreeze();
	okay = okay && ipuDmaFreeze();
	okay = okay && gifFreeze();
	okay = okay && gifDmaFreeze();
	okay = okay && sprFreeze();
	okay = okay && mtvuFreeze();
	if (!okay)
		return false;

	// Fifth Block - iop-related systems
	// ---------------------------------
	if (!FreezeTag("IOP-Subsystems"))
		return false;

	FreezeMem(iopMem->Sif, sizeof(iopMem->Sif)); // iop's sif memory (not really needed, but oh well)

	okay = okay && psxRcntFreeze();

	// TODO: move all the others over to StateWrapper too...
	if (!okay)
		return false;
	{
		// This is horrible. We need to move the rest over...
		std::optional<StateWrapper::VectorMemoryStream> save_stream;
		std::optional<StateWrapper::ReadOnlyMemoryStream> load_stream;
		if (IsSaving())
			save_stream.emplace();
		else
			load_stream.emplace(&m_memory[m_idx], static_cast<int>(m_memory.size()) - m_idx);

		StateWrapper sw(IsSaving() ? static_cast<StateWrapper::IStream*>(&save_stream.value()) :
									 static_cast<StateWrapper::IStream*>(&load_stream.value()),
			IsSaving() ? StateWrapper::Mode::Write : StateWrapper::Mode::Read, g_SaveVersion);

		okay = okay && g_Sio0.DoState(sw);
		okay = okay && g_Sio2.DoState(sw);
		okay = okay && g_MultitapArr.at(0).DoState(sw);
		okay = okay && g_MultitapArr.at(1).DoState(sw);

		if (!okay || !sw.IsGood())
			return false;

		if (IsSaving())
		{
			FreezeMem(const_cast<u8*>(save_stream->GetBuffer().data()), save_stream->GetPosition());
		}
		else
		{
			const int new_idx = m_idx + static_cast<int>(load_stream->GetPosition());
			if (static_cast<size_t>(new_idx) >= m_memory.size())
				return false;

			m_idx = new_idx;
		}
	}

	okay = okay && cdrFreeze();
	okay = okay && cdvdFreeze();

	// technically this is HLE BIOS territory, but we don't have enough such stuff
	// to merit an HLE Bios sub-section... yet.
	okay = okay && deci2Freeze();

	okay = okay && InputRecordingFreeze();

	okay = okay && handleFreeze(); //file handles

	return okay;
}


// --------------------------------------------------------------------------------------
//  memSavingState (implementations)
// --------------------------------------------------------------------------------------
// uncompressed to/from memory state saves implementation

memSavingState::memSavingState(VmStateBuffer& save_to)
	: SaveStateBase(save_to)
{
}

// Saving of state data
void memSavingState::FreezeMem(void* data, int size)
{
	if (!size)
		return;

	const int new_size = m_idx + size;
	if (static_cast<u32>(new_size) > m_memory.size())
		m_memory.resize(static_cast<u32>(new_size));

	std::memcpy(&m_memory[m_idx], data, size);
	m_idx += size;
}

// --------------------------------------------------------------------------------------
//  memLoadingState  (implementations)
// --------------------------------------------------------------------------------------
memLoadingState::memLoadingState(const VmStateBuffer& load_from)
	: SaveStateBase(const_cast<VmStateBuffer&>(load_from))
{
}

// Loading of state data from a memory buffer...
void memLoadingState::FreezeMem(void* data, int size)
{
	if (static_cast<u32>(m_idx + size) > m_memory.size())
		m_error = true;

	if (m_error)
	{
		std::memset(data, 0, size);
		return;
	}

	const u8* const src = &m_memory[m_idx];
	m_idx += size;
	std::memcpy(data, src, size);
}

static const char* EntryFilename_StateVersion = "PCSX2 Savestate Version.id";
static const char* EntryFilename_Screenshot = "Screenshot.png";
static const char* EntryFilename_InternalStructures = "PCSX2 Internal Structures.dat";
static constexpr u32 STATE_PCSX2_VERSION_SIZE = 32;

struct SaveStateVersionIndicator
{
	u32 save_version;
	char version[STATE_PCSX2_VERSION_SIZE];
};

struct SysState_Component
{
	const char* name;
	int (*freeze)(FreezeAction, freezeData*);
};

static int SysState_MTGSFreeze(FreezeAction mode, freezeData* fP)
{
	MTGS::FreezeData sstate = {fP, 0};
	MTGS::Freeze(mode, sstate);
	return sstate.retval;
}

static constexpr SysState_Component SPU2_{"SPU2", SPU2freeze};
static constexpr SysState_Component GS{"GS", SysState_MTGSFreeze};

static bool SysState_ComponentFreezeIn(const std::optional<std::vector<u8>>& data, SysState_Component comp)
{
	if (!data.has_value())
		return true;

	freezeData fP = {0, nullptr};
	if (comp.freeze(FreezeAction::Size, &fP) != 0)
		fP.size = 0;

	Console.WriteLn("  Loading %s", comp.name);

	std::unique_ptr<u8[]> buffer;
	if (fP.size > 0)
	{
		buffer = std::make_unique<u8[]>(fP.size);
		fP.data = buffer.get();

		if (data->size() != static_cast<size_t>(fP.size))
		{
			Console.Error(fmt::format("* {}: Save data has the wrong size", comp.name));
			return false;
		}

		std::memcpy(fP.data, data->data(), data->size());
	}

	if (comp.freeze(FreezeAction::Load, &fP) != 0)
	{
		Console.Error(fmt::format("* {}: Failed to load freeze data", comp.name));
		return false;
	}

	return true;
}

static bool SysState_ComponentFreezeOut(SaveStateBase& writer, SysState_Component comp)
{
	freezeData fP = {};
	if (comp.freeze(FreezeAction::Size, &fP) != 0)
	{
		Console.Error(fmt::format("* {}: Failed to get freeze size", comp.name));
		return false;
	}

	if (fP.size == 0)
		return true;

	const int size = fP.size;
	writer.PrepBlock(size);

	Console.WriteLn("  Saving %s", comp.name);

	fP.data = writer.GetBlockPtr();
	if (comp.freeze(FreezeAction::Save, &fP) != 0)
	{
		Console.Error(fmt::format("* {}: Failed to save freeze data", comp.name));
		return false;
	}

	writer.CommitBlock(size);
	return true;
}

static bool SysState_ComponentFreezeInNew(
	const std::optional<std::vector<u8>>& data, const char* name, bool (*do_state_func)(StateWrapper&))
{
	const std::span<const u8> bytes = data.has_value() ? std::span<const u8>(*data) : std::span<const u8>();
	StateWrapper::ReadOnlyMemoryStream stream(bytes.data(), bytes.size());
	StateWrapper sw(&stream, StateWrapper::Mode::Read, g_SaveVersion);

	return do_state_func(sw);
}

static bool SysState_ComponentFreezeOutNew(SaveStateBase& writer, const char* name, u32 reserve, bool (*do_state_func)(StateWrapper&))
{
	StateWrapper::VectorMemoryStream stream(reserve);
	StateWrapper sw(&stream, StateWrapper::Mode::Write, g_SaveVersion);

	if (!do_state_func(sw))
		return false;

	const int size = static_cast<int>(stream.GetBuffer().size());
	if (size > 0)
	{
		writer.PrepBlock(size);
		std::memcpy(writer.GetBlockPtr(), stream.GetBuffer().data(), size);
		writer.CommitBlock(size);
	}

	return true;
}

// --------------------------------------------------------------------------------------
//  BaseSavestateEntry
// --------------------------------------------------------------------------------------
class BaseSavestateEntry
{
protected:
	BaseSavestateEntry() = default;

public:
	virtual ~BaseSavestateEntry() = default;

	virtual const char* GetFilename() const = 0;
	virtual bool FreezeIn(const std::optional<std::vector<u8>>& data) const = 0;
	virtual bool FreezeOut(SaveStateBase& writer) const = 0;
	virtual bool IsRequired() const = 0;
};

class MemorySavestateEntry : public BaseSavestateEntry
{
protected:
	MemorySavestateEntry() {}
	virtual ~MemorySavestateEntry() = default;

public:
	virtual bool FreezeIn(const std::optional<std::vector<u8>>& data) const;
	virtual bool FreezeOut(SaveStateBase& writer) const;
	virtual bool IsRequired() const { return true; }

protected:
	virtual u8* GetDataPtr() const = 0;
	virtual u32 GetDataSize() const = 0;
};

bool MemorySavestateEntry::FreezeIn(const std::optional<std::vector<u8>>& data) const
{
	const u32 expectedSize = GetDataSize();
	const size_t bytes_read = data.has_value() ? std::min<size_t>(data->size(), expectedSize) : 0;
	if (bytes_read > 0)
		std::memcpy(GetDataPtr(), data->data(), bytes_read);
	if (bytes_read != expectedSize)
	{
		Console.WriteLn(Color_Yellow, " '%s' is incomplete (expected 0x%x bytes, loading only 0x%x bytes)",
			GetFilename(), expectedSize, static_cast<u32>(bytes_read));
	}

	return true;
}

bool MemorySavestateEntry::FreezeOut(SaveStateBase& writer) const
{
	writer.FreezeMem(GetDataPtr(), GetDataSize());
	return writer.IsOkay();
}

// --------------------------------------------------------------------------------------
//  SavestateEntry_* (EmotionMemory, IopMemory, etc)
// --------------------------------------------------------------------------------------
// Implementation Rationale:
//  The address locations of PS2 virtual memory components is fully dynamic, so we need to
//  resolve the pointers at the time they are requested (eeMem, iopMem, etc).  Thusly, we
//  cannot use static struct member initializers -- we need virtual functions that compute
//  and resolve the addresses on-demand instead... --air

class SavestateEntry_EmotionMemory final : public MemorySavestateEntry
{
public:
	~SavestateEntry_EmotionMemory() override = default;

	const char* GetFilename() const override { return "eeMemory.bin"; }
	u8* GetDataPtr() const override { return eeMem->Main; }
	uint GetDataSize() const override { return Ps2MemSize::ExposedRam; }

	virtual bool FreezeIn(const std::optional<std::vector<u8>>& data) const override
	{
		return MemorySavestateEntry::FreezeIn(data);
	}
};

class SavestateEntry_IopMemory final : public MemorySavestateEntry
{
public:
	~SavestateEntry_IopMemory() override = default;

	const char* GetFilename() const override { return "iopMemory.bin"; }
	u8* GetDataPtr() const override { return iopMem->Main; }
	uint GetDataSize() const override { return Ps2MemSize::ExposedIopRam; }
};

class SavestateEntry_HwRegs final : public MemorySavestateEntry
{
public:
	~SavestateEntry_HwRegs() override = default;

	const char* GetFilename() const override { return "eeHwRegs.bin"; }
	u8* GetDataPtr() const override { return eeHw; }
	uint GetDataSize() const override { return sizeof(eeHw); }
};

class SavestateEntry_IopHwRegs final : public MemorySavestateEntry
{
public:
	~SavestateEntry_IopHwRegs() = default;

	const char* GetFilename() const override { return "iopHwRegs.bin"; }
	u8* GetDataPtr() const override { return iopHw; }
	uint GetDataSize() const override { return sizeof(iopHw); }
};

class SavestateEntry_Scratchpad final : public MemorySavestateEntry
{
public:
	~SavestateEntry_Scratchpad() = default;

	const char* GetFilename() const override { return "Scratchpad.bin"; }
	u8* GetDataPtr() const override { return eeMem->Scratch; }
	uint GetDataSize() const override { return sizeof(eeMem->Scratch); }
};

class SavestateEntry_VU0mem final : public MemorySavestateEntry
{
public:
	~SavestateEntry_VU0mem() = default;

	const char* GetFilename() const override { return "vu0Memory.bin"; }
	u8* GetDataPtr() const override { return vuRegs[0].Mem; }
	uint GetDataSize() const override { return VU0_MEMSIZE; }
};

class SavestateEntry_VU1mem final : public MemorySavestateEntry
{
public:
	~SavestateEntry_VU1mem() = default;

	const char* GetFilename() const override { return "vu1Memory.bin"; }
	u8* GetDataPtr() const override { return vuRegs[1].Mem; }
	uint GetDataSize() const override { return VU1_MEMSIZE; }
};

class SavestateEntry_VU0prog final : public MemorySavestateEntry
{
public:
	~SavestateEntry_VU0prog() = default;

	const char* GetFilename() const override { return "vu0MicroMem.bin"; }
	u8* GetDataPtr() const override { return vuRegs[0].Micro; }
	uint GetDataSize() const override { return VU0_PROGSIZE; }
};

class SavestateEntry_VU1prog final : public MemorySavestateEntry
{
public:
	~SavestateEntry_VU1prog() = default;

	const char* GetFilename() const override { return "vu1MicroMem.bin"; }
	u8* GetDataPtr() const override { return vuRegs[1].Micro; }
	uint GetDataSize() const override { return VU1_PROGSIZE; }
};

class SavestateEntry_SPU2 final : public BaseSavestateEntry
{
public:
	~SavestateEntry_SPU2() override = default;

	const char* GetFilename() const override { return "SPU2.bin"; }
	bool FreezeIn(const std::optional<std::vector<u8>>& data) const override { return SysState_ComponentFreezeIn(data, SPU2_); }
	bool FreezeOut(SaveStateBase& writer) const override { return SysState_ComponentFreezeOut(writer, SPU2_); }
	bool IsRequired() const override { return true; }
};

class SavestateEntry_USB final : public BaseSavestateEntry
{
public:
	~SavestateEntry_USB() override = default;

	const char* GetFilename() const override { return "USB.bin"; }
	bool FreezeIn(const std::optional<std::vector<u8>>& data) const override { return SysState_ComponentFreezeInNew(data, "USB", &USB::DoState); }
	bool FreezeOut(SaveStateBase& writer) const override { return SysState_ComponentFreezeOutNew(writer, "USB", 16 * 1024, &USB::DoState); }
	bool IsRequired() const override { return false; }
};

class SavestateEntry_PAD final : public BaseSavestateEntry
{
public:
	~SavestateEntry_PAD() override = default;

	const char* GetFilename() const override { return "PAD.bin"; }
	bool FreezeIn(const std::optional<std::vector<u8>>& data) const override { return SysState_ComponentFreezeInNew(data, "PAD", &Pad::Freeze); }
	bool FreezeOut(SaveStateBase& writer) const override { return SysState_ComponentFreezeOutNew(writer, "PAD", 16 * 1024, &Pad::Freeze); }
	bool IsRequired() const override { return true; }
};

class SavestateEntry_GS final : public BaseSavestateEntry
{
public:
	~SavestateEntry_GS() = default;

	const char* GetFilename() const { return "GS.bin"; }
	bool FreezeIn(const std::optional<std::vector<u8>>& data) const { return SysState_ComponentFreezeIn(data, GS); }
	bool FreezeOut(SaveStateBase& writer) const { return SysState_ComponentFreezeOut(writer, GS); }
	bool IsRequired() const { return true; }
};

class SaveStateEntry_Achievements final : public BaseSavestateEntry
{
	~SaveStateEntry_Achievements() override = default;

	const char* GetFilename() const override { return "Achievements.bin"; }
	bool FreezeIn(const std::optional<std::vector<u8>>& data) const override
	{
		if (!Achievements::IsActive())
			return true;

		if (data.has_value())
			Achievements::LoadState(*data);
		else
			Achievements::LoadState(std::span<const u8>());

		return true;
	}

	bool FreezeOut(SaveStateBase& writer) const override
	{
		if (!Achievements::IsActive())
			return true;

		Achievements::SaveState(writer);
		return writer.IsOkay();
	}

	bool IsRequired() const override { return false; }
};

// (cpuRegs, iopRegs, VPU/GIF/DMAC structures should all remain as part of a larger unified
//  block, since they're all PCSX2-dependent and having separate files in the archie for them
//  would not be useful).
//

static const std::unique_ptr<BaseSavestateEntry> SavestateEntries[] = {
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_EmotionMemory),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_IopMemory),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_HwRegs),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_IopHwRegs),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_Scratchpad),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU0mem),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU1mem),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU0prog),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU1prog),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_SPU2),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_USB),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_PAD),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_GS),
	std::unique_ptr<BaseSavestateEntry>(new SaveStateEntry_Achievements),
};

std::unique_ptr<SaveStateEntryList> SaveState_DownloadState(Error* error)
{
	std::unique_ptr<SaveStateEntryList> destlist = std::make_unique<SaveStateEntryList>();
	destlist->GetBuffer().resize(1024 * 1024 * 64);

	memSavingState saveme(destlist->GetBuffer());
	SaveStateEntry internals(EntryFilename_InternalStructures);
	internals.SetDataIndex(saveme.GetCurrentPos());

	if (!saveme.FreezeBios())
	{
		Error::SetString(error, "FreezeBios() failed");
		return nullptr;
	}

	if (!saveme.FreezeInternals(error))
	{
		if (!error->IsValid())
			Error::SetString(error, "FreezeInternals() failed");

		return nullptr;
	}

	internals.SetDataSize(saveme.GetCurrentPos() - internals.GetDataIndex());
	destlist->Add(internals);

	for (const std::unique_ptr<BaseSavestateEntry>& entry : SavestateEntries)
	{
		uint startpos = saveme.GetCurrentPos();
		if (!entry->FreezeOut(saveme))
		{
			Error::SetString(error, fmt::format("FreezeOut() failed for {}.", entry->GetFilename()));
			destlist.reset();
			break;
		}

		destlist->Add(
			SaveStateEntry(entry->GetFilename())
				.SetDataIndex(startpos)
				.SetDataSize(saveme.GetCurrentPos() - startpos));
	}

	return destlist;
}

std::unique_ptr<SaveStateScreenshotData> SaveState_SaveScreenshot()
{
	static constexpr u32 SCREENSHOT_WIDTH = 640;
	static constexpr u32 SCREENSHOT_HEIGHT = 480;

	u32 width, height;
	std::vector<u32> pixels;
	if (!MTGS::SaveMemorySnapshot(SCREENSHOT_WIDTH, SCREENSHOT_HEIGHT, true, false, &width, &height, &pixels))
	{
		// saving failed for some reason, device lost?
		return nullptr;
	}

	std::unique_ptr<SaveStateScreenshotData> data = std::make_unique<SaveStateScreenshotData>();
	data->width = width;
	data->height = height;
	data->pixels = std::move(pixels);
	return data;
}

static bool SaveState_EncodeScreenshot(SaveStateScreenshotData* data, std::vector<u8>* encoded_png)
{
	png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	png_infop info_ptr = nullptr;
	if (!png_ptr)
		return false;

	ScopedGuard cleanup([&png_ptr, &info_ptr]() {
		if (png_ptr)
			png_destroy_write_struct(&png_ptr, info_ptr ? &info_ptr : nullptr);
	});

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
		return false;

	if (setjmp(png_jmpbuf(png_ptr)))
		return false;

	encoded_png->clear();
	png_set_write_fn(png_ptr, encoded_png, [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
		auto* const output = static_cast<std::vector<u8>*>(png_get_io_ptr(png_ptr));
		output->insert(output->end(), data_ptr, data_ptr + size); }, nullptr);
	png_set_compression_level(png_ptr, 5);
	png_set_IHDR(png_ptr, info_ptr, data->width, data->height, 8, PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png_ptr, info_ptr);

	for (u32 y = 0; y < data->height; ++y)
	{
		// ensure the alpha channel is set to opaque
		u32* row = &data->pixels[y * data->width];
		for (u32 x = 0; x < data->width; x++)
			row[x] |= 0xFF000000u;

		png_write_row(png_ptr, reinterpret_cast<png_bytep>(row));
	}

	png_write_end(png_ptr, nullptr);

	return true;
}

bool SaveState_SaveScreenshotToFile(const char* filename, Error* error)
{
	std::unique_ptr<SaveStateScreenshotData> screenshot = SaveState_SaveScreenshot();
	if (!screenshot)
	{
		Error::SetString(error, TRANSLATE_STR("SaveState", "Failed to capture save state screenshot."));
		return false;
	}

	std::vector<u8> encoded_screenshot;
	if (!SaveState_EncodeScreenshot(screenshot.get(), &encoded_screenshot))
	{
		Error::SetString(error, TRANSLATE_STR("SaveState", "Failed to encode save state screenshot."));
		return false;
	}

	if (!FileSystem::WriteBinaryFile(filename, encoded_screenshot.data(), encoded_screenshot.size()))
	{
		Error::SetStringFmt(error,
			TRANSLATE_FS("SaveState", "Failed to save screenshot to '{}'."), filename);
		return false;
	}

	return true;
}

struct SaveStateScreenshotReadContext
{
	std::span<const u8> data;
	size_t position = 0;
};

static bool SaveState_DecodeScreenshot(
	const std::vector<u8>& encoded_png, u32* out_width, u32* out_height, std::vector<u32>* out_pixels)
{
	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png_ptr)
		return false;

	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
	{
		png_destroy_read_struct(&png_ptr, nullptr, nullptr);
		return false;
	}

	ScopedGuard cleanup([&png_ptr, &info_ptr]() {
		png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
	});

	if (setjmp(png_jmpbuf(png_ptr)))
		return false;

	SaveStateScreenshotReadContext context{encoded_png};
	png_set_read_fn(png_ptr, &context, [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
		auto* const context = static_cast<SaveStateScreenshotReadContext*>(png_get_io_ptr(png_ptr));
		if (context->position > context->data.size() || size > (context->data.size() - context->position))
			png_error(png_ptr, "Unexpected end of save state screenshot");
		std::memcpy(data_ptr, context->data.data() + context->position, size);
		context->position += size;
	});

	png_read_info(png_ptr, info_ptr);

	png_uint_32 width = 0;
	png_uint_32 height = 0;
	int bitDepth = 0;
	int colorType = -1;
	if (png_get_IHDR(png_ptr, info_ptr, &width, &height, &bitDepth, &colorType, nullptr, nullptr, nullptr) != 1 ||
		width == 0 || height == 0)
	{
		return false;
	}

	const png_uint_32 bytesPerRow = png_get_rowbytes(png_ptr, info_ptr);
	std::vector<u8> rowData(bytesPerRow);

	*out_width = width;
	*out_height = height;
	out_pixels->resize(width * height);

	for (u32 y = 0; y < height; y++)
	{
		png_read_row(png_ptr, static_cast<png_bytep>(rowData.data()), nullptr);

		const u8* row_ptr = rowData.data();
		u32* out_ptr = &out_pixels->at(y * width);
		if (colorType == PNG_COLOR_TYPE_RGB)
		{
			for (u32 x = 0; x < width; x++)
			{
				u32 pixel = static_cast<u32>(*(row_ptr)++);
				pixel |= static_cast<u32>(*(row_ptr)++) << 8;
				pixel |= static_cast<u32>(*(row_ptr)++) << 16;
				pixel |= static_cast<u32>(*(row_ptr)++) << 24;
				*(out_ptr++) = pixel | 0xFF000000u; // make opaque
			}
		}
		else if (colorType == PNG_COLOR_TYPE_RGBA)
		{
			for (u32 x = 0; x < width; x++)
			{
				u32 pixel;
				std::memcpy(&pixel, row_ptr, sizeof(u32));
				row_ptr += sizeof(u32);
				*(out_ptr++) = pixel | 0xFF000000u; // make opaque
			}
		}
	}

	return true;
}

static bool SaveState_WriteFile(
	const std::string& directory, const char* filename, const void* data, size_t size, Error* error)
{
	const std::string path = Path::Combine(directory, filename);
	if (!FileSystem::WriteBinaryFile(path.c_str(), data, size))
	{
		Error::SetStringFmt(error, TRANSLATE_FS("SaveState", "Failed to write save state component '{}'."), filename);
		return false;
	}
	return true;
}

bool SaveState_SaveToDirectory(
	std::unique_ptr<SaveStateEntryList> srclist, std::unique_ptr<SaveStateScreenshotData> screenshot,
	const char* directory, const char* screenshot_filename, Error* error)
{
	if (!FileSystem::CreateDirectoryPath(directory, false, error))
	{
		if (!error->IsValid())
			Error::SetStringFmt(error, TRANSLATE_FS("SaveState", "Failed to create save state directory '{}'."), directory);
		return false;
	}

	SaveStateVersionIndicator version = {g_SaveVersion, {}};
	StringUtil::Strlcpy(version.version, BuildVersion::GitTaggedCommit ? BuildVersion::GitTag : "Unknown", std::size(version.version));
	if (!SaveState_WriteFile(directory, EntryFilename_StateVersion, &version, sizeof(version), error))
		return false;

	for (uint i = 0; i < srclist->GetLength(); ++i)
	{
		const SaveStateEntry& entry = (*srclist)[i];
		if (entry.GetDataSize() && !SaveState_WriteFile(directory, entry.GetFilename().c_str(),
									   srclist->GetPtr(entry.GetDataIndex()), entry.GetDataSize(), error))
		{
			return false;
		}
	}

	std::vector<u8> encoded_screenshot;
	if (screenshot && !SaveState_EncodeScreenshot(screenshot.get(), &encoded_screenshot))
	{
		Error::SetString(error, TRANSLATE_STR("SaveState", "Failed to encode save state screenshot."));
		return false;
	}

	if (!encoded_screenshot.empty() &&
		!SaveState_WriteFile(directory, EntryFilename_Screenshot, encoded_screenshot.data(), encoded_screenshot.size(), error))
	{
		return false;
	}

	if (screenshot_filename && !encoded_screenshot.empty() &&
		!FileSystem::WriteBinaryFile(screenshot_filename, encoded_screenshot.data(), encoded_screenshot.size()))
	{
		Error::SetStringFmt(error,
			TRANSLATE_FS("SaveState", "Failed to save screenshot to '{}'."), screenshot_filename);
		return false;
	}

	return true;
}

bool SaveState_ReadScreenshot(const std::string& directory, u32* out_width, u32* out_height, std::vector<u32>* out_pixels)
{
	const std::optional<std::vector<u8>> screenshot =
		FileSystem::ReadBinaryFile(Path::Combine(directory, EntryFilename_Screenshot).c_str());
	if (!screenshot.has_value())
		return false;

	return SaveState_DecodeScreenshot(*screenshot, out_width, out_height, out_pixels);
}

static bool CheckVersion(const std::string& directory, Error* error)
{
	const std::optional<std::vector<u8>> data =
		FileSystem::ReadBinaryFile(Path::Combine(directory, EntryFilename_StateVersion).c_str());
	if (!data.has_value() || data->size() < sizeof(u32))
	{
		Error::SetString(error, "Save state directory does not contain version indicator.");
		return false;
	}
	u32 savever;
	std::memcpy(&savever, data->data(), sizeof(savever));

	char version_string[STATE_PCSX2_VERSION_SIZE] = {};
	if (data->size() >= sizeof(SaveStateVersionIndicator))
	{
		std::memcpy(version_string, data->data() + sizeof(u32), STATE_PCSX2_VERSION_SIZE);
		version_string[STATE_PCSX2_VERSION_SIZE - 1] = 0;
	}
	else
		StringUtil::Strlcpy(version_string, "Unknown", std::size(version_string));

	// Major version mismatch.  Means we can't load this savestate at all.  Support for it
	// was removed entirely.
	// check for a "minor" version incompatibility; which happens if the savestate being loaded is a newer version
	// than the emulator recognizes.  99% chance that trying to load it will just corrupt emulation or crash.
	if (savever > g_SaveVersion || (savever >> 16) != (g_SaveVersion >> 16))
	{
		std::string current_emulator_version = BuildVersion::GitTag;
		if (current_emulator_version.empty())
		{
			current_emulator_version = "Unknown";
		}
		Error::SetString(error, fmt::format(TRANSLATE_FS("SaveState", "This save state was created with PCSX2 version {0}. It is no longer compatible "
																	  "with your current PCSX2 version {1}.\n\n"
																	  "If you have any unsaved progress on this save state, you can download the compatible PCSX2 version {0} "
																	  "from pcsx2.net, load the save state, and save your progress to the memory card."),
									version_string, current_emulator_version));
		return false;
	}

	return true;
}

static bool CheckFileExistsInState(const std::string& directory, const char* name, bool required)
{
	if (FileSystem::FileExists(Path::Combine(directory, name).c_str()))
	{
		DevCon.WriteLn(Color_Green, " ... found '%s'", name);
		return true;
	}

	if (required)
		Console.WriteLn(Color_Red, " ... not found '%s'!", name);
	else
		DevCon.WriteLn(Color_Red, " ... not found '%s'!", name);

	return false;
}

static bool LoadInternalStructuresState(const std::string& directory, Error* error)
{
	std::optional<std::vector<u8>> buffer =
		FileSystem::ReadBinaryFile(Path::Combine(directory, EntryFilename_InternalStructures).c_str());
	if (!buffer.has_value() || buffer->size() > std::numeric_limits<int>::max())
		return false;

	memLoadingState state(*buffer);
	if (!state.FreezeBios())
		return false;

	if (!state.FreezeInternals(error))
		return false;

	return true;
}

bool SaveState_LoadFromDirectory(const std::string& directory, Error* error)
{
	if (!FileSystem::DirectoryExists(directory.c_str()))
	{
		Error::SetString(error, "Save state directory does not exist.");
		return false;
	}

	if (!CheckVersion(directory, error))
		return false;

	// check that all parts are included
	const bool has_internal_structures = CheckFileExistsInState(directory, EntryFilename_InternalStructures, true);
	bool entry_present[std::size(SavestateEntries)];

	// Log any parts and pieces that are missing, and then generate an exception.
	bool allPresent = has_internal_structures;
	for (u32 i = 0; i < std::size(SavestateEntries); i++)
	{
		const bool required = SavestateEntries[i]->IsRequired();
		entry_present[i] = CheckFileExistsInState(directory, SavestateEntries[i]->GetFilename(), required);
		if (!entry_present[i] && required)
		{
			allPresent = false;
			break;
		}
	}
	if (!allPresent)
	{
		Error::SetString(error, "Some required components were not found or are incomplete.");
		return false;
	}

	PreLoadPrep();

	if (!LoadInternalStructuresState(directory, error))
	{
		if (!error->IsValid())
			Error::SetString(error, "Save state corruption in internal structures.");

		VMManager::Reset();
		return false;
	}

	for (u32 i = 0; i < std::size(SavestateEntries); ++i)
	{
		std::optional<std::vector<u8>> data;
		if (entry_present[i])
			data = FileSystem::ReadBinaryFile(Path::Combine(directory, SavestateEntries[i]->GetFilename()).c_str());
		if ((entry_present[i] && !data.has_value()) || !SavestateEntries[i]->FreezeIn(data))
		{
			Error::SetString(error, fmt::format("Save state corruption in {}.", SavestateEntries[i]->GetFilename()));
			VMManager::Reset();
			return false;
		}
	}

	PostLoadPrep();
	return true;
}

void SaveState_ReportLoadErrorOSD(const std::string& message, std::optional<s32> slot, bool backup)
{
	std::string full_message;
	if (slot.has_value())
	{
		if (backup)
			full_message = fmt::format(
				TRANSLATE_FS("SaveState", "Failed to load state from backup slot {}: {}"), *slot, message);
		else
			full_message = fmt::format(
				TRANSLATE_FS("SaveState", "Failed to load state from slot {}: {}"), *slot, message);
	}
	else
	{
		full_message = fmt::format(TRANSLATE_FS("SaveState", "Failed to load state: {}"), message);
	}

	Host::AddIconOSDMessage("LoadState", ICON_FA_TRIANGLE_EXCLAMATION,
		full_message, Host::OSD_WARNING_DURATION);
}

void SaveState_ReportSaveErrorOSD(const std::string& message, std::optional<s32> slot)
{
	std::string full_message;
	if (slot.has_value())
		full_message = fmt::format(
			TRANSLATE_FS("SaveState", "Failed to save state to slot {}: {}"), *slot, message);
	else
		full_message = fmt::format(TRANSLATE_FS("SaveState", "Failed to save state: {}"), message);

	Host::AddIconOSDMessage("SaveState", ICON_FA_TRIANGLE_EXCLAMATION,
		full_message, Host::OSD_WARNING_DURATION);
}

// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "INISettingsInterface.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Console.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <iterator>
#include <mutex>

#ifdef _WIN32
#include <io.h> // _mktemp_s
#else
#include <stdlib.h> // mktemp
#include <unistd.h>
#endif

// To prevent races between saving and loading settings, particularly with game settings,
// we only allow one ini to be parsed at any point in time.
static std::mutex s_ini_load_save_mutex;
static bool s_save_suppressed = false;

static std::FILE* GetTemporaryFile(std::string* temporary_filename, const std::string& original_filename,
	const char* mode, Error* error)
{
	temporary_filename->clear();
	temporary_filename->reserve(original_filename.length() + 8);
	temporary_filename->append(original_filename);

#ifdef _WIN32
	temporary_filename->append(".XXXXXXX");
	const errno_t err = _mktemp_s(temporary_filename->data(), temporary_filename->length() + 1);
	if (err != 0)
	{
		Error::SetErrno(error, "_mktemp_s() failed: ", err);
		return nullptr;
	}

	return FileSystem::OpenCFile(temporary_filename->c_str(), mode, error);
#else
	temporary_filename->append(".XXXXXX");
	const int fd = mkstemp(temporary_filename->data());
	if (fd < 0)
	{
		Error::SetErrno(error, "mkstemp() failed: ", errno);
		return nullptr;
	}

	std::FILE* fp = fdopen(fd, mode);
	if (!fp)
	{
		Error::SetErrno(error, "mkstemp() failed: ", errno);
		close(fd);
		return nullptr;
	}

	return fp;
#endif
}

INISettingsInterface::INISettingsInterface(std::string filename, std::string section_prefix)
	: m_filename(std::move(filename))
	, m_section_prefix(std::move(section_prefix))
	, m_ini(true, true)
{
}

void INISettingsInterface::SetSaveSuppressed(bool suppressed)
{
	s_save_suppressed = suppressed;
}

std::string INISettingsInterface::GetSectionName(const char* section) const
{
	return m_section_prefix.empty() ? std::string(section) : m_section_prefix + section;
}

const char* INISettingsInterface::GetValue(const char* section, const char* key) const
{
	if (!m_section_prefix.empty())
	{
		const std::string override_section = GetSectionName(section);
		if (const char* value = m_ini.GetValue(override_section.c_str(), key))
			return value;
	}

	return m_ini.GetValue(section, key);
}

INISettingsInterface::~INISettingsInterface()
{
	if (m_dirty)
		Save();
}

bool INISettingsInterface::Load()
{
	if (m_filename.empty())
		return false;

	std::unique_lock lock(s_ini_load_save_mutex);
	SI_Error err = SI_FAIL;
	auto fp = FileSystem::OpenManagedCFile(m_filename.c_str(), "rb");
	if (fp)
		err = m_ini.LoadFile(fp.get());

	return (err == SI_OK);
}

bool INISettingsInterface::Save(Error* error)
{
	if (s_save_suppressed)
	{
		m_dirty = false;
		return true;
	}

	if (m_filename.empty())
	{
		Error::SetStringView(error, "Filename is not set.");
		return false;
	}

	std::unique_lock lock(s_ini_load_save_mutex);
	std::string temp_filename;
	std::FILE* fp = GetTemporaryFile(&temp_filename, m_filename, "wb", error);
	SI_Error err = SI_FAIL;
	if (fp)
	{
		err = m_ini.SaveFile(fp, false);
		std::fclose(fp);

		if (err != SI_OK)
		{
			Error::SetStringFmt(error, "INI SaveFile() failed: {}", static_cast<int>(err));

			// remove temporary file
			FileSystem::DeleteFilePath(temp_filename.c_str());
		}
		else if (!FileSystem::RenamePath(temp_filename.c_str(), m_filename.c_str(), error))
		{
			Console.Error("Failed to rename '%s' to '%s'", temp_filename.c_str(), m_filename.c_str());
			FileSystem::DeleteFilePath(temp_filename.c_str());
			return false;
		}
	}

	if (err != SI_OK)
	{
		Console.Warning("Failed to save settings to '%s'.", m_filename.c_str());
		return false;
	}

	m_dirty = false;
	return true;
}

void INISettingsInterface::Clear()
{
	if (m_section_prefix.empty())
	{
		m_ini.Reset();
	}
	else
	{
		std::list<CSimpleIniA::Entry> entries;
		m_ini.GetAllSections(entries);
		for (const CSimpleIniA::Entry& entry : entries)
		{
			if (std::string_view(entry.pItem).starts_with(m_section_prefix))
				m_ini.Delete(entry.pItem, nullptr);
		}
	}
	m_dirty = true;
}

bool INISettingsInterface::IsEmpty()
{
	if (m_section_prefix.empty())
		return (m_ini.GetKeyCount() == 0);

	std::list<CSimpleIniA::Entry> entries;
	m_ini.GetAllSections(entries);
	for (const CSimpleIniA::Entry& entry : entries)
	{
		const std::string_view section(entry.pItem);
		if ((section.starts_with(m_section_prefix) || !section.starts_with("CRC.")) && m_ini.GetSectionSize(entry.pItem) > 0)
			return false;
	}
	return true;
}

bool INISettingsInterface::GetIntValue(const char* section, const char* key, int* value) const
{
	const char* str_value = GetValue(section, key);
	if (!str_value)
		return false;

	std::optional<int> parsed_value = StringUtil::FromChars<int>(str_value, 10);
	if (!parsed_value.has_value())
		return false;

	*value = parsed_value.value();
	return true;
}

bool INISettingsInterface::GetUIntValue(const char* section, const char* key, uint* value) const
{
	const char* str_value = GetValue(section, key);
	if (!str_value)
		return false;

	std::optional<uint> parsed_value = StringUtil::FromChars<uint>(str_value, 10);
	if (!parsed_value.has_value())
		return false;

	*value = parsed_value.value();
	return true;
}

bool INISettingsInterface::GetFloatValue(const char* section, const char* key, float* value) const
{
	const char* str_value = GetValue(section, key);
	if (!str_value)
		return false;

	std::optional<float> parsed_value = StringUtil::FromChars<float>(str_value);
	if (!parsed_value.has_value())
		return false;

	*value = parsed_value.value();
	return true;
}

bool INISettingsInterface::GetDoubleValue(const char* section, const char* key, double* value) const
{
	const char* str_value = GetValue(section, key);
	if (!str_value)
		return false;

	std::optional<double> parsed_value = StringUtil::FromChars<double>(str_value);
	if (!parsed_value.has_value())
		return false;

	*value = parsed_value.value();
	return true;
}

bool INISettingsInterface::GetBoolValue(const char* section, const char* key, bool* value) const
{
	const char* str_value = GetValue(section, key);
	if (!str_value)
		return false;

	std::optional<bool> parsed_value = StringUtil::FromChars<bool>(str_value);
	if (!parsed_value.has_value())
		return false;

	*value = parsed_value.value();
	return true;
}

bool INISettingsInterface::GetStringValue(const char* section, const char* key, std::string* value) const
{
	const char* str_value = GetValue(section, key);
	if (!str_value)
		return false;

	value->assign(str_value);
	return true;
}

bool INISettingsInterface::GetStringValue(const char* section, const char* key, SmallStringBase* value) const
{
	const char* str_value = GetValue(section, key);
	if (!str_value)
		return false;

	value->assign(str_value);
	return true;
}

void INISettingsInterface::SetIntValue(const char* section, const char* key, int value)
{
	m_dirty = true;
	const std::string write_section = GetSectionName(section);
	m_ini.SetValue(write_section.c_str(), key, StringUtil::ToChars(value).c_str(), nullptr, true);
}

void INISettingsInterface::SetUIntValue(const char* section, const char* key, uint value)
{
	m_dirty = true;
	const std::string write_section = GetSectionName(section);
	m_ini.SetValue(write_section.c_str(), key, StringUtil::ToChars(value).c_str(), nullptr, true);
}

void INISettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
	m_dirty = true;
	const std::string write_section = GetSectionName(section);
	m_ini.SetValue(write_section.c_str(), key, StringUtil::ToChars(value).c_str(), nullptr, true);
}

void INISettingsInterface::SetDoubleValue(const char* section, const char* key, double value)
{
	m_dirty = true;
	const std::string write_section = GetSectionName(section);
	m_ini.SetValue(write_section.c_str(), key, StringUtil::ToChars(value).c_str(), nullptr, true);
}

void INISettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
	m_dirty = true;
	const std::string write_section = GetSectionName(section);
	m_ini.SetBoolValue(write_section.c_str(), key, value, nullptr, true);
}

void INISettingsInterface::SetStringValue(const char* section, const char* key, const char* value)
{
	m_dirty = true;
	const std::string write_section = GetSectionName(section);
	m_ini.SetValue(write_section.c_str(), key, value, nullptr, true);
}

bool INISettingsInterface::ContainsValue(const char* section, const char* key) const
{
	return (GetValue(section, key) != nullptr);
}

void INISettingsInterface::DeleteValue(const char* section, const char* key)
{
	m_dirty = true;
	const std::string write_section = GetSectionName(section);
	m_ini.Delete(write_section.c_str(), key);
}

void INISettingsInterface::ClearSection(const char* section)
{
	m_dirty = true;
	const std::string write_section = GetSectionName(section);
	m_ini.Delete(write_section.c_str(), nullptr);
	m_ini.SetValue(write_section.c_str(), nullptr, nullptr);
}

void INISettingsInterface::RemoveSection(const char* section)
{
	const std::string write_section = GetSectionName(section);
	if (!m_ini.GetSection(write_section.c_str()))
		return;

	m_dirty = true;
	m_ini.Delete(write_section.c_str(), nullptr);
}

void INISettingsInterface::RemoveEmptySections()
{
	std::list<CSimpleIniA::Entry> entries;
	m_ini.GetAllSections(entries);
	for (const CSimpleIniA::Entry& entry : entries)
	{
		if (m_ini.GetSectionSize(entry.pItem) > 0)
			continue;

		m_dirty = true;
		m_ini.Delete(entry.pItem, nullptr);
	}
}

std::vector<std::string> INISettingsInterface::GetStringList(const char* section, const char* key) const
{
	if (!m_section_prefix.empty())
	{
		const std::string override_section = GetSectionName(section);
		if (m_ini.GetValue(override_section.c_str(), key))
			return GetStringListForSection(override_section.c_str(), key);
	}

	return GetStringListForSection(section, key);
}

std::vector<std::string> INISettingsInterface::GetStringListForSection(const char* section, const char* key) const
{
	std::list<CSimpleIniA::Entry> entries;
	if (!m_ini.GetAllValues(section, key, entries))
		return {};
	if (!m_section_prefix.empty() && entries.size() == 1 && entries.front().pItem[0] == '\0')
		return {};

	std::vector<std::string> ret;
	ret.reserve(entries.size());
	for (const CSimpleIniA::Entry& entry : entries)
		ret.emplace_back(entry.pItem);
	return ret;
}

void INISettingsInterface::SetStringList(const char* section, const char* key, const std::vector<std::string>& items)
{
	m_dirty = true;
	const std::string write_section = GetSectionName(section);
	m_ini.Delete(write_section.c_str(), key);

	if (items.empty() && !m_section_prefix.empty())
		m_ini.SetValue(write_section.c_str(), key, "", nullptr, false);
	else
	{
		for (const std::string& sv : items)
			m_ini.SetValue(write_section.c_str(), key, sv.c_str(), nullptr, false);
	}
}

bool INISettingsInterface::RemoveFromStringList(const char* section, const char* key, const char* item)
{
	std::vector<std::string> items = GetStringList(section, key);
	const auto it = std::find(items.begin(), items.end(), item);
	if (it == items.end())
		return false;
	items.erase(it);
	SetStringList(section, key, items);
	return true;
}

bool INISettingsInterface::AddToStringList(const char* section, const char* key, const char* item)
{
	std::vector<std::string> items = GetStringList(section, key);
	if (std::find(items.begin(), items.end(), item) != items.end())
		return false;
	items.emplace_back(item);
	SetStringList(section, key, items);
	return true;
}

std::vector<std::pair<std::string, std::string>> INISettingsInterface::GetKeyValueList(const char* section) const
{
	std::vector<std::pair<std::string, std::string>> output = GetKeyValueListForSection(section);
	if (m_section_prefix.empty())
		return output;

	const std::string override_section = GetSectionName(section);
	std::vector<std::pair<std::string, std::string>> overrides = GetKeyValueListForSection(override_section.c_str());
	for (const auto& [key, value] : overrides)
	{
		output.erase(std::remove_if(output.begin(), output.end(), [&key](const auto& entry) {
			return StringUtil::compareNoCase(entry.first, key);
		}),
			output.end());
	}
	output.insert(output.end(), std::make_move_iterator(overrides.begin()), std::make_move_iterator(overrides.end()));
	return output;
}

std::vector<std::pair<std::string, std::string>> INISettingsInterface::GetKeyValueListForSection(const char* section) const
{
	using Entry = CSimpleIniA::Entry;
	using KVEntry = std::pair<const char*, Entry>;
	std::vector<KVEntry> entries;
	std::vector<std::pair<std::string, std::string>> output;
	std::list<Entry> keys;
	if (m_ini.GetAllKeys(section, keys))
	{
		std::list<Entry> values;
		for (Entry& key : keys)
		{
			if (!m_ini.GetAllValues(section, key.pItem, values)) // [[unlikely]]
			{
				Console.Error("Got no values for a key returned from GetAllKeys!");
				continue;
			}
			for (const Entry& value : values)
				entries.emplace_back(key.pItem, value);
		}
	}
	std::sort(entries.begin(), entries.end(), [](const KVEntry& a, const KVEntry& b) {
		return a.second.nOrder < b.second.nOrder;
	});
	for (const KVEntry& entry : entries)
		output.emplace_back(entry.first, entry.second.pItem);
	return output;
}

void INISettingsInterface::SetKeyValueList(const char* section, const std::vector<std::pair<std::string, std::string>>& items)
{
	m_dirty = true;
	const std::string write_section = GetSectionName(section);
	m_ini.Delete(write_section.c_str(), nullptr);
	for (const std::pair<std::string, std::string>& item : items)
		m_ini.SetValue(write_section.c_str(), item.first.c_str(), item.second.c_str(), nullptr, false);
}

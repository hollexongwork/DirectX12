#include "Main.h"
#include "ConfigFile.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>

// ============================================================
//  ConfigFile : 簡易 INI リーダ / ライタ実装。
// ============================================================

// ---- 前後の空白 (と行末 CR) を落とすトリム ----
static std::string Trim(const std::string& s)
{
	const char* ws = " \t\r\n";
	size_t begin = s.find_first_not_of(ws);
	if (begin == std::string::npos)
		return std::string();
	size_t end = s.find_last_not_of(ws);
	return s.substr(begin, end - begin + 1);
}

// ------------------------------------------------------------
//  セクション検索 / 追加
// ------------------------------------------------------------
ConfigFile::Section* ConfigFile::FindSection(const std::string& Name)
{
	for (auto& section : m_Sections)
	{
		if (section.Name == Name)
			return &section;
	}
	return nullptr;
}

const ConfigFile::Section* ConfigFile::FindSection(const std::string& Name) const
{
	for (const auto& section : m_Sections)
	{
		if (section.Name == Name)
			return &section;
	}
	return nullptr;
}

ConfigFile::Section& ConfigFile::GetOrAddSection(const std::string& Name)
{
	if (Section* found = FindSection(Name))
		return *found;

	m_Sections.push_back(Section{ Name, {} });
	return m_Sections.back();
}

bool ConfigFile::HasSection(const std::string& SectionName) const
{
	return FindSection(SectionName) != nullptr;
}

// ------------------------------------------------------------
//  ファイル I/O
// ------------------------------------------------------------
bool ConfigFile::Load(const std::string& Path)
{
	std::ifstream file(Path);
	if (!file)
		return false;

	Clear();

	Section* current = nullptr;
	std::string line;
	while (std::getline(file, line))
	{
		std::string trimmed = Trim(line);

		// 空行 / コメント行はスキップ
		if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
			continue;

		// [Section]
		if (trimmed.front() == '[' && trimmed.back() == ']')
		{
			std::string name = Trim(trimmed.substr(1, trimmed.size() - 2));
			current = &GetOrAddSection(name);
			continue;
		}

		// Key=Value (最初の '=' で分割。パス等が値に来ても安全)
		size_t eq = trimmed.find('=');
		if (eq == std::string::npos || current == nullptr)
			continue;

		std::string key = Trim(trimmed.substr(0, eq));
		std::string value = Trim(trimmed.substr(eq + 1));
		if (key.empty())
			continue;

		current->Pairs.push_back({ key, value });
	}

	return true;
}

bool ConfigFile::Save(const std::string& Path) const
{
	// テキストモード: Windows CRT が \n -> CRLF に変換する。
	std::ofstream file(Path);
	if (!file)
		return false;

	for (size_t i = 0; i < m_Sections.size(); ++i)
	{
		const Section& section = m_Sections[i];

		file << "[" << section.Name << "]\n";
		for (const auto& pair : section.Pairs)
		{
			file << pair.first << "=" << pair.second << "\n";
		}

		if (i + 1 < m_Sections.size())
			file << "\n";
	}

	return (bool)file;
}

// ------------------------------------------------------------
//  Set 系
// ------------------------------------------------------------
void ConfigFile::SetString(const std::string& Section, const std::string& Key, const std::string& Value)
{
	auto& pairs = GetOrAddSection(Section).Pairs;
	for (auto& pair : pairs)
	{
		if (pair.first == Key)
		{
			pair.second = Value;
			return;
		}
	}
	pairs.push_back({ Key, Value });
}

void ConfigFile::SetFloat(const std::string& Section, const std::string& Key, float Value)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%.9g", Value);
	SetString(Section, Key, buf);
}

void ConfigFile::SetInt(const std::string& Section, const std::string& Key, int Value)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", Value);
	SetString(Section, Key, buf);
}

void ConfigFile::SetUInt(const std::string& Section, const std::string& Key, unsigned int Value)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%u", Value);
	SetString(Section, Key, buf);
}

void ConfigFile::SetBool(const std::string& Section, const std::string& Key, bool Value)
{
	SetString(Section, Key, Value ? "true" : "false");
}

void ConfigFile::SetFloat3(const std::string& Section, const std::string& Key, const XMFLOAT3& Value)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "%.9g,%.9g,%.9g", Value.x, Value.y, Value.z);
	SetString(Section, Key, buf);
}

void ConfigFile::SetFloat4(const std::string& Section, const std::string& Key, const XMFLOAT4& Value)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "%.9g,%.9g,%.9g,%.9g", Value.x, Value.y, Value.z, Value.w);
	SetString(Section, Key, buf);
}

// ------------------------------------------------------------
//  Get 系
// ------------------------------------------------------------
bool ConfigFile::GetString(const std::string& Section, const std::string& Key, std::string& Out) const
{
	const ConfigFile::Section* section = FindSection(Section);
	if (!section)
		return false;

	for (const auto& pair : section->Pairs)
	{
		if (pair.first == Key)
		{
			Out = pair.second;
			return true;
		}
	}
	return false;
}

bool ConfigFile::GetFloat(const std::string& Section, const std::string& Key, float& Out) const
{
	std::string value;
	if (!GetString(Section, Key, value))
		return false;

	char* end = nullptr;
	float parsed = strtof(value.c_str(), &end);
	if (end == value.c_str())
		return false;	// 数値として読めない

	Out = parsed;
	return true;
}

bool ConfigFile::GetInt(const std::string& Section, const std::string& Key, int& Out) const
{
	std::string value;
	if (!GetString(Section, Key, value))
		return false;

	char* end = nullptr;
	long parsed = strtol(value.c_str(), &end, 10);
	if (end == value.c_str())
		return false;

	Out = (int)parsed;
	return true;
}

bool ConfigFile::GetUInt(const std::string& Section, const std::string& Key, unsigned int& Out) const
{
	std::string value;
	if (!GetString(Section, Key, value))
		return false;

	char* end = nullptr;
	unsigned long parsed = strtoul(value.c_str(), &end, 10);
	if (end == value.c_str())
		return false;

	Out = (unsigned int)parsed;
	return true;
}

bool ConfigFile::GetBool(const std::string& Section, const std::string& Key, bool& Out) const
{
	std::string value;
	if (!GetString(Section, Key, value))
		return false;

	for (auto& c : value) c = (char)tolower(c);

	if (value == "true" || value == "1")   { Out = true;  return true; }
	if (value == "false" || value == "0")  { Out = false; return true; }
	return false;
}

bool ConfigFile::GetFloat3(const std::string& Section, const std::string& Key, XMFLOAT3& Out) const
{
	std::string value;
	if (!GetString(Section, Key, value))
		return false;

	XMFLOAT3 parsed;
	if (sscanf(value.c_str(), "%f , %f , %f", &parsed.x, &parsed.y, &parsed.z) != 3)
		return false;

	Out = parsed;
	return true;
}

bool ConfigFile::GetFloat4(const std::string& Section, const std::string& Key, XMFLOAT4& Out) const
{
	std::string value;
	if (!GetString(Section, Key, value))
		return false;

	XMFLOAT4 parsed;
	if (sscanf(value.c_str(), "%f , %f , %f , %f", &parsed.x, &parsed.y, &parsed.z, &parsed.w) != 4)
		return false;

	Out = parsed;
	return true;
}

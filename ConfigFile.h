#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>
using namespace DirectX;

// ============================================================
//  ConfigFile
//  簡易 INI リーダ / ライタ。
//  [Section] + Key=Value のプレーンテキストで、セクション・キーの
//  挿入順を保持したまま書き出す (手編集しやすいように)。
//
//  Get 系は「キーが存在すれば Out に書いて true」を返す。
//  存在しないキーでは Out に触れないので、呼び出し側の既定値が
//  そのまま生きる (GConfig->GetFloat)。
// ============================================================

class ConfigFile
{
private:
	struct Section
	{
		std::string Name;
		std::vector<std::pair<std::string, std::string>> Pairs;
	};

	std::vector<Section> m_Sections;

	Section*       FindSection(const std::string& Name);
	const Section* FindSection(const std::string& Name) const;
	Section&       GetOrAddSection(const std::string& Name);

public:
	void Clear() { m_Sections.clear(); }

	// ---- ファイル I/O ----
	// Load はファイルが存在しない / 開けない場合に false (初回起動を想定)。
	bool Load(const std::string& Path);
	bool Save(const std::string& Path) const;

	bool HasSection(const std::string& SectionName) const;

	// ---- Set (既存キーは上書き / 無ければ追加) ----
	void SetString(const std::string& Section, const std::string& Key, const std::string& Value);
	void SetFloat (const std::string& Section, const std::string& Key, float Value);
	void SetInt   (const std::string& Section, const std::string& Key, int Value);
	void SetUInt  (const std::string& Section, const std::string& Key, unsigned int Value);
	void SetBool  (const std::string& Section, const std::string& Key, bool Value);
	void SetFloat3(const std::string& Section, const std::string& Key, const XMFLOAT3& Value);
	void SetFloat4(const std::string& Section, const std::string& Key, const XMFLOAT4& Value);

	// ---- Get (キーが在れば true / Out 更新。無ければ Out は不変) ----
	bool GetString(const std::string& Section, const std::string& Key, std::string& Out) const;
	bool GetFloat (const std::string& Section, const std::string& Key, float& Out) const;
	bool GetInt   (const std::string& Section, const std::string& Key, int& Out) const;
	bool GetUInt  (const std::string& Section, const std::string& Key, unsigned int& Out) const;
	bool GetBool  (const std::string& Section, const std::string& Key, bool& Out) const;
	bool GetFloat3(const std::string& Section, const std::string& Key, XMFLOAT3& Out) const;
	bool GetFloat4(const std::string& Section, const std::string& Key, XMFLOAT4& Out) const;
};

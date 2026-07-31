#include "Main.h"
#include "World.h"

#include <cctype>
#include <cstring>

UWorld::~UWorld()
{
	for (auto& actor : m_Actors)
	{
		// アクター本体 + コンポーネント両方の EndPlay を配送する
		// (BeginPlay 済みの場合のみ。内部でガードされる)
		actor->DispatchEndPlay();
		actor->UnregisterAllComponents();
	}
	m_Actors.clear();
}

void UWorld::BeginPlay()
{
	if (m_HasBegunPlay) return;
	m_HasBegunPlay = true;

	for (auto& actor : m_Actors)
	{
		actor->DispatchBeginPlay();
	}
}

void UWorld::Tick(float DeltaTime)
{
	// Tick 中の SpawnActor による push_back (再確保) に備えて
	// インデックスループで巡回する
	for (size_t i = 0; i < m_Actors.size(); ++i)
	{
		AActor* actor = m_Actors[i].get();

		if (actor->IsPendingKill()) continue;

		if (actor->bCanEverTick)
		{
			actor->Tick(DeltaTime);
		}
		actor->TickComponents(DeltaTime);
	}

	// ---- 破棄予約されたアクターの削除 (PendingKill 相当) ----
	// EndPlay 中の DestroyActor が m_PendingKillActors へ push_back しても
	// 安全なように、ローカルへ swap してから処理する (range-for 中の
	// push_back によるイテレータ無効化の防止)。処理中に新たに積まれた
	// 分も while で同フレーム内に消化する。
	while (!m_PendingKillActors.empty())
	{
		std::vector<AActor*> pendingKill;
		pendingKill.swap(m_PendingKillActors);

		for (AActor* dead : pendingKill)
		{
			auto it = std::find_if(m_Actors.begin(), m_Actors.end(),
				[dead](const std::unique_ptr<AActor>& actor) { return actor.get() == dead; });

			if (it != m_Actors.end())
			{
				// アクター本体 + コンポーネント両方の EndPlay を配送
				(*it)->DispatchEndPlay();
				(*it)->UnregisterAllComponents();
				m_Actors.erase(it);
			}
		}
	}
}

void UWorld::SendAllEndOfFrameUpdates()
{
	m_Scene.UpdateAllPrimitiveSceneInfos();
	m_Scene.UpdateAllLightSceneInfos();
}

void UWorld::DestroyActor(AActor* Actor)
{
	if (Actor == nullptr || Actor->IsPendingKill()) return;

	Actor->MarkPendingKill();
	m_PendingKillActors.push_back(Actor);
}

bool UWorld::ContainsActor(const AActor* Actor) const
{
	if (Actor == nullptr) return false;

	for (const auto& actor : m_Actors)
	{
		if (actor.get() == Actor)
		{
			return true;
		}
	}
	return false;
}

// ------------------------------------------------------------
//  型名ユーティリティ
// ------------------------------------------------------------

std::string UWorld::CleanTypeName(const char* RawName)
{
	std::string name = (RawName != nullptr) ? RawName : "";

	// MSVC: "class APointLight" / "struct Foo" / "enum Bar"
	static const char* prefixes[] = { "class ", "struct ", "enum " };
	for (const char* prefix : prefixes)
	{
		const size_t len = strlen(prefix);
		if (name.compare(0, len, prefix) == 0)
		{
			name.erase(0, len);
			break;
		}
	}

	// GCC / Clang: 先頭に長さの数字が付く ("11APointLight")
	size_t digits = 0;
	while (digits < name.size() && isdigit((unsigned char)name[digits]))
	{
		++digits;
	}
	name.erase(0, digits);

	// ネームスペース修飾を除去 ("Foo::Bar" -> "Bar")
	const size_t pos = name.rfind("::");
	if (pos != std::string::npos)
	{
		name = name.substr(pos + 2);
	}

	return name;
}

std::string UWorld::GetClassDisplayName(const AActor* Actor)
{
	return (Actor != nullptr) ? CleanTypeName(typeid(*Actor).name()) : std::string("None");
}

std::string UWorld::GetClassDisplayName(const UActorComponent* Component)
{
	return (Component != nullptr) ? CleanTypeName(typeid(*Component).name()) : std::string("None");
}

// ------------------------------------------------------------
//  既定ラベルの割り当て
//  型名の 'A' プレフィックスを除いた基本名に、重複時のみ
//  連番サフィックスを付ける (例: "PointLight", "PointLight2")。
// ------------------------------------------------------------
void UWorld::AssignDefaultLabel(AActor* Actor)
{
	if (Actor == nullptr) return;

	std::string base = GetClassDisplayName(Actor);

	if (base.size() > 1 && base[0] == 'A' && isupper((unsigned char)base[1]))
	{
		base.erase(0, 1);
	}
	if (base.empty())
	{
		base = "Actor";
	}

	auto isTaken = [&](const std::string& label)
		{
			for (const auto& actor : m_Actors)
			{
				if (actor.get() != Actor && actor->GetActorLabel() == label)
				{
					return true;
				}
			}
			return false;
		};

	std::string label = base;
	int suffix = 2;
	while (isTaken(label))
	{
		label = base + std::to_string(suffix++);
	}

	Actor->SetActorLabel(label);
}

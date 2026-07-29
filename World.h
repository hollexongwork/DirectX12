#pragma once
#include <vector>
#include <memory>
#include <string>
#include <type_traits>
#include "Actor.h"
#include "Scene.h"

// ============================================================
//  UWorld
//  UWorld に相当。アクターの生成 (SpawnActor)・破棄・
//  Tick ディスパッチと FScene の所有を担う。
//
//  Outliner / SettingsManager 対応:
//    - SpawnActor が型名から一意な既定ラベルを割り当てる
//      (例: "PointLight", "PointLight2")
//    - GetActors() で全アクターをスポーン順に列挙できる
//    - CleanTypeName / GetClassDisplayName は typeid 名から
//      "class " 等の装飾を除いた表示名を返す
// ============================================================

class UWorld
{
private:
	// 注意: m_Scene は m_Actors より先に宣言すること。
	// (メンバ破棄は宣言の逆順なので、アクター破棄時の
	//  Scene からの登録解除が安全に行える)
	FScene m_Scene;

	std::vector<std::unique_ptr<AActor>> m_Actors;
	std::vector<AActor*>                 m_PendingKillActors;

	bool m_HasBegunPlay = false;

	// 型名から一意な既定ラベルを割り当てる (SpawnActor から呼ばれる)
	void AssignDefaultLabel(AActor* Actor);

public:
	UWorld() = default;
	~UWorld();

	// ---- スポーン (UWorld::SpawnActor) ----
	template <class T, class... Args>
	T* SpawnActor(Args&&... args)
	{
		static_assert(std::is_base_of<AActor, T>::value, "T must derive from AActor");

		auto actor = std::make_unique<T>(std::forward<Args>(args)...);
		T* ptr = actor.get();
		m_Actors.push_back(std::move(actor));

		ptr->SetWorld(this);
		AssignDefaultLabel(ptr);
		ptr->RegisterAllComponents();

		if (m_HasBegunPlay)
		{
			ptr->DispatchBeginPlay();
		}

		return ptr;
	}

	// ---- 検索 (UGameplayStatics::GetActorOfClass 相当) ----
	template <class T>
	T* GetActorOfClass() const
	{
		for (const auto& actor : m_Actors)
		{
			if (auto* casted = dynamic_cast<T*>(actor.get()))
			{
				return casted;
			}
		}
		return nullptr;
	}

	// ---- 一括検索 (UGameplayStatics::GetAllActorsOfClass 相当) ----
	// ImGui のライトパネルなどが基底型 (ALight 等) で全件列挙する
	template <class T>
	void GetActorsOfClass(std::vector<T*>& OutActors) const
	{
		for (const auto& actor : m_Actors)
		{
			if (auto* casted = dynamic_cast<T*>(actor.get()))
			{
				OutActors.push_back(casted);
			}
		}
	}

	// ---- 全アクター列挙 (Outliner / SettingsManager 用) ----
	// スポーン順を保持する。
	const std::vector<std::unique_ptr<AActor>>& GetActors() const { return m_Actors; }

	// 指定アクターがこのワールドに登録されているか (選択の生存確認用)
	bool ContainsActor(const AActor* Actor) const;

	// ---- 型名ユーティリティ ----
	// typeid(...).name() から "class " / "struct " / 先頭の長さ数字 /
	// ネームスペースを除いた表示名を返す (例: "class APointLight" -> "APointLight")。
	static std::string CleanTypeName(const char* RawName);
	static std::string GetClassDisplayName(const AActor* Actor);
	static std::string GetClassDisplayName(const UActorComponent* Component);

	// ---- ライフサイクル ----
	void BeginPlay();
	void Tick(float DeltaTime);

	void DestroyActor(AActor* Actor);

	// 変更されたレンダーステート / トランスフォームをプロキシへ反映する。
	// 毎フレーム、Tick の後・描画の前に呼ぶこと。
	// (UWorld::SendAllEndOfFrameUpdates)
	void SendAllEndOfFrameUpdates();

	FScene* GetScene() { return &m_Scene; }
};

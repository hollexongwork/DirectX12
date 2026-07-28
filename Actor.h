#pragma once
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <string>
#include <type_traits>
#include "SceneComponent.h"

using namespace DirectX;

class UWorld;

// ============================================================
//  AActor
//  トランスフォームは自身では持たず、
//  RootComponent (USceneComponent) に委譲する。
//  生成は必ず UWorld::SpawnActor<T>() を通すこと。
//
//  ActorLabel: Outliner / Details 用の表示名 (
//  エディタ上アクター名に相当)。SpawnActor が型名から一意な
//  既定ラベルを割り当て、SettingsManager が永続化する。
// ============================================================

class AActor
{
protected:
	UWorld*          m_World = nullptr;
	USceneComponent* m_RootComponent = nullptr;

	std::vector<std::unique_ptr<UActorComponent>> m_OwnedComponents;

	// Outliner / Details 用の表示名 (UWorld::SpawnActor が既定値を割り当てる)
	std::string m_ActorLabel;

	bool m_HasBegunPlay = false;
	bool m_PendingKill = false;

public:
	// PrimaryActorTick.bCanEverTick 相当
	bool bCanEverTick = true;

	AActor() = default;
	virtual ~AActor();

	// ---- ライフサイクル ----
	virtual void BeginPlay() {}
	virtual void Tick(float DeltaTime) {}
	virtual void EndPlay() {}

	void DispatchBeginPlay();
	void TickComponents(float DeltaTime);

	// ---- コンポーネント生成 (CreateDefaultSubobject) ----
	template <class T, class... Args>
	T* CreateDefaultSubobject(Args&&... args)
	{
		static_assert(std::is_base_of<UActorComponent, T>::value, "T must derive from UActorComponent");

		auto component = std::make_unique<T>(std::forward<Args>(args)...);
		T* ptr = component.get();
		ptr->SetOwner(this);
		m_OwnedComponents.push_back(std::move(component));

		// 最初に生成された SceneComponent を自動的に Root にする
		if (m_RootComponent == nullptr)
		{
			if (auto* scene = dynamic_cast<USceneComponent*>(ptr))
			{
				m_RootComponent = scene;
			}
		}

		// スポーン後の動的追加にも対応
		if (m_World != nullptr)
		{
			ptr->RegisterComponent(m_World);
			if (m_HasBegunPlay)
			{
				ptr->DispatchBeginPlay();
			}
		}

		return ptr;
	}

	// ---- コンポーネント検索 (GetComponentByClass) ----
	template <class T>
	T* GetComponentByClass() const
	{
		for (const auto& component : m_OwnedComponents)
		{
			if (auto* casted = dynamic_cast<T*>(component.get()))
			{
				return casted;
			}
		}
		return nullptr;
	}

	// ---- コンポーネント列挙 (Outliner / Details / SettingsManager 用) ----
	// 生成順 (CreateDefaultSubobject 呼び出し順) を保持する。
	const std::vector<std::unique_ptr<UActorComponent>>& GetComponents() const { return m_OwnedComponents; }

	void RegisterAllComponents();
	void UnregisterAllComponents();

	void             SetRootComponent(USceneComponent* Root) { m_RootComponent = Root; }
	USceneComponent* GetRootComponent() const { return m_RootComponent; }

	// ---- 表示名 (SetActorLabel / GetActorLabel) ----
	void               SetActorLabel(const std::string& Label) { m_ActorLabel = Label; }
	const std::string& GetActorLabel() const { return m_ActorLabel; }

	// ---- ワールド ----
	void    SetWorld(UWorld* World) { m_World = World; }
	UWorld* GetWorld() const { return m_World; }

	void Destroy();
	bool IsPendingKill() const { return m_PendingKill; }
	void MarkPendingKill() { m_PendingKill = true; }

	bool HasBegunPlay() const { return m_HasBegunPlay; }

	// ---- トランスフォーム (RootComponent へ委譲) ----
	void SetActorLocation(const XMFLOAT3& Location);
	void SetActorRotation(const XMFLOAT3& Rotation);	// ラジアン
	void SetActorScale3D(const XMFLOAT3& Scale);

	XMFLOAT3 GetActorLocation() const;
	XMFLOAT3 GetActorRotation() const;
	XMFLOAT3 GetActorScale3D() const;

	XMFLOAT3 GetActorForwardVector() const;
	XMFLOAT3 GetActorRightVector() const;
	XMFLOAT3 GetActorUpVector() const;
};

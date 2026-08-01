#include "Main.h"
#include "Actor.h"
#include "World.h"

AActor::~AActor()
{
	// コンポーネント破棄前に Scene などから登録解除しておく
	UnregisterAllComponents();
}

void AActor::DispatchBeginPlay()
{
	if (m_HasBegunPlay) return;
	m_HasBegunPlay = true;

	// コンポーネント → アクター本体の順で BeginPlay。
	// BeginPlay 中の CreateDefaultSubobject (動的追加) による
	// push_back (再確保) に備えてインデックスループで巡回する
	// (range-for だとイテレータ無効化で UB)。追加分もこのループで
	// BeginPlay される。
	for (size_t i = 0; i < m_OwnedComponents.size(); ++i)
	{
		m_OwnedComponents[i]->DispatchBeginPlay();
	}

	BeginPlay();
}

void AActor::DispatchEndPlay()
{
	if (!m_HasBegunPlay) return;
	m_HasBegunPlay = false;

	// アクター本体 → コンポーネントの順で EndPlay (BeginPlay の逆順)
	EndPlay();

	for (size_t i = 0; i < m_OwnedComponents.size(); ++i)
	{
		m_OwnedComponents[i]->DispatchEndPlay();
	}
}

void AActor::TickComponents(float DeltaTime)
{
	// TickComponent 中の動的追加に備えてインデックスループ
	// (DispatchBeginPlay と同様)
	for (size_t i = 0; i < m_OwnedComponents.size(); ++i)
	{
		if (m_OwnedComponents[i]->bCanEverTick)
		{
			m_OwnedComponents[i]->TickComponent(DeltaTime);
		}
	}
}

void AActor::RegisterAllComponents()
{
	for (auto& component : m_OwnedComponents)
	{
		component->RegisterComponent(m_World);
	}
}

void AActor::UnregisterAllComponents()
{
	for (auto& component : m_OwnedComponents)
	{
		component->UnregisterComponent();
	}
}

void AActor::Destroy()
{
	if (m_World)
	{
		m_World->DestroyActor(this);
	}
}

// ------------------------------------------------------------
//  トランスフォーム (RootComponent へ委譲)
// ------------------------------------------------------------

void AActor::SetActorLocation(const XMFLOAT3& Location)
{
	if (m_RootComponent)
	{
		m_RootComponent->SetRelativeLocation(Location);
	}
}

void AActor::SetActorRotation(const XMFLOAT3& Rotation)
{
	if (m_RootComponent)
	{
		m_RootComponent->SetRelativeRotation(Rotation);
	}
}

void AActor::SetActorScale3D(const XMFLOAT3& Scale)
{
	if (m_RootComponent)
	{
		m_RootComponent->SetRelativeScale3D(Scale);
	}
}

XMFLOAT3 AActor::GetActorLocation() const
{
	return m_RootComponent ? m_RootComponent->GetComponentLocation() : XMFLOAT3(0.0f, 0.0f, 0.0f);
}

XMFLOAT3 AActor::GetActorRotation() const
{
	return m_RootComponent ? m_RootComponent->GetRelativeRotation() : XMFLOAT3(0.0f, 0.0f, 0.0f);
}

XMFLOAT3 AActor::GetActorScale3D() const
{
	return m_RootComponent ? m_RootComponent->GetRelativeScale3D() : XMFLOAT3(1.0f, 1.0f, 1.0f);
}

XMFLOAT3 AActor::GetActorForwardVector() const
{
	return m_RootComponent ? m_RootComponent->GetForwardVector() : XMFLOAT3(0.0f, 0.0f, 1.0f);
}

XMFLOAT3 AActor::GetActorRightVector() const
{
	return m_RootComponent ? m_RootComponent->GetRightVector() : XMFLOAT3(1.0f, 0.0f, 0.0f);
}

XMFLOAT3 AActor::GetActorUpVector() const
{
	return m_RootComponent ? m_RootComponent->GetUpVector() : XMFLOAT3(0.0f, 1.0f, 0.0f);
}

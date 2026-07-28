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

	// コンポーネント → アクター本体の順で BeginPlay
	for (auto& component : m_OwnedComponents)
	{
		component->DispatchBeginPlay();
	}

	BeginPlay();
}

void AActor::TickComponents(float DeltaTime)
{
	for (auto& component : m_OwnedComponents)
	{
		if (component->bCanEverTick)
		{
			component->TickComponent(DeltaTime);
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

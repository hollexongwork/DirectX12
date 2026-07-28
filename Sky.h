#pragma once
#include "Actor.h"
#include "StaticMeshComponent.h"

class ASky : public AActor
{
private:
	UStaticMeshComponent* m_MeshComponent = nullptr;

	class ACameraActor* m_Camera = nullptr;

public:
	ASky();

	void BeginPlay() override;
	void Tick(float DeltaTime) override;

	UStaticMeshComponent* GetMeshComponent() const { return m_MeshComponent; }
};

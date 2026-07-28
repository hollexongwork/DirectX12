#pragma once
#include "Actor.h"
#include "StaticMeshComponent.h"

class ATable : public AActor
{
private:
	UStaticMeshComponent* m_MeshComponent = nullptr;

public:
	ATable();

	UStaticMeshComponent* GetMeshComponent() const { return m_MeshComponent; }
	Material& GetMaterial(unsigned int SlotIndex = 0) { return m_MeshComponent->GetMaterial(SlotIndex); }
};

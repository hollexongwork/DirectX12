#pragma once
#include "Actor.h"
#include "StaticMeshComponent.h"

class ALion : public AActor
{
private:
	UStaticMeshComponent* m_MeshComponent = nullptr;

public:
	ALion();

	UStaticMeshComponent* GetMeshComponent() const { return m_MeshComponent; }
	Material& GetMaterial() { return m_MeshComponent->GetMaterial(0); }
};

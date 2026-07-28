#pragma once
#include "Actor.h"
#include "StaticMeshComponent.h"

class ACat : public AActor
{
private:
	UStaticMeshComponent* m_MeshComponent = nullptr;

public:
	ACat();

	UStaticMeshComponent* GetMeshComponent() const { return m_MeshComponent; }
	Material& GetMaterial() { return m_MeshComponent->GetMaterial(0); }
};

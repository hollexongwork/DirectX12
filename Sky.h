#pragma once
#include "StaticMeshActor.h"

// ============================================================
//  ASky
//  AStaticMeshActor 派生の空ドーム。
//  カメラ位置へ毎フレーム追従し、常にビューを覆う (Tick 有効)。
// ============================================================

class ASky : public AStaticMeshActor
{
private:
	class ACameraActor* m_Camera = nullptr;

public:
	ASky();

	void BeginPlay() override;
	void Tick(float DeltaTime) override;
};

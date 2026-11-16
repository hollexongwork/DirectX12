#pragma once
#include "Actor.h"
#include "StaticMeshComponent.h"

// ============================================================
//  AStaticMeshActor
//  AStaticMeshActor に相当。UStaticMeshComponent を
//  Root に持つだけの汎用スタティックメッシュ配置アクター。
//  シーンに置く個々の物体は派生クラス (ACat / ATable / ALion /
//  AHorse / AFogVolume / ATree ...) がコンストラクタでメッシュ /
//  テクスチャ / マテリアル / 既定の配置を構成し、GameManager は
//  スポーン (登録) だけを行う。
// ============================================================

class AStaticMeshActor : public AActor
{
private:
	UStaticMeshComponent* m_StaticMeshComponent = nullptr;

public:
	AStaticMeshActor();

	UStaticMeshComponent* GetStaticMeshComponent() const { return m_StaticMeshComponent; }

protected:
	// 石像 / テーブル共通の Lit マテリアル (BaseColor はテクスチャ乗算前の定数)。
	// 呼び出し後に GetStaticMeshComponent()->MarkRenderStateDirty() を呼ぶこと
	static void ApplyDefaultLitMaterial(Material& material)
	{
		material.Params.BaseColor = { 0.5f, 0.0f, 0.5f, 1.0f };
		material.Params.EmissionColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		material.Params.Metallic = 0.0f;
		material.Params.Specular = 0.0f;
		material.Params.Roughness = 1.0f;
		material.Params.NormalWeight = 1.0f;
		material.Params.Unlit = FALSE;
	}
};

#pragma once
#include "RenderManager.h"
#include "Material.h"
#include "Actor.h"
#include "PrimitiveComponent.h"

// ============================================================
//  UFieldQuadComponent
//  地面クアッド (手組み頂点バッファ) を描く専用プリミティブ。
//  描画本体は FFieldQuadSceneProxy (Field.cpp 内) へ移設済み。
//  ※ 旧 Field::Draw が行っていたディレクショナルライト設定は
//    FScene 側へ移設済み (描画コンポーネントは描画のみ行う)。
// ============================================================

class UFieldQuadComponent : public UPrimitiveComponent
{
private:
	std::unique_ptr<VERTEX_BUFFER> m_VertexBuffer;

	std::unique_ptr<TEXTURE> m_Diffuse;
	std::unique_ptr<TEXTURE> m_Normal;
	std::unique_ptr<TEXTURE> m_ARM;

	Material m_Material;

public:
	UFieldQuadComponent();

	// 直接書き換えた場合は MarkRenderStateDirty() を呼ぶこと
	Material& GetMaterial() { return m_Material; }

	// ---- 境界 (フラスタムカリング用) ----
	// 手組みクアッド (±10m, Y=0) のローカル AABB をワールドへ変換
	FBoxSphereBounds CalcBounds(const XMMATRIX& LocalToWorld) const override;

	FPrimitiveSceneProxy* CreateSceneProxy() override;
};

class AField : public AActor
{
private:
	UFieldQuadComponent* m_QuadComponent = nullptr;

public:
	AField();

	UFieldQuadComponent* GetQuadComponent() const { return m_QuadComponent; }
	Material& GetMaterial() { return m_QuadComponent->GetMaterial(); }
};

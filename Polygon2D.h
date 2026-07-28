#pragma once
#include "RenderManager.h"
#include "Actor.h"
#include "PrimitiveComponent.h"

// ============================================================
//  UPolygon2DComponent / APolygon2D
//  画面座標系の 2D ポリゴン (オーバーレイ)。
//  描画本体は FPolygon2DSceneProxy (Polygon2D.cpp 内) へ移設済み。
//  注意: プロキシの DrawPrimitive 内で CAMERA 定数を正射影に
//  上書きするため、使う場合はシーン内で最後にスポーン
//  (= 最後に描画) すること。
// ============================================================

class UPolygon2DComponent : public UPrimitiveComponent
{
private:
	std::unique_ptr<VERTEX_BUFFER> m_VertexBuffer;
	std::unique_ptr<TEXTURE> m_Texture;

	XMFLOAT4 m_VertexColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

public:
	UPolygon2DComponent();

	void TickComponent(float DeltaTime) override;

	// ---- 境界 (フラスタムカリング用) ----
	// 画面座標系オーバーレイは 3D フラスタムの対象外のため
	// 「無限」境界 (HALF_WORLD_MAX) で常に可視にする
	FBoxSphereBounds CalcBounds(const XMMATRIX& LocalToWorld) const override;

	FPrimitiveSceneProxy* CreateSceneProxy() override;

	XMFLOAT4 GetVertexColor() const { return m_VertexColor; }
	void     SetVertexColor(const XMFLOAT4& v) { m_VertexColor = v; }
};

class APolygon2D : public AActor
{
private:
	UPolygon2DComponent* m_PolygonComponent = nullptr;

public:
	APolygon2D();

	XMFLOAT4 GetVertexColor() const { return m_PolygonComponent->GetVertexColor(); }
	void     SetVertexColor(const XMFLOAT4& v) { m_PolygonComponent->SetVertexColor(v); }
};

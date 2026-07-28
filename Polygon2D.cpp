#include "Main.h"
#include "RenderManager.h"
#include "Polygon2D.h"
#include "PrimitiveSceneProxy.h"

// ============================================================
//  FPolygon2DSceneProxy
//  UPolygon2DComponent のレンダー側ミラー。頂点バッファは
//  ゲーム側 TickComponent が毎フレーム書き込む共有 GPU リソースを
//  参照する (頂点色の反映はバッファ経由なので再生成不要)。
// ============================================================

class FPolygon2DSceneProxy : public FPrimitiveSceneProxy
{
private:
	const VERTEX_BUFFER* m_VertexBuffer = nullptr;
	const TEXTURE*       m_Texture = nullptr;

public:
	FPolygon2DSceneProxy(const UPrimitiveComponent* Component,
		const VERTEX_BUFFER* VertexBuffer, const TEXTURE* Texture)
		: FPrimitiveSceneProxy(Component)
		, m_VertexBuffer(VertexBuffer)
		, m_Texture(Texture)
	{
	}

	void DrawPrimitive(RenderManager* RM) const override
	{
		// Matrix Setting (2D オーバーレイなので常に単位行列)
		{
			XMMATRIX localToWorld = XMMatrixIdentity();
			PRIMITIVE_CONSTANT constant{};
			XMStoreFloat4x4(&constant.LocalToWorld, XMMatrixTranspose(localToWorld));
			RM->SetConstant(RenderManager::CONSTANT_TYPE::PRIMITIVE, &constant, sizeof(constant));
		}

		// 画面座標系の正射影で VIEW 定数を上書きする
		// (このプリミティブより後に 3D を描かないこと)
		{
			XMMATRIX view = XMMatrixIdentity();

			XMMATRIX projection;
			projection = XMMatrixOrthographicOffCenterLH(0.0f,
				(float)RM->GetBackBufferWidth(),
				(float)RM->GetBackBufferHeight(),
				0.0f, 0.0f, 1.0f);

			// ライト系フィールドは Unlit パスでは参照されないためゼロで良い
			VIEW_CONSTANT constant{};
			XMStoreFloat4x4(&constant.View, XMMatrixTranspose(view));
			XMStoreFloat4x4(&constant.Projection, XMMatrixTranspose(projection));

			RM->SetConstant(RenderManager::CONSTANT_TYPE::VIEW, &constant, sizeof(constant));
		}

		// Vertex Buffer Setting
		RM->SetVertexBuffer(m_VertexBuffer);

		// Texture Setting
		RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_Texture);

		// Topology Setting
		RM->GetGraphicsCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

		// Draw
		RM->GetGraphicsCommandList()->DrawInstanced(4, 1, 0, 0);
	}
};

// ------------------------------------------------------------
//  UPolygon2DComponent
// ------------------------------------------------------------

UPolygon2DComponent::UPolygon2DComponent()
{
	RenderManager* renderManager = RenderManager::GetInstance();

	m_VertexBuffer = renderManager->CreateVertexBuffer(sizeof(VERTEX_3D), 4);

	VERTEX_3D* buffer{};
	HRESULT hr = m_VertexBuffer->Resource->Map(0, nullptr, (void**)&buffer);
	assert(SUCCEEDED(hr));

	buffer[0].Position = { 0.0f,0.0f,0.0f };
	buffer[1].Position = { 200.0f,0.0f,0.0f };
	buffer[2].Position = { 0.0f,200.0f,0.0f };
	buffer[3].Position = { 200.0f,200.0f,0.0f };

	buffer[0].Color = { 1.0f,1.0f,1.0f,1.0f };
	buffer[1].Color = { 1.0f,1.0f,1.0f,1.0f };
	buffer[2].Color = { 1.0f,1.0f,1.0f,1.0f };
	buffer[3].Color = { 1.0f,1.0f,1.0f,1.0f };

	buffer[0].Normal = { 0.0f,1.0f,0.0f };
	buffer[1].Normal = { 0.0f,1.0f,0.0f };
	buffer[2].Normal = { 0.0f,1.0f,0.0f };
	buffer[3].Normal = { 0.0f,1.0f,0.0f };

	buffer[0].TexCoord = { 0.0f,0.0f };
	buffer[1].TexCoord = { 1.0f,0.0f };
	buffer[2].TexCoord = { 0.0f,1.0f };
	buffer[3].TexCoord = { 1.0f,1.0f };

	m_VertexBuffer->Resource->Unmap(0, nullptr);

	m_Texture = renderManager->LoadTexture("Asset/Texture/field004.dds");
}

void UPolygon2DComponent::TickComponent(float DeltaTime)
{
	VERTEX_3D* buffer{};
	m_VertexBuffer->Resource->Map(0, nullptr, (void**)&buffer);

	buffer[0].Position = { 0.0f,0.0f,0.0f };
	buffer[1].Position = { 200.0f,0.0f,0.0f };
	buffer[2].Position = { 0.0f,200.0f,0.0f };
	buffer[3].Position = { 200.0f,200.0f,0.0f };

	buffer[0].Color = m_VertexColor;
	buffer[1].Color = m_VertexColor;
	buffer[2].Color = m_VertexColor;
	buffer[3].Color = m_VertexColor;

	buffer[0].Normal = { 0.0f,1.0f,0.0f };
	buffer[1].Normal = { 0.0f,1.0f,0.0f };
	buffer[2].Normal = { 0.0f,1.0f,0.0f };
	buffer[3].Normal = { 0.0f,1.0f,0.0f };

	buffer[0].TexCoord = { 0.0f,0.0f };
	buffer[1].TexCoord = { 1.0f,0.0f };
	buffer[2].TexCoord = { 0.0f,1.0f };
	buffer[3].TexCoord = { 1.0f,1.0f };

	m_VertexBuffer->Resource->Unmap(0, nullptr);
}

FBoxSphereBounds UPolygon2DComponent::CalcBounds(const XMMATRIX& LocalToWorld) const
{
	// 画面座標系の 2D オーバーレイは 3D フラスタムで判定できないため、
	// HALF_WORLD_MAX の「無限」境界を返して常にフラスタムを通す
	// (トランスフォームには依存しない)
	return FBoxSphereBounds(
		{ 0.0f, 0.0f, 0.0f },
		{ HALF_WORLD_MAX, HALF_WORLD_MAX, HALF_WORLD_MAX },
		HALF_WORLD_MAX);
}

FPrimitiveSceneProxy* UPolygon2DComponent::CreateSceneProxy()
{
	return new FPolygon2DSceneProxy(this, m_VertexBuffer.get(), m_Texture.get());
}

// ------------------------------------------------------------
//  APolygon2D
// ------------------------------------------------------------

APolygon2D::APolygon2D()
{
	m_PolygonComponent = CreateDefaultSubobject<UPolygon2DComponent>();
}

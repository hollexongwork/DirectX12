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
	// フレーム毎のダブルバッファ (描画時に現在フレーム側を選択)
	const VERTEX_BUFFER* m_VertexBuffer[2] = {};
	const TEXTURE* m_Texture = nullptr;

public:
	FPolygon2DSceneProxy(const UPrimitiveComponent* Component,
		const VERTEX_BUFFER* VertexBuffer0, const VERTEX_BUFFER* VertexBuffer1,
		const TEXTURE* Texture)
		: FPrimitiveSceneProxy(Component)
		, m_Texture(Texture)
	{
		m_VertexBuffer[0] = VertexBuffer0;
		m_VertexBuffer[1] = VertexBuffer1;
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

		// Vertex Buffer Setting (現在フレーム側を選択。
		// TickComponent が書いたのと同じ側になる)
		RM->SetVertexBuffer(m_VertexBuffer[RM->GetCurrentFrameIndex()]);

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

// ------------------------------------------------------------
//  4頂点クワッドの書き込み (コンストラクタ / TickComponent 共用)
// ------------------------------------------------------------
static void WriteQuadVertices(const VERTEX_BUFFER* VertexBuffer, const XMFLOAT4& Color)
{
	VERTEX_3D* buffer{};
	HRESULT hr = VertexBuffer->Resource->Map(0, nullptr, (void**)&buffer);
	assert(SUCCEEDED(hr));
	if (FAILED(hr)) return;

	buffer[0].Position = { 0.0f,0.0f,0.0f };
	buffer[1].Position = { 200.0f,0.0f,0.0f };
	buffer[2].Position = { 0.0f,200.0f,0.0f };
	buffer[3].Position = { 200.0f,200.0f,0.0f };

	buffer[0].Color = Color;
	buffer[1].Color = Color;
	buffer[2].Color = Color;
	buffer[3].Color = Color;

	buffer[0].Normal = { 0.0f,1.0f,0.0f };
	buffer[1].Normal = { 0.0f,1.0f,0.0f };
	buffer[2].Normal = { 0.0f,1.0f,0.0f };
	buffer[3].Normal = { 0.0f,1.0f,0.0f };

	buffer[0].TexCoord = { 0.0f,0.0f };
	buffer[1].TexCoord = { 1.0f,0.0f };
	buffer[2].TexCoord = { 0.0f,1.0f };
	buffer[3].TexCoord = { 1.0f,1.0f };

	VertexBuffer->Resource->Unmap(0, nullptr);
}

UPolygon2DComponent::UPolygon2DComponent()
{
	RenderManager* renderManager = RenderManager::GetInstance();

	// フレーム毎にダブルバッファ (in-flight フレームとの書き込み競合防止)。
	// 両方を初期データで埋めておく。
	for (int i = 0; i < 2; ++i)
	{
		m_VertexBuffer[i] = renderManager->CreateVertexBuffer(sizeof(VERTEX_3D), 4);
		WriteQuadVertices(m_VertexBuffer[i].get(), m_VertexColor);
	}

	m_Texture = renderManager->LoadTexture("Asset/Texture/field004.dds");
}

void UPolygon2DComponent::TickComponent(float DeltaTime)
{
	// 現在フレーム側のバッファのみ書き換える。もう一方は前フレームの
	// in-flight 描画が GPU で読んでいる可能性があるため触らない。
	const unsigned int frameIndex = RenderManager::GetInstance()->GetCurrentFrameIndex();
	WriteQuadVertices(m_VertexBuffer[frameIndex].get(), m_VertexColor);
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
	return new FPolygon2DSceneProxy(this,
		m_VertexBuffer[0].get(), m_VertexBuffer[1].get(), m_Texture.get());
}

// ------------------------------------------------------------
//  APolygon2D
// ------------------------------------------------------------

APolygon2D::APolygon2D()
{
	m_PolygonComponent = CreateDefaultSubobject<UPolygon2DComponent>();
}

#include "Main.h"
#include "RenderManager.h"
#include "AssetManager.h"
#include "Field.h"
#include "PrimitiveSceneProxy.h"

// ============================================================
//  FFieldQuadSceneProxy
//  UFieldQuadComponent のレンダー側ミラー。頂点バッファ /
//  テクスチャは shared_ptr の値スナップショット (プロキシ自身が
//  参照カウントで生存を保証する)、マテリアルも値スナップショット。
//  生成後はコンポーネントに一切触れない。
// ============================================================

class FFieldQuadSceneProxy : public FPrimitiveSceneProxy
{
private:
	std::shared_ptr<VERTEX_BUFFER> m_VertexBuffer;
	std::shared_ptr<TEXTURE>       m_Diffuse;
	std::shared_ptr<TEXTURE>       m_Normal;
	std::shared_ptr<TEXTURE>       m_ARM;
	Material                       m_Material;

	// テクスチャ + マテリアル定数 (b2) + クアッド VB / トポロジをバインド
	void BindQuad(RenderManager* RM) const
	{
		RM->SetVertexBuffer(m_VertexBuffer.get());

		RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_Diffuse.get());
		RM->SetTexture(RenderManager::TEXTURE_TYPE::NORMAL, m_Normal.get());
		RM->SetTexture(RenderManager::TEXTURE_TYPE::MSRA, m_ARM.get());

		RM->GetGraphicsCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

		m_Material.Bind(RM);
	}

public:
	FFieldQuadSceneProxy(const UPrimitiveComponent* Component,
		std::shared_ptr<VERTEX_BUFFER> VertexBuffer,
		std::shared_ptr<TEXTURE> Diffuse, std::shared_ptr<TEXTURE> Normal, std::shared_ptr<TEXTURE> ARM,
		const Material& Mat)
		: FPrimitiveSceneProxy(Component)
		, m_VertexBuffer(std::move(VertexBuffer))
		, m_Diffuse(std::move(Diffuse))
		, m_Normal(std::move(Normal))
		, m_ARM(std::move(ARM))
		, m_Material(Mat)
	{
	}

	// ---- FPrimitiveViewRelevance (単一マテリアル) ----
	FPrimitiveViewRelevance GetViewRelevance() const override
	{
		FPrimitiveViewRelevance relevance;
		relevance.bOpaque = false;

		switch (m_Material.GetBlendMode())
		{
		case EBlendMode::BLEND_Masked:
			relevance.bMasked = true;
			break;
		case EBlendMode::BLEND_Translucent:
		case EBlendMode::BLEND_Additive:
			relevance.bNormalTranslucency = true;
			break;
		case EBlendMode::BLEND_Opaque:
		default:
			relevance.bOpaque = true;
			break;
		}
		return relevance;
	}

	// ---- ベースパス (Opaque / Masked -> G-Buffer) ----
	void DrawPrimitive(RenderManager* RM) const override
	{
		// Translucent / Additive はトランスルーセンシーパスが描く
		if (IsTranslucentBlendMode(m_Material.GetBlendMode()))
			return;

		// Matrix Setting (PRIMITIVE 定数: プロキシに積まれたワールド行列)
		UploadPrimitiveConstant(RM);

		// Two Sided はカリング無効 PSO
		RM->SetPipelineState(m_Material.IsTwoSided() ? "BasePassTwoSided" : "BasePass");

		BindQuad(RM);

		// Draw
		RM->GetGraphicsCommandList()->DrawInstanced(4, 1, 0, 0);
	}

	// ---- トランスルーセンシーパス (Translucent / Additive) ----
	void DrawTranslucency(RenderManager* RM,
		ETranslucencyDrawMode Mode = ETranslucencyDrawMode::Standard) const override
	{
		const EBlendMode blendMode = m_Material.GetBlendMode();
		if (!IsTranslucentBlendMode(blendMode))
			return;

		UploadPrimitiveConstant(RM);

		const bool bTwoSided = m_Material.IsTwoSided();
		const bool bAdditive = (blendMode == EBlendMode::BLEND_Additive);

		// Additive は順序非依存のため深度プリパス不要 (StaticMesh と同じ規約)
		const char* pipeline = nullptr;
		switch (Mode)
		{
		case ETranslucencyDrawMode::DepthPrepass:
			if (bAdditive)
				return;
			pipeline = bTwoSided ? "TranslucencyDepthPrepassTwoSided" : "TranslucencyDepthPrepass";
			break;

		case ETranslucencyDrawMode::ColorEqual:
			if (bAdditive)
				pipeline = bTwoSided ? "TranslucencyAdditiveTwoSided" : "TranslucencyAdditive";
			else
				pipeline = bTwoSided ? "TranslucencyEqualTwoSided" : "TranslucencyEqual";
			break;

		case ETranslucencyDrawMode::Standard:
		default:
			if (bAdditive)
				pipeline = bTwoSided ? "TranslucencyAdditiveTwoSided" : "TranslucencyAdditive";
			else
				pipeline = bTwoSided ? "TranslucencyTwoSided" : "Translucency";
			break;
		}
		RM->SetPipelineState(pipeline);

		BindQuad(RM);

		RM->GetGraphicsCommandList()->DrawInstanced(4, 1, 0, 0);
	}

	// シャドウ深度パス: 地面クアッドを深度のみ描画。
	// Masked は t0 + b2 をバインドして OpacityMask を clip、
	// Two Sided はカリング無効、Translucent / Additive は描かない。
	void DrawShadowDepth(RenderManager* RM) const override
	{
		const EBlendMode blendMode = m_Material.GetBlendMode();
		if (IsTranslucentBlendMode(blendMode))
			return;

		UploadPrimitiveConstant(RM);

		const bool bTwoSided = m_Material.IsTwoSided();

		if (IsMaskedBlendMode(blendMode) && m_Diffuse)
		{
			RM->SetPipelineState(bTwoSided ? "ShadowDepthMaskedTwoSided" : "ShadowDepthMasked");
			RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, m_Diffuse.get());
			m_Material.Bind(RM);	// b2 (OpacityMaskClipValue)
		}
		else
		{
			RM->SetPipelineState(bTwoSided ? "ShadowDepthTwoSided" : "ShadowDepth");
		}

		RM->SetVertexBuffer(m_VertexBuffer.get());
		RM->GetGraphicsCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		RM->GetGraphicsCommandList()->DrawInstanced(4, 1, 0, 0);
	}
};

// ------------------------------------------------------------
//  UFieldQuadComponent
// ------------------------------------------------------------

UFieldQuadComponent::UFieldQuadComponent()
{
	RenderManager* renderManager = RenderManager::GetInstance();

	m_VertexBuffer = renderManager->CreateVertexBuffer(sizeof(VERTEX_3D), 4);

	VERTEX_3D* buffer{};
	HRESULT hr = m_VertexBuffer->Resource->Map(0, nullptr, (void**)&buffer);
	assert(SUCCEEDED(hr));

	buffer[0].Position = { -10.0f,0.0f,10.0f };
	buffer[1].Position = { 10.0f,0.0f,10.0f };
	buffer[2].Position = { -10.0f,0.0f,-10.0f };
	buffer[3].Position = { 10.0f,0.0f,-10.0f };

	buffer[0].Color = { 1.0f,1.0f,1.0f,1.0f };
	buffer[1].Color = { 1.0f,1.0f,1.0f,1.0f };
	buffer[2].Color = { 1.0f,1.0f,1.0f,1.0f };
	buffer[3].Color = { 1.0f,1.0f,1.0f,1.0f };

	buffer[0].Normal = { 0.0f,1.0f,0.0f };
	buffer[1].Normal = { 0.0f,1.0f,0.0f };
	buffer[2].Normal = { 0.0f,1.0f,0.0f };
	buffer[3].Normal = { 0.0f,1.0f,0.0f };

	buffer[0].TexCoord = { 0.0f * 3.0f,0.0f * 3.0f };
	buffer[1].TexCoord = { 1.0f * 3.0f,0.0f * 3.0f };
	buffer[2].TexCoord = { 0.0f * 3.0f,1.0f * 3.0f };
	buffer[3].TexCoord = { 1.0f * 3.0f,1.0f * 3.0f };

	m_VertexBuffer->Resource->Unmap(0, nullptr);

	// テクスチャは FAssetManager のキャッシュ経由で共有取得する
	// (BaseColor のみ sRGB として読む)
	m_Diffuse = FAssetManager::GetInstance()->LoadTexture("Asset/Texture/wood_table_diff_4k_1.dds", true);
	m_Normal = FAssetManager::GetInstance()->LoadTexture("Asset/Texture/wood_table_nor_dx_4k_1.dds");
	m_ARM = FAssetManager::GetInstance()->LoadTexture("Asset/Texture/wood_table_arm_4k_1.dds");
}

FBoxSphereBounds UFieldQuadComponent::CalcBounds(const XMMATRIX& LocalToWorld) const
{
	// コンストラクタで組んだクアッド頂点 (±10m, Y=0) のローカル AABB
	const FBoxSphereBounds localBounds = FBoxSphereBounds::FromMinMax(
		{ -10.0f, 0.0f, -10.0f },
		{ 10.0f, 0.0f,  10.0f });

	return localBounds.TransformBy(LocalToWorld);
}

FPrimitiveSceneProxy* UFieldQuadComponent::CreateSceneProxy()
{
	// shared_ptr を値コピーで渡す (プロキシが参照カウントで保持する)
	return new FFieldQuadSceneProxy(this,
		m_VertexBuffer,
		m_Diffuse, m_Normal, m_ARM,
		m_Material);
}

// ------------------------------------------------------------
//  AField
// ------------------------------------------------------------

AField::AField()
{
	m_QuadComponent = CreateDefaultSubobject<UFieldQuadComponent>();
}

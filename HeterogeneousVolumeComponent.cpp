#include "Main.h"
#include "RenderManager.h"
#include "HeterogeneousVolumeComponent.h"
#include "HeterogeneousVolumes.h"
#include "PrimitiveSceneProxy.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// ============================================================
//  FProceduralCloudParams
// ============================================================

namespace
{
	template <class T>
	T ClampValue(T v, T lo, T hi)
	{
		return (v < lo) ? lo : ((v > hi) ? hi : v);
	}

	// 決定的な乱数 (PCG 系の整数ハッシュを進める。std::rand に依存しない)
	struct FCloudRandom
	{
		unsigned int State;

		explicit FCloudRandom(unsigned int Seed) : State(Seed * 747796405u + 2891336453u) {}

		unsigned int NextUInt()
		{
			State = State * 747796405u + 2891336453u;
			unsigned int word = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
			return (word >> 22u) ^ word;
		}

		// [0, 1)
		float NextFloat()
		{
			return (float)(NextUInt() >> 8) * (1.0f / 16777216.0f);
		}

		float Range(float Min, float Max)
		{
			return Min + (Max - Min) * NextFloat();
		}
	};
}

void FProceduralCloudParams::Sanitize()
{
	NumPuffs = ClampValue(NumPuffs, 1, (int)MaxPuffs);
	PuffRadiusMin = ClampValue(PuffRadiusMin, 0.05f, 1.0f);
	PuffRadiusMax = ClampValue(PuffRadiusMax, PuffRadiusMin, 1.0f);
	Spread = ClampValue(Spread, 0.0f, 1.0f);
	TowerHeight = ClampValue(TowerHeight, 0.0f, 1.0f);
	BottomHeight = ClampValue(BottomHeight, -1.0f, 0.9f);
	BottomSoftness = ClampValue(BottomSoftness, 0.001f, 0.5f);
	SmoothUnion = ClampValue(SmoothUnion, 0.001f, 0.5f);
	EdgeSoftness = ClampValue(EdgeSoftness, 0.002f, 0.5f);
	BillowFrequency = ClampValue(BillowFrequency, 0.1f, 32.0f);
	BillowStrength = ClampValue(BillowStrength, 0.0f, 0.5f);
	BillowOctaves = ClampValue(BillowOctaves, 1, 5);
	BillowPerlinMix = ClampValue(BillowPerlinMix, 0.0f, 1.0f);
	DetailFrequency = ClampValue(DetailFrequency, 0.1f, 64.0f);
	DetailStrength = ClampValue(DetailStrength, 0.0f, 0.3f);
	DetailOctaves = ClampValue(DetailOctaves, 1, 4);
	DensityVariation = ClampValue(DensityVariation, 0.0f, 1.0f);
	BorderFade = ClampValue(BorderFade, 0.005f, 0.5f);
}

unsigned long long FProceduralCloudParams::ComputeHash() const
{
	// 全メンバが 4 バイト (パディング無し) なのでバイト列をそのままハッシュする
	static_assert(sizeof(FProceduralCloudParams) % 4 == 0, "FProceduralCloudParams must be tightly packed");

	const unsigned char* bytes = reinterpret_cast<const unsigned char*>(this);
	unsigned long long hash = 1469598103934665603ull;
	for (size_t i = 0; i < sizeof(FProceduralCloudParams); ++i)
	{
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

XMFLOAT3 FProceduralCloudParams::ComputeNoiseOffset() const
{
	FCloudRandom random(Seed ^ 0x9E3779B9u);
	return XMFLOAT3(random.Range(0.0f, 64.0f), random.Range(0.0f, 64.0f), random.Range(0.0f, 64.0f));
}

unsigned int FProceduralCloudParams::GeneratePuffs(const XMFLOAT3& AspectScale, XMFLOAT4 OutPuffs[MaxPuffs]) const
{
	const float ax = (std::max)(AspectScale.x, 1e-3f);
	const float ay = (std::max)(AspectScale.y, 1e-3f);
	const float az = (std::max)(AspectScale.z, 1e-3f);

	// ビロー / ディテールの押し出しと縁のなじみを見込んだ余白 (ボックスからはみ出さない)
	const float margin = BillowStrength + DetailStrength + EdgeSoftness + BorderFade * (std::min)(ax, (std::min)(ay, az));

	const float bottomY = BottomHeight * ay;
	const unsigned int count = (unsigned int)ClampValue(NumPuffs, 1, (int)MaxPuffs);

	FCloudRandom random(Seed);

	for (unsigned int i = 0; i < count; ++i)
	{
		// 水平: 円盤状 (中心に寄せる)。最初の 1 個は中心に置いて芯にする
		const float angle = random.Range(0.0f, 6.2831853f);
		const float radial = (i == 0) ? 0.0f : std::pow(random.NextFloat(), 0.7f);
		const float centerness = 1.0f - radial;

		float x = std::cos(angle) * radial * ax * Spread;
		float z = std::sin(angle) * radial * az * Spread;

		// 半径: 中心ほど大きい (垂直半幅が基準)
		float radius = random.Range(PuffRadiusMin, PuffRadiusMax) * ay * (0.75f + 0.4f * centerness);

		// 3 軸とも余白込みで収まる大きさへ
		const float maxRadius = (std::min)(ax, (std::min)(ay, az)) - margin;
		radius = (std::min)(radius, (std::max)(maxRadius, 0.02f));

		// 高さ: 雲底に座らせ、中心ほど上へ積む (塔)
		const float lift = random.NextFloat();
		const float topLimit = ay - margin - radius;
		const float baseY = bottomY + radius * 0.55f;
		float y = baseY + lift * lift * TowerHeight * (0.3f + 0.7f * centerness) * (std::max)(topLimit - baseY, 0.0f);
		y = (std::min)(y, topLimit);

		x = ClampValue(x, -(ax - margin - radius), ax - margin - radius);
		z = ClampValue(z, -(az - margin - radius), az - margin - radius);

		OutPuffs[i] = XMFLOAT4(x, y, z, radius);
	}

	for (unsigned int i = count; i < MaxPuffs; ++i)
	{
		OutPuffs[i] = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	return count;
}

// ============================================================
//  FProceduralFireParams
// ============================================================

void FProceduralFireParams::Sanitize()
{
	bAnimate = (bAnimate != 0) ? 1 : 0;
	BaseRadius = ClampValue(BaseRadius, 0.05f, 1.0f);
	TipRadiusScale = ClampValue(TipRadiusScale, 0.0f, 1.0f);
	TaperPower = ClampValue(TaperPower, 0.05f, 8.0f);
	FlameHeight = ClampValue(FlameHeight, 0.05f, 1.0f);
	Turbulence = ClampValue(Turbulence, 0.0f, 1.0f);
	NoiseFrequency = ClampValue(NoiseFrequency, 0.05f, 32.0f);
	NoiseOctaves = ClampValue(NoiseOctaves, 1, 5);
	NoiseStrength = ClampValue(NoiseStrength, 0.0f, 4.0f);
	HeightErosion = ClampValue(HeightErosion, 0.0f, 8.0f);
	RiseSpeed = ClampValue(RiseSpeed, 0.0f, 20.0f);
	EvolutionSpeed = ClampValue(EvolutionSpeed, 0.0f, 20.0f);
	HeatGain = ClampValue(HeatGain, 0.0f, 8.0f);
	FlameOpacity = ClampValue(FlameOpacity, 0.0f, 4.0f);
	SmokeDensity = ClampValue(SmokeDensity, 0.0f, 4.0f);
	SmokeStart = ClampValue(SmokeStart, 0.0f, 1.0f);
}

unsigned long long FProceduralFireParams::ComputeHash() const
{
	static_assert(sizeof(FProceduralFireParams) % 4 == 0, "FProceduralFireParams must be tightly packed");

	const unsigned char* bytes = reinterpret_cast<const unsigned char*>(this);
	unsigned long long hash = 1469598103934665603ull;
	for (size_t i = 0; i < sizeof(FProceduralFireParams); ++i)
	{
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

float FProceduralFireParams::ComputeNoiseOffset() const
{
	FCloudRandom random(Seed ^ 0x51ED270Bu);
	return random.Range(0.0f, 64.0f);
}

// ============================================================
//  FHeterogeneousVolumeSceneProxy
//  UHeterogeneousVolumeComponent のレンダー側ミラー
//  (FHeterogeneousVolumeSceneProxy 相当)。サーフェス系のパスには
//  一切参加せず、FHeterogeneousVolumeData だけをレンダラへ渡す。
// ============================================================
class FHeterogeneousVolumeSceneProxy : public FPrimitiveSceneProxy
{
private:
	// トランスフォーム / 境界は SetTransform でプッシュされるので、取得時に写す
	mutable FHeterogeneousVolumeData m_Data;

public:
	explicit FHeterogeneousVolumeSceneProxy(const UHeterogeneousVolumeComponent* Component)
		: FPrimitiveSceneProxy(Component)
	{
		m_Data.VolumeResolution = Component->GetVolumeResolution();
		m_Data.StepFactor = Component->GetStepFactor();
		m_Data.ShadowStepFactor = Component->GetShadowStepFactor();
		m_Data.ShadowBiasFactor = Component->GetShadowBiasFactor();
		m_Data.LightingDownsampleFactor = Component->GetLightingDownsampleFactor();
		m_Data.Source = Component->GetVolumeSource();
		m_Data.Cloud = Component->GetCloudParams();
		m_Data.Cloud.Sanitize();
		m_Data.Fire = Component->GetFireParams();
		m_Data.Fire.Sanitize();
		m_Data.Material = Component->GetVolumeMaterial();
		m_Data.RenderResource = Component->GetRenderResource();

		// 影 / Distance Field / ベロシティには参加しない
		m_bCastShadow = false;
		m_bAffectDistanceField = false;
		m_bRenderVelocity = false;
	}

	// どのメッシュパスにも流れない (FHeterogeneousVolumeRenderer が直接描く)
	FPrimitiveViewRelevance GetViewRelevance() const override
	{
		FPrimitiveViewRelevance relevance;
		relevance.bOpaque = false;
		return relevance;
	}

	void DrawPrimitive(RenderManager* RHI) const override {}

	const FHeterogeneousVolumeData* GetHeterogeneousVolumeData() const override
	{
		m_Data.LocalToWorld = m_LocalToWorld;
		m_Data.Bounds = m_Bounds;
		return &m_Data;
	}
};

// ============================================================
//  UHeterogeneousVolumeComponent
// ============================================================

UHeterogeneousVolumeComponent::UHeterogeneousVolumeComponent()
{
	// GPU リソースの入れ物 (中身は FHeterogeneousVolumeRenderer が遅延生成する)
	m_RenderResource = std::make_shared<FHeterogeneousVolumeRenderResource>();

	// ボリュームは影 / Distance Field / ベロシティに参加しない
	m_bCastShadow = false;
	m_bAffectDistanceFieldLighting = false;
	m_bRenderVelocity = false;
}

void UHeterogeneousVolumeComponent::SetVolumeResolution(const XMUINT3& Resolution)
{
	m_VolumeResolution.x = ClampValue(Resolution.x, 16u, 384u);
	m_VolumeResolution.y = ClampValue(Resolution.y, 16u, 384u);
	m_VolumeResolution.z = ClampValue(Resolution.z, 16u, 384u);
	MarkRenderStateDirty();
}

FBoxSphereBounds UHeterogeneousVolumeComponent::CalcBounds(const XMMATRIX& LocalToWorld) const
{
	return FBoxSphereBounds::FromMinMax({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f }).TransformBy(LocalToWorld);
}

FPrimitiveSceneProxy* UHeterogeneousVolumeComponent::CreateSceneProxy()
{
	return new FHeterogeneousVolumeSceneProxy(this);
}

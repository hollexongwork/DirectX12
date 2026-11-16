#include "Main.h"
#include "Campfire.h"
#include "ProceduralMeshGenerator.h"
#include "ProceduralTree.h"
#include <cmath>
#include <fstream>

namespace
{
	const unsigned int WoodMaterialSlot = 0;
	const unsigned int StoneMaterialSlot = 1;

	bool FileExists(const std::string& Path)
	{
		return !Path.empty() && std::ifstream(Path, std::ios::binary).good();
	}

	// Y 軸方向の円柱 (原点中心, 高さ = |To - From|) を From -> To の線分へ写す行列
	XMMATRIX MakeSegmentTransform(FXMVECTOR From, FXMVECTOR To)
	{
		const XMVECTOR axis = XMVectorSubtract(To, From);
		const XMVECTOR direction = XMVector3Normalize(axis);
		const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

		XMMATRIX rotation = XMMatrixIdentity();
		const float cosAngle = XMVectorGetX(XMVector3Dot(up, direction));
		if (cosAngle < 0.9999f)
		{
			XMVECTOR rotationAxis = XMVector3Cross(up, direction);
			if (XMVectorGetX(XMVector3LengthSq(rotationAxis)) < 1e-8f)
			{
				rotationAxis = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);	// 真下向き
			}
			const float clamped = (cosAngle < -1.0f) ? -1.0f : cosAngle;
			rotation = XMMatrixRotationAxis(XMVector3Normalize(rotationAxis), std::acos(clamped));
		}

		const XMVECTOR middle = XMVectorScale(XMVectorAdd(From, To), 0.5f);
		return rotation * XMMatrixTranslationFromVector(middle);
	}

	// 丸太 1 本 (From が太い側)
	void AppendLog(FProceduralMeshSection& Wood, const XMFLOAT3& From, const XMFLOAT3& To, float Radius)
	{
		const XMVECTOR from = XMLoadFloat3(&From);
		const XMVECTOR to = XMLoadFloat3(&To);
		const float length = XMVectorGetX(XMVector3Length(XMVectorSubtract(to, from)));

		FProceduralMeshSection log = FProceduralMeshBuilder::CreateCylinder(Radius, Radius * 0.85f, length, 12, 1, true);
		FProceduralMeshBuilder::Transform(log, MakeSegmentTransform(from, to));
		FProceduralMeshBuilder::Append(Wood, log);
	}

	// 薪 (ティピー型に組んだ 6 本 + 地面に寝かせた 3 本) と囲みの石 9 個
	FProceduralMeshData BuildCampfireMesh()
	{
		FProceduralMeshData data;

		// ---- 薪 ----
		FProceduralMeshSection wood;
		wood.MaterialIndex = WoodMaterialSlot;

		const float twoPi = 6.2831853f;

		// ティピー: 根元を半径 0.42 m の円周に置き、頂部を中心近くへ寄せる
		const int teepeeCount = 6;
		for (int i = 0; i < teepeeCount; ++i)
		{
			const float angle = twoPi * ((float)i + 0.15f * (float)(i % 2)) / (float)teepeeCount;
			const float baseRadius = 0.42f + 0.04f * (float)(i % 3);
			const float topRadius = 0.07f;
			const float topAngle = angle + 0.6f;	// 頂部は少しねじって互いに寄りかからせる

			const XMFLOAT3 from = { std::cos(angle) * baseRadius, 0.03f, std::sin(angle) * baseRadius };
			const XMFLOAT3 to = { std::cos(topAngle) * topRadius, 0.62f + 0.05f * (float)(i % 2), std::sin(topAngle) * topRadius };
			AppendLog(wood, from, to, 0.055f + 0.008f * (float)(i % 3));
		}

		// 地面に寝かせた薪 (中心を通る弦)
		const int groundCount = 3;
		for (int i = 0; i < groundCount; ++i)
		{
			const float angle = twoPi * ((float)i + 0.5f) / (float)groundCount * 0.5f + 0.35f;
			const float halfLength = 0.34f;
			const float radius = 0.06f;
			const XMFLOAT3 from = { std::cos(angle) * halfLength, radius + 0.005f * (float)i, std::sin(angle) * halfLength };
			const XMFLOAT3 to = { -std::cos(angle) * halfLength, radius + 0.03f * (float)i, -std::sin(angle) * halfLength };
			AppendLog(wood, from, to, radius);
		}
		data.Sections.push_back(std::move(wood));

		// ---- 石 (半径 0.8 m の輪) ----
		FProceduralMeshSection stones;
		stones.MaterialIndex = StoneMaterialSlot;

		const int stoneCount = 9;
		for (int i = 0; i < stoneCount; ++i)
		{
			FProceduralRockParams rock;
			rock.Radius = 0.13f + 0.03f * (float)((i * 7) % 3);
			rock.Subdivisions = 2;
			rock.Displacement = 0.28f;
			rock.Frequency = 3.0f;
			rock.Octaves = 3;
			rock.Seed = 11u + (unsigned int)i * 5u;
			rock.Stretch = { 1.0f + 0.15f * (float)(i % 2), 0.7f, 1.0f };
			rock.bFlatShading = false;

			const float angle = twoPi * ((float)i + 0.2f * (float)(i % 2)) / (float)stoneCount;
			const float ringRadius = 0.8f + 0.04f * (float)(i % 3);

			FProceduralMeshSection stone = FProceduralMeshBuilder::CreateRock(rock);
			FProceduralMeshBuilder::Transform(stone,
				XMMatrixRotationY(angle * 1.7f)
				* XMMatrixTranslation(std::cos(angle) * ringRadius, rock.Radius * 0.45f, std::sin(angle) * ringRadius));
			FProceduralMeshBuilder::Append(stones, stone);
		}
		data.Sections.push_back(std::move(stones));

		return data;
	}
}

ACampfire::ACampfire()
{
	bCanEverTick = true;	// ライトのゆらぎ

	// ------------------------------------------------------------
	//  C0 (Root): 薪 + 石
	// ------------------------------------------------------------
	m_WoodComponent = CreateDefaultSubobject<UStaticMeshComponent>();

	// 既定の配置: テーブルの +Z 側、地面の上 (アクターの位置 = 火床の中心)。
	// SetActorLocation は Root に書くので、Root (薪) を作った後に呼ぶこと
	SetActorLocation({ 4.0f, 0.0f, 6.5f });
	m_WoodComponent->SetProceduralMesh("CampfireWoodAndStones_v1", BuildCampfireMesh());
	m_WoodComponent->SetNumMaterialSlots(2);

	// テクスチャ: 薪は木の樹皮 (プロシージャルツリーと共有)。石は砂のテクスチャを灰色に寄せて使う。
	// FStaticMeshSceneProxy::BindSlot は未設定のテクスチャを直前のバインドのままにするので、
	// 両スロットとも BaseColor / Normal / ARM を必ず割り当てる
	FProceduralTreeTextures::EnsureGenerated();
	using T = FProceduralTreeTextures;
	const std::string barkDiff = T::GetBarkBaseColorPath();
	const std::string barkNormal = FileExists(T::GetBarkNormalPath()) ? T::GetBarkNormalPath() : T::GetFlatNormalPath();
	const std::string barkArm = T::GetBarkARMPath();
	const std::string stoneDiff = "Asset/Texture/coast_sand_02_diff_4k.dds";
	const std::string stoneNormal = "Asset/Texture/coast_sand_02_nor_dx_4k.dds";

	if (FileExists(barkDiff))   m_WoodComponent->SetBaseColorTexture(WoodMaterialSlot, barkDiff.c_str());
	if (FileExists(barkNormal)) m_WoodComponent->SetNormalTexture(WoodMaterialSlot, barkNormal.c_str());
	if (FileExists(barkArm))    m_WoodComponent->SetARMTexture(WoodMaterialSlot, barkArm.c_str());

	if (FileExists(stoneDiff))        m_WoodComponent->SetBaseColorTexture(StoneMaterialSlot, stoneDiff.c_str());
	else if (FileExists(barkDiff))    m_WoodComponent->SetBaseColorTexture(StoneMaterialSlot, barkDiff.c_str());
	if (FileExists(stoneNormal))      m_WoodComponent->SetNormalTexture(StoneMaterialSlot, stoneNormal.c_str());
	else if (FileExists(barkNormal))  m_WoodComponent->SetNormalTexture(StoneMaterialSlot, barkNormal.c_str());
	if (FileExists(barkArm))          m_WoodComponent->SetARMTexture(StoneMaterialSlot, barkArm.c_str());

	{
		// 焦げた薪 (樹皮テクスチャを暗く)
		Material& wood = m_WoodComponent->GetMaterial(WoodMaterialSlot);
		wood = Material();
		wood.Params.BaseColor = { 0.42f, 0.36f, 0.32f, 1.0f };
		wood.Params.Metallic = 0.0f;
		wood.Params.Specular = 0.2f;
		wood.Params.Roughness = 0.9f;
		wood.Params.NormalWeight = 1.0f;

		// 石 (砂のテクスチャを灰色へ)
		Material& stone = m_WoodComponent->GetMaterial(StoneMaterialSlot);
		stone = Material();
		stone.Params.BaseColor = { 0.55f, 0.55f, 0.58f, 1.0f };
		stone.Params.Metallic = 0.0f;
		stone.Params.Specular = 0.3f;
		stone.Params.Roughness = 0.85f;
		stone.Params.NormalWeight = 1.0f;

		m_WoodComponent->MarkRenderStateDirty();	// GetMaterial() 直接書き換えの反映
	}

	// ------------------------------------------------------------
	//  C1: 炎 (Heterogeneous Volume, ソース = ProceduralFire)
	//  ボックスは幅 1.2 m x 高さ 1.9 m。底面 (火床) を薪の根元より少し上へ置く
	// ------------------------------------------------------------
	m_FlameComponent = CreateDefaultSubobject<UHeterogeneousVolumeComponent>();
	m_FlameComponent->SetupAttachment(m_WoodComponent);
	m_FlameComponent->SetRelativeScale3D({ 0.6f, 0.95f, 0.6f });	// 半幅 [m]
	m_FlameComponent->SetRelativeLocation({ 0.0f, 0.95f + 0.08f, 0.0f });

	m_FlameComponent->SetVolumeSource(EHeterogeneousVolumeSource::ProceduralFire);
	m_FlameComponent->SetVolumeResolution({ 64, 128, 64 });	// 1.9 cm x 1.5 cm ボクセル (RG16F 2 MB)
	m_FlameComponent->SetStepFactor(1.5f);

	{
		// 炎のマテリアル: 熱 -> 黒体放射 (1200..2700 K)。すす / 煙は暗い灰色
		FHeterogeneousVolumeMaterial material;
		material.Albedo = { 0.10f, 0.09f, 0.08f };
		material.Extinction = 2.5f;
		// 黒体の式は 1600 K 以下がほぼ純赤 (G/R < 0.16) なので、橙〜黄の炎にするには
		// 実際のすすの温度 (1200..1800 K) より高めのレンジへ写す
		material.BlackbodyTemperatureMin = 1200.0f;
		material.BlackbodyTemperatureMax = 2700.0f;
		material.BlackbodyIntensity = 12.0f;
		material.DirectionalLightIntensityScale = 1.0f;
		m_FlameComponent->SetVolumeMaterial(material);
	}

	// ------------------------------------------------------------
	//  C2: 炎の明かり (ポイントライト。Tick でゆらす)
	// ------------------------------------------------------------
	m_LightComponent = CreateDefaultSubobject<UPointLightComponent>();
	m_LightComponent->SetupAttachment(m_WoodComponent);
	m_LightComponent->SetRelativeLocation({ 0.0f, 0.75f, 0.0f });
	m_LightComponent->SetIntensity(200.0f);				// lm (火床から 0.75 m。既存の 800 lm の電球より近いので控えめ)
	m_LightComponent->SetVolumetricScatteringIntensity(0.5f);	// 濃いフォグの中でも光芒が白飛びしないように
	m_LightComponent->SetUseTemperature(true);
	m_LightComponent->SetTemperature(1900.0f);			// 炎の色
	m_LightComponent->SetAttenuationRadius(14.0f);
	m_LightComponent->SetSourceRadius(0.25f);			// 炎の大きさ (やわらかいハイライト)
}

void ACampfire::Tick(float DeltaTime)
{
	if (m_LightComponent == nullptr)
	{
		return;
	}

	// 基本周波数 + 非整数倍の倍音 2 つ (周期が揃わないのでゆらぎが繰り返して見えない)
	m_FlickerTime += DeltaTime * m_FlickerSpeed;
	const float wave =
		0.50f * std::sin(m_FlickerTime)
		+ 0.30f * std::sin(m_FlickerTime * 2.31f + 1.3f)
		+ 0.20f * std::sin(m_FlickerTime * 4.17f + 4.7f);
	const float flicker = 1.0f + m_FlickerAmount * wave;	// 1 ± FlickerAmount

	// 「今の Intensity / 前回の係数」= ゆらぎの中心。Details / INI で変えた値にそのまま追従する
	const float center = m_LightComponent->GetIntensity() / m_LastFlicker;
	m_LightComponent->SetIntensity(center * flicker);
	m_LastFlicker = flicker;
}

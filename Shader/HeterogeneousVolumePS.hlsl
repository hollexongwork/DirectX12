// b2 を Volume ドメイン (Heterogeneous Volume) のマテリアル定数に差し替える。
// サーフェス用の MaterialConstantBuffer は宣言しない (ConstantBuffers.hlsl)
#define OVERRIDE_MATERIAL_CONSTANT_BUFFER 1

#include "HeightFogCommon.hlsl"

// =============================================================
//  HeterogeneousVolumePS
//  RenderWithLiveShading (HeterogeneousVolumesLiveShadingPipeline.usf の
//  レイマーチ) 相当。フルスクリーンクアッド (DeferredVS) で描き、
//  ピクセルごとに視線をボリュームの OBB (単位ボックス [-1,1]^3 を
//  LocalToWorld で写したもの) と交差させてレイマーチする。
//
//    密度        : HeterogeneousVolumeDensity (t41.r, 0..1) x Extinction [1/m]
//    発光        : Emissive x 密度 + 黒体放射 (t41.g の熱 0..1 を BlackbodyTemperatureMin..Max [K] へ
//                  写し、BlackBody ノードと同じ式で色と強さにする) x BlackbodyIntensity x 熱
//    太陽        : DirectionalLightColor (b0, 選択されたフォワードディレクショナルライト)
//                  x exp(-Extinction x 光学的長さ) (ライティングキャッシュ t42.r)
//                  x 2 ローブ HG 位相関数。マルチスキャッタはオクターブ近似
//                  (Wrenninge 2015: 寄与 b^i / 遮蔽 a^i / 偏心 c^i)
//    天空光      : IBL irradiance (t6) の真上 / 真下 x キャッシュ (t42.g / .b) の遮蔽
//    積分        : Frostbite のエネルギー保存積分 (属性はステップ内で一定とみなす)
//    シーン深度  : 不透明の手前までで打ち切る (t3)
//    フォグ      : 透過率で重み付けした平均深度で Exponential Height Fog /
//                  Volumetric Fog を掛ける (r.HeterogeneousVolumes.HeightFog 相当)
//
//  出力: rgb = ボリュームのインスキャッタ (加算), a = 透過率 (乗算)
//  ブレンド (PSO "HeterogeneousVolume", EBlendStatePreset::HeightFog):
//    SceneColor' = Src.rgb + SceneColor x Src.a
//
//  [PORT] ローカルライト / ボリュームがシーンへ落とす影 / 低解像度レンダリング +
//  アップサンプルは未対応。
// =============================================================

// C++ 側 (HeterogeneousVolumes.h) の FHeterogeneousVolumePassConstant と 1:1 ミラー必須 (176 bytes)
cbuffer HeterogeneousVolumeConstantBuffer : register(b2)
{
    float4x4 HVWorldToBox; // ワールド -> ボックス空間 [-1,1]^3 (転置済み)
    float4 HVAlbedo; // rgb = Albedo, w = Extinction [1/m] (密度 1 のとき)
    float4 HVEmissive; // rgb = Emissive [/m], w = AmbientIntensity
    float4 HVMarchParams; // x = StepSize [m], y = MaxStepCount, z = Jitter (0/1), w = ApplyFog (0/1)
    float4 HVPhaseParams; // x = g (前方), y = g (後方), z = 前方ローブの重み, w = IndirectLighting (0/1)
    float4 HVMultiScatter; // x = Contribution, y = Occlusion, z = Eccentricity, w = Octaves (1..4)
    float4 HVLightParams; // x = DirectionalLightIntensityScale, y = AmbientOcclusionScale, z = GroundBounce, w = 未使用
    float4 HVBlackbody; // x = TemperatureMin [K], y = TemperatureMax [K], z = Intensity [/m] (0 = 無効), w = 未使用
};

// 黒体放射の色 x 強さ (MaterialExpressionBlackBody と同じ式 [M])。
// 色度は Krystek の近似 (CIE 1960 UCS) から XYZ -> 線形 sRGB、強さは pow(0.0004 T, 4)。
// 低温では青が負になるので 0 で切る
float3 HVBlackBody(float Temperature)
{
    const float T = max(Temperature, 300.0f);
    const float u = (0.860117757f + 1.54118254e-4f * T + 1.28641212e-7f * T * T)
        / (1.0f + 8.42420235e-4f * T + 7.08145163e-7f * T * T);
    const float v = (0.317398726f + 4.22806245e-5f * T + 4.20481691e-8f * T * T)
        / (1.0f - 2.89741816e-5f * T + 1.61456053e-7f * T * T);

    const float x = 3.0f * u / (2.0f * u - 8.0f * v + 4.0f);
    const float y = 2.0f * v / (2.0f * u - 8.0f * v + 4.0f);
    const float z = 1.0f - x - y;

    const float Y = 1.0f;
    const float X = Y / y * x;
    const float Z = Y / y * z;

    const float3 rgb = float3(
        3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
        -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
        0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z);

    return max(rgb, 0.0f) * pow(0.0004f * T, 4.0f);
}

// Interleaved Gradient Noise (TemporalAACommon.hlsl と同式。フレームごとにずらす)
float HVInterleavedGradientNoise(float2 PixelPos, float FrameId)
{
    PixelPos += FrameId * (float2(47.0f, 17.0f) * 0.695f);
    const float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(PixelPos, magic.xy)));
}

float HVHenyeyGreenstein(float g, float CosTheta)
{
    float g2 = g * g;
    float denom = max(1.0f + g2 - 2.0f * g * CosTheta, 1e-4f);
    return (1.0f - g2) / (4.0f * PI * pow(denom, 1.5f));
}

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;

    // ---- 視線 (カメラ -> 遠クリップ面上の点) ----
    const float2 uv = input.TexCoord;
    const float2 ndcXY = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);

    float4 farPos = mul(float4(ndcXY, 1.0f, 1.0f), InvViewProjection);
    farPos /= farPos.w;

    const float3 rayOrigin = WorldCameraOrigin.xyz;
    const float3 rayDir = normalize(farPos.xyz - rayOrigin);

    // ---- シーン深度までの距離 [m] ----
    const float depth = TextureDepth.Load(int3(input.Position.xy, 0)).r;
    float4 scenePos = mul(float4(ndcXY, depth, 1.0f), InvViewProjection);
    scenePos /= scenePos.w;
    const float sceneDistance = dot(scenePos.xyz - rayOrigin, rayDir);

    // ---- OBB との交差 (ボックス空間のスラブ法。t はワールドのメートル) ----
    const float3 boxOrigin = mul(float4(rayOrigin, 1.0f), HVWorldToBox).xyz;
    const float3 boxDir = mul(float4(rayDir, 0.0f), HVWorldToBox).xyz;

    const float3 safeDir = float3(
        abs(boxDir.x) > 1e-6f ? boxDir.x : 1e-6f,
        abs(boxDir.y) > 1e-6f ? boxDir.y : 1e-6f,
        abs(boxDir.z) > 1e-6f ? boxDir.z : 1e-6f);
    const float3 t1 = (-1.0f - boxOrigin) / safeDir;
    const float3 t2 = (1.0f - boxOrigin) / safeDir;
    const float3 tMin3 = min(t1, t2);
    const float3 tMax3 = max(t1, t2);

    const float tNear = max(max(tMin3.x, max(tMin3.y, tMin3.z)), 0.0f);
    const float tFar = min(min(tMax3.x, min(tMax3.y, tMax3.z)), sceneDistance);

    if (tFar <= tNear)
    {
        discard;
    }

    // ---- ステップ ----
    const float marchLength = tFar - tNear;
    const uint maxSteps = max((uint) HVMarchParams.y, 1u);
    const uint numSteps = clamp((uint) ceil(marchLength / max(HVMarchParams.x, 1e-4f)), 1u, maxSteps);
    const float dt = marchLength / (float) numSteps;

    float jitter = 0.5f;
    [flatten]
    if (HVMarchParams.z > 0.5f)
    {
        jitter = HVInterleavedGradientNoise(input.Position.xy, (float) StateFrameIndexMod8);
    }

    // ---- ライティングの定数部 ----
    const float extinctionScale = HVAlbedo.w;
    const float3 albedo = saturate(HVAlbedo.rgb);
    const float cosTheta = dot(rayDir, DirectionalLightDirection.xyz);
    const float3 sunColor = DirectionalLightColor.rgb * HVLightParams.x;

    // マルチスキャッタのオクターブごとの位相関数 (視線とライトで決まるのでループの外)
    const uint numOctaves = clamp((uint) (HVMultiScatter.w + 0.5f), 1u, 4u);
    float octavePhase[4];
    float octaveContribution[4];
    float octaveOcclusion[4];
    {
        float a = 1.0f;
        float b = 1.0f;
        float c = 1.0f;
        [unroll]
        for (uint o = 0; o < 4; ++o)
        {
            float phase = lerp(
                HVHenyeyGreenstein(HVPhaseParams.y * c, cosTheta),
                HVHenyeyGreenstein(HVPhaseParams.x * c, cosTheta),
                saturate(HVPhaseParams.z));
            octavePhase[o] = phase;
            octaveContribution[o] = (o < numOctaves) ? b : 0.0f;
            octaveOcclusion[o] = a;
            a *= HVMultiScatter.y;
            b *= HVMultiScatter.x;
            c *= HVMultiScatter.z;
        }
    }

    // 天空光: irradiance キューブは E(n) / π (= コサイン加重の平均ラディアンス。
    // IBL_Irradiance.hlsl の畳み込みが π x 平均で正規化済みで、サーフェスは albedo を
    // 掛けるだけで使う) を持つので、そのまま半球の平均ラディアンスとして使う。
    // 上半球 (空) と下半球 (地面の照り返し) をそれぞれキャッシュの遮蔽で減衰させて足す。
    // 遮蔽は太陽と同じオクターブ近似 (正規化して 0..1) にして、厚い雲の中まで
    // 多重散乱で回り込む分を表す
    float3 skyTop = float3(0.0f, 0.0f, 0.0f);
    float3 skyBottom = float3(0.0f, 0.0f, 0.0f);
    [branch]
    if (HVPhaseParams.w > 0.5f)
    {
        skyTop = IrradianceCube.SampleLevel(Sampler2, float3(0.0f, 1.0f, 0.0f), 0.0f).rgb;
        skyBottom = IrradianceCube.SampleLevel(Sampler2, float3(0.0f, -1.0f, 0.0f), 0.0f).rgb;
        skyTop *= HVEmissive.w;
        skyBottom *= HVEmissive.w * HVLightParams.z;
    }
    const float ambientOcclusionScale = HVLightParams.y * extinctionScale;

    // オクターブの寄与の合計 (天空光の遮蔽の正規化用)
    const float octaveContributionSum =
        max(octaveContribution[0] + octaveContribution[1] + octaveContribution[2] + octaveContribution[3], 1e-4f);

    // ---- レイマーチ ----
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0f;
    float weightedDistance = 0.0f;
    float weightSum = 0.0f;

    [loop]
    for (uint i = 0; i < numSteps; ++i)
    {
        const float t = tNear + ((float) i + jitter) * dt;
        const float3 uvw = (boxOrigin + boxDir * t) * 0.5f + 0.5f;

        const float2 volumeSample = HeterogeneousVolumeDensity.SampleLevel(Sampler2, uvw, 0.0f).rg;
        const float density = volumeSample.r;
        const float heat = volumeSample.g; // 雲 (R16F) では 0

        [branch]
        if (density > 0.001f || heat > 0.001f)
        {
            const float sigmaT = density * extinctionScale;

            // ---- 発光: Emissive x 密度 + 黒体放射 (熱 -> 温度) ----
            float3 source = HVEmissive.rgb * density;
            [branch]
            if (heat > 0.001f && HVBlackbody.z > 0.0f)
            {
                source += HVBlackBody(lerp(HVBlackbody.x, HVBlackbody.y, heat)) * (HVBlackbody.z * heat);
            }

            // ---- 散乱 (太陽 + 天空光) ----
            [branch]
            if (density > 0.001f)
            {
            const float3 cache = HeterogeneousVolumeLightingCache.SampleLevel(Sampler2, uvw, 0.0f).rgb;

            // 太陽 (マルチスキャッタのオクターブ近似)
            const float sunOpticalDepth = cache.r * extinctionScale;
            float sunScatter = 0.0f;
            [unroll]
            for (uint o = 0; o < 4; ++o)
            {
                sunScatter += octaveContribution[o] * exp(-sunOpticalDepth * octaveOcclusion[o]) * octavePhase[o];
            }
            float3 inScattering = sunColor * sunScatter;

            // 天空光 + 地面の照り返し (オクターブ近似の遮蔽, 0..1)
            const float skyOpticalDepth = cache.g * ambientOcclusionScale;
            const float groundOpticalDepth = cache.b * ambientOcclusionScale;
            float skyVisibility = 0.0f;
            float groundVisibility = 0.0f;
            [unroll]
            for (uint a = 0; a < 4; ++a)
            {
                skyVisibility += octaveContribution[a] * exp(-skyOpticalDepth * octaveOcclusion[a]);
                groundVisibility += octaveContribution[a] * exp(-groundOpticalDepth * octaveOcclusion[a]);
            }
            inScattering += (skyTop * skyVisibility + skyBottom * groundVisibility) / octaveContributionSum;

            source += sigmaT * albedo * inScattering;
            }

            // エネルギー保存積分: ∫ T(s) S ds = S (1 - exp(-σt dt)) / σt (σt -> 0 では S dt)
            const float stepTransmittance = exp(-sigmaT * dt);
            const float stepIntegral = (sigmaT > 1e-4f) ? (1.0f - stepTransmittance) / sigmaT : dt;
            const float3 contribution = transmittance * source * stepIntegral;
            radiance += contribution;

            // フォグ用の平均深度の重み: 不透明度 + 放った光 (ほぼ透明な炎でも重みを持つ)
            const float weight = transmittance * (1.0f - stepTransmittance)
                + dot(contribution, float3(1.0f, 1.0f, 1.0f) / 3.0f);
            weightedDistance += t * weight;
            weightSum += weight;

            transmittance *= stepTransmittance;
            if (transmittance < 0.003f)
            {
                transmittance = 0.0f;
                break;
            }
        }
    }

    // ---- フォグ (ボリュームの平均深度で評価) ----
    [branch]
    if (HVMarchParams.w > 0.5f && weightSum > 1e-5f)
    {
        const float meanDistance = weightedDistance / weightSum;
        const float3 meanPos = rayOrigin + rayDir * meanDistance;
        const float viewDepth = mul(float4(meanPos, 1.0f), View).z;
        const float4 fog = ComputeFogInscatteringAndOpacity(meanPos, input.Position.xy, viewDepth);
        radiance = radiance * fog.a + fog.rgb * (1.0f - transmittance);
    }

    output.Color = float4(radiance, transmittance);
    return output;
}

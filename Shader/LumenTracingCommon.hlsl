#ifndef LUMEN_TRACING_COMMON_HLSL
#define LUMEN_TRACING_COMMON_HLSL

// =============================================================
//  LumenTracingCommon
//  LumenTracingCommon.ush / LumenSurfaceCacheSampling 相当。
//  Lumen シーン (メッシュ SDF 群) へのコーントレースと、ヒット先の
//  Surface Cache (FinalLighting アトラス) の採光を提供する。
//
//  スクリーン GI (DeferredPS) と Surface Cache ライティング CS
//  (LumenSceneDirectLighting_CS / LumenRadiosity_CS) の両方から
//  インクルードされるため、リソースはレジスタ非依存の共通名で参照
//  する。インクルード元は以下の名前を宣言 (または #define) すること:
//
//    Texture3D<float>                    LumenDistanceFieldAtlas
//    StructuredBuffer<FLumenSceneObject> LumenSceneObjects
//    StructuredBuffer<FLumenCardData>    LumenCardBuffer
//    Texture2D<float4>                   LumenFinalLightingAtlas
//    Texture2D<float>                    LumenDepthAtlas
//    SamplerState                        LumenTraceSampler
//
//  (構造体は Structs.hlsl の FLumenSceneObject / FLumenCardData)
// =============================================================

// カード 1 枚の解像度 (C++ LUMEN_CARD_RESOLUTION と 1:1)
#define LUMEN_CARD_RESOLUTION_F 64.0f

// -------------------------------------------------------------
//  SDF サンプル (ボリューム空間 [-1,1] -> ワールド距離 [m])
// -------------------------------------------------------------
float SampleLumenObjectDistance(FLumenSceneObject Obj, float3 VolumePos)
{
    // 半テクセル内側へクランプ (スロット間のリニア補間ブリード防止)
    float3 uvw = clamp(VolumePos * 0.5f + 0.5f, 0.0078125f, 0.9921875f);
    float3 uv = uvw * Obj.VolumeUVScaleAndDistance.xyz + Obj.VolumeUVAdd.xyz;
    return LumenDistanceFieldAtlas.SampleLevel(LumenTraceSampler, uv, 0.0f)
        * Obj.VolumeUVScaleAndDistance.w;
}

// -------------------------------------------------------------
//  コーントレース結果
// -------------------------------------------------------------
struct FLumenTraceResult
{
    bool bHit;
    float HitT; // 最近ヒットまでの距離 [m] (ミス時は MaxT)
    uint HitObject; // 最近ヒットのオブジェクトインデックス
    float Visibility; // コーンの最小クリアランス (1=完全に空, ヒットで 0)
};

// -------------------------------------------------------------
//  Lumen シーンへのコーントレース (スフィアトレース)
//  DistanceFieldShadowing.hlsl と同じマーチ規約:
//    - レイ - AABB (スラブ法) でオブジェクトを棄却
//    - 受光面自身の表皮 (SDF ~ 0 の帯) の内側から開始した場合は
//      表皮を抜けるまで遮蔽判定を保留する (自己交差防止)
//  既知の最近ヒットより奥はマーチしない (tFar クランプ)。
// -------------------------------------------------------------
FLumenTraceResult TraceLumenScene(float3 RayStart, float3 RayDir, float MaxT,
    float TanConeAngle, uint NumObjects)
{
    FLumenTraceResult result;
    result.bHit = false;
    result.HitT = MaxT;
    result.HitObject = 0u;
    result.Visibility = 1.0f;

    [loop]
    for (uint i = 0; i < NumObjects; ++i)
    {
        FLumenSceneObject obj = LumenSceneObjects[i];
        if (obj.bValid == 0u)
        {
            continue;
        }

        // ---- ボリューム空間 [-1,1] へ変換 ----
        float3 oV = mul(float4(RayStart, 1.0f), obj.WorldToVolume).xyz;
        float3 dV = mul(float4(RayDir, 0.0f), obj.WorldToVolume).xyz;

        // ---- レイ - AABB (スラブ法, ゼロ除算ガード) ----
        float3 dSign = float3(
            (dV.x >= 0.0f) ? 1.0f : -1.0f,
            (dV.y >= 0.0f) ? 1.0f : -1.0f,
            (dV.z >= 0.0f) ? 1.0f : -1.0f);
        float3 dSafe = dSign * max(abs(dV), 1e-6f);

        float3 t0 = (float3(-1.0f, -1.0f, -1.0f) - oV) / dSafe;
        float3 t1 = (float3(1.0f, 1.0f, 1.0f) - oV) / dSafe;
        float3 tSmall = min(t0, t1);
        float3 tBig = max(t0, t1);

        float tNear = max(max(tSmall.x, tSmall.y), tSmall.z);
        float tFar = min(min(tBig.x, tBig.y), tBig.z);
        tFar = min(tFar, min(MaxT, result.HitT)); // 既知ヒットより奥は不要

        if (tFar <= max(tNear, 0.0f))
        {
            continue;
        }

        const float voxelWorld = obj.VolumeUVAdd.w; // SDF 1 ボクセルのワールド幅 [m]
        const float distanceScale = obj.VolumeUVScaleAndDistance.w;
        const float minStep = voxelWorld * 0.5f;
        const float maxStep = distanceScale * 0.5f;
        const float hitEpsilon = voxelWorld * 0.25f;
        const float surfaceExit = voxelWorld * 0.5f; // 開始表皮の脱出しきい値
        const float skipStep = voxelWorld * 0.25f;
        const float skipEnter = -hitEpsilon * 0.5f;
        const float skipMaxTravel = voxelWorld * 4.0f;

        float t = max(tNear, 0.0f);
        const float tStart = t;
        bool bInsideStartSurface = false;

        [loop]
        for (uint stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            float3 pV = oV + dV * t;
            float d = SampleLumenObjectDistance(obj, pV);

            if (stepIndex == 0)
            {
                // 表皮スキップは「レイ始点がこのオブジェクトの内側/境界に
                // ある」= 自己交差のときだけ有効にする。tNear > 0 は他の
                // オブジェクトへの進入点なので、そこで有効にすると薄い壁や
                // シルエット際で最初の数ボクセルを無条件に貫通し、
                // 光が壁を抜けてしまう。
                bInsideStartSurface = (tNear <= 0.0f) && (d < surfaceExit);
            }

            [branch]
            if (bInsideStartSurface)
            {
                if (d >= surfaceExit || (t - tStart) > skipMaxTravel)
                {
                    bInsideStartSurface = false; // 表皮を抜けた -> 通常マーチへ
                }
                else if (d < skipEnter)
                {
                    // 内部へ潜った = 実ヒット (裏面スタート / 薄物の裏側)
                    result.bHit = true;
                    result.HitT = t;
                    result.HitObject = i;
                    result.Visibility = 0.0f;
                    break;
                }
                else
                {
                    t += skipStep;
                    if (t > tFar)
                    {
                        break;
                    }
                    continue;
                }
            }

            // コーン幅に対するクリアランスでソフト可視率
            result.Visibility = min(result.Visibility,
                saturate(d / max(TanConeAngle * t, 1e-4f)));

            if (d < hitEpsilon)
            {
                result.bHit = true;
                result.HitT = t;
                result.HitObject = i;
                result.Visibility = 0.0f;
                break;
            }

            t += clamp(d, minStep, maxStep);
            if (t > tFar)
            {
                break;
            }
        }
    }

    return result;
}

// -------------------------------------------------------------
//  遮蔽トレース (ソフトシャドウ係数のみ。1 = 遮蔽なし)
//  Surface Cache 直接光の DF シャドウに使う。
// -------------------------------------------------------------
float TraceLumenOcclusion(float3 RayStart, float3 RayDir, float MaxT,
    float TanConeAngle, uint NumObjects)
{
    FLumenTraceResult result = TraceLumenScene(RayStart, RayDir, MaxT, TanConeAngle, NumObjects);
    return result.Visibility;
}

// -------------------------------------------------------------
//  ヒット法線 (SDF 勾配の中心差分, ボリューム空間 -> ワールド)
//  勾配は WorldToVolume の 3x3 を前掛けするとワールド方向になる
//  (逆転置変換と等価。スケールは normalize で吸収)。
// -------------------------------------------------------------
float3 ComputeLumenHitNormal(FLumenSceneObject Obj, float3 WorldPos, float3 RayStart)
{
    float3 pV = mul(float4(WorldPos, 1.0f), Obj.WorldToVolume).xyz;

    const float e = 0.03125f; // 1 ボクセル (2 / 64)
    float3 grad;
    grad.x = SampleLumenObjectDistance(Obj, pV + float3(e, 0, 0))
           - SampleLumenObjectDistance(Obj, pV - float3(e, 0, 0));
    grad.y = SampleLumenObjectDistance(Obj, pV + float3(0, e, 0))
           - SampleLumenObjectDistance(Obj, pV - float3(0, e, 0));
    grad.z = SampleLumenObjectDistance(Obj, pV + float3(0, 0, e))
           - SampleLumenObjectDistance(Obj, pV - float3(0, 0, e));

    float3 worldGrad = mul((float3x3) Obj.WorldToVolume, grad);
    float len = length(worldGrad);
    // 勾配が縮退 (ボリューム境界のクランプ等) した場合は入射方向の逆を使う。
    // 固定の上向きを返すとカード選択が的外れになり黒い斑点になる。
    float3 fallback = WorldPos - RayStart;
    float fallbackLen = length(fallback);
    return (len > 1e-6f) ? worldGrad / len
         : ((fallbackLen > 1e-6f) ? -fallback / fallbackLen : float3(0.0f, 1.0f, 0.0f));
}

// -------------------------------------------------------------
//  Surface Cache 採光 (SampleLumenCardInterpolated 相当)
//  ヒット点の法線とカード法線の内積で重み付けし、オブジェクトの
//  全カードから FinalLighting をブレンドする。
//    - FinalLighting.a = 有効率 (ジオメトリが焼かれたテクセルのみ 1)
//    - 深度テスト: カードから見えていない (格納深度より奥の) 点は
//      棄却してライトリークを防ぐ
//  戻り値 = 出射ラディアンス (直接光 + 間接光 + Emissive 合成済み)
// -------------------------------------------------------------
float3 SampleLumenSurfaceCache(FLumenSceneObject Obj, float3 WorldHitPos, float3 WorldHitNormal)
{
    float3 lighting = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;

    [loop]
    for (uint c = 0; c < Obj.NumCards; ++c)
    {
        FLumenCardData card = LumenCardBuffer[Obj.CardOffset + c];
        if (card.CardExtentAndValid.w < 0.5f)
        {
            continue;
        }

        // カード法線と面法線の一致度 (裏向きカードは棄却)
        float weight = dot(WorldHitNormal, card.CardDirection.xyz);
        if (weight <= 0.05f)
        {
            continue;
        }

        // カード空間へ (深度レンジは [0, 2 * ext.z])
        float3 cardPos = mul(float4(WorldHitPos, 1.0f), card.WorldToCard).xyz;
        float3 ext = card.CardExtentAndValid.xyz;

        if (abs(cardPos.x) > ext.x || abs(cardPos.y) > ext.y ||
            cardPos.z < -0.25f * ext.z || cardPos.z > 2.25f * ext.z)
        {
            continue;
        }

        // カード UV (ビュー上方向 +Y はテクスチャ v と逆)
        float2 uv = float2(cardPos.x / ext.x, -cardPos.y / ext.y) * 0.5f + 0.5f;

        // タイル内側へ半テクセルクランプ (隣タイルへのブリード防止)
        const float halfTexel = 0.5f / LUMEN_CARD_RESOLUTION_F;
        uv = clamp(uv, halfTexel, 1.0f - halfTexel);

        float2 atlasUV = uv * card.AtlasUVScaleBias.xy + card.AtlasUVScaleBias.zw;

        float4 final = LumenFinalLightingAtlas.SampleLevel(LumenTraceSampler, atlasUV, 0.0f);

        // 深度テスト: 格納深度より一定以上奥ならカードから見えていない
        float storedZ = LumenDepthAtlas.SampleLevel(LumenTraceSampler, atlasUV, 0.0f)
            * (2.0f * ext.z);
        float depthDelta = cardPos.z - storedZ;
        float depthWeight = saturate(1.0f - depthDelta / max(0.2f * ext.z, 0.01f));

        weight *= final.a * depthWeight;

        lighting += final.rgb * weight;
        totalWeight += weight;
    }

    return (totalWeight > 0.001f) ? lighting / totalWeight : float3(0.0f, 0.0f, 0.0f);
}

// =============================================================
//  Global Distance Field (クリップマップ) トレース
//  LumenSceneLightingCommon.hlsl (コンピュート側) だけが有効化する
//  (LUMEN_SUPPORTS_GLOBAL_SDF)。グラフィックス側のピクセル毎
//  フォールバック経路はメッシュ SDF ループのみを使う。
// =============================================================
#if LUMEN_SUPPORTS_GLOBAL_SDF

// クリップマップサンプル (ClipmapParams: xyz=中心, w=半径)
float SampleLumenGlobalSDFClipmap(Texture3D<float> Clipmap, float4 ClipmapParams, float3 WorldPos)
{
    float3 uvw = (WorldPos - ClipmapParams.xyz) / (2.0f * ClipmapParams.w) + 0.5f;
    // 半テクセル内側へクランプ (境界の補間ブリード防止)
    float halfTexel = 0.5f / LUMEN_GLOBAL_SDF_RESOLUTION;
    uvw = clamp(uvw, halfTexel, 1.0f - halfTexel);
    return Clipmap.SampleLevel(LumenTraceSampler, uvw, 0.0f);
}

// ワールド点がクリップマップ内か (マージン付き)
bool IsInsideLumenClipmap(float4 ClipmapParams, float3 WorldPos, float Margin)
{
    float3 d = abs(WorldPos - ClipmapParams.xyz);
    float bound = ClipmapParams.w * Margin;
    return max(d.x, max(d.y, d.z)) < bound;
}

// -------------------------------------------------------------
//  Global SDF スフィアトレース (クリップマップ 0 -> 1 の自動選択)
//    OutVisibility はコーンの最小クリアランスを累積更新する
// -------------------------------------------------------------
bool TraceLumenGlobalSDF(float3 RayStart, float3 RayDir, float TMin, float TMax,
    float TanConeAngle, float4 Clipmap0, float4 Clipmap1,
    inout float OutVisibility, out float OutHitT)
{
    OutHitT = TMax;

    if (Clipmap0.w <= 0.0f)
    {
        return false; // Global SDF 無効
    }

    const float voxel0 = (2.0f * Clipmap0.w) / LUMEN_GLOBAL_SDF_RESOLUTION;
    const float voxel1 = (2.0f * Clipmap1.w) / LUMEN_GLOBAL_SDF_RESOLUTION;

    float t = TMin;

    [loop]
    for (uint stepIndex = 0; stepIndex < 64; ++stepIndex)
    {
        float3 p = RayStart + RayDir * t;

        // クリップマップ選択 (内側 90% までは高精細の 0 を使う)
        bool bUseClip0 = IsInsideLumenClipmap(Clipmap0, p, 0.9f);
        bool bInsideClip1 = IsInsideLumenClipmap(Clipmap1, p, 1.0f);

        if (!bUseClip0 && !bInsideClip1)
        {
            break; // 全クリップマップ外 = これ以上の遮蔽情報なし (ミス)
        }

        float d = bUseClip0
            ? SampleLumenGlobalSDFClipmap(LumenGlobalSDF0, Clipmap0, p)
            : SampleLumenGlobalSDFClipmap(LumenGlobalSDF1, Clipmap1, p);

        float voxel = bUseClip0 ? voxel0 : voxel1;

        OutVisibility = min(OutVisibility, saturate(d / max(TanConeAngle * t, 1e-4f)));

        if (d < voxel * 0.5f)
        {
            OutHitT = t;
            OutVisibility = 0.0f;
            return true;
        }

        t += clamp(d, voxel * 0.5f, 4.0f * voxel1);
        if (t > TMax)
        {
            break;
        }
    }

    return false;
}

// -------------------------------------------------------------
//  Global SDF ヒットのオブジェクト解決
//  (ヒット点に最も近い SDF を持つオブジェクトを返す。
//   UE の Global DF -> Surface Cache 採光のオブジェクトグリッドの
//   線形探索版)
// -------------------------------------------------------------
uint ResolveLumenObjectAtPoint(float3 WorldPos, uint NumObjects, out bool bFound)
{
    uint bestObject = 0u;
    float bestDistance = 1e9f;
    bFound = false;

    [loop]
    for (uint i = 0; i < NumObjects; ++i)
    {
        FLumenSceneObject obj = LumenSceneObjects[i];
        if (obj.bValid == 0u)
        {
            continue;
        }

        float3 pV = mul(float4(WorldPos, 1.0f), obj.WorldToVolume).xyz;

        // ボリューム外は AABB 距離 + 境界サンプルで近似
        float3 clamped = clamp(pV, -1.0f, 1.0f);
        float3 delta = pV - clamped;
        // ボリューム空間距離 -> ワールドは半径スケール近似 (選択にのみ使用)
        float outside = length(delta) * obj.VolumeUVScaleAndDistance.w;
        float d = outside + SampleLumenObjectDistance(obj, clamped);

        if (d < bestDistance)
        {
            bestDistance = d;
            bestObject = i;
            bFound = true;
        }
    }

    return bestObject;
}

// -------------------------------------------------------------
//  ハイブリッドトレース (UE の Detail Trace + Global Trace):
//    近距離 (DetailDistance まで) = メッシュ SDF ループ (高精細)
//    遠距離                       = Global SDF クリップマップ
// -------------------------------------------------------------
FLumenTraceResult TraceLumenSceneHybrid(float3 RayStart, float3 RayDir, float MaxT,
    float TanConeAngle, uint NumObjects, float DetailDistance,
    float4 Clipmap0, float4 Clipmap1)
{
    const float nearT = min(MaxT, DetailDistance);

    // ---- 近距離: メッシュ SDF (高精細) ----
    FLumenTraceResult result = TraceLumenScene(RayStart, RayDir, nearT, TanConeAngle, NumObjects);

    [branch]
    if (result.bHit || MaxT <= DetailDistance)
    {
        result.HitT = result.bHit ? result.HitT : MaxT;
        return result;
    }

    // ---- 遠距離: Global SDF ----
    float farHitT;
    bool bFarHit = TraceLumenGlobalSDF(RayStart, RayDir,
        nearT * 0.9f, MaxT, TanConeAngle, Clipmap0, Clipmap1,
        result.Visibility, farHitT);

    [branch]
    if (bFarHit)
    {
        bool bFound;
        float3 hitPos = RayStart + RayDir * farHitT;
        uint hitObject = ResolveLumenObjectAtPoint(hitPos, NumObjects, bFound);

        if (bFound)
        {
            result.bHit = true;
            result.HitT = farHitT;
            result.HitObject = hitObject;
        }
    }

    result.HitT = result.bHit ? result.HitT : MaxT;
    return result;
}

#endif // LUMEN_SUPPORTS_GLOBAL_SDF

// -------------------------------------------------------------
//  半球コサイン分布のレイ方向 (Fibonacci スパイラル)
//    Index / NumRays : レイ番号 / 総数
//    RandomRotation  : 方位角の回転 [rad] (ピクセル / テクセルごとの
//                      ジッタ。IGN やハッシュを渡す)
//  戻り値はワールド空間 (Normal 周りの接空間から変換済み)
// -------------------------------------------------------------
float3 GetLumenHemisphereRay(float3 Normal, uint Index, uint NumRays, float RandomRotation)
{
    // コサイン重点サンプル: cosTheta = sqrt(1 - u), u = (i + 0.5) / N
    float u = ((float) Index + 0.5f) / (float) NumRays;
    float cosTheta = sqrt(saturate(1.0f - u));
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));

    const float GOLDEN_ANGLE = 2.39996323f;
    float phi = GOLDEN_ANGLE * (float) Index + RandomRotation;

    float3 local = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    // 接空間基底 (縮退ガード付き)
    float3 up = (abs(Normal.y) < 0.999f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, Normal));
    float3 bitangent = cross(Normal, tangent);

    return normalize(tangent * local.x + bitangent * local.y + Normal * local.z);
}

#endif

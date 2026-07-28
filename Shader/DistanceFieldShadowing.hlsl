#ifndef DISTANCE_FIELD_SHADOWING_HLSL
#define DISTANCE_FIELD_SHADOWING_HLSL

#include "Common.hlsl"

// =============================================================
//  DistanceFieldShadowing
//  DistanceFieldShadowing.usf (RayTraced Distance Field
//  Shadows) 相当。シーンの各メッシュ SDF (t17 アトラス + t18
//  オブジェクトバッファ) をスフィアトレースし、コーン幅
//  (光源の見かけ角) に対する最小クリアランスからソフトな
//  遮蔽係数を求める。
//
//    shadow = min(shadow, saturate(sdf / (tanConeAngle * t)))
//
//  オブジェクト数は DFShadowParams0.x (b5)。タイルカリングは
//  未実装のため全オブジェクトをレイ - AABB で棄却する。
// =============================================================

// -------------------------------------------------------------
//  レイマーチ本体 (1 = 影なし, 0 = 完全遮蔽)
//    RayStart     : ワールド開始点 (自己遮蔽バイアス適用済みで渡す)
//    RayDir       : ワールド方向 (正規化)
//    MaxRayT      : 最大トレース距離 [m]
//    TanConeAngle : 光源の見かけ半角の tan (ソフトネス)
// -------------------------------------------------------------
float RayTraceDistanceFieldShadow(float3 RayStart, float3 RayDir, float MaxRayT, float TanConeAngle)
{
    const uint numObjects = (uint) DFShadowParams0.x;
    float shadow = 1.0f;

    [loop]
    for (uint i = 0; i < numObjects; ++i)
    {
        FDFObjectData obj = DFObjects[i];

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
        tFar = min(tFar, MaxRayT);

        if (tFar <= max(tNear, 0.0f))
        {
            continue; // ボリュームと交差しない
        }

        const float distanceScale = obj.VolumeUVScaleAndDistance.w; // 値 -> ワールド距離 [m]
        const float voxelWorld = distanceScale * 0.0078125f /*(1.0f / 128.0f)*/; // 1 ボクセルのワールド幅 [m]
        const float minStep = voxelWorld * 0.5f;
        const float maxStep = distanceScale * 0.5f;
        const float hitEpsilon = voxelWorld * 0.25f;
        const float surfaceExit = voxelWorld * 0.5f; // 受光面表皮の脱出しきい値
        const float skipStep = voxelWorld * 0.25f; // スキップ中の歩幅 (薄物検出のため細かく)
        const float skipEnter = -hitEpsilon * 0.5f; // スキップ中の進入判定 (-1/8 ボクセル)
        const float skipMaxTravel = voxelWorld * 4.0f; // スキップの最大這い距離

        float t = max(tNear, 0.0f);
        const float tStart = t;

        // 受光面自身の表皮 (SDF がほぼ 0 の帯) の内側から開始した場合のみ、
        // 表皮を抜けるまで遮蔽判定を保留する。一律の開始オフセットと違い
        // 他オブジェクトの接地影を食わないため、シャドウマップと影の
        // 位置がずれない。しきい値はボクセル幅基準でメッシュサイズに追従。
        // スキップ距離には上限を設け、斜光でレイが表皮を長く這う場合に
        // 遮蔽が無登録のまま進む (ターミネータ付近が明るく抜ける) のを防ぐ
        bool bInsideStartSurface = false;

        [loop]
        for (uint stepIndex = 0; stepIndex < 48; ++stepIndex)
        {
            float3 pV = oV + dV * t;

            // 半テクセル内側へクランプ (スロット間のリニア補間ブリード防止。
            // 境界パディング領域なので SDF 値への影響はない)
            float3 uvw = clamp(pV * 0.5f + 0.5f, 0.0078125f, 0.9921875f);
            float3 uv = uvw * obj.VolumeUVScaleAndDistance.xyz + obj.VolumeUVAdd.xyz;

            float d = DistanceFieldAtlasTexture.SampleLevel(Sampler2, uv, 0.0f) * distanceScale;

            if (stepIndex == 0)
            {
                bInsideStartSurface = (d < surfaceExit);
            }

            [branch]
            if (bInsideStartSurface)
            {
                if (d >= surfaceExit || (t - tStart) > skipMaxTravel)
                {
                    // 表皮を抜けた / 這い距離の上限に達した -> 通常マーチへ
                    bInsideStartSurface = false;
                }
                else if (d < skipEnter)
                {
                    // 内部へ潜った = 裏面スタートまたは別オブジェクトへの進入
                    // (真下の接地影・薄い部位の裏側など)。受光面自身の表皮なら
                    // d は小さな正の値に留まるため、負側への潜りは遮蔽確定。
                    // しきい値を浅く (-1/8 ボクセル)・歩幅を細かく (1/4 ボクセル)
                    // することで、厚み約 0.4 ボクセル以上の薄物も貫通せず検出する
                    shadow = 0.0f;
                    break;
                }
                else
                {
                    // 表皮内: 遮蔽を登録せず細かい歩幅で脱出を待つ
                    t += skipStep;
                    if (t > tFar)
                    {
                        break;
                    }
                    continue;
                }
            }

            // コーン幅に対するクリアランスでソフト遮蔽
            shadow = min(shadow, saturate(d / max(TanConeAngle * t, 1e-4f)));

            if (d < hitEpsilon)
            {
                shadow = 0.0f;
                break;
            }

            t += clamp(d, minStep, maxStep);
            if (t > tFar)
            {
                break;
            }
        }

        if (shadow < 0.005f)
        {
            return 0.0f;
        }
    }

    return shadow;
}

#endif

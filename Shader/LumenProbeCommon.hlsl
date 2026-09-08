#ifndef LUMEN_PROBE_COMMON_HLSL
#define LUMEN_PROBE_COMMON_HLSL

// =============================================================
//  LumenProbeCommon
//  Screen Probe Gather / Radiance Cache 共通のプローブヘルパ:
//    - octahedral / hemi-octahedral 方向マッピング
//    - SH L1 (2 バンド) の射影 / 評価
//    - スクリーンスペーストレース (前フレーム SceneColor 採光)
//    - ハッシュ / ジッタ
//  LumenSceneLightingCommon.hlsl の後にインクルードすること
//  (b0 パスパラメータ / スクリーン系リソース t15-t18 を参照する)。
// =============================================================

#ifndef LUMEN_PI
#define LUMEN_PI 3.14159265358979323846f
#endif

// -------------------------------------------------------------
//  ハッシュ / ジッタ
// -------------------------------------------------------------
uint LumenHashUint(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16);
    seed *= 9u;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15);
    return seed;
}

float LumenHashToFloat01(uint hash)
{
    return (float) (hash & 0xFFFFu) / 65536.0f;
}

// R2 数列によるフレームジッタ (octa テクセル内サブサンプル)
float2 LumenGetFrameJitter(uint FrameNumber, uint ProbeSeed)
{
    float2 r2 = frac(float2(0.75487767f, 0.56984029f) * (float) (FrameNumber & 1023u));
    float2 scramble = float2(
        LumenHashToFloat01(LumenHashUint(ProbeSeed)),
        LumenHashToFloat01(LumenHashUint(ProbeSeed ^ 0x9E3779B9u)));
    return frac(r2 + scramble);
}

// -------------------------------------------------------------
//  スクリーンプローブのアンカーピクセル
//  プローブ i のアンカー = i * downsample + ジッタ (PassProbeJitter.xy)。
//  ジッタは 16 フレーム周期の Halton(2,3) でセル内を巡回する
//  (ScreenProbeGather 配置ジッタ相当)。固定格子だとカメラ移動で
//  プローブが面の上を滑り、16px 補間の位相がうねりとして見える
//  (= 揺らぎ)。ジッタでそれをフレーム間ノイズに変え、プローブ SH と
//  フル解像度のテンポラル蓄積が平均してワールド固定の値に収束させる。
// -------------------------------------------------------------
uint2 LumenGetProbeAnchor(uint2 Probe)
{
    const uint downsample = (uint) PassProbeParams0.z;
    const uint2 screenSize = (uint2) PassProbeParams1.xy;
    return min(Probe * downsample + (uint2) PassProbeJitter.xy, screenSize - 1u);
}

uint2 LumenGetPrevProbeAnchor(uint2 Probe)
{
    const uint downsample = (uint) PassProbeParams0.z;
    const uint2 screenSize = (uint2) PassProbeParams1.xy;
    return min(Probe * downsample + (uint2) PassProbeJitter.zw, screenSize - 1u);
}

// -------------------------------------------------------------
//  接空間基底
// -------------------------------------------------------------
// Duff et al. "Building an Orthonormal Basis, Revisited" の分岐なし構成。
// 法線から一意・連続に基底を作る (N.z = -1 の 1 方向を除く)。
// 素朴な up ベクトル方式だと |N.y| ~ 1 (床/天井) の分岐境界で基底が
// 90 度飛ぶため、ノーマルマップされた床で隣接プローブ同士の octahedral
// テクセルが別方向を指し、空間フィルタ (LumenScreenProbeFilter_CS) が
// 異なる方向の放射輝度を混ぜてしまう。
void LumenBuildTangentBasis(float3 Normal, out float3 Tangent, out float3 Bitangent)
{
    float s = (Normal.z >= 0.0f) ? 1.0f : -1.0f;
    float a = -1.0f / (s + Normal.z);
    float b = Normal.x * Normal.y * a;
    Tangent = float3(1.0f + s * Normal.x * Normal.x * a, s * b, -s * Normal.x);
    Bitangent = float3(b, s + Normal.y * Normal.y * a, -Normal.y);
}

// -------------------------------------------------------------
//  Hemi-octahedral マッピング (接空間 z >= 0 半球 <-> [0,1]^2)
// -------------------------------------------------------------
float3 LumenHemiOctahedronToDirection(float2 UV)
{
    float2 e = UV * 2.0f - 1.0f;
    float2 p = float2(e.x + e.y, e.x - e.y) * 0.5f;
    float3 dir = float3(p.x, p.y, 1.0f - abs(p.x) - abs(p.y));
    dir.z = max(dir.z, 0.001f);
    return normalize(dir);
}

// -------------------------------------------------------------
//  Octahedral マッピング (全球 <-> [0,1]^2, Radiance Cache 用)
// -------------------------------------------------------------
float3 LumenOctahedronToDirection(float2 UV)
{
    float2 e = UV * 2.0f - 1.0f;
    float3 dir = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
    if (dir.z < 0.0f)
    {
        float2 signs = float2(
            (dir.x >= 0.0f) ? 1.0f : -1.0f,
            (dir.y >= 0.0f) ? 1.0f : -1.0f);
        dir.xy = (1.0f - abs(dir.yx)) * signs;
    }
    return normalize(dir);
}

// 全球方向 -> octahedral [0,1]^2 (LumenOctahedronToDirection の逆。
// プローブ Aux の zw に法線を保存し、テンポラル履歴の面一致検証に使う)
float2 LumenDirectionToOctahedron(float3 Dir)
{
    float3 n = Dir / max(abs(Dir.x) + abs(Dir.y) + abs(Dir.z), 1e-6f);
    float2 e = n.xy;
    if (n.z < 0.0f)
    {
        float2 signs = float2(
            (n.x >= 0.0f) ? 1.0f : -1.0f,
            (n.y >= 0.0f) ? 1.0f : -1.0f);
        e = (1.0f - abs(n.yx)) * signs;
    }
    return e * 0.5f + 0.5f;
}

// -------------------------------------------------------------
//  SH L1 (2 バンド)
//  係数レイアウト:(c0, c1 = y, c2 = z, c3 = x) - RGBA と 1:1
// -------------------------------------------------------------
float4 LumenSH1Basis(float3 Dir)
{
    return float4(0.282095f, 0.488603f * Dir.y, 0.488603f * Dir.z, 0.488603f * Dir.x);
}

// 放射輝度 L を方向 Dir / 立体角重み Weight で射影した係数
void LumenSH1Project(float3 Radiance, float3 Dir, float Weight,
    inout float4 SHR, inout float4 SHG, inout float4 SHB)
{
    float4 basis = LumenSH1Basis(Dir) * Weight;
    SHR += Radiance.r * basis;
    SHG += Radiance.g * basis;
    SHB += Radiance.b * basis;
}

// 法線 N の半球コサイン畳み込み -> 平均入射ラディアンス
//   E(n) = pi*A0*c0 + (2pi/3)*A1*(c1..c3)  ->  Lmean = E / pi
float3 LumenSH1EvaluateMeanRadiance(float4 SHR, float4 SHG, float4 SHB, float3 N)
{
    float4 eval = float4(0.282095f,
        0.488603f * N.y, 0.488603f * N.z, 0.488603f * N.x);
    eval.yzw *= (2.0f / 3.0f);

    return max(float3(dot(SHR, eval), dot(SHG, eval), dot(SHB, eval)),
        float3(0.0f, 0.0f, 0.0f));
}

// -------------------------------------------------------------
//  スクリーンスペーストレース
//  現フレームの LinearDepth に対してレイマーチし、ヒットを
//  前フレーム SceneColor (リプロジェクション) で採光する。
//  戻り値: 0 = ミス (SDF へフォールバック), 1 = ヒット
// -------------------------------------------------------------
#define LUMEN_SCREEN_TRACE_STEPS 12

bool LumenScreenSpaceTrace(float3 RayStart, float3 RayDir, float MaxT,
    float StartOffset, out float3 OutRadiance)
{
    OutRadiance = float3(0.0f, 0.0f, 0.0f);

    // 履歴が無い / スクリーントレース無効のフレームは不可
    // (PassProbeParams1.w = 履歴有効, PassReflectionParams.w = 有効フラグ)
    if (PassProbeParams1.w < 0.5f || PassReflectionParams.w < 0.5f)
    {
        return false;
    }

    const float thickness = max(PassPrevCameraOrigin.w, 0.05f);

    // 指数的ステップ (近距離を密に)
    float prevT = StartOffset;

    [loop]
    for (uint stepIndex = 0; stepIndex < LUMEN_SCREEN_TRACE_STEPS; ++stepIndex)
    {
        float stepRatio = ((float) stepIndex + 1.0f) / (float) LUMEN_SCREEN_TRACE_STEPS;
        float t = StartOffset + (MaxT - StartOffset) * stepRatio * stepRatio;

        float3 p = RayStart + RayDir * t;

        float4 clipPos = mul(float4(p, 1.0f), PassViewProjection);
        if (clipPos.w <= 0.01f)
        {
            return false; // カメラ背後
        }

        float2 ndc = clipPos.xy / clipPos.w;
        if (abs(ndc.x) > 1.0f || abs(ndc.y) > 1.0f)
        {
            return false; // 画面外へ出た
        }

        float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        // 深度は点サンプル (バイリニアだとシルエットで前後の深度が混ざり、
        // 実在しない中間深度に「ヒット」してエッジがカメラ移動で明滅する)
        int2 depthPixel = clamp((int2) (uv * PassProbeParams1.xy),
            int2(0, 0), (int2) PassProbeParams1.xy - 1);
        float sceneDist = LumenLinearDepth.Load(int3(depthPixel, 0)).r;

        if (sceneDist <= 0.0f)
        {
            continue; // 深度なし (スカイ)
        }

        // LinearDepth.r は平面ビュー深度 (view Z)。レイ側も同じ距離尺度で
        // 比較する (clip.w = view Z。放射距離だと画面周辺で系統誤差が出る)
        float rayDist = clipPos.w;

        if (rayDist > sceneDist + 0.02f)
        {
            if (rayDist < sceneDist + thickness)
            {
                // ヒット: 前フレーム SceneColor をリプロジェクションで採光
                float4 prevClip = mul(float4(p, 1.0f), PassPrevViewProjection);
                if (prevClip.w > 0.01f)
                {
                    float2 prevNDC = prevClip.xy / prevClip.w;
                    if (abs(prevNDC.x) < 1.0f && abs(prevNDC.y) < 1.0f)
                    {
                        float2 prevUV = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);

                        // 履歴深度検証: 採光点が前フレームでも同じ深度で見えていた
                        // ときだけ採用する。カメラ移動で前フレームには別の面
                        // (手前の物体 / スカイ) が写っていた位置を拾うと、
                        // その色が今フレームの面の放射輝度として混入し明滅する
                        int2 prevPixel = clamp((int2) (prevUV * PassProbeParams1.xy),
                            int2(0, 0), (int2) PassProbeParams1.xy - 1);
                        float prevSceneDist = LumenPrevLinearDepth.Load(int3(prevPixel, 0)).r;
                        if (prevSceneDist <= 0.0f ||
                            abs(prevClip.w - prevSceneDist) > thickness)
                        {
                            return false; // ディスオクルージョン -> SDF / HWRT へ
                        }

                        OutRadiance = LumenPrevSceneColor.SampleLevel(
                            LumenTraceSampler, prevUV, 0.0f).rgb;
                        return true;
                    }
                }
            }

            // 厚みを超えて潜った / 履歴が無い: 不確定 -> SDF へフォールバック
            return false;
        }

        prevT = t;
    }

    return false;
}

// -------------------------------------------------------------
//  スクリーンピクセルのワールド再構築 (デバイス深度 + InvViewProjection)
// -------------------------------------------------------------
float3 LumenReconstructWorldPosition(uint2 Pixel, float DeviceDepth)
{
    float2 uv = ((float2) Pixel + 0.5f) / PassProbeParams1.xy;
    float4 ndcPos = float4(
        uv.x * 2.0f - 1.0f,
        (1.0f - uv.y) * 2.0f - 1.0f,
        DeviceDepth,
        1.0f);
    float4 worldPos = mul(ndcPos, PassInvViewProjection);
    return worldPos.xyz / worldPos.w;
}

// -------------------------------------------------------------
//  Radiance Cache: テクスチャ (トロイダル) インデックス変換
//    C0     = 現ボリューム最小コーナーのワールドセル
//    TexIdx = ワールドセル mod N
// -------------------------------------------------------------
int3 LumenRCWorldCellFromTexIndex(int3 TexIndex, int3 MinWorldCell, int N)
{
    // MinWorldCell 以上で TexIndex と mod N が一致する一意のセル
    int3 rel = ((TexIndex - MinWorldCell) % N + N) % N;
    return MinWorldCell + rel;
}

#endif

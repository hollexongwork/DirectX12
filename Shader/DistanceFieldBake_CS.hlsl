// =============================================================
//  DistanceFieldBake_CS
//  Mesh Distance Field 生成 (GenerateDistanceFieldVolumeData)
//  の簡易 GPU 版。ボクセルごとに:
//    - 全三角形への最短距離 (Ericson 最近接点法)
//    - +X / +Y / +Z 3 軸のレイ交差数の多数決で内外判定
//  を行い、符号付き距離を正規化半幅で割って [-1,1] に格納する。
//
//  独立ルートシグネチャ (FDistanceFieldAtlas 専用):
//    b0 = ベイクパラメータ / t0 = 三角形頂点列 / u0 = 出力ボクセル
//  ロード時に一度だけ実行され、結果はディスクへキャッシュされる。
// =============================================================

cbuffer DFBakeParams : register(b0)
{
    float3 VolumeMin;           // ボリューム最小コーナー (ローカル)
    float InvNormalizeExtent;   // 1 / 正規化半幅
    float3 VoxelSize;           // ボクセルサイズ (ローカル)
    uint NumTriangles;
    uint VolumeResolution;      // = DF_VOLUME_RES
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

StructuredBuffer<float3> TriangleVertices : register(t0); // 3 頂点 x 三角形数
RWStructuredBuffer<float> OutDistanceField : register(u0);

// -------------------------------------------------------------
//  点 - 三角形 最短距離の 2 乗 (Ericson: Real-Time Collision Detection)
// -------------------------------------------------------------
float PointTriangleDistanceSq(float3 p, float3 a, float3 b, float3 c)
{
    float3 ab = b - a;
    float3 ac = c - a;
    float3 ap = p - a;

    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
    {
        return dot(ap, ap); // 頂点 A
    }

    float3 bp = p - b;
    float d3 = dot(ab, bp);
    float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
    {
        return dot(bp, bp); // 頂点 B
    }

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        float v = d1 / (d1 - d3); // 辺 AB
        float3 q = a + v * ab - p;
        return dot(q, q);
    }

    float3 cp = p - c;
    float d5 = dot(ab, cp);
    float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
    {
        return dot(cp, cp); // 頂点 C
    }

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        float w = d2 / (d2 - d6); // 辺 AC
        float3 q = a + w * ac - p;
        return dot(q, q);
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); // 辺 BC
        float3 q = b + w * (c - b) - p;
        return dot(q, q);
    }

    // 面内部
    float denom = 1.0f / (va + vb + vc);
    float v2 = vb * denom;
    float w2 = vc * denom;
    float3 q = a + ab * v2 + ac * w2 - p;
    return dot(q, q);
}

// -------------------------------------------------------------
//  レイ - 三角形交差 (Moller-Trumbore, t > 0 のみ)
// -------------------------------------------------------------
bool RayIntersectsTriangle(float3 o, float3 d, float3 a, float3 b, float3 c)
{
    float3 e1 = b - a;
    float3 e2 = c - a;
    float3 pv = cross(d, e2);
    float det = dot(e1, pv);
    if (abs(det) < 1e-8f)
    {
        return false;
    }
    float invDet = 1.0f / det;

    float3 tv = o - a;
    float u = dot(tv, pv) * invDet;
    if (u < 0.0f || u > 1.0f)
    {
        return false;
    }

    float3 qv = cross(tv, e1);
    float v = dot(d, qv) * invDet;
    if (v < 0.0f || (u + v) > 1.0f)
    {
        return false;
    }

    float t = dot(e2, qv) * invDet;
    return t > 0.0f;
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= VolumeResolution))
    {
        return;
    }

    float3 p = VolumeMin + ((float3) id + 0.5f) * VoxelSize;

    float minDistSq = 1e30f;
    uint crossX = 0;
    uint crossY = 0;
    uint crossZ = 0;

    [loop]
    for (uint t = 0; t < NumTriangles; ++t)
    {
        float3 a = TriangleVertices[t * 3 + 0];
        float3 b = TriangleVertices[t * 3 + 1];
        float3 c = TriangleVertices[t * 3 + 2];

        minDistSq = min(minDistSq, PointTriangleDistanceSq(p, a, b, c));

        crossX += RayIntersectsTriangle(p, float3(1.0f, 0.0f, 0.0f), a, b, c) ? 1 : 0;
        crossY += RayIntersectsTriangle(p, float3(0.0f, 1.0f, 0.0f), a, b, c) ? 1 : 0;
        crossZ += RayIntersectsTriangle(p, float3(0.0f, 0.0f, 1.0f), a, b, c) ? 1 : 0;
    }

    // 3 軸の交差偶奇の多数決で内外判定 (開いたメッシュへの耐性)
    uint insideVotes = (crossX & 1) + (crossY & 1) + (crossZ & 1);
    float signValue = (insideVotes >= 2) ? -1.0f : 1.0f;

    float dist = clamp(signValue * sqrt(minDistSq) * InvNormalizeExtent, -1.0f, 1.0f);

    uint index = (id.z * VolumeResolution + id.y) * VolumeResolution + id.x;
    OutDistanceField[index] = dist;
}

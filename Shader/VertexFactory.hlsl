#ifndef VERTEX_FACTORY_HLSL
#define VERTEX_FACTORY_HLSL

// =============================================================
//  VertexFactory.hlsl
//  頂点ファクトリの抽象 (LocalVertexFactory.ush / GpuSkinVertexFactory.ush)。
//  メッシュパスの頂点シェーダ (GeometryVS / ShadowDepthVS / VelocityVS) は
//  入力を FVertexFactoryInput で受け、ローカル空間の位置 / 法線 / 接線を
//    VertexFactoryGetLocalPosition / VertexFactoryGetLocalNormal /
//    VertexFactoryGetLocalTangent / VertexFactoryGetPreviousLocalPosition
//  で取る。
//
//    GPUSKIN_VERTEX_FACTORY 未定義 : ローカル VF。入力をそのまま返す
//                                   (既存シェーダとビット一致する)
//    GPUSKIN_VERTEX_FACTORY = 1    : GPU スキニング。BLENDINDICES / BLENDWEIGHT と
//                                   ボーン行列 (t39 今 / t40 前フレーム) で
//                                   位置 / 法線 / 接線を変換する
//
//  Common.hlsl (Structs.hlsl の VS_INPUT) の後に include する。
//  C++ 側: RenderManager.h VERTEX_SKINNED / EVertexFactoryType、
//          SkeletalMeshComponent.cpp FSkeletalMeshObjectGPUSkin (FBoneMatrix)。
// =============================================================

#if GPUSKIN_VERTEX_FACTORY

// ---- ボーン行列 (FBoneMatrix = float3x4、C++ 側は転置済みの上 3 行) ----
// mul(Bone, float4(p, 1)) で ReferenceToLocal を適用する (UE の GetBoneMatrix と同じ規約)
struct FBoneMatrix
{
    float4 Row0;
    float4 Row1;
    float4 Row2;
};

StructuredBuffer<FBoneMatrix> BoneMatrices : register(t39);         // 今フレームの ReferenceToLocal
StructuredBuffer<FBoneMatrix> PreviousBoneMatrices : register(t40); // 前フレームの ReferenceToLocal (ベロシティ)

struct FVertexFactoryInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    float2 TexCoord : TEXCOORD;
    float4 Color : COLOR;
    uint4 BlendIndices : BLENDINDICES;   // セクションの BoneMap 内のローカル添字
    float4 BlendWeights : BLENDWEIGHT;   // R8G8B8A8_UNORM (合計 1)
};

struct FVertexFactoryIntermediates
{
    float3x4 BlendMatrix;           // 今フレームのブレンド済みボーン行列
    float3x4 PreviousBlendMatrix;   // 前フレーム
};

float3x4 GetBoneMatrix(uint Index)
{
    const FBoneMatrix b = BoneMatrices[Index];
    return float3x4(b.Row0, b.Row1, b.Row2);
}

float3x4 GetPreviousBoneMatrix(uint Index)
{
    const FBoneMatrix b = PreviousBoneMatrices[Index];
    return float3x4(b.Row0, b.Row1, b.Row2);
}

// 4 影響の加重和 (FGPUSkinVertexFactory の CalcBoneMatrix)
float3x4 CalcBoneMatrix(FVertexFactoryInput Input)
{
    float3x4 m = Input.BlendWeights.x * GetBoneMatrix(Input.BlendIndices.x);
    m += Input.BlendWeights.y * GetBoneMatrix(Input.BlendIndices.y);
    m += Input.BlendWeights.z * GetBoneMatrix(Input.BlendIndices.z);
    m += Input.BlendWeights.w * GetBoneMatrix(Input.BlendIndices.w);
    return m;
}

float3x4 CalcPreviousBoneMatrix(FVertexFactoryInput Input)
{
    float3x4 m = Input.BlendWeights.x * GetPreviousBoneMatrix(Input.BlendIndices.x);
    m += Input.BlendWeights.y * GetPreviousBoneMatrix(Input.BlendIndices.y);
    m += Input.BlendWeights.z * GetPreviousBoneMatrix(Input.BlendIndices.z);
    m += Input.BlendWeights.w * GetPreviousBoneMatrix(Input.BlendIndices.w);
    return m;
}

FVertexFactoryIntermediates GetVertexFactoryIntermediates(FVertexFactoryInput Input)
{
    FVertexFactoryIntermediates i;
    i.BlendMatrix = CalcBoneMatrix(Input);
    i.PreviousBlendMatrix = CalcPreviousBoneMatrix(Input);
    return i;
}

float3 VertexFactoryGetLocalPosition(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return mul(Intermediates.BlendMatrix, float4(Input.Position, 1.0f));
}

float3 VertexFactoryGetPreviousLocalPosition(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return mul(Intermediates.PreviousBlendMatrix, float4(Input.Position, 1.0f));
}

// 法線 / 接線はボーン行列の 3x3 (一様スケール前提。UE の SkinTangents と同じ)
float3 VertexFactoryGetLocalNormal(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return mul((float3x3) Intermediates.BlendMatrix, Input.Normal);
}

float3 VertexFactoryGetLocalTangent(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return mul((float3x3) Intermediates.BlendMatrix, Input.Tangent);
}

#else // ---- ローカル VF (FLocalVertexFactory) ----

#define FVertexFactoryInput VS_INPUT

struct FVertexFactoryIntermediates
{
    float Dummy;
};

FVertexFactoryIntermediates GetVertexFactoryIntermediates(FVertexFactoryInput Input)
{
    FVertexFactoryIntermediates i;
    i.Dummy = 0.0f;
    return i;
}

float3 VertexFactoryGetLocalPosition(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return Input.Position;
}

float3 VertexFactoryGetPreviousLocalPosition(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return Input.Position;
}

float3 VertexFactoryGetLocalNormal(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return Input.Normal;
}

float3 VertexFactoryGetLocalTangent(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return Input.Tangent;
}

#endif // GPUSKIN_VERTEX_FACTORY

#endif // VERTEX_FACTORY_HLSL

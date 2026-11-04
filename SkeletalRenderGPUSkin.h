#pragma once
#include <vector>
#include <cstdint>
#include "RenderManager.h"

// ============================================================
//  FSkeletalMeshObjectGPUSkin
//  FSkeletalMeshObjectGPUSkin (SkeletalRenderGPUSkin.h) +
//  FGPUBaseSkinVertexFactory::FShaderDataType (ボーン行列バッファ) に相当。
//  USkinnedMeshComponent が所有し (shared_ptr)、FSkeletalMeshSceneProxy が
//  同じものを参照する。毎フレーム USkinnedMeshComponent::
//  SendRenderDynamicData_Concurrent が ReferenceToLocal 行列列を
//  Update() で書き込み、プロキシが描画時に t39 (今) / t40 (前) へバインドする。
//
//  バッファ:
//    FBoneMatrix (float3x4 = 3 x float4, 48 B) x NumBones の
//    StructuredBuffer をアップロードヒープ (永続 Map) に 3 本持つリング。
//    Update のたびに次の本へ書く。3 本なのは、フレーム N (in-flight) が
//    「今」と「前」の 2 本を読んでいる間に、フレーム N+1 の書き込みが
//    フレーム N-1 (完了済み) の本に当たるようにするため。
//
//  前フレームの判定 (UE の PreviousRevisionNumber / UpdatedFrameNumber と同じ):
//    直前のフレーム (FScene::GetFrameNumber() - 1) に更新された本があれば
//    それが「前フレーム」、無ければ (一時停止中など) 今の本を使う = ボーン速度 0。
//
//  行列の規約: C++ の ReferenceToLocal (行ベクトル規約 4x4) を転置し、
//  上 3 行を Row0..2 に入れる。HLSL は mul(float3x4, float4(p, 1))
//  (VertexFactory.hlsl GetBoneMatrix)。
// ============================================================

struct FBoneMatrix
{
	XMFLOAT4 Row[3];
};
static_assert(sizeof(FBoneMatrix) == 48, "FBoneMatrix must be 3 x float4 (HLSL FBoneMatrix)");

class FSkeletalMeshObjectGPUSkin
{
private:
	static constexpr unsigned int NumBuffers = 3;
	static constexpr unsigned long long NeverUpdated = ~0ull;

	struct FBoneBuffer
	{
		ComPtr<ID3D12Resource> Resource;
		FBoneMatrix*           Mapped = nullptr;
		unsigned int           SRVIndex = 0;
		unsigned long long     UpdatedFrame = NeverUpdated;
	};

	FBoneBuffer  m_Buffers[NumBuffers];
	unsigned int m_Current = 0;
	unsigned int m_NumBones = 0;
	bool         m_bValid = false;

public:
	explicit FSkeletalMeshObjectGPUSkin(unsigned int NumBones);
	~FSkeletalMeshObjectGPUSkin();

	FSkeletalMeshObjectGPUSkin(const FSkeletalMeshObjectGPUSkin&) = delete;
	FSkeletalMeshObjectGPUSkin& operator=(const FSkeletalMeshObjectGPUSkin&) = delete;

	bool IsValid() const { return m_bValid; }
	unsigned int GetNumBones() const { return m_NumBones; }

	// 今フレームの ReferenceToLocal (転置前の行ベクトル規約) を次の本へ書く
	// (FSkeletalMeshObjectGPUSkin::Update -> FDynamicSkelMeshObjectDataGPUSkin)。
	// 要素数が NumBones に満たない分は単位行列
	void Update(const std::vector<XMFLOAT4X4>& ReferenceToLocal, unsigned long long FrameNumber);

	// t39 にバインドする SRV (今フレーム)
	unsigned int GetBoneBufferSRVIndex() const { return m_Buffers[m_Current].SRVIndex; }

	// t40 にバインドする SRV (前フレーム)。直前のフレームに更新した本が無ければ今の本
	unsigned int GetPreviousBoneBufferSRVIndex(unsigned long long FrameNumber) const;
};

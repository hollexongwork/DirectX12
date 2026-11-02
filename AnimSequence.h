#pragma once
#include <string>
#include <vector>
#include <memory>
#include "AnimationRuntime.h"
#include "Skeleton.h"

// ============================================================
//  UAnimSequenceBase / UAnimSequence
//  UAnimSequenceBase / UAnimSequence (Animation/AnimSequence.h) に相当。
//  ボーンごとの生トラック (FRawAnimSequenceTrack) を一定フレーム
//  レート (SamplingFrameRate) で持ち、GetAnimationPose / GetBonePose
//  で時刻のポーズを抽出する。トラックを持たないボーンは
//  リファレンスポーズになる (UE の BuildPoseFromRawData と同じ)。
//
//  圧縮 (FCompressedAnimSequence) / カーブ / 通知 / ルートモーション /
//  加算アニメーションは持たない。
// ============================================================

// ボーン 1 本分のキー列。キー数 1 = 定数トラック (UE と同じ)。
// Pos / Rot / Scale で個別にキー数が異なってもよい (各自 NumberOfKeys または 1)
struct FRawAnimSequenceTrack
{
	std::vector<XMFLOAT3> PosKeys;
	std::vector<XMFLOAT4> RotKeys;		// 四元数 (x, y, z, w)
	std::vector<XMFLOAT3> ScaleKeys;	// 空 = スケール 1

	bool IsEmpty() const { return PosKeys.empty() && RotKeys.empty() && ScaleKeys.empty(); }
};

// トラック添字 -> スケルトンのボーン添字 (FTrackToSkeletonMap)
struct FTrackToSkeletonMap
{
	int BoneTreeIndex = INDEX_NONE;
};

// フレームレート (FFrameRate)
struct FFrameRate
{
	int Numerator = 30;
	int Denominator = 1;

	float AsDecimal() const { return (Denominator != 0) ? static_cast<float>(Numerator) / static_cast<float>(Denominator) : 0.0f; }
	float AsInterval() const { return (Numerator != 0) ? static_cast<float>(Denominator) / static_cast<float>(Numerator) : 0.0f; }
};

class UAnimSequenceBase
{
protected:
	std::string                m_Name;
	std::shared_ptr<USkeleton> m_Skeleton;
	float                      m_SequenceLength = 0.0f;	// [s]

public:
	// 再生速度の倍率 (UAnimSequenceBase::RateScale)
	float RateScale = 1.0f;

	virtual ~UAnimSequenceBase() = default;

	const std::string& GetName() const { return m_Name; }
	void SetName(const std::string& Name) { m_Name = Name; }

	const std::shared_ptr<USkeleton>& GetSkeleton() const { return m_Skeleton; }
	void SetSkeleton(std::shared_ptr<USkeleton> Skeleton) { m_Skeleton = std::move(Skeleton); }

	// 再生長 [s] (UAnimSequenceBase::GetPlayLength)
	virtual float GetPlayLength() const { return m_SequenceLength; }

	// 時刻のポーズ (UAnimSequenceBase::GetAnimationPose)。
	// 既定はリファレンスポーズ
	virtual void GetAnimationPose(FAnimationPoseData& OutPoseData, const FAnimExtractContext& ExtractionContext) const;

	// このメッシュ (スケルトン) で再生できるか
	bool IsCompatibleWith(const USkeleton* Skeleton) const
	{
		return m_Skeleton != nullptr && Skeleton != nullptr && m_Skeleton->IsCompatible(Skeleton);
	}
};

class UAnimSequence : public UAnimSequenceBase
{
private:
	std::vector<FRawAnimSequenceTrack> m_RawAnimationData;
	std::vector<FTrackToSkeletonMap>   m_TrackToSkeletonMapTable;
	int                                m_NumberOfKeys = 0;
	FFrameRate                         m_SamplingFrameRate = { 30, 1 };

public:
	// キー数とフレームレートを設定し SequenceLength を導く (NumberOfKeys - 1 フレーム分)
	void SetSamplingFrameRate(const FFrameRate& FrameRate) { m_SamplingFrameRate = FrameRate; }
	void SetNumberOfKeys(int NumberOfKeys)
	{
		m_NumberOfKeys = (std::max)(1, NumberOfKeys);
		m_SequenceLength = static_cast<float>(m_NumberOfKeys - 1) * m_SamplingFrameRate.AsInterval();
	}
	// 再生長を直接指定する (キー列が長さ全体に等間隔で並ぶ)
	void SetSequenceLength(float Length) { m_SequenceLength = (std::max)(0.0f, Length); }

	// トラックを追加する。同じボーンのトラックがあれば置き換える。
	// ボーン添字が無効なら無視して false
	bool AddTrack(int BoneIndex, const FRawAnimSequenceTrack& Track);
	void ClearTracks() { m_RawAnimationData.clear(); m_TrackToSkeletonMapTable.clear(); }

	int GetNumberOfKeys() const { return m_NumberOfKeys; }
	const FFrameRate& GetSamplingFrameRate() const { return m_SamplingFrameRate; }
	int GetNumTracks() const { return static_cast<int>(m_RawAnimationData.size()); }
	const std::vector<FRawAnimSequenceTrack>& GetRawAnimationData() const { return m_RawAnimationData; }
	const std::vector<FTrackToSkeletonMap>& GetTrackToSkeletonMapTable() const { return m_TrackToSkeletonMapTable; }
	int GetTrackIndexForBone(int BoneIndex) const;

	// ---- ポーズ抽出 ----
	void GetAnimationPose(FAnimationPoseData& OutPoseData, const FAnimExtractContext& ExtractionContext) const override;

	// UAnimSequence::GetBonePose: 全ボーンをリファレンスポーズにしてからトラックを上書き
	void GetBonePose(FCompactPose& OutPose, FBlendedCurve& OutCurve, const FAnimExtractContext& ExtractionContext) const;

	// 1 トラックの時刻 Time の変換 (ExtractBoneTransform + キー補間)。
	// 平行移動 / スケールは線形、回転は slerp (UE の FAnimationRuntime と同じ)
	void ExtractBoneTransform(const FRawAnimSequenceTrack& Track, FTransform& OutAtom, float Time) const;
};

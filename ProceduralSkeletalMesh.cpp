#include "Main.h"
#include "ProceduralSkeletalMesh.h"

#include <cmath>

namespace
{
	constexpr float kPi = 3.14159265358979323846f;

	float SmoothStep(float Edge0, float Edge1, float X)
	{
		const float t = (std::max)(0.0f, (std::min)(1.0f, (X - Edge0) / (Edge1 - Edge0)));
		return t * t * (3.0f - 2.0f * t);
	}

	// 高さ y でのボーン影響 (関節前後で隣接ボーンをブレンド)
	void ComputeChainWeights(const FProceduralSkeletalChainParams& P, float Y, FSkeletalMeshImportVertex& V)
	{
		const float L = P.BoneLength;
		const int numBones = (std::max)(1, P.NumBones);
		int bone = static_cast<int>(std::floor(Y / L));
		bone = (std::max)(0, (std::min)(numBones - 1, bone));

		// 最も近い関節 (y = j * L, j = 1..NumBones-1)
		const int jointBelow = bone;			// 関節 bone (= bone 自身の根元) : bone-1 と bone のブレンド
		const int jointAbove = bone + 1;		// 関節 bone+1 : bone と bone+1 のブレンド
		const float b = (std::max)(0.0f, (std::min)(P.BlendHalfWidth, L * 0.5f));

		if (jointAbove <= numBones - 1 && Y > jointAbove * L - b)
		{
			const float w = SmoothStep(jointAbove * L - b, jointAbove * L + b, Y);
			V.AddInfluence(bone, 1.0f - w);
			V.AddInfluence(bone + 1, w);
		}
		else if (jointBelow >= 1 && Y < jointBelow * L + b)
		{
			const float w = SmoothStep(jointBelow * L - b, jointBelow * L + b, Y);
			V.AddInfluence(bone - 1, 1.0f - w);
			V.AddInfluence(bone, w);
		}
		else
		{
			V.AddInfluence(bone, 1.0f);
		}
	}
}

// ============================================================
//  BuildChain
// ============================================================
void FProceduralSkeletalMeshBuilder::BuildChain(const FProceduralSkeletalChainParams& Params, FSkeletalMeshImportData& OutData)
{
	OutData = FSkeletalMeshImportData{};

	FProceduralSkeletalChainParams P = Params;
	P.NumBones = (std::max)(1, P.NumBones);
	P.BoneLength = (std::max)(0.01f, P.BoneLength);
	P.Radius = (std::max)(0.001f, P.Radius);
	P.Slices = (std::max)(3u, P.Slices);
	P.RingsPerBone = (std::max)(1u, P.RingsPerBone);

	// ---- リファレンススケルトン ----
	OutData.RefSkeleton.Add(FMeshBoneInfo("Root", INDEX_NONE), FTransform::Identity());
	for (int i = 1; i < P.NumBones; ++i)
	{
		FTransform pose;
		pose.Translation = { 0.0f, P.BoneLength, 0.0f };
		OutData.RefSkeleton.Add(FMeshBoneInfo("Bone" + std::to_string(i), i - 1), pose);
	}

	// ---- 筒 ----
	const float height = P.BoneLength * static_cast<float>(P.NumBones);
	const unsigned int numRings = P.RingsPerBone * static_cast<unsigned int>(P.NumBones) + 1;
	const unsigned int ringVerts = P.Slices + 1;	// 継ぎ目の頂点を複製 (UV の切れ目)

	auto RadiusAt = [&](float y) { return P.Radius * (1.0f + (P.TipRadiusScale - 1.0f) * (y / height)); };
	const float slope = (RadiusAt(height) - RadiusAt(0.0f)) / height;	// dr/dy

	for (unsigned int r = 0; r < numRings; ++r)
	{
		const float v = static_cast<float>(r) / static_cast<float>(numRings - 1);
		const float y = v * height;
		const float radius = RadiusAt(y);

		for (unsigned int s = 0; s < ringVerts; ++s)
		{
			const float u = static_cast<float>(s) / static_cast<float>(P.Slices);
			// 接線 = +U の規約 (dP/dV = cross(N, dP/dU) で +Y): u が増えると theta は減る
			const float theta = (1.0f - u) * 2.0f * kPi;
			const float c = std::cos(theta);
			const float sn = std::sin(theta);

			FSkeletalMeshImportVertex vert;
			vert.Position = { radius * c, y, radius * sn };
			// 先細りの法線 (cos, -slope, sin) を正規化
			const float nl = 1.0f / std::sqrt(1.0f + slope * slope);
			vert.Normal = { c * nl, -slope * nl, sn * nl };
			vert.Tangent = { sn, 0.0f, -c };
			vert.TexCoord = { u * P.UVScale.x, v * P.UVScale.y };
			vert.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
			ComputeChainWeights(P, y, vert);
			OutData.Vertices.push_back(vert);
		}
	}

	for (unsigned int r = 0; r + 1 < numRings; ++r)
	{
		const unsigned int lower = r * ringVerts;
		const unsigned int upper = (r + 1) * ringVerts;
		for (unsigned int s = 0; s < P.Slices; ++s)
		{
			// 幾何法線 = cross(v1 - v0, v2 - v0) が外向き (ProceduralMeshGenerator の規約)
			OutData.Indices.push_back(lower + s);
			OutData.Indices.push_back(upper + s);
			OutData.Indices.push_back(lower + s + 1);

			OutData.Indices.push_back(lower + s + 1);
			OutData.Indices.push_back(upper + s);
			OutData.Indices.push_back(upper + s + 1);
		}
	}

	// ---- キャップ (底 / 先端) ----
	auto AddCap = [&](float y, bool bTop)
		{
			const float radius = RadiusAt(y);
			const unsigned int center = static_cast<unsigned int>(OutData.Vertices.size());

			FSkeletalMeshImportVertex cv;
			cv.Position = { 0.0f, y, 0.0f };
			cv.Normal = { 0.0f, bTop ? 1.0f : -1.0f, 0.0f };
			cv.Tangent = { 1.0f, 0.0f, 0.0f };
			cv.TexCoord = { 0.5f, 0.5f };
			ComputeChainWeights(P, y, cv);
			OutData.Vertices.push_back(cv);

			const unsigned int ring = static_cast<unsigned int>(OutData.Vertices.size());
			for (unsigned int s = 0; s < P.Slices; ++s)
			{
				const float theta = (1.0f - static_cast<float>(s) / static_cast<float>(P.Slices)) * 2.0f * kPi;
				FSkeletalMeshImportVertex rv = cv;
				rv.Position = { radius * std::cos(theta), y, radius * std::sin(theta) };
				rv.TexCoord = { 0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta) };
				OutData.Vertices.push_back(rv);
			}

			for (unsigned int s = 0; s < P.Slices; ++s)
			{
				const unsigned int a = ring + s;
				const unsigned int b = ring + (s + 1) % P.Slices;
				// theta は s と逆向きに回るので、底 (-Y) は (center, b, a)、先端 (+Y) は (center, a, b)
				OutData.Indices.push_back(center);
				OutData.Indices.push_back(bTop ? a : b);
				OutData.Indices.push_back(bTop ? b : a);
			}
		};
	AddCap(0.0f, false);
	AddCap(height, true);

	FSkeletalMeshImportSection section;
	section.MaterialIndex = 0;
	section.FirstIndex = 0;
	section.NumTriangles = static_cast<unsigned int>(OutData.Indices.size() / 3);
	OutData.Sections.push_back(section);
}

// ============================================================
//  アニメーション
// ============================================================
namespace
{
	std::shared_ptr<UAnimSequence> CreateChainSequence(const std::shared_ptr<USkeleton>& Skeleton,
		const FProceduralSkeletalChainParams& Params, const std::string& Name,
		float Length, float AmplitudeDeg, float SideAmplitudeDeg, float PhaseStepRad, float FrameRate)
	{
		auto sequence = std::make_shared<UAnimSequence>();
		sequence->SetName(Name);
		sequence->SetSkeleton(Skeleton);

		FFrameRate frameRate;
		frameRate.Numerator = (std::max)(1, static_cast<int>(std::lround(FrameRate)));
		frameRate.Denominator = 1;
		sequence->SetSamplingFrameRate(frameRate);

		const int numKeys = (std::max)(2, static_cast<int>(std::lround(Length * FrameRate)) + 1);
		sequence->SetNumberOfKeys(numKeys);
		sequence->SetSequenceLength(Length);

		if (!Skeleton) return sequence;
		const FReferenceSkeleton& ref = Skeleton->GetReferenceSkeleton();
		const int numBones = (std::min)(ref.GetNum(), Params.NumBones);

		for (int bone = 1; bone < numBones; ++bone)
		{
			FRawAnimSequenceTrack track;
			track.PosKeys.push_back(ref.GetRefBonePose()[bone].Translation);	// 定数トラック
			track.RotKeys.resize(numKeys);

			for (int k = 0; k < numKeys; ++k)
			{
				// 先頭と末尾のキーが一致するよう t = k / (numKeys - 1) * Length で 1 周期
				const float t = static_cast<float>(k) / static_cast<float>(numKeys - 1);
				const float phase = 2.0f * kPi * t + static_cast<float>(bone) * PhaseStepRad;
				const float bend = XMConvertToRadians(AmplitudeDeg) * std::sin(phase);
				const float side = XMConvertToRadians(SideAmplitudeDeg) * std::sin(phase * 2.0f + 1.3f);

				const XMVECTOR qz = XMQuaternionRotationAxis(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), bend);
				const XMVECTOR qx = XMQuaternionRotationAxis(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), side);
				XMStoreFloat4(&track.RotKeys[k], XMQuaternionNormalize(XMQuaternionMultiply(qx, qz)));
			}

			sequence->AddTrack(bone, track);
		}

		return sequence;
	}
}

std::shared_ptr<UAnimSequence> FProceduralSkeletalMeshBuilder::CreateWaveSequence(const std::shared_ptr<USkeleton>& Skeleton,
	const FProceduralSkeletalChainParams& Params, const std::string& Name,
	float Length, float AmplitudeDeg, float PhaseStepRad, float FrameRate)
{
	return CreateChainSequence(Skeleton, Params, Name, Length, AmplitudeDeg, AmplitudeDeg * 0.5f, PhaseStepRad, FrameRate);
}

std::shared_ptr<UAnimSequence> FProceduralSkeletalMeshBuilder::CreateIdleSequence(const std::shared_ptr<USkeleton>& Skeleton,
	const FProceduralSkeletalChainParams& Params, const std::string& Name,
	float Length, float AmplitudeDeg, float FrameRate)
{
	return CreateChainSequence(Skeleton, Params, Name, Length, AmplitudeDeg, AmplitudeDeg * 0.5f, 0.35f, FrameRate);
}

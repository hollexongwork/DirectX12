#pragma once
#include <DirectXMath.h>
#include "PostProcessSettings.h"

using namespace DirectX;

// ============================================================
//  FSceneView
//  FSceneView に相当する「1 フレーム分のビュー情報」の
//  値スナップショット。ゲーム側 (UWorld::CalcSceneView) が
//  フレーム先頭で 1 回だけ構築し、レンダラ (FSceneRenderer /
//  FShadowSceneRenderer) はこの構造体だけを読む。
//  レンダラが UCameraComponent / APostProcessVolume に直接
//  触れることはもう無い (ゲーム側とレンダー側のデータフローを
//  一方向にする。将来のレンダースレッド化の土台)。
// ============================================================

struct FSceneView
{
	// アクティブカメラが存在するか。false のフレームは
	// ビュー定数 / CSM を更新しない (従来のカメラ不在時挙動と同じ)。
	bool bValid = false;

	// ---- ビュー / 射影行列 (転置前) ----
	// レンダラ側で VIEW 定数 (b0) へ転置して詰められる。
	XMFLOAT4X4 ViewMatrix;
	XMFLOAT4X4 ProjectionMatrix;

	// ---- カメラパラメータ ----
	// CSM のカスケードフィッティング (FShadowSceneRenderer::
	// SetupDirectionalShadows) と VIEW 定数 (WorldCameraOrigin /
	// NearFar) が参照する。
	XMFLOAT3 ViewOrigin = { 0.0f, 0.0f, 0.0f };	// カメラワールド位置 [m]
	XMFLOAT3 ViewForward = { 0.0f, 0.0f, 1.0f };	// 正規化済み前方ベクトル
	float    FOV = 45.0f;							// 垂直画角 [度]
	float    NearClip = 0.1f;
	float    FarClip = 500.0f;
	float    AspectRatio = 16.0f / 9.0f;

	// ---- 解決済みポストプロセス設定 (FFinalPostProcessSettings 相当) ----
	// APostProcessVolume からゲーム側 (UWorld::CalcSceneView) で
	// 解決済み。ボリューム不在 / 無効時は既定値が入る。
	PP_SETTINGS FinalPostProcessSettings{};

	FSceneView()
	{
		// 行列は単位行列で初期化 (bValid=false でも安全に読める)
		XMStoreFloat4x4(&ViewMatrix, XMMatrixIdentity());
		XMStoreFloat4x4(&ProjectionMatrix, XMMatrixIdentity());
	}
};

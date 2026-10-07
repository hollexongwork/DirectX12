#pragma once
#include "SceneComponent.h"

// ============================================================
//  UCameraComponent
//  UCameraComponent に相当。ビュー/プロジェクション情報を
//  保持する。レンダラはカメラを直接読まず、ゲーム側の
//  UWorld::CalcSceneView がフレーム先頭で FSceneView (値
//  スナップショット) を構築して渡す。カメラ自身はもう描画パスに
//  関与しない。
// ============================================================

struct FSceneView;

class UCameraComponent : public USceneComponent
{
private:
	float m_FOV = 45.0f;	// 度 (FieldOfView)
	float m_NearClip = 0.1f;
	float m_FarClip = 500.0f;

public:
	void OnRegister() override;
	void OnUnregister() override;

	// UWorld::CalcSceneView がフレーム先頭に 1 回呼ぶ。
	// ビュー / 射影行列とカメラパラメータを FSceneView へ書き込み、
	// bValid を立てる (ディレクショナルライト系のビュー定数解決は
	// SetupLightConstants が担当するため関与しない)。
	void GetSceneView(FSceneView& OutView, float AspectRatio) const;

	float GetFieldOfView() const { return m_FOV; }
	void  SetFieldOfView(float FieldOfView) { m_FOV = FieldOfView; }

	float GetNearClip() const { return m_NearClip; }
	float GetFarClip() const { return m_FarClip; }
	void  SetNearClip(float NearClip) { m_NearClip = NearClip; }
	void  SetFarClip(float FarClip) { m_FarClip = FarClip; }

	// ---- カメラカットのラッチ (テレポート / Reset / Load 時の bCameraCut) ----
	// NotifyCameraCut で立て、ACameraActor::Tick が IsCameraCutPending を読んで
	// コントローラの慣性を捨て (消費しない)、UWorld::CalcSceneView が
	// ConsumeCameraCut で消費して FSceneView::bCameraCut に反映する。
	void NotifyCameraCut() { m_bCameraCutPending = true; }            // テレポート / Reset / Load
	bool IsCameraCutPending() const { return m_bCameraCutPending; }    // ACameraActor::Tick が読む (消費しない)
	bool ConsumeCameraCut() { const bool b = m_bCameraCutPending; m_bCameraCutPending = false; return b; }

private:
	bool m_bCameraCutPending = false;
};

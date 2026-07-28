#pragma once
#include "SceneComponent.h"

// ============================================================
//  UCameraComponent
//  UCameraComponent に相当。ビュー/プロジェクション情報を
//  保持し、FSceneRenderer がビュー定数 (VIEW_CONSTANT, b0) を
//  構築する際に参照する。カメラ自身はもう描画パスに関与しない。
// ============================================================

struct VIEW_CONSTANT;

class UCameraComponent : public USceneComponent
{
private:
	float m_FOV = 45.0f;	// 度 (FieldOfView)
	float m_NearClip = 0.1f;
	float m_FarClip = 500.0f;

public:
	void OnRegister() override;
	void OnUnregister() override;

	// FSceneRenderer::RenderBasePass がビュー定数を構築する際に呼ぶ。
	// カメラ由来のフィールドのみ書き込む (ディレクショナルライト系は
	// SetupLightConstants が担当するため触らない)。
	void GetViewConstants(VIEW_CONSTANT& OutConstant, float AspectRatio) const;

	float GetFieldOfView() const { return m_FOV; }
	void  SetFieldOfView(float FieldOfView) { m_FOV = FieldOfView; }

	float GetNearClip() const { return m_NearClip; }
	float GetFarClip() const { return m_FarClip; }
	void  SetNearClip(float NearClip) { m_NearClip = NearClip; }
	void  SetFarClip(float FarClip) { m_FarClip = FarClip; }
};

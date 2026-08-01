#include "Main.h"
#include "Scene.h"
#include "PrimitiveComponent.h"
#include "LightComponent.h"

void FScene::AddPrimitive(UPrimitiveComponent* Primitive)
{
	for (const auto& info : m_Primitives)
	{
		if (info.Component == Primitive) return;
	}

	// レンダー側ミラー (プロキシ) を生成してシーンが所有する
	FPrimitiveSceneInfo info;
	info.Component = Primitive;
	info.Proxy.reset(Primitive->CreateSceneProxy());

	Primitive->SetSceneProxy(info.Proxy.get());
	Primitive->ClearRenderStateDirty();	// 生成直後は最新スナップショット

	m_Primitives.push_back(std::move(info));

	// 初回フレームのトランスフォーム / 境界プッシュを予約する
	// (登録前に立った未エンキューのフラグをクリアしてから積み直す。
	//  従来の「毎フレームプッシュ」時代の初回フレーム挙動を保存)
	Primitive->ClearRenderTransformDirty();
	Primitive->MarkRenderTransformDirty();
}

void FScene::RemovePrimitive(UPrimitiveComponent* Primitive)
{
	auto it = std::find_if(m_Primitives.begin(), m_Primitives.end(),
		[Primitive](const FPrimitiveSceneInfo& info) { return info.Component == Primitive; });

	if (it != m_Primitives.end())
	{
		Primitive->SetSceneProxy(nullptr);
		m_Primitives.erase(it);
	}

	// ダーティリストからも除去する (ダングリングポインタ防止)
	m_PrimitiveRenderStateDirtyList.erase(
		std::remove(m_PrimitiveRenderStateDirtyList.begin(), m_PrimitiveRenderStateDirtyList.end(), Primitive),
		m_PrimitiveRenderStateDirtyList.end());
	m_PrimitiveTransformDirtyList.erase(
		std::remove(m_PrimitiveTransformDirtyList.begin(), m_PrimitiveTransformDirtyList.end(), Primitive),
		m_PrimitiveTransformDirtyList.end());

	// 「フラグ = エンキュー済み」の対応を保つためフラグも下ろす
	// (再登録時は AddPrimitive が積み直す)
	Primitive->ClearRenderTransformDirty();
}

void FScene::UpdateAllPrimitiveSceneInfos()
{
	// 処理中のエンキュー (push_back による再確保) に備えて
	// ローカルへ swap してから処理する (World::Tick の PendingKill と同じ理由)
	std::vector<UPrimitiveComponent*> renderStateDirty;
	renderStateDirty.swap(m_PrimitiveRenderStateDirtyList);
	std::vector<UPrimitiveComponent*> transformDirty;
	transformDirty.swap(m_PrimitiveTransformDirtyList);

	// ---- レンダーステートダーティ (プッシュ型) ----
	// リストにあるプリミティブだけプロキシをその場で作り直す (描画順は不変)
	for (UPrimitiveComponent* component : renderStateDirty)
	{
		auto it = std::find_if(m_Primitives.begin(), m_Primitives.end(),
			[component](const FPrimitiveSceneInfo& info) { return info.Component == component; });
		if (it == m_Primitives.end()) continue;

		it->Proxy.reset(component->CreateSceneProxy());
		component->SetSceneProxy(it->Proxy.get());
		component->ClearRenderStateDirty();

		// メッシュ / マテリアル変更で境界も変わり得るため、
		// 境界を再計算してプッシュしておく (従来の毎フレーム
		// UpdateBounds 相当の挙動を保存)
		component->SendRenderTransform();
	}

	// ---- トランスフォームダーティ (プッシュ型) ----
	// 移動したプリミティブだけトランスフォーム + 境界 + 可視性を
	// プロキシへプッシュする。アタッチ階層の親移動は
	// USceneComponent::MarkRenderTransformDirty が子へ再帰伝搬する
	// ため漏れない。
	for (UPrimitiveComponent* component : transformDirty)
	{
		component->SendRenderTransform();
		component->ClearRenderTransformDirty();
	}
}

// ============================================================
//  ライト (FScene::AddLight / RemoveLight)
//  プリミティブと同じプロキシ + ダーティリストパターン。
// ============================================================

void FScene::AddLight(ULightComponent* Light)
{
	for (const auto& info : m_Lights)
	{
		if (info.Component == Light) return;
	}

	// レンダー側ミラー (プロキシ) を生成してシーンが所有する
	FLightSceneInfo info;
	info.Component = Light;
	info.Proxy.reset(Light->CreateLightSceneProxy());

	Light->SetSceneProxy(info.Proxy.get());
	Light->ClearRenderStateDirty();	// 生成直後は最新スナップショット

	m_Lights.push_back(std::move(info));

	// 初回フレームのトランスフォームプッシュを予約する (プリミティブと同じ)
	Light->ClearRenderTransformDirty();
	Light->MarkRenderTransformDirty();
}

void FScene::RemoveLight(ULightComponent* Light)
{
	auto it = std::find_if(m_Lights.begin(), m_Lights.end(),
		[Light](const FLightSceneInfo& info) { return info.Component == Light; });

	if (it != m_Lights.end())
	{
		Light->SetSceneProxy(nullptr);
		m_Lights.erase(it);
	}

	// ダーティリストからも除去する (ダングリングポインタ防止)
	m_LightRenderStateDirtyList.erase(
		std::remove(m_LightRenderStateDirtyList.begin(), m_LightRenderStateDirtyList.end(), Light),
		m_LightRenderStateDirtyList.end());
	m_LightTransformDirtyList.erase(
		std::remove(m_LightTransformDirtyList.begin(), m_LightTransformDirtyList.end(), Light),
		m_LightTransformDirtyList.end());

	Light->ClearRenderTransformDirty();
}

void FScene::UpdateAllLightSceneInfos()
{
	// 処理中のエンキューに備えてローカルへ swap してから処理する
	// (UpdateAllPrimitiveSceneInfos と同じ理由)
	std::vector<ULightComponent*> renderStateDirty;
	renderStateDirty.swap(m_LightRenderStateDirtyList);
	std::vector<ULightComponent*> transformDirty;
	transformDirty.swap(m_LightTransformDirtyList);

	// ---- レンダーステートダーティ (プッシュ型) ----
	// プロパティ変更 -> プロキシをその場で作り直す
	// (強度 / 色 / 半径 / コーン角などは全てここで反映される。
	//  プロキシ生成時にトランスフォームもスナップショットされる)
	for (ULightComponent* component : renderStateDirty)
	{
		auto it = std::find_if(m_Lights.begin(), m_Lights.end(),
			[component](const FLightSceneInfo& info) { return info.Component == component; });
		if (it == m_Lights.end()) continue;

		it->Proxy.reset(component->CreateLightSceneProxy());
		component->SetSceneProxy(it->Proxy.get());
		component->ClearRenderStateDirty();
	}

	// ---- トランスフォームダーティ (プッシュ型) ----
	// 移動したライトだけ位置 / 発光方向 / 幅軸をプッシュする
	for (ULightComponent* component : transformDirty)
	{
		component->SendRenderTransform();
		component->ClearRenderTransformDirty();
	}
}

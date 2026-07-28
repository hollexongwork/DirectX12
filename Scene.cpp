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
}

void FScene::UpdateAllPrimitiveSceneInfos()
{
	for (auto& info : m_Primitives)
	{
		UPrimitiveComponent* component = info.Component;

		// レンダーステート変更 -> プロキシをその場で作り直す (描画順は不変)
		if (component->IsRenderStateDirty())
		{
			info.Proxy.reset(component->CreateSceneProxy());
			component->SetSceneProxy(info.Proxy.get());
			component->ClearRenderStateDirty();
		}

		// トランスフォーム + 可視性は毎フレームプッシュする。
		// (アタッチ階層の親移動も漏れなく反映される。プリミティブ数が
		//  数百を超えたらダーティフラグで間引く最適化を入れる)
		component->SendRenderTransform();
	}
}

// ============================================================
//  ライト (FScene::AddLight / RemoveLight)
//  プリミティブと同じプロキシパターン。
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
}

void FScene::UpdateAllLightSceneInfos()
{
	for (auto& info : m_Lights)
	{
		ULightComponent* component = info.Component;

		// プロパティ変更 -> プロキシをその場で作り直す
		// (強度 / 色 / 半径 / コーン角などは全てここで反映される)
		if (component->IsRenderStateDirty())
		{
			info.Proxy.reset(component->CreateLightSceneProxy());
			component->SetSceneProxy(info.Proxy.get());
			component->ClearRenderStateDirty();
		}

		// 位置 / 発光方向 / 幅軸は毎フレームプッシュする
		component->SendRenderTransform();
	}
}
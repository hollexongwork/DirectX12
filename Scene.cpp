#include "Main.h"
#include "Scene.h"
#include "PrimitiveComponent.h"
#include "LightComponent.h"
#include "ExponentialHeightFogComponent.h"

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

	// ベロシティ履歴の登録 (Prev = Current = 生成時の変換, テレポート保留)。
	// 最初のプッシュはテレポート扱いなのでスポーン時の姿勢から速度は出ない
	m_VelocityData.Register(Primitive, info.Proxy->GetLocalToWorld());

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

	// ベロシティ履歴も破棄 (同じアドレスに別コンポーネントが来ても古い Prev を使わない)
	m_VelocityData.Remove(Primitive);

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
	// ---- ベロシティ: 前フレームの描画値 -> Prev (全エントリ) ----
	// プッシュの有無に関わらず毎フレーム行う (動きが止まったプリミティブは次のフレームで
	// 速度 0 に戻る。古い Prev が残り続ける危険の解消)
	m_VelocityData.StartFrame();

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

		// ベロシティ: 今フレーム描く変換 (キーはコンポーネントなのでプロキシ再生成を跨いで履歴が残る)
		m_VelocityData.UpdateTransform(component, it->Proxy->GetLocalToWorld());
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

		// ベロシティ: 今フレーム描く変換 (プロキシへプッシュした値そのもの)
		if (const FPrimitiveSceneProxy* proxy = component->GetSceneProxy())
		{
			m_VelocityData.UpdateTransform(component, proxy->GetLocalToWorld());
		}
	}

	// ---- ベロシティ: テレポート保留をすべて下ろす (今フレームのプッシュで消費済み) ----
	m_VelocityData.EndFrameUpdates();
}

// ============================================================
//  ライト (FScene::AddLight / RemoveLight / UpdateLightTransform /
//  UpdateLightColorAndBrightness)
//  登録簿を書き換えるのは *_RenderThread。シングルスレッドなので直接呼ぶ。
//  呼ばれるのはゲーム側フェーズ (Tick / SendAllEndOfFrameUpdates / UI) だけで、
//  レンダラがプロキシを読んでいる最中には呼ばれない。
// ============================================================

FScene::~FScene()
{
	// 通常は UWorld の破棄で全コンポーネントが登録解除済み。残っていれば破棄する
	for (FLightSceneInfoCompact& compact : m_Lights)
	{
		delete compact.LightSceneInfo;
		compact.LightSceneInfo = nullptr;
	}
}

void FScene::AddLight(ULightComponent* Light)
{
	// 二重登録防止
	if (Light->GetSceneProxy() != nullptr)
	{
		return;
	}

	// レンダー側ミラー (プロキシ) を生成する
	FLightSceneProxy* Proxy = Light->CreateSceneProxy();
	if (Proxy == nullptr)
	{
		return;
	}

	// プロキシをライトへ関連付ける
	Light->SetSceneProxy(Proxy);

	// トランスフォームと位置を入れる
	Proxy->SetTransform(Light->GetLightToWorldNoScale(), Light->GetLightPosition());

	// FLightSceneInfo を作る (プロキシの所有者)
	Proxy->m_LightSceneInfo = new FLightSceneInfo(Proxy, true);
	Proxy->m_LightSceneInfo->Scene = this;

	AddLightSceneInfo_RenderThread(Proxy->m_LightSceneInfo);
}

void FScene::AddLightSceneInfo_RenderThread(FLightSceneInfo* LightSceneInfo)
{
	// ライトリストへ追加する (空き番号があれば再利用)
	if (!m_FreeLightIds.empty())
	{
		LightSceneInfo->Id = m_FreeLightIds.back();
		m_FreeLightIds.pop_back();
		m_Lights[LightSceneInfo->Id] = FLightSceneInfoCompact(LightSceneInfo);
	}
	else
	{
		LightSceneInfo->Id = (int)m_Lights.size();
		m_Lights.push_back(FLightSceneInfoCompact(LightSceneInfo));
	}

	const bool bDirectionalLight = LightSceneInfo->Proxy->GetLightType() == LightType_Directional;
	if (bDirectionalLight)
	{
		m_DirectionalLights.push_back(LightSceneInfo);

		if (m_SimpleDirectionalLight == nullptr)
		{
			m_SimpleDirectionalLight = LightSceneInfo;
		}
	}

	ProcessAtmosphereLightAddition_RenderThread(LightSceneInfo);
}

void FScene::ProcessAtmosphereLightAddition_RenderThread(FLightSceneInfo* LightSceneInfo)
{
	if (LightSceneInfo->Proxy->IsUsedAsAtmosphereSunLight())
	{
		const unsigned char Index = LightSceneInfo->Proxy->GetAtmosphereSunLightIndex();
		if (m_AtmosphereLights[Index] == nullptr ||	// 未設定なら採用
			GetLightColorLuminance(LightSceneInfo->Proxy->GetColor()) > GetLightColorLuminance(m_AtmosphereLights[Index]->Proxy->GetColor()))	// 設定済みなら明るい方
		{
			m_AtmosphereLights[Index] = LightSceneInfo;
		}
	}
}

void FScene::ProcessAtmosphereLightRemoval_RenderThread(FLightSceneInfo* LightSceneInfo)
{
	// 明るさ / 添字の変更は「外してから入れ直す」ので、外されるライトの添字だけを見ればよい
	const unsigned char Index = LightSceneInfo->Proxy->GetAtmosphereSunLightIndex();
	if (m_AtmosphereLights[Index] == LightSceneInfo)
	{
		m_AtmosphereLights[Index] = nullptr;
		float SelectedLightLuminance = 0.0f;

		for (FLightSceneInfo* LightInfo : m_DirectionalLights)
		{
			const float LightLuminance = GetLightColorLuminance(LightInfo->Proxy->GetColor());
			if (LightInfo != LightSceneInfo
				&& LightInfo->Proxy->IsUsedAsAtmosphereSunLight() && LightInfo->Proxy->GetAtmosphereSunLightIndex() == Index
				&& (m_AtmosphereLights[Index] == nullptr || SelectedLightLuminance < LightLuminance))
			{
				m_AtmosphereLights[Index] = LightInfo;
				SelectedLightLuminance = LightLuminance;
			}
		}
	}
}

void FScene::RemoveLight(ULightComponent* Light)
{
	FLightSceneProxy* Proxy = Light->GetSceneProxy();
	if (Proxy == nullptr)
	{
		return;
	}

	FLightSceneInfo* LightSceneInfo = Proxy->GetLightSceneInfo();

	// プロキシとライトの関連を切る
	Light->SetSceneProxy(nullptr);

	RemoveLightSceneInfo_RenderThread(LightSceneInfo);
}

void FScene::RemoveLightSceneInfo_RenderThread(FLightSceneInfo* LightSceneInfo)
{
	const bool bDirectionalLight = LightSceneInfo->Proxy->GetLightType() == LightType_Directional;

	if (bDirectionalLight)
	{
		m_DirectionalLights.erase(
			std::remove(m_DirectionalLights.begin(), m_DirectionalLights.end(), LightSceneInfo),
			m_DirectionalLights.end());

		if (LightSceneInfo == m_SimpleDirectionalLight)
		{
			// 外したのが SimpleDirectionalLight なら、残りの先頭を代わりにする
			m_SimpleDirectionalLight = m_DirectionalLights.empty() ? nullptr : m_DirectionalLights.front();
		}
	}

	ProcessAtmosphereLightRemoval_RenderThread(LightSceneInfo);

	// ライトリストから外し、番号を空きへ戻す
	if (LightSceneInfo->Id >= 0 && (size_t)LightSceneInfo->Id < m_Lights.size())
	{
		m_Lights[LightSceneInfo->Id] = FLightSceneInfoCompact();
		m_FreeLightIds.push_back(LightSceneInfo->Id);
		LightSceneInfo->Id = -1;
	}

	// FLightSceneInfo とプロキシを破棄する (プロキシは FLightSceneInfo のデストラクタが破棄)
	delete LightSceneInfo;
}

void FScene::UpdateLightTransform(ULightComponent* Light)
{
	FLightSceneProxy* Proxy = Light->GetSceneProxy();
	if (Proxy == nullptr)
	{
		return;
	}

	FLightSceneInfo* LightSceneInfo = Proxy->GetLightSceneInfo();
	if (LightSceneInfo && LightSceneInfo->bVisible)
	{
		// トランスフォームと位置を更新する
		Proxy->SetTransform(Light->GetLightToWorldNoScale(), Light->GetLightPosition());

		// FLightSceneInfoCompact (境界球) も取り直す
		if (LightSceneInfo->Id != -1)
		{
			m_Lights[LightSceneInfo->Id].Init(LightSceneInfo);
		}
	}
}

void FScene::UpdateLightColorAndBrightness(ULightComponent* Light)
{
	FLightSceneProxy* Proxy = Light->GetSceneProxy();
	if (Proxy == nullptr)
	{
		return;
	}

	FLightSceneInfo* LightSceneInfo = Proxy->GetLightSceneInfo();
	if (LightSceneInfo && LightSceneInfo->bVisible)
	{
		const XMFLOAT3 NewColor = Light->GetColoredLightBrightness();

		Proxy->SetColor(NewColor);
		Proxy->m_IndirectLightingScale = Light->GetIndirectLightingIntensity();
		Proxy->m_VolumetricScatteringIntensity = fmaxf(Light->GetVolumetricScatteringIntensity(), 0.0f);

		// FLightSceneInfoCompact の色も更新する
		if (LightSceneInfo->Id != -1)
		{
			m_Lights[LightSceneInfo->Id].Color = NewColor;
		}
	}
}

void FScene::RemoveLightFromDirtyLists(ULightComponent* Light)
{
	m_LightRenderStateDirtyList.erase(
		std::remove(m_LightRenderStateDirtyList.begin(), m_LightRenderStateDirtyList.end(), Light),
		m_LightRenderStateDirtyList.end());
	m_LightTransformDirtyList.erase(
		std::remove(m_LightTransformDirtyList.begin(), m_LightTransformDirtyList.end(), Light),
		m_LightTransformDirtyList.end());
}

void FScene::UpdateAllLightSceneInfos()
{
	// 処理中のエンキューに備えてローカルへ swap してから処理する
	// (UpdateAllPrimitiveSceneInfos と同じ理由)
	std::vector<ULightComponent*> renderStateDirty;
	renderStateDirty.swap(m_LightRenderStateDirtyList);
	std::vector<ULightComponent*> transformDirty;
	transformDirty.swap(m_LightTransformDirtyList);

	// ---- レンダーステートダーティ ----
	// プロパティ変更 -> プロキシを作り直す (RecreateRenderState_Concurrent)。
	// シーンへの出し入れ (bAffectsWorld / 可視性 / Intensity の 0 跨ぎ) もここで決まる。
	// 空いた番号はすぐ再利用されるので、作り直しても FScene::Lights 内の位置は変わらない
	for (ULightComponent* component : renderStateDirty)
	{
		component->ClearRenderStateDirty();
		component->RecreateRenderState();
	}

	// ---- トランスフォームダーティ ----
	// 移動したライトだけトランスフォームを送る (作り直したライトは最新を持つので何も変わらない)
	for (ULightComponent* component : transformDirty)
	{
		component->ClearRenderTransformDirty();
		component->SendRenderTransform();
	}
}

// ============================================================
//  Exponential Height Fog (FScene::AddExponentialHeightFog /
//  RemoveExponentialHeightFog)
//  プロキシは持たず、コンポーネントの値スナップショット
//  (FExponentialHeightFogSceneInfo) を登録順に保持する。
//  プロパティ / トランスフォーム変更はダーティリスト経由で
//  その場で再スナップショットする (順序不変)。
// ============================================================

void FScene::AddExponentialHeightFog(UExponentialHeightFogComponent* FogComponent)
{
	if (FogComponent == nullptr) return;

	for (const auto& info : m_ExponentialFogs)
	{
		if (info.Component == FogComponent) return;
	}

	m_ExponentialFogs.push_back(FExponentialHeightFogSceneInfo(FogComponent));
	FogComponent->ClearRenderStateDirty();	// 生成直後は最新スナップショット
}

void FScene::RemoveExponentialHeightFog(UExponentialHeightFogComponent* FogComponent)
{
	m_ExponentialFogs.erase(
		std::remove_if(m_ExponentialFogs.begin(), m_ExponentialFogs.end(),
			[FogComponent](const FExponentialHeightFogSceneInfo& info) { return info.Component == FogComponent; }),
		m_ExponentialFogs.end());

	// ダーティリストからも除去する (ダングリングポインタ防止)
	m_FogRenderStateDirtyList.erase(
		std::remove(m_FogRenderStateDirtyList.begin(), m_FogRenderStateDirtyList.end(), FogComponent),
		m_FogRenderStateDirtyList.end());

	FogComponent->ClearRenderStateDirty();
}

void FScene::UpdateAllExponentialHeightFogSceneInfos()
{
	std::vector<UExponentialHeightFogComponent*> renderStateDirty;
	renderStateDirty.swap(m_FogRenderStateDirtyList);

	for (UExponentialHeightFogComponent* component : renderStateDirty)
	{
		auto it = std::find_if(m_ExponentialFogs.begin(), m_ExponentialFogs.end(),
			[component](const FExponentialHeightFogSceneInfo& info) { return info.Component == component; });
		if (it == m_ExponentialFogs.end())
		{
			component->ClearRenderStateDirty();
			continue;
		}

		// その場で再スナップショット (登録順 = 優先度は不変)
		*it = FExponentialHeightFogSceneInfo(component);
		component->ClearRenderStateDirty();
	}
}

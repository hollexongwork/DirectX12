#pragma once

// ============================================================
//  UActorComponent
// の UActorComponent に相当する最小実装。
//  AActor が所有し、UWorld への登録 (Register) を通じて
//  BeginPlay / TickComponent / EndPlay のライフサイクルを受け取る。
// ============================================================

class AActor;
class UWorld;

class UActorComponent
{
protected:
	AActor* m_Owner = nullptr;
	UWorld* m_World = nullptr;
	bool    m_Registered = false;
	bool    m_HasBegunPlay = false;

public:
	// PrimaryComponentTick.bCanEverTick 相当
	bool bCanEverTick = true;

	UActorComponent() = default;
	virtual ~UActorComponent() = default;

	// ---- ライフサイクル (派生側でオーバーライド) ----
	virtual void BeginPlay() {}
	virtual void TickComponent(float DeltaTime) {}
	virtual void EndPlay() {}

	// ---- 登録フック (OnRegister / OnUnregister) ----
	virtual void OnRegister() {}
	virtual void OnUnregister() {}

	// ---- 登録処理 (AActor::RegisterAllComponents から呼ばれる) ----
	void RegisterComponent(UWorld* World)
	{
		if (m_Registered) return;
		m_World = World;
		m_Registered = true;
		OnRegister();
	}

	void UnregisterComponent()
	{
		if (!m_Registered) return;
		OnUnregister();
		m_Registered = false;
	}

	void DispatchBeginPlay()
	{
		if (m_HasBegunPlay) return;
		m_HasBegunPlay = true;
		BeginPlay();
	}

	// BeginPlay 済みの場合のみ EndPlay を1回だけ呼ぶ
	// (AActor::DispatchEndPlay から呼ばれる)
	void DispatchEndPlay()
	{
		if (!m_HasBegunPlay) return;
		m_HasBegunPlay = false;
		EndPlay();
	}

	// ---- アクセサ ----
	void    SetOwner(AActor* Owner) { m_Owner = Owner; }
	AActor* GetOwner() const { return m_Owner; }
	UWorld* GetWorld() const { return m_World; }
	bool    IsRegistered() const { return m_Registered; }
};

#include "Main.h"
#include "RenderManager.h"
#include "CameraComponent.h"
#include "SceneView.h"
#include "World.h"

void UCameraComponent::OnRegister()
{
	// 最初に登録されたカメラを自動的にアクティブビューにする
	if (GetWorld() && GetWorld()->GetScene()->GetActiveCamera() == nullptr)
	{
		GetWorld()->GetScene()->SetActiveCamera(this);
	}
}

void UCameraComponent::OnUnregister()
{
	if (GetWorld() && GetWorld()->GetScene()->GetActiveCamera() == this)
	{
		GetWorld()->GetScene()->SetActiveCamera(nullptr);
	}
}

void UCameraComponent::GetSceneView(FSceneView& OutView, float AspectRatio) const
{
	XMMATRIX world = GetComponentToWorld();

	XMVECTOR position = world.r[3];
	XMVECTOR forward = XMVector3Normalize(world.r[2]);
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX View = XMMatrixLookToLH(position, forward, up);
	XMMATRIX Projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(m_FOV), AspectRatio, m_NearClip, m_FarClip);

	// 行列は転置前で渡す (VIEW 定数への転置はレンダラ側で行う)
	XMStoreFloat4x4(&OutView.ViewMatrix, View);
	XMStoreFloat4x4(&OutView.ProjectionMatrix, Projection);

	XMStoreFloat3(&OutView.ViewOrigin, position);
	XMStoreFloat3(&OutView.ViewForward, forward);
	OutView.FOV = m_FOV;
	OutView.NearClip = m_NearClip;
	OutView.FarClip = m_FarClip;
	OutView.AspectRatio = AspectRatio;

	OutView.bValid = true;
}

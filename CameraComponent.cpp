#include "Main.h"
#include "RenderManager.h"
#include "CameraComponent.h"
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

void UCameraComponent::GetViewConstants(VIEW_CONSTANT& OutConstant, float AspectRatio) const
{
	XMMATRIX world = GetComponentToWorld();

	XMVECTOR position = world.r[3];
	XMVECTOR forward = XMVector3Normalize(world.r[2]);
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX View = XMMatrixLookToLH(position, forward, up);
	XMMATRIX Projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(m_FOV), AspectRatio, m_NearClip, m_FarClip);

	XMMATRIX viewProjection = View * Projection;
	XMMATRIX invViewProjection = XMMatrixInverse(nullptr, viewProjection);

	XMStoreFloat4x4(&OutConstant.View, XMMatrixTranspose(View));
	XMStoreFloat4x4(&OutConstant.Projection, XMMatrixTranspose(Projection));
	XMStoreFloat4x4(&OutConstant.InvViewProjection, XMMatrixTranspose(invViewProjection));

	XMFLOAT3 pos;
	XMStoreFloat3(&pos, position);
	OutConstant.WorldCameraOrigin = { pos.x, pos.y, pos.z, 1.0f };
	OutConstant.NearFar = { m_NearClip, m_FarClip, 0.0f, 0.0f };
}

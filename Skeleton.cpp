#include "Main.h"
#include "Skeleton.h"

bool USkeleton::IsCompatible(const USkeleton* Other) const
{
	if (Other == nullptr) return false;
	if (Other == this) return true;
	return m_ReferenceSkeleton.IsSameSkeleton(Other->m_ReferenceSkeleton);
}

bool USkeleton::IsCompatible(const FReferenceSkeleton& Other) const
{
	return m_ReferenceSkeleton.IsSameSkeleton(Other);
}

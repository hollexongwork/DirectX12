#include "Main.h"
#include "Function.h"

XMFLOAT3 Function::RotateAround(const XMFLOAT3& center, const XMFLOAT3& rotate, const float& len)
{
	XMFLOAT3 position;
	position.x = center.x + sinf(rotate.y) * cosf(rotate.x) * len;
	position.y = center.y - sinf(rotate.x) * len;
	position.z = center.z + cosf(rotate.y) * cosf(rotate.x) * len; 
	//position.z = center.z - cosf(rotate.y) * cosf(rotate.x) * len; 
	return position;
}


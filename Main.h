#pragma once

#define WIN32_LEAN_AND_MEAN 
#define _CRT_SECURE_NO_WARNINGS

#include <fstream>
#include <memory>

#include <vector>
#include <list>
#include <unordered_map>
#include <algorithm>

#include <string>
#include <cassert>

#include <typeinfo>

#include <Windows.h>
#include <mmsystem.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
using namespace Microsoft::WRL;


#include <DirectXMath.h>
using namespace DirectX;


#pragma comment( lib, "winmm.lib" )
#pragma comment( lib, "d3d12.lib" )
#pragma comment( lib, "dxgi.lib" )




HWND GetWindow();

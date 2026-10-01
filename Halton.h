#pragma once
#include <cstdint>

// ============================================================
//  Halton
//  UE の Halton (Math/Halton.h) 相当: 基数 Base の Radical Inverse
//  (Van der Corput 列) の Index 番目の値を [0, 1) で返す。
//  Index = 0 は 0。低食い違い列としてジッタ位相に使う:
//    - Volumetric Fog のセル内ジッタ (Halton 2/3/5, 8 フレーム周期)
//    - Lumen スクリーンプローブの配置ジッタ (Halton 2/3, 16 フレーム周期)
//    - TAA のサブピクセルジッタ (Halton 2/3)
//  ※ 浮動小数の評価順は置換前の VolumetricFog / Lumen の実装と同一
//    (Fraction を毎桁 Base で除算)。UE の Fraction *= InvBase とは
//    Base 3/5 で最下位 1 ULP 異なり得るが、数式としては同一。
//    既存出力とのビット一致を優先してこちらを採る。
// ============================================================
inline float Halton(uint32_t Index, uint32_t Base)
{
	float Result = 0.0f;
	float Fraction = 1.0f / (float)Base;
	while (Index > 0)
	{
		Result += Fraction * (float)(Index % Base);
		Index /= Base;
		Fraction /= (float)Base;
	}
	return Result;
}

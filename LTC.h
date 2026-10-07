#pragma once

// ============================================================
//  LTC テーブル (LTC.h / LTC.cpp)
//  レクトライトのスペキュラ (RectGGXApproxLTC) が引く
//  Linearly Transformed Cosines の当てはめ結果 (GGX)。
//  [Heitz et al. 2016, "Real-Time Polygonal-Light Shading with
//   Linearly Transformed Cosines"]
//
//  LTC.cpp は Tools/LTCFit/LTCFit.cpp が生成する (手で編集しない)。
//  GGX のローブを 64 x 64 (ラフネス x 視線角) で当てはめたもの。
//
//  レイアウト (LTC_Size x LTC_Size。添字 = y * LTC_Size + x):
//    x = ラフネス           (x / (LTC_Size - 1))
//    y = sqrt(1 - NoV)      (y / (LTC_Size - 1))
//    LTCMat : LTC の逆行列を中央要素で正規化した 4 成分 (r0c0, r2c0, r0c2, r2c2)
//    LTCAmp : (大きさ = F0 が 1 のときの方向アルベド, フレネル項)
//  FSystemTextures が 2 枚のテクスチャ (t37 / t38) にして、シェーダは
//  UV = (Roughness, sqrt(1 - NoV)) * 63/64 + 0.5/64 で線形サンプルする。
// ============================================================

static const int LTC_Size = 64;

extern const float LTCMat[LTC_Size * LTC_Size][4];
extern const float LTCAmp[LTC_Size * LTC_Size][2];

#include "TemporalAACommon.hlsl"
#include "VelocityCommon.hlsl"

// =============================================================
//  TemporalAASelfTest_CS (§6.9 GPU ヘルパーパリティ)
//  TAA CS と同じヘルパー (TemporalAACommon.hlsl / VelocityCommon.hlsl) を GPU で評価し、
//  64 個の float を u0 へ書く。C++ (TemporalAASelfTest.cpp) が読み戻して期待値と比べる
//  (許容誤差 1e-5、rcp に依存する項目 (†) は 2e-3)。
//  入力には実行時定数 One (= 1) / Zero (= 0) を掛けて fxc の定数畳み込みを防ぐ
//  (ヘルパーを実際に GPU で計算させる。C++ がルート CBV スロット 2 に書く)。
//  1 スレッドグループ 1 スレッド。未使用の添字 (39..63) は 0。
// =============================================================

cbuffer TemporalAASelfTestParameters : register(b0)
{
    float One;      // 1.0
    float Zero;     // 0.0
    uint  ZeroU;    // 0
    uint  Pad;
};

RWStructuredBuffer<float> Out : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    [loop] for (uint k = 0; k < 64; ++k)
    {
        Out[k] = Zero;
    }

    // 0,1: EncodeVelocityToTexture(-0.01357995, 0) -> 0.49660417, 0.49999237
    const float2 e0 = EncodeVelocityToTexture(float2(-0.01357995f, 0.0f) * One);
    Out[0] = e0.x;
    Out[1] = e0.y;
    // 2,3: DecodeVelocityFromTexture((32545, 32767) / 65535) -> -0.0135772, 0
    const float2 d0 = DecodeVelocityFromTexture(float2(32545.0f, 32767.0f) * One / 65535.0f);
    Out[2] = d0.x;
    Out[3] = d0.y;
    // 4: EncodeVelocityToTexture(3, 0).x (±2 クランプ) -> 0.99899237
    Out[4] = EncodeVelocityToTexture(float2(3.0f, 0.0f) * One).x;
    // 5, 6: ComputeSampleWeigth
    Out[5] = ComputeSampleWeigth(float2(0.5f, 0.0f) * One, 1.0f * One);
    Out[6] = ComputeSampleWeigth(float2(0.0f, -0.0833333f) * One, 2.0f * One);
    // 7-9: RGBToYCoCg(1, 0.5, 0.25) -> 2.25, 1.5, -0.25
    const float3 ycc = RGBToYCoCg(float3(1.0f, 0.5f, 0.25f) * One);
    Out[7] = ycc.x;
    Out[8] = ycc.y;
    Out[9] = ycc.z;
    // 10-12: YCoCgToRGB(2.25, 1.5, -0.25) -> 1, 0.5, 0.25
    const float3 rgb = YCoCgToRGB(float3(2.25f, 1.5f, -0.25f) * One);
    Out[10] = rgb.x;
    Out[11] = rgb.y;
    Out[12] = rgb.z;
    // 13 (†): HdrWeightY(2.25, 1) -> 0.16
    Out[13] = HdrWeightY(2.25f * One, 1.0f * One);
    // 14,15 (†): WeightedLerpFactors(1/6, 0.1, 0.04) -> 0.9756098, 0.0243902
    const float2 wl = WeightedLerpFactors((1.0f / 6.0f) * One, 0.1f * One, 0.04f * One);
    Out[14] = wl.x;
    Out[15] = wl.y;
    // 16-18: ViewZToDeviceZ (標準 Z, 遠方規則): z = 10 (†) / 500 (遠方 = Q) / 499 (†)
    const float4 DepthParams = float4(1.00020004f, -0.100020004f, 499.5f, 0.0f) * One;
    Out[16] = ViewZToDeviceZ(10.0f * One, DepthParams);
    Out[17] = ViewZToDeviceZ(500.0f * One, DepthParams);
    Out[18] = ViewZToDeviceZ(499.0f * One, DepthParams);
    // 19-27: SelectClosestDepthCross (標準 Z = min) -> (Offset.x, Offset.y, ClosestZ)
    int2 o;
    float z;
    SelectClosestDepthCross(7.0f * One, float4(5.0f, 3.0f, 4.0f, 6.0f) * One, 2 + (int)ZeroU, o, z);
    Out[19] = (float)o.x;
    Out[20] = (float)o.y;
    Out[21] = z;
    SelectClosestDepthCross(7.0f * One, float4(9.0f, 2.0f, 8.0f, 1.0f) * One, 2 + (int)ZeroU, o, z);
    Out[22] = (float)o.x;
    Out[23] = (float)o.y;
    Out[24] = z;
    SelectClosestDepthCross(7.0f * One, float4(9.0f, 9.0f, 9.0f, 9.0f) * One, 2 + (int)ZeroU, o, z);
    Out[25] = (float)o.x;
    Out[26] = (float)o.y;
    Out[27] = z;
    // 28,29: CatmullRom5Taps((10.75, 10.75) / 100, (100, 100, 0.01, 0.01)) -> TapW[2], TapUV[2].x * 100 - 10.5
    float2 TapUV[5];
    float TapW[5];
    CatmullRom5Taps(float2(10.75f, 10.75f) * One / 100.0f, float4(100.0f, 100.0f, 0.01f, 0.01f) * One, TapUV, TapW);
    Out[28] = TapW[2];
    Out[29] = TapUV[2].x * 100.0f - 10.5f;
    // 30,31: MitchellNetravali(0.5), (1.5) -> 0.5347222, -0.0347222
    Out[30] = MitchellNetravali(0.5f * One);
    Out[31] = MitchellNetravali(1.5f * One);
    // 32,33: QuantizeForFloatRenderTarget(1, E = 0.5, 1/64 | 1/1024) -> 1.0078125, 1.00048828
    Out[32] = QuantizeForFloatRenderTarget(float3(1.0f, 1.0f, 1.0f) * One, 0.5f * One, float3(1.0f, 1.0f, 1.0f) * (One / 64.0f)).x;
    Out[33] = QuantizeForFloatRenderTarget(float3(1.0f, 1.0f, 1.0f) * One, 0.5f * One, float3(1.0f, 1.0f, 1.0f) * (One / 1024.0f)).x;
    // 34,35: Hammersley16(0, 1, Rand3DPCG16(p).xy).x -> 0.10681152 (0,0,0) / 0.78860474 (10,20,3)
    Out[34] = Hammersley16(ZeroU, 1u + ZeroU, Rand3DPCG16(int3(0, 0, 0) + (int)ZeroU).xy).x;
    Out[35] = Hammersley16(ZeroU, 1u + ZeroU, Rand3DPCG16(int3(10, 20, 3) + (int)ZeroU).xy).x;
    // 36: InterleavedGradientNoise((1, 0), 0) -> 0.5557134
    Out[36] = InterleavedGradientNoise(float2(1.0f, 0.0f) * One, Zero);
    // 37,38: IsVelocityWritten (0 = 未書き込みの番兵)
    Out[37] = IsVelocityWritten(float2(0.0f, 0.0f) * One) ? 1.0f : 0.0f;
    Out[38] = IsVelocityWritten(float2(1.0f / 65535.0f, 0.0f) * One) ? 1.0f : 0.0f;
}

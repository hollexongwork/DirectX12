# Final Design: UE5 Gen4 TAAU ("TemporalAA.usf" with upsampling) for the DX12 renderer — complete version

**Status:** FINAL. This is the normative specification. Implementers read only this document plus the source tree.

**Baseline:** repository `D:\Work\DX12\DirectX12`, branch `TAA`, HEAD `444dee0`. File:line references are to that tree.

**How this document was produced:**
- Three candidate designs were judged: A-fidelity, B-integration and C-correctness. The combined judge scores were C 100.5, A 98 and B 89.
- **Base: design C.** It has the highest combined score and supplies the normative conventions, the derivations, the self-tests and the frame model.
- The grafts that both judges listed from A and B are merged in. §0.3 lists them.
- Every error a judge reported was checked against the source or the UE reference. §0.4 records the verdict and the action for each one, including the cases where a judge was wrong.
- There are no open questions. Every item that was low-confidence or undecided in the candidates is resolved in Appendix C.

**Tags used in this document:**

| Tag | Meaning |
|---|---|
| **[H] / [M] / [L]** | Confidence that the item matches UE, carried over from `ue5_taau_reference.md`, or from our own recollection of UE 4.26–5.x source where noted. |
| **[PORT]** | A deliberate deviation from UE, with the reason given. |
| **[FIX]** | Fixes a pre-existing engine bug that the feature depends on. |
| **[GATE]** | The behaviour is switched on by a stage gate (§2.4, `TAAStageGates`). |

**Code style for implementers:**
- Comments are in **Japanese**. Code snippets in this document already use Japanese comments where a comment is part of the specification.
- Use UE naming: `F*` for render structs and classes, `E*` for enums, `U*`/`A*` for game objects.
- Settings follow the Params struct → ImGui → SettingsManager INI pattern.
- All new sources are UTF-8. The project already passes `/utf-8`.
- New `.hlsl` files need `vcxproj` FxCompile entries for **both** Debug|x64 and Release|x64, plus a `.filters` entry. Headers get `ExcludedFromBuild`.

---

## 0. Decisions

### 0.1 Key decisions

1. **Exact-size render targets [PORT].**
   - Every render-resolution texture is allocated at exactly the render extent `R`, and every viewport starts at (0,0). UE instead renders into a view rect inside a larger buffer.
   - Every existing screen shader samples by viewport UV and loads by `SV_Position`, so all of them stay valid without code changes.
   - When `R` changes, targets are reallocated at the top of `BeginFrame` (§5).
2. **Five extents.**

   | Symbol | Meaning (UE name) | Size |
   |---|---|---|
   | `O` | Output: back buffer (UnscaledViewRect / SecondaryViewRect) | fixed 1920×1080 |
   | `R` | Render (ViewRect) | `ceil(O·f)` per axis. `f = ScreenPercentage/100`, clamped per method (§4.2). |
   | `S` | "Secondary" rect of the TAA pass (UE local `SecondaryViewRect`) | `O` for TemporalUpscale, `R` for Main |
   | `H` | TAA output = history written this frame | `S`, or `trunc(S·HistoryUpscaleFactor)` for MainSuperSampling |
   | `P` | Post-process extent (AutoExposure, Bloom, Tonemap) | `S` when TAA runs (after the Mitchell-Netravali downsample in SuperSampling). `R` when AA is off. |

   `Hp` is the extent of the history read this frame, i.e. last frame's `H`.
3. **UE framework shape, one-to-one where the engine allows it:**
   - `FViewMatrices` holds the jittered and NoAA matrices and provides `HackAddTemporalAAProjectionJitter`.
   - `FViewInfo` is rebuilt every frame.
   - `FSceneViewState` persists across frames. It holds the sample index, the frame index, `PrevFrameViewInfo` and the history pool.
   - `FPreviousViewInfo` and `FTemporalAAHistory` follow UE.
   - `FSceneVelocityData` lives in `FScene` and is keyed by component.
   - `ITemporalUpscaler` / `FDefaultTemporalUpscaler::AddPasses` / `AddTemporalAAPass` / `ComputeMitchellNetravaliDownsample` follow UE.
   - `FViewFamilyInfo` / `ComputeViewFamilyInfo` stands in for `FLegacyScreenPercentageDriver` + `PrepareViewRectsForRendering`.
4. **The TAA is a compute pass** (Gen4 `FTAAStandaloneCS`).
   - It uses 8×8 thread groups and its own compute root signature.
   - There are **11 permutation wrappers**, plus a Mitchell-Netravali CS and a GPU self-test CS.
   - The graphics root signature gains only two pixel-visible SRV slots: **t35 `VELOCITY`** and **t36 `TEMPORAL_AA_DEBUG`**.
5. **Jitter goes into `b0.Projection` and `b0.InvViewProjection`**, in `_31/_32` of the untransposed matrix. Rasterization and every depth-based world reconstruction stay exactly consistent.
   - The NoAA matrices are used for culling, `ClipToPrevClip` and the volumetric-fog history.
   - The **jittered** previous VP is used for Lumen, because Lumen's `PrevSceneColor`/`PrevLinearDepth` are jittered rasters.
   - The velocity pass also uses the jittered previous VP. It un-jitters in the PS with `TemporalAAJitter.xy/.zw`, which is the UE formulation.
6. **Ordering-hazard fix.**
   - `CopySceneColorHistory` only copies textures; it no longer writes matrices.
   - During frame N every consumer reads the immutable snapshot `m_ViewInfo.PrevViewInfo` (frame N−1).
   - `CommitViewState()` runs as the **last step of `RenderPostProcessing`**. It writes `m_ViewState.PrevFrameViewInfo`: matrices, TAA history and `ViewRectSize`.
7. **Velocity.**
   - It is written by a separate pass after the base pass: `FSceneRenderer::RenderVelocities`, the UE4 default, equivalent to UE5 `r.VelocityOutputPass=2`.
   - Only primitives whose `LocalToWorld` differs from last frame's draw. Masked materials clip.
   - The UE small-object skip (`MotionBlurPerObjectSize = 0.5`) is applied.
   - The target is `R16G16_UNORM` with the Gen4 encoding. An encoded value of 0 is the "not written" sentinel. The encoder clamps to ±2 **[PORT]**.
   - The sky dome follows the camera but writes **no** velocity (`UPrimitiveComponent::SetRenderVelocity(false)`, like UE's sky). Its motion is translation with the camera, so the far-pixel rule (`d = Q`, rotation-only reprojection) reconstructs it exactly. Writing velocity for it would set the dynamic anti-ghost alpha on the whole sky (final review).
8. **Depth-test invariance.**
   - GeometryVS and VelocityVS both compute `SV_Position` through one shared helper marked `precise`, which is UE's `INVARIANT`.
   - The documented fallback, used only if the S4 coverage check fails, is `DepthBias = −4` on the `Velocity*` PSOs.
9. **TAA depth input.**
   - The TAA reads opaque `LinearDepth.R` (view Z) and converts it to device Z with `d = Q − Q·n/z`. After translucency the scene depth buffer holds translucent depth; `LinearDepth` does not.
   - In standard Z, "closest" means **min**.
   - A **far pixel** (closest view Z ≥ 0.999·Far) is reprojected as a point at infinity by substituting `d = Q = f/(f−n)` into `ClipToPrevClip`. That gives rotation-only reprojection, which is UE's infinite-far behaviour. It needs no extra matrix (Appendix A.5).
10. **Responsive AA without stencil [PORT].**
    - A render-resolution `R8_UNORM` mask is drawn at the end of `RenderTranslucency` for translucent subsets whose material has `bEnableResponsiveAA`.
    - The TAA reads the mask at the input pixel.
11. **HDR weighting** is `w = 1/(Y·E + 4)`, with `Y = R + 2G + B`.
    - `E` is the **previous frame's** exposure: the AutoExposure result buffer when `PP_FLAG_AUTO_EXPOSURE` is set and the buffer is initialised, otherwise `PostProcess.Exposure`.
    - There is no pre-exposure, so `HistoryPreExposureCorrection = 1`.
12. **Post order (UE):**
    1. `CopySceneColorHistory` (Lumen)
    2. DOF at `R`
    3. TAA `R`→`H`
    4. [Mitchell-Netravali `H`→`S`]
    5. AutoExposure (on `P`, or on the half-res TAA output)
    6. Bloom (chain fixed at `O/2`; the threshold pass box-prefilters the input)
    7. LUT
    8. Tonemap at `P`
    9. [primary spatial upscale `P`→`O`, or tonemap merged with upscale]
    10. [debug visualize]
    11. screenshot
    12. `CommitViewState`
    13. ImGui

    **Every size after the TAA is taken from the actual texture the TAA produced**, not from a planned value. A missing-PSO fallback therefore still upscales correctly.
13. **Automatic view mip bias** is applied with `SampleBias(Sampler, uv, MaterialTextureMipBias)` at the six material sampling sites and in `VelocityMaskedPS`.
    - It is non-zero only under TemporalUpscale.
    - Shadow and Lumen card views upload a zero-initialised `VIEW_CONSTANT{}`, so they get bias 0 automatically, as in UE.
14. **`r.TemporalAA.HistoryScreenPercentage` applies to both Main and TAAU [H].** This is UE 4.26 `FDefaultTemporalUpscaler::AddPasses` behaviour. MainSuperSampling is followed by a Mitchell-Netravali downsample back to `S`.
15. **Stochastic quantization matches UE Gen4.**
    - The noise is `E = Hammersley16(0, 1, Rand3DPCG16(PixelPos, FrameMod8)).x ∈ [0,1)`.
    - The error is the destination format's ULP, floored to a power of two (`QuantizeForFloatRenderTarget`).
    - It applies to every history format: FP16 (2⁻¹⁰) and R11G11B10 (2⁻⁶, 2⁻⁶, 2⁻⁵).
16. **Jitter follows UE 4.26/5.x `PrepareViewStateForVisibility` [M]** (§4.4).
    - Under TAAU the sample count is multiplied by `1/f²` and **truncated to int**. Outside TAAU, `r.TemporalAASamples=5` gives `N = 4`.
    - The TemporalUpscale branch (uniform Halton) is tested **before** the 2/3/4/5 pattern switch, so the fixed patterns are never used under TAAU.
    - The switch is keyed on the **CVar value**, as in UE 4.26 (`CVarTemporalAASamplesValue == 2..5`). Outside TAAU this equals `N`, except that CVar 5 selects the compressed-plus pattern with `N = 4`. Each table is indexed with `Index % len` as a guard.
    - The index resets on a camera cut.
17. **Debug #615 is fixed in S0 [FIX].** Full-screen PSOs get `EDepthStatePreset::None` (`DepthEnable=FALSE`, `DSVFormat=UNKNOWN`). Debug-only environment switches `DX12_DEBUG_NO_BREAK` and `DX12_DEBUG_GBV` make the debug layer usable as an instrument.
18. **Resolution changes** (§5): `FlushAndResetCommandList` → release (deferred queue) → `WaitGPU` → create.
    - The first step executes any command recorded during construction that still references the old resources.
    - The second wait frees the old memory before the new allocation, which caps peak VRAM.
    - The TAA history is **not** invalidated. It is resampled in screen-UV space, as in UE.
19. **LightGrid is allocated once at capacity** `ceil(2·O/64)` cells. `SetViewSize(W,H)` sets the dimensions each frame, so there is no LightGrid reallocation path.
20. **DOF radius is invariant to screen percentage.** `MaxBlurSize` is multiplied by `R.x/O.x` while DOF runs (the DOF radius is in half-res texels, `DOFBlurPS.hlsl:47`).
21. **Stage gates [GATE].** Compile-time constants (`namespace TAAStageGates` in `ScreenPercentage.h`) switch features on stage by stage. Code defaults stay final from S1, so nothing is ever persisted to the INI with a temporary value. Before S5 the default image equals the baseline.
22. **Verification harness:**
    - a command-line test driver (`-taatest`: fixed dt, scripted motion, BMP capture, no INI writes, exit code);
    - CPU self-tests plus GPU HLSL-helper parity (`-taaselftest`);
    - 13 debug views, each with a stated failure signature;
    - a DBWIN debug-layer message diff and GBV runs;
    - `.cso` hash rules plus an `fxc /dumpbin` instruction diff;
    - leak and VRAM counters;
    - PSNR against a 4-spp SSAA reference.

### 0.2 Normative conventions (all shaders and C++ must follow these)

| Item | Definition |
|---|---|
| Matrix math | DirectXMath row vectors: `clip = v · M`. C++ keeps matrices **untransposed** in `FViewMatrices`/`FViewInfo` and uploads `XMMatrixTranspose(M)`. HLSL reads that with the default column_major packing, so HLSL sees `M` and computes `mul(v, M)`. `XMFLOAT4X4::_31` (row 2, col 0) in C++ is `M._31` in HLSL. |
| Projection | `XMMatrixPerspectiveFovLH(fovY, aspect = O.x/O.y, n, f)`, **standard Z**: `z_ndc = Q·(1 − n/z_view)` with `Q = f/(f−n)` and `clip.w = z_view`. Depth is cleared to 1.0 and tested with LESS_EQUAL. `DepthClipEnable=FALSE`, so geometry beyond far is clamped to depth 1. A point at view-Z = ∞ has `z_ndc = Q` (> 1). |
| Pixel index | `p = (x, y)`, origin at the top left, +y down. The pixel centre is `p + 0.5`. |
| ViewportUV | `u = (p + 0.5) / E` for extent `E`. |
| ScreenPos (UE) = NDC.xy | `s = (2u.x − 1, 1 − 2u.y)`, +y up. Inverse: `u = (0.5·s.x + 0.5, 0.5 − 0.5·s.y)`. |
| Displacement | `Δpx = (Δs.x·E.x/2, −Δs.y·E.y/2)` |
| Jitter | `J_px = (sx, sy)` in **render** pixels, +y down (UE `TemporalJitterPixels`). `J_ndc = (2·sx/R.x, −2·sy/R.y)` (UE `TemporalAAProjectionJitter`) is added to `Projection._31/_32`. **A scene point whose unjittered render-pixel position is `q` appears at `q + J_px` in the jittered render** (A.2). |
| Velocity | `V = s_cur(unjittered) − s_prev(unjittered)` of the same surface point, in ScreenPos units. The history position is `HSP = s − V`. |
| Closer | smaller device Z, which is equivalent to smaller view Z. |
| Device Z from view Z | `d = Q − Q·n / z_view`, the exact inverse of `LinearDepthPS` (`z = n·f / (f − d·(f − n))`). A far pixel (`z ≥ 0.999·f`) uses `d = Q`. |
| Resource "read state" | `PIXEL_SHADER_RESOURCE \| NON_PIXEL_SHADER_RESOURCE`, abbreviated **RD**. `PSR` = `PIXEL_SHADER_RESOURCE` only. `NPSR` = `NON_PIXEL_SHADER_RESOURCE` only. |

### 0.3 Grafts merged into the base (C)

| From | Graft | Where |
|---|---|---|
| A | `-taatest` command-line scenario driver: fixed dt, scripted camera and object motion, setting overrides, BMP capture, INI saving disabled, exit code | §9.3, stage ST |
| A | PSNR against an SSAA reference: `-aa=0 -sp=200 -upscaleq=1`, i.e. an exact 2×2 box = 4-spp ordered grid | §9.5 |
| A | `EndFrame` re-binds the back-buffer RTV and the output viewport before ImGui. The GPU self-test request is deferred to the next `BeginFrame`. | §4.1, S0 |
| A | `DX12_DEBUG_NO_BREAK` / `DX12_DEBUG_GBV` environment switches | S0 |
| A | `SetConstant` `assert(Size ≤ 512)` plus a runtime log-and-return guard | S0 |
| A | UE `PrimitiveHasVelocityForView` small-object skip, `kMotionBlurPerObjectSize = 0.5` | §4.5 |
| A | `r.Tonemapper.MergeWithUpscale.Mode/Threshold` | §4.7 |
| A | `HistoryScreenPercentage` applies to Main as well | §4.2 |
| A | UE stochastic quantization (Rand3DPCG16 / Hammersley16, E ∈ [0,1)); C's power-of-two floor is kept | §6.1 |
| A | Camera-cut latch on `UCameraComponent`, controller velocity reset, teleport marking through `Component->GetWorld()` | §4.5, §7.4 |
| A | Split input/output view through a second tonemap draw with a scissor, so it is exact | §6.8 |
| A | `ITemporalUpscaler` / `FDefaultTemporalUpscaler` / `FTAAPassParameters` structure | §2 |
| A | Free-SRV counter, velocity and responsive thumbnails, TemporalUpscaler I/O grid view | §7 |
| B | DOF `MaxBlurSize × R.x/O.x` | §5.3 |
| B | Rotation-only reprojection for far pixels (implemented as `d = Q`, Appendix A.5) | §6.5 |
| B | Register-free `TemporalAACommon.hlsl`. Each CS declares its own cbuffer and resources. | §6.1 |
| B | Post-chain sizes derived from the actual TAA output texture | §4.7 |
| B | History bypass while Lumen/LightGrid debug views are active; an AA-method change is a camera cut | §4.4, §4.8 |
| B | `Velocity*` PSO DepthBias −4 as a documented fallback | §3.8, risk R1 |
| B | `fxc /dumpbin` instruction-stream diff for cbuffer-growth stages | §9.1 |
| B | R11G11B10 `UAV_TYPED_STORE` capability check; barrier de-duplication | §4.8 |
| B | Dark-launch intent: the default image equals the baseline until TAA is implemented. It is realised through gates rather than a temporary default value, see decision 21. | §2.4 |
| C | Everything not listed above: conventions, data model, CommitViewState, self-tests, 13 debug views with failure signatures, `HISTORY_HAS_ALPHA`, ±2 clamp, w ≤ 0 guards, resize ordering, LightGrid capacity | – |

### 0.4 Judge-finding resolution log

Every finding was checked against the source (`D:\Work\DX12\DirectX12`) or, for UE facts, against the reference plus our own recollection of UE source.

| # | Finding (judge → design) | Verdict | Action in this design |
|---|---|---|---|
| 1 | Sample count must truncate, not round (A used `lround`) | **Correct.** UE assigns `float` to `int32`. | `N = (int)(8·max(1, 1/f²))`: 50 % → 32, 67 % → 17, 71 % → 15, 75 % → 14. |
| 2 | 2/3/4/5 tables keyed on the CVar even under TAAU (A); keyed on scaled N, which reaches 5 and indexes out of bounds (B) | Both are real defects. A's defect is the branch **order**, not the key: UE 4.26 keys the switch on `CVarTemporalAASamplesValue`, but only after the TemporalUpscale branch. | UE order is reproduced: `if (N == 1) 0 [PORT]; else if (TemporalUpscale) uniform Halton; else switch (CVar) {2,3,4,5 tables with Index % len}; else Gaussian`. The tables are unreachable under TAAU (see #3). Keying on the CVar keeps pattern 5 (compressed plus) reachable, which N-keying would not (B's own open question). |
| 3 | "UE uses the fixed tables under TAAU at 100 %" (judge on C) | **Judge incorrect [M].** In UE 4.26 `PreVisibilityFrameSetup` and UE5 `PrepareViewStateForVisibility`, the `TemporalUpscale` branch comes before the count switch. In 4.26, `MaterialTextureMipBias` is even computed inside that branch. | C's behaviour is kept, and is now UE-faithful rather than a [PORT]. |
| 4 | "UE does not reset the jitter index on camera cut" (judge on A, C) | **Judge incorrect [M].** 4.26/5.x: `if (TemporalSampleIndex >= TemporalAASamples \|\| View.bCameraCut) TemporalSampleIndex = 0;`. The reference statement conflicts with this. | Reset kept. The effect is invisible, because a cut frame discards history anyway. |
| 5 | No history-alpha guard for a Q0 (R11) → Q2 switch (A) | Correct. | `TAA_FLAG_HISTORY_HAS_ALPHA` (§3.4). |
| 6 | `ApplyActor`/`ApplyComponent` are static; `m_World`/`m_CameraActor` do not compile there | **Verified** (SettingsManager.h:297, 299). Also, `ACameraActor` has no public controller accessor (Camera.h). | Use `Component->GetWorld()->GetScene()->MarkPrimitiveTeleported(...)` (`UActorComponent::GetWorld()` exists, ActorComponent.h:72). The camera uses `UCameraComponent::NotifyCameraCut()`; `ACameraActor::Tick` resets its private controller (§4.5, §7.4). |
| 7 | DOF radius changes with the screen percentage | **Verified** (DOFBlurPS.hlsl:47 "ハーフ解像度テクセル単位"). | `MaxBlurSize × R.x/O.x` during DOF (§5.3). |
| 8 | Encoder without a clamp collides with the 0 sentinel (A) | Correct: \|V\| > 2.0038 → UNORM 0. | Clamp to ±2 (§6.1). |
| 9 | S3 "rotate: dome shows rotational motion" (A) | Correct: the sky copies only the camera **location** (Sky.cpp:45-46). | S4 acceptance: under pure yaw **no** velocity is written for the sky. The motion appears only as depth-derived motion in MotionVectors (§8 S4). |
| 10 | A's RHI depth refactor is too invasive | Agreed. | The RHI keeps the depth buffer, plus `ReleaseDepthBuffer`/`CreateDepthBuffer` rewriting the single DSV slot in place (§5.2). |
| 11 | MotionBlurPerObjectSize small-object ghosting (A); "UE default is 0" (C) | C's claim is wrong. UE `FPostProcessSettings::MotionBlurPerObjectSize` defaults to 0.5 [M]. | Ported with 0.5. A non-persisted debug toggle disables it (§4.5). |
| 12 | `ApplyActor` first line `m_World->RequestCameraCut()` does not compile (B) | Verified (static). | See #6. |
| 13 | B: tables keyed on N out of bounds; no index reset | Correct. | See #2, #4. |
| 14 | B: SAMPLE_DISTANCE left as pseudo-code | Correct. | Fully written in §6.5. |
| 15 | B: Main-config history reallocated on SP change | Correct (UE resamples). | The history is kept and resampled through `Hp` (§4.8). |
| 16 | B: global depth-derived responsive mask | Correct. | Per-material `bEnableResponsiveAA` mask pass (§4.6). |
| 17 | B: CPU readback exposure (lag, race) | Correct. | GPU AutoExposure buffer at t4, previous frame, with a dummy on frame 0 (§4.12). |
| 18 | B: bloom fixed at O/2 even when `P` = 2·O | Correct. | Chain fixed at `O/2` (SP-invariant halo, like DOF item 7); the threshold pass box-prefilters the input so `P` = 2·O is still properly decimated (§5.3, final review). |
| 19 | B: #615 not fixed, Debug needs a scratch patch | Correct. | Fixed in S0. |
| 20 | B: non-deterministic CopyFromScreen capture | Correct. | Back-buffer readback capture plus the `-taatest` driver. |
| 21 | B: HSP only under TAAU | Correct (UE applies it to Main as well). | Decision 14. |
| 22 | B: Low quality uses a bilinear history | Low-confidence claim. | `AA_BICUBIC 1` for every quality (reference §4.11 [M]). |
| 23 | B: SAMPLE_DISTANCE threshold saturated | Correct (UE extrapolates). | Unclamped `lerp(1.51, 1.3, UF − 1)`: 1.615 at UF 0.5. |
| 24 | B: `bIsFrozen` does not freeze the index | Correct. | With the override active, the state index does not advance (§4.4). |
| 25 | C: MainSuperSampling only under TAAU | Correct. | Decision 14. |
| 26 | C: centred `E − 0.5` quantization | Correct. | UE E ∈ [0,1) (decision 15). |
| 27 | C: `EncodeTAADebug` elided; FilterCurrentFrame and box in prose | Correct. | Full code in §6.5. |
| 28 | C: scenarios need ImGui/F9, not command-line automatable | Correct. | Driver graft. |
| 29 | C: `TemporalAACommon.hlsl` holds b0 and all resources, clashing with MN/self-test | Correct. | Register-free common header (§6.1). |
| 30 | C: "Run Self Test" button flushes mid-ImGui | Correct. | Deferred to the next `BeginFrame`, plus the EndFrame rebind. |
| 31 | C: `assert(postExtent == P)` breaks the fallback | Correct. | Sizes derived from the actual texture (decision 12). |
| 32 | C: no `SetConstant` size guard | Correct. | S0. |
| 33 | C: staging, R11/downsample flags active before their permutations exist | Correct. | Gates `kTAAExtras` (S7); `ComputeViewFamilyInfo` cannot select them earlier. |
| 34 | A: one dummy used as both SRV and UAV | Correct (formally a state conflict). | A separate permanent-UAV dummy (§3.9). |
| 35 | A: downsample average includes out-of-viewport lanes | Correct. | Validity-weighted average (§6.5). |
| 36 | A: velocity clear (0,0,0,0) vs optimized clear (0,0,0,1) | **Verified** (RenderManager.cpp:966-970 → warning #820). | Clear with `{0,0,0,1}` (RG = 0 is still the sentinel). |
| 37 | A: "byte-identical BMPs across runs" is fragile | Correct (LightGrid/Lumen atomics). | Criteria use a measured noise floor `N0` (§9.5). |
| 38 | A: Stage 0 "Debug build runs 600 frames" may be blocked by other pre-existing errors | Correct. The ColorGradingLUT PSR-read-by-compute bug is detected by GBV on **every** run: the committed `EngineSettings.ini` loads `[ArtistLUT] Path=Asset/Texture/LUTs\Adventure.DDS` (left in PSR by `LoadTexture`), and the 1×1 `m_FallbackTex` is also created in PSR (`ColorGradingLUTBaker.cpp:222`). Both are bound to the bake CS at t0. (Revised; the earlier "only with an artist LUT loaded" understated it.) | Acceptance: "#615 absent". The LUT states are fixed in S0 **[FIX]** (S0 task 10). Any other pre-existing ERROR is recorded in the baseline D0 (plain and GBV, §9.6) and the run switches to `DX12_DEBUG_NO_BREAK`. |
| 39 | New finding: resize at the first `BeginFrame` after init | **Verified hazard.** The constructor records barriers into the open command list (e.g. `FSceneTextures::Init`). C's "release → WaitGPU" would free resources that the still-open list references. | Resize starts with `FlushAndResetCommandList()` (§5.2). |
| 40 | New finding: the test driver must move the camera **before** `World.Tick` | The sky copies the camera location inside `World.Tick` (Sky.cpp:45). | The driver hook runs before `m_World.Tick`, and the camera's own input is disabled in test mode (§9.3). |

---

## 1. Scope

### 1.1 Ported features

| # | UE feature (Gen4 TAAU) | Port | Stage |
|---|---|---|---|
| 1 | Screen-percentage framework: render vs output extent, `r.ScreenPercentage`, `EPrimaryScreenPercentageMethod` (TemporalUpscale / SpatialUpscale, with automatic fallback to Spatial when AA is not temporal), UE TAAU fraction limits 0.5–2.0 [H] | `FViewFamilyInfo` + `ComputeViewFamilyInfo()` (§4.2). TAAU is clamped to [50, 200] %. Spatial is clamped to [10, 200] % **[PORT]**: UE allows 1–400 %, and 400 % at 1080p would need about 6 GB. | S3 |
| 2 | Runtime resolution change | Reallocation at the top of `BeginFrame` (§5). The TAA history is kept and resampled (UE). Lumen and fog histories reset for one frame. | S3 |
| 3 | Halton jitter [H/M]: count ×1/f² (truncated) under TAAU, clamp [1,255]; uniform Halton(2,3) under TAAU; windowed-Gaussian Box-Muller otherwise; 2/3/4/5 patterns keyed on the CVar outside TAAU (CVar 5 → N = 4, compressed plus); index reset on cut; `r.TemporalAA.Debug.OverrideTemporalIndex` freezes the index | Ported (§4.4). One **[PORT]**: `N == 1` gives a zero offset instead of UE's constant Gaussian sample #0, so `-samples=1` compares exactly with AA off. | S2 (sequence), S5 (active), S6 (TAAU scaling) |
| 4 | `FViewMatrices` jittered and NoAA (`HackAddTemporalAAProjectionJitter`, `ProjectionNoAA`, `GetTemporalAAJitter`) | Ported (§2.4). | S1/S2 |
| 5 | `FPreviousViewInfo`, `FSceneViewState::PrevFrameViewInfo`, `View.PrevViewInfo`, `bCameraCut`, `IsLargeCameraMovement` (45°, 10000 cm), `bPrevTransformsReset`, history invalidation | Ported. The commit happens at end of frame. Cut sources: game request, camera-component latch, active-camera change, first frame, `bValid` transition, AA-method change, debug reset. | S1 |
| 6 | Velocity: `FSceneVelocityData`, `PreviousLocalToWorld`, a pass for moving primitives only (masked clip, two-sided), `PrimitiveHasVelocityForView` (cut skip plus small-object skip), R16G16_UNORM Gen4 encoding with the 0 sentinel, sky dome | Ported (§4.5). | S4 |
| 7 | Camera motion from depth via `ClipToPrevClip` (NoAA × NoAA) [H] | Ported. Opaque LinearDepth → device Z **[PORT]**; w ≤ 0 guard **[PORT]**; far pixel at `d = Q` (rotation-only). | S5 |
| 8 | Pass configs Main / MainUpsampling / MainSuperSampling [H]; `r.TemporalAA.HistoryScreenPercentage` 100–200 on both Main and TAAU; Mitchell-Netravali downsample | Ported. The MN kernel is [L]: B = C = 1/3, support of 2 output pixels, separable, normalized. | S5 (Main), S6 |
| 9 | `r.TemporalAA.Quality` 0–3 (`ETAAQuality` Low/Medium/High/MediumHigh) as permutations; SuperSampling forces High | Ported (§6.5 table). | S5/S6 |
| 10 | Closest-depth dilation, X pattern at ±2 input pixels [H] | Ported with **min** (standard Z). | S5 |
| 11 | YCoCg working space | Ported, unnormalized (Y = R+2G+B). | S5 |
| 12 | Neighbourhood boxes: MIN_MAX + AA_ROUND (Main), SAMPLE_DISTANCE (MainUpsampling), VARIANCE 1.25σ (MainSuperSampling) | Ported (§6.5). | S5/S6 |
| 13 | 5-tap Catmull-Rom history with manual UV clamp [H] | Ported. | S5 |
| 14 | HDR weighting with previous-frame exposure | Ported: `1/(Y·E + 4)`. | S5 |
| 15 | BlendFinal chain [H/M] | Ported: FTW·CFW → velocity lerp to 0.2 at 40 → `max(.., 0.01·Lh/\|ΔL\|)` → responsive 0.25 → IgnoreHistory 1. | S5 |
| 16 | Dynamic anti-ghost via history alpha | Ported, plus a `HISTORY_HAS_ALPHA` guard **[PORT]**. | S5 |
| 17 | Responsive AA (`bEnableResponsiveAA`) | A mask texture replaces stencil bit 3 **[PORT]**. | S7 |
| 18 | NaN / negative / 65504 guards | Ported on the output. Input and history are also sanitized **[PORT]**. | S5 |
| 19 | `r.TemporalAA.R11G11B10History` + stochastic quantization | Ported. R11 is allowed for Quality Low/Medium, not SuperSampling, when the UAV typed store is supported **[M]**. | S5 (FP16 QE), S7 (R11) |
| 20 | TAA half-res downsample output (Low quality, `r.TemporalAA.AllowDownsampling`) feeding eye adaptation and bloom | Ported. | S7 |
| 21 | Primary spatial upscale `r.Upscale.Quality` 0–5, `r.Upscale.Softness`; `r.Tonemapper.MergeWithUpscale.Mode/Threshold` | Ported (§6.7, §4.7). Modes 2/4 are [M]; mode 5 is [L]. | S3 |
| 22 | Automatic view mip bias (`r.ViewTextureMipBias.Min/Offset`), TAAU only | Ported via `SampleBias` + b0 **[PORT]**: UE uses the material sampler state. | S2 (plumbing), S6 (active) |
| 23 | Post-chain order: DOF at R before TAA; AE/Bloom/Tonemap after TAA | Ported. | S3/S5 |
| 24 | Unjittered frustum culling | Ported: culling uses `ViewProjectionNoAA`. | S1 |
| 25 | Debug visualizations: VisualizeMotionVectors, VisualizeTemporalUpscaler I/O, AA_DEBUG / DebugOutput, input/output split, `OverrideTemporalIndex` | 13 views (§6.8), thumbnails, stats. | S4–S7 |
| 26 | Every setting with its UE CVar name and default, in ImGui and persisted to INI | Ported (§7). | S1 + per stage |
| 27 | Temporal noise rotation (`View.StateFrameIndexMod8`) for effects TAA must integrate | b0 `StateFrameIndexMod8`. DeferredPS IGN gains a frame term in S8. | S2/S8 |

### 1.2 Explicitly excluded

| UE feature | Why excluded |
|---|---|
| TSR (Gen5), `r.TemporalAA.Algorithm` | Out of scope by request. `r.AntiAliasingMethod=4` maps to 2. |
| Third-party upscaler selection (`r.TemporalAA.Upscaler`) | There are no plugins. The `ITemporalUpscaler` interface exists but has one implementation. |
| DiaphragmDOF / SSR / LightShaft / Hair TAA configs, `RGB_COC` payload | The engine has none of these. Its DOF is a Gaussian DOF without temporal filtering. |
| `RGB_OPACITY` payload (`r.PostProcessing.PropagateAlpha`) | There is no alpha output. Alpha carries the anti-ghost flag. |
| Secondary screen percentage / DPI | The back buffer is fixed and there is no DPI scaling. The secondary fraction is 1. |
| Dynamic resolution | Not requested. The framework accepts any per-frame fraction; only the budget driver is missing. |
| `TAA_SCREEN_PERCENTAGE_RANGE` LDS permutations | Performance only. Inputs are read with `Load`, so results are identical. |
| Pixel-shader TAA path / second stencil-tested `TAA_RESPONSIVE` pass | Compute only. Responsiveness is a runtime mask read. |
| Pre-exposure (`r.UsePreExposure`) | The engine renders absolute HDR. The correction is the constant 1; the hook is `SceneColorPreExposure`. |
| `r.TemporalAAPauseCorrect`, `bStatePrevViewInfoIsReadOnly` | Single view, rendered every frame. `FSceneVelocityData` gives zero motion while paused. |
| Motion blur, velocity flatten | There is no motion blur, so the half-res downsample is always allowed. |
| FXAA / MSAA (`r.AntiAliasingMethod` 1/3) | Not requested. They map to None at INI load, and the UI shows them disabled. |
| `r.Tonemapper.Sharpen` | Optional in UE and not requested. |
| `r.VelocityOutputPass` 0/1, `r.BasePassOutputsVelocity` | They would change the shared `PS_INPUT` and the G-buffer MRT set. Only variant 2 is implemented. |
| Skinned / WPO / `AlwaysHasVelocity` primitives, translucent "Output Velocity" | No such content. The hook `FPrimitiveSceneProxy::AlwaysHasVelocity()` returns false. |
| Orthographic jitter (`M[3][0]`) | No orthographic main view. |
| Window resize / `ResizeBuffers` | The back buffer is fixed. `O` is read every frame, so a future resize path only reallocates the post targets and the history (a size change → reset). |


---

## 2. Architecture

### 2.1 New files

All C++ files go in the repo root, next to the existing sources. All HLSL files go in `Shader/`.

| File | Contents (UE counterpart) | Stage |
|---|---|---|
| `AntiAliasingSettings.h` | `EAntiAliasingMethod`, `EPrimaryScreenPercentageMethod`, `ETemporalAADebugView`, `FAntiAliasingParams` (persisted CVars), `FTemporalAADebugSettings` (not persisted). No dependencies, so `SettingsManager.h` can include it. | S1 |
| `ScreenPercentage.h/.cpp` | Constants `kMin/Max*ResolutionFraction`, `ETAAPassConfig`, `ETAAQuality`, `FViewFamilyInfo`, `ComputeViewFamilyInfo()`, `GetTemporalAAHistoryUpscaleFactor()`, `namespace TAAStageGates` (UE `FLegacyScreenPercentageDriver` + `PrepareViewRectsForRendering` + `FDefaultTemporalUpscaler` config selection) | S1 (R = O only), S3 (full) |
| `ViewMatrices.h` | `FViewMatrices`, header-only | S1 |
| `SceneViewState.h/.cpp` | `FTAATexture`, `FTemporalAAHistory`, `FPreviousViewInfo`, `FSceneViewState`, `FViewInfo`, `IsLargeCameraMovement()`, `ComputeTemporalAASample()` | S1/S2 |
| `Halton.h` | `inline float Halton(uint32_t Index, uint32_t Base)` (UE `Halton`) | S0 |
| `SceneRenderingUtils.h` | Header-only `inline` screen-pass helpers shared by `SceneRenderer.cpp`, `VelocityRendering.cpp` and `PostProcessUpscale.cpp`: the existing `TransitionToRenderTarget` (PSR → RT), `TransitionToShaderResource` (RT → PSR) and `SetViewportAndScissor`, **moved** out of the `SceneRenderer.cpp` anonymous namespace (`:27-57`), plus the new `TransitionReadToRenderTarget` (RD → RT) and `TransitionRenderTargetToRead` (RT → RD). Anonymous-namespace functions are invisible to other translation units, so they cannot stay there. | S0 (pure move; the two RD helpers are added unused) |
| `SceneVelocityData.h` | `FComponentVelocityData`, `FSceneVelocityData` (UE `ScenePrivate.h`); owned by `FScene` | S4 |
| `VelocityRendering.h/.cpp` | `FSceneVelocityData` implementation, `FSceneRenderer::RenderVelocities()`, `PrimitiveHasVelocityForView()`, `kMotionBlurPerObjectSize`, CPU encode/decode helpers (for self-tests) | S4 |
| `TemporalAA.h/.cpp` | `ETAAFlags`, `FTemporalAAParameters` (CB mirror), `FMitchellNetravaliParameters`, `FTAAPassParameters`, `FTAAOutputs`, `ITemporalUpscaler`, `FDefaultTemporalUpscaler` (RS, PSOs, CB ring, aux targets, dummies, `AddPasses`, `AddTemporalAAPass`, `ComputeMitchellNetravaliDownsample`, GPU self-test dispatch), `ComputeTemporalAASampleWeights()`, `ComputePixelFormatQuantizationError()` | S5 |
| `PostProcessUpscale.h/.cpp` | `EUpscaleMethod`, `ShouldMergeTonemapWithUpscale()`, `ComputeUpscaleUnsharpAmount()`, `FSceneRenderer::AddPrimaryUpscalePass()` (UE `AddUpscalePass`) | S3 |
| `TemporalAASelfTest.h/.cpp` | `int RunTemporalAASelfTests(FSceneRenderer&, bool bIncludeGPU)` (CPU vectors of Appendix A + GPU parity) | S1 (CPU) → S5 (GPU) |
| `ScreenshotCapture.h/.cpp` | `FScreenshotCapture`: back buffer → READBACK → 24-bit BMP | ST |
| `TemporalAATestDriver.h/.cpp` | `FTemporalAATestDriver`: command line, scenarios, overrides, capture schedule, log, exit | ST (+ keys per stage) |
| `Shader/VelocityCommon.hlsl` (header) | `EncodeVelocityToTexture`, `DecodeVelocityFromTexture`, `IsVelocityWritten`, `VELOCITY_VS_OUTPUT` | S4 |
| `Shader/BasePassVertexCommon.hlsl` (header) | `GetBasePassClipPosition()` (`precise`; UE `INVARIANT`) | S4 |
| `Shader/VelocityVS.hlsl` | velocity VS | S4 |
| `Shader/VelocityPixelShader.hlsl` (header) + `VelocityPS.hlsl`, `VelocityMaskedPS.hlsl` | velocity PS body plus 2 wrappers (`VELOCITY_MASKED` 0/1) | S4 |
| `Shader/VisualizeTemporalAAPS.hlsl` | debug views 1, 2, 4 and display of the CS debug texture (5–13) | S4 (1, 2), S5 (3, 5–13), S6 (4). Created in S4 and edited in S5 and S6, so it is on those stages' "may change" list (§9.1). |
| `Shader/PostProcessUpscale.hlsl` (header) + `PostProcessUpscale_{Nearest,Bilinear,Directional,CatmullRom,Lanczos,Gaussian}_PS.hlsl` | primary spatial upscale | S3 |
| `Shader/TemporalAACommon.hlsl` (header, **register-free**) | YCoCg, HDR weight, sample weight, depth conversion, closest-depth selection, Catmull-Rom weights, Mitchell-Netravali, Rand3DPCG16, Hammersley16, quantization, sanitize, IGN, debug heat map | S3 (created for upscale mode 3), S5 (TAA) |
| `Shader/TemporalAA.hlsl` (header) | TAA cbuffer, resources, `main` for every configuration | S5 |
| `Shader/TemporalAA_{Main,Upsampling}_{Low,Low_Downsample,Medium,High,MediumHigh}_CS.hlsl` (10) + `TemporalAA_SuperSampling_CS.hlsl` | permutation wrappers | S5 (Main without Downsample), S6 (Upsampling without Downsample, SuperSampling), S7 (the 2 Downsample wrappers) |
| `Shader/TemporalAAMitchellNetravali_CS.hlsl` | MainSuperSampling downsample | S6 |
| `Shader/TemporalAASelfTest_CS.hlsl` | GPU helper parity | S5 |
| `Shader/ResponsiveAAPS.hlsl` | responsive mask | S7 |

### 2.2 Changed files (summary; the per-stage detail is in §8)

| File | Change |
|---|---|
| `RenderManager.h/.cpp` | Grows `VIEW_CONSTANT` to 448 B and `PRIMITIVE_CONSTANT` to 128 B. Adds `TEXTURE_TYPE::VELOCITY` (t35) and `TEMPORAL_AA_DEBUG` (t36), and `EDepthStatePreset::None`, for which `CreatePipeline` sets `DSVFormat = UNKNOWN`. `CreateRenderTarget(..., bAllowUnorderedAccess)`. `RENDER_TARGET` gains `UAVIndex`/`UAVHandle`/`Width`/`Height`/`Format`. Adds `ReleaseDepthBuffer`/`CreateDepthBuffer`, `SetDefaultViewportSize`/`RestoreDefaultViewport`, free-descriptor and queue-length getters and `QueryLocalVideoMemoryUsage`, and a `SetConstant` size guard. Adds the `DX12_DEBUG_*` environment switches and the `AllocateRTVSlot` empty check. Adds the optional-PSO path: `CreatePipeline(..., bool bOptional = false)` and `HasPipelineState(const char*)` (§3.8). New PSOs. |
| `SceneTextures.h/.cpp` | `Init(RHI, W, H)`, `Release(RHI)`, `Extent`. New `Velocity` (R16G16_UNORM) and `ResponsiveAAMask` (R8_UNORM). New member `LinearDepthDisplaySRVIndex`, so the display SRV can be released. |
| `SceneRenderer.h/.cpp` | New members (§2.5). **Removes** `m_PrevViewProjectionT`, `m_PrevInvViewProjectionT`, `m_PrevViewOrigin` and `m_bHistoryValid`. New methods (§2.5). The post chain is rewired. `EndFrame` rebinds the back buffer. `InitDOF(W,H)`, `InitBloom(W,H)`. The anonymous-namespace screen-pass helpers move to `SceneRenderingUtils.h` (S0). |
| `ColorGradingLUTBaker.cpp` | **[FIX]** (S0): `m_FallbackTex` is created in RD instead of PSR, and `LoadArtistLUT` transitions the loaded texture PSR → RD, because the bake CS reads t0 (§0.4 #38). |
| `SceneView.h` | `bool bCameraCut = false;` and `const void* CameraId = nullptr;` |
| `World.h/.cpp` | `RequestCameraCut()`. `CalcSceneView` becomes non-const: it fills `bCameraCut` (world request, or the camera latch consumed, or a different active camera) and `CameraId`, then clears the request. |
| `CameraComponent.h/.cpp` | `NotifyCameraCut()`, `IsCameraCutPending() const`, `ConsumeCameraCut()` |
| `Camera.h/.cpp` | `SetInputEnabled(bool)` (test driver). `Tick` calls `m_CameraController.ResetVelocity()` when the component's cut latch is pending. |
| `GameManager.h/.cpp` | Calls `RenderVelocities` after `RenderBasePass`. Owns the test driver (hooks in `Begin`, `Update`, `Draw`). The constructor takes the command line. |
| `Main.cpp` | Passes `lpCmdLine`. Returns the driver's exit code. |
| `Time.h/.cpp` | `SetFixedDeltaTime(float)` (0 = off) |
| `Scene.h/.cpp` | `FSceneVelocityData m_VelocityData`, `MarkPrimitiveTeleported()`, `GetVelocityData()`. Hooks in `AddPrimitive`, `RemovePrimitive` and `UpdateAllPrimitiveSceneInfos`. |
| `PrimitiveSceneProxy.h/.cpp` | `UploadPrimitiveConstant(RHI, const XMFLOAT4X4* PreviousLocalToWorld = nullptr)`. New virtuals `DrawVelocity`, `DrawResponsiveAA`, `HasResponsiveAATranslucency`, `AlwaysHasVelocity`. |
| `StaticMeshComponent.cpp`, `Field.cpp`, `Polygon2D.cpp` | Implement the new proxy methods. The inline `PRIMITIVE_CONSTANT` writers also fill `PreviousLocalToWorld` (identity). |
| `Material.h/.cpp` | CPU-only `bool bEnableResponsiveAA = false;` with `ShouldEnableResponsiveAA()`/`SetEnableResponsiveAA()`. It is **not** part of the b2 mirror. |
| `AutoExposure.h/.cpp` | **[FIX]** (S0): Within a frame the result buffer is used in RD, not PSR; between command lists it decays to COMMON (buffer rule). Adds `IsResultValid()` and `PrepareResultForRead()` (§4.12). |
| `LumenScene.h/.cpp` | `InitScreenTextures()` is split into `InitGlobalTextures()` (GlobalSDF, RCSH) and `CreateScreenTextures(W,H)`. New `ReleaseScreenTextures()`. The destructor lambda becomes the member `ReleaseComputeTexture`. The card-capture viewport restore becomes `RestoreDefaultViewport()`. |
| `VolumetricFog.h/.cpp`, `FogRendering.h/.cpp` | `CreateVolumes(W,H)` / `ReleaseVolumes()`. `m_ViewWidth/Height` replace the back-buffer size for `ScreenSize`. `FFogSceneRenderer::GetVolumetricFog()` is used for resizing. |
| `LightGridInjection.h/.cpp` | `Init(CapacityW, CapacityH)` allocates at capacity. `SetViewSize(W,H)` sets the grid dimensions, `ScreenWidth/Height` and `MaxCulledLightLinks` each frame. |
| `ShadowRendering.cpp` | The viewport restore becomes `m_RHI->RestoreDefaultViewport()`. |
| `PostProcessSettings.h` + `Shader/ConstantBuffers.hlsl` | b4 pads renamed: `_pp_pad0` → `UpscaleUnsharpAmount`, `_pp_pad1` → `VisualizeMode` (uint), `_pp_pad2` → `VisualizeScale`. |
| `Shader/ConstantBuffers.hlsl`, `Resources.hlsl`, `GeometryVS.hlsl`, `GeometryPS.hlsl`, `TranslucentPS.hlsl`, `DeferredPS.hlsl` (S8) | Layout mirrors, t35/t36, `GetBasePassClipPosition`, `SampleBias`, IGN frame term |
| `ImGuiManager.h/.cpp` | Anti-Aliasing window, menus, thumbnails, material checkbox, Test Motion |
| `SettingsManager.h/.cpp` | `[AntiAliasing]` group, material key, teleport/cut hooks in `ApplyComponent`, `SetSaveEnabled` |
| `DirectX12.vcxproj(.filters)` | ClCompile/ClInclude and FxCompile entries (§6.10) |

### 2.3 Integration: who owns and calls what

```
wWinMain(lpCmdLine) -> GameManager(g_Window, lpCmdLine)  [ドライバがコマンドラインを解析]
GameManager::Begin
  World.BeginPlay; SettingsManager.Initialize (INI); TestDriver.OnBegin (上書き / INI 保存停止 / 固定 dt / カメラ入力停止); ImGui.Start
GameManager::Update
  Time.Update (固定 dt 可) ; Input.Update
  TestDriver.PreWorldTick(frame)          <- カメラ / アクターのトランスフォーム, SP 変更, カット要求
  World.Tick                                <- ACameraActor::Tick (カットラッチ -> ResetVelocity), ASky::Tick (カメラ位置追従)
  World.SendAllEndOfFrameUpdates
      FScene::UpdateAllPrimitiveSceneInfos:
          m_VelocityData.StartFrame()                  <- 全エントリ Prev = Current
          (プロキシ再生成 / SendRenderTransform) ; m_VelocityData.UpdateTransform(comp, proxy L2W)
          m_VelocityData.EndFrameUpdates()             <- テレポート保留フラグをクリア
GameManager::Draw
  view = World.CalcSceneView(O.x/O.y)       <- bCameraCut (ワールド要求 | カメララッチ消費 | カメラ変更), CameraId
  SceneRenderer.BeginFrame()                <- [自己テスト要求] -> PrepareViewRectsForRendering (FViewFamilyInfo, ResizeRenderTargets,
                                               LightGrid.SetViewSize, 既定ビューポート = R) -> RHI.BeginFrame -> G-Buffer
  SceneRenderer.RenderBasePass(view)        <- PrepareViewStateForVisibility (FViewInfo: ジッタ, 前フレーム, カット, C2P, ミップバイアス, b0)
                                               -> NoAA カリング -> ベースパス
  SceneRenderer.RenderVelocities(scene)     <- 新規
  SceneRenderer.RenderShadowDepths / RenderLumenScene (ビューポート復帰 = R)
  SceneRenderer.RenderLighting               <- LightGrid(R) / Fog(R, NoAA prev) / LinearDepth / Lumen(R, ジッタ込み prev) / Deferred / HeightFog
  SceneRenderer.RenderTranslucency           <- + RenderResponsiveAAMask
  SceneRenderer.RenderPostProcessing         <- CopySceneColorHistory / DOF / TAA(+MN) / AE / Bloom / LUT / Tonemap / Upscale / Visualize / Screenshot / CommitViewState
  ImGuiManager.Draw
  SceneRenderer.EndFrame                     <- バックバッファ RTV + ビューポート O を再バインド -> ImGui -> Present
  TestDriver.PostFrame                       <- キャプチャ書き出し, ログ, 終了判定
```

### 2.4 Key declarations (normative; the real code carries Japanese comments)

```cpp
// ======================= AntiAliasingSettings.h =======================
enum class EAntiAliasingMethod : int { None = 0, FXAA = 1, TemporalAA = 2, MSAA = 3, TSR = 4 }; // UE と同値 (1/3/4 は未実装)
enum class EPrimaryScreenPercentageMethod : int { SpatialUpscale = 0, TemporalUpscale = 1 };
enum class ETemporalAADebugView : int
{
    Off = 0,
    MotionVectors = 1,          // PS (VisualizeMotionVectors)
    VelocityMask = 2,           // PS
    InputOutputSplit = 3,       // 2 回目のトーンマップ (左半分シザー)
    TemporalUpscalerIO = 4,     // PS 2x2 (VisualizeTemporalUpscaler)
    BlendFinal = 5,             // 5..13 は TAA CS の DebugOutput (AA_DEBUG 相当)
    Rejection = 6,
    HistoryClamp = 7,
    ReprojectionError = 8,
    FilteredTemporalWeight = 9,
    ClosestDepthOffset = 10,
    ResponsiveMask = 11,
    DynamicAntiGhost = 12,
    InputSampleAlignment = 13,
    Count
};
inline bool IsTemporalAADebugViewFromCS(ETemporalAADebugView V)
{ return V >= ETemporalAADebugView::BlendFinal && V < ETemporalAADebugView::Count; }

struct FAntiAliasingParams                         // INI [AntiAliasing] (キー = フィールド名)。全フィールドの範囲は §7.1
{
    int   AntiAliasingMethod = 2;                   // r.AntiAliasingMethod
    float ScreenPercentage = 100.0f;                // r.ScreenPercentage
    bool  bTemporalAAUpsampling = true;             // r.TemporalAA.Upsampling  [PORT: UE4 既定 0]
    int   TemporalAAQuality = 2;                    // r.TemporalAA.Quality
    int   TemporalAASamples = 8;                    // r.TemporalAASamples
    float TemporalAACurrentFrameWeight = 0.04f;     // r.TemporalAACurrentFrameWeight
    float TemporalAAFilterSize = 1.0f;              // r.TemporalAAFilterSize
    bool  bTemporalAACatmullRom = false;            // r.TemporalAACatmullRom
    bool  bTemporalAAUpsampleFiltered = true;       // r.TemporalAAUpsampleFiltered
    float TemporalAAHistoryScreenPercentage = 100.0f; // r.TemporalAA.HistoryScreenPercentage
    bool  bTemporalAAR11G11B10History = true;       // r.TemporalAA.R11G11B10History
    bool  bTemporalAAAllowDownsampling = true;      // r.TemporalAA.AllowDownsampling
    int   UpscaleQuality = 3;                       // r.Upscale.Quality
    float UpscaleSoftness = 1.0f;                   // r.Upscale.Softness
    int   TonemapperMergeWithUpscaleMode = 0;       // r.Tonemapper.MergeWithUpscale.Mode
    float TonemapperMergeWithUpscaleThreshold = 0.49f; // r.Tonemapper.MergeWithUpscale.Threshold
    float ViewTextureMipBiasOffset = -0.3f;         // r.ViewTextureMipBias.Offset
    float ViewTextureMipBiasMin = -2.0f;            // r.ViewTextureMipBias.Min
    float CameraRotationThreshold = 45.0f;          // GEngine->CameraRotationThreshold [度]
    float CameraTranslationThreshold = 100.0f;      // GEngine->CameraTranslationThreshold (10000cm) [m]
};

struct FTemporalAADebugSettings                    // 非永続 (Lumen DebugMode と同じ扱い)
{
    ETemporalAADebugView DebugView = ETemporalAADebugView::Off;
    float VisualizeScale = 1.0f;                    // [0.1, 64] 可視化の増幅
    int   OverrideTemporalIndex = -1;               // r.TemporalAA.Debug.OverrideTemporalIndex (>=0 で固定)
    int   FilteredTemporalWeightMode = 0;           // 0 = 空間重み総和, 1 = 最近傍重み, 2 = 1.0
    bool  bForceJitterWithoutTAA = false;           // S2 検証用
    bool  bDisableJitter = false;                   // ジッタ 0 (比較用)
    bool  bForceVelocityPass = false;               // TAA 無効でもベロシティを描く
    bool  bForceResponsiveAA = false;               // 全半透明を Responsive 扱い
    bool  bDisableVelocitySmallObjectCull = false;  // MotionBlurPerObjectSize カリング無効
    bool  bRequestHistoryReset = false;             // ワンショット (カメラカット扱い)
    bool  bRequestSelfTest = false;                 // ワンショット (次の BeginFrame 先頭で実行)
    bool  bRequestReallocate = false;               // ワンショット: 同一サイズでも ResizeRenderTargets を実行 (S3a 検証 / リークテスト)
};

// ======================= ScreenPercentage.h =======================
constexpr float kMinTAAUpsampleResolutionFraction = 0.5f;   // UE
constexpr float kMaxTAAUpsampleResolutionFraction = 2.0f;   // UE
constexpr float kMinSpatialResolutionFraction     = 0.1f;   // [PORT] UE 0.01
constexpr float kMaxSpatialResolutionFraction     = 2.0f;   // [PORT] UE 4.0

namespace TAAStageGates                            // 段階導入ゲート。S8 で削除 (全 true 相当に畳む)
{
    constexpr bool kScreenPercentage   = false;    // S3 で true
    constexpr bool kTemporalAA         = false;    // S5 で true (Main のみ)
    constexpr bool kTemporalUpsampling = false;    // S6 で true (MainUpsampling + MainSuperSampling + MN + ミップバイアス)
    constexpr bool kTAAExtras          = false;    // S7 で true (R11G11B10 / ハーフ解像度出力 / Responsive AA)
}

enum class ETAAPassConfig : int { Main = 0, MainUpsampling = 1, MainSuperSampling = 2 };
enum class ETAAQuality    : int { Low = 0, Medium = 1, High = 2, MediumHigh = 3 };
inline bool IsTAAUpsamplingConfig(ETAAPassConfig P) { return P != ETAAPassConfig::Main; }

struct FViewFamilyInfo
{
    EAntiAliasingMethod            AntiAliasingMethod = EAntiAliasingMethod::None;   // 実効値
    EPrimaryScreenPercentageMethod PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::SpatialUpscale;
    float    ResolutionFraction = 1.0f;                // クランプ後の要求値
    float    EffectivePrimaryResolutionFraction = 1.0f;// R.x / O.x (UE と同じく X で定義)
    XMUINT2  OutputExtent{};                           // O
    XMUINT2  RenderExtent{};                           // R
    XMUINT2  SecondaryExtent{};                        // S (TAA 無効時は R)
    XMUINT2  HistoryExtent{};                          // H (TAA 無効時は R)
    XMUINT2  PostProcessExtent{};                      // P (予定値。実際の後段サイズは TAA 出力テクスチャから取る)
    bool     bTemporalAA = false;
    ETAAPassConfig TAAPass = ETAAPassConfig::Main;
    ETAAQuality    TAAQuality = ETAAQuality::High;
    float    HistoryUpscaleFactor = 1.0f;              // clamp(HSP/100, 1, 2)
    bool     bTAADownsample = false;
    bool     bR11G11B10History = false;
    bool     bSpatialUpscale = false;                  // P != O
};
struct FTAAStageGateValues { bool bScreenPercentage, bTemporalAA, bTemporalUpsampling, bTAAExtras; };
float           GetTemporalAAHistoryUpscaleFactor(const FAntiAliasingParams& P);   // clamp(HSP/100, 1, 2)
FViewFamilyInfo ComputeViewFamilyInfo(const FAntiAliasingParams& Params, XMUINT2 OutputExtent, bool bR11G11B10Supported);  // TAAStageGates を適用
FViewFamilyInfo ComputeViewFamilyInfoEx(const FAntiAliasingParams& Params, XMUINT2 OutputExtent, bool bR11G11B10Supported,
                                        const FTAAStageGateValues& Gates);                                      // 本体 (自己テストは全 true)

// ======================= ViewMatrices.h =======================
inline constexpr XMFLOAT4X4 kIdentity4x4{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
struct FViewMatrices                               // すべて転置前 (row-vector)。未初期化読みを防ぐため全行列を単位行列で初期化
{
    XMFLOAT4X4 ViewMatrix = kIdentity4x4;
    XMFLOAT4X4 ProjectionNoAAMatrix = kIdentity4x4;         // ジッタ無し
    XMFLOAT4X4 ProjectionMatrix = kIdentity4x4;             // = NoAA + (_31,_32) += TemporalAAProjectionJitter
    XMFLOAT4X4 ViewProjectionMatrix = kIdentity4x4;         // View * Projection (ジッタ込み)
    XMFLOAT4X4 ViewProjectionNoAAMatrix = kIdentity4x4;
    XMFLOAT4X4 InvViewProjectionMatrix = kIdentity4x4;      // ジッタ込み
    XMFLOAT4X4 InvViewProjectionNoAAMatrix = kIdentity4x4;
    XMFLOAT3   ViewOrigin{};
    XMFLOAT2   TemporalAAProjectionJitter{ 0.0f, 0.0f };   // NDC (UE GetTemporalAAJitter)

    void Init(const XMFLOAT4X4& View, const XMFLOAT4X4& ProjNoAA, const XMFLOAT3& Origin); // ジッタ 0 で全派生行列を計算
    void HackAddTemporalAAProjectionJitter(XMFLOAT2 J);   // assert(現在ジッタ == 0); _31 += J.x; _32 += J.y; Recompute
    void HackRemoveTemporalAAProjectionJitter();
    void RecomputeDerivedMatrices();                       // VP / InvVP (両系統)
    XMFLOAT2 GetTemporalAAJitter() const { return TemporalAAProjectionJitter; }
};

// ======================= SceneViewState.h =======================
struct FTAATexture                                  // UAV 付き RENDER_TARGET + 追跡状態 (UE pooled RT の代替)
{
    std::unique_ptr<RENDER_TARGET> RT;
    XMUINT2     Extent{};
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;   // CreateRenderTarget の初期状態
    bool Matches(XMUINT2 E, DXGI_FORMAT F) const { return RT && Extent.x == E.x && Extent.y == E.y && Format == F; }
    void Allocate(RenderManager* RHI, XMUINT2 E, DXGI_FORMAT F, const wchar_t* Name); // RT.reset() (遅延解放) + 生成 (UAV 付き), State = PSR
    void Release() { RT.reset(); Extent = {}; Format = DXGI_FORMAT_UNKNOWN; }
};

struct FTemporalAAHistory                           // UE FTemporalAAHistory (RT[0] のみ)
{
    int         RTSlot = -1;                         // FSceneViewState::TemporalAAHistoryPool の添字
    XMUINT2     ReferenceBufferSize{};               // テクスチャ実寸 (exact-size)
    XMUINT2     ViewportSize{};                      // = ReferenceBufferSize (ViewportRect.Min = 0)
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    bool IsValid() const { return RTSlot >= 0; }
    void SafeRelease() { *this = FTemporalAAHistory{}; }
};

struct FPreviousViewInfo                            // UE FPreviousViewInfo
{
    FViewMatrices      ViewMatrices;                 // 前フレームのジッタ込み + NoAA + ジッタ値
    FTemporalAAHistory TemporalAAHistory;
    XMUINT2            ViewRectSize{};               // [PORT] Lumen / Fog 履歴 (レンダー解像度) の有効判定用
    float              SceneColorPreExposure = 1.0f; // プリエクスポージャ無し
};

class FSceneViewState                               // UE FSceneViewState (ビュー 1 つ分の永続状態)
{
public:
    uint32_t          TemporalAASampleIndex = 0;
    uint32_t          FrameIndex = 0;                // StateFrameIndex (CommitViewState で +1)
    FPreviousViewInfo PrevFrameViewInfo;
    bool              bPrevFrameViewInfoValid = false;
    bool              bForceCameraCut = true;         // 次フレームを強制カット (初回 / bValid 復帰)
    EAntiAliasingMethod PrevAntiAliasingMethod = EAntiAliasingMethod::None;
    FTAATexture       TemporalAAHistoryPool[2];       // ピンポン
    FTAATexture* GetHistoryTexture(const FTemporalAAHistory& H) { return H.IsValid() ? &TemporalAAHistoryPool[H.RTSlot] : nullptr; }
    uint32_t GetFrameIndexMod8() const { return FrameIndex & 7u; }
};

struct FViewInfo                                    // UE FViewInfo (1 フレーム分)
{
    bool      bValid = false;
    XMUINT2   ViewRectSize{};                        // R
    XMUINT2   UnscaledViewRectSize{};                // O
    EAntiAliasingMethod            AntiAliasingMethod = EAntiAliasingMethod::None;
    EPrimaryScreenPercentageMethod PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::SpatialUpscale;
    FViewMatrices     ViewMatrices;
    FPreviousViewInfo PrevViewInfo;                  // フレーム先頭のスナップショット (リセット適用後, フレーム中不変)
    bool      bCameraCut = false;
    bool      bPrevTransformsReset = false;          // カット or 大移動: Prev 行列 = 今フレーム
    bool      bPrevViewInfoValid = false;            // 前フレームのビュー情報が使えるか (初回 / カットで false)
    XMFLOAT2  TemporalJitterPixels{};                // レンダー px (+y 下)
    int       TemporalJitterIndex = 0;
    int       TemporalJitterSequenceLength = 1;
    float     MaterialTextureMipBias = 0.0f;
    XMFLOAT4X4 ClipToPrevClip = kIdentity4x4;        // 転置前 = InvVP_NoAA(cur) * VP_NoAA(prev)。ComputeClipToPrevClip (カメラ相対, double) で合成
    float     NearClip = 0.1f, FarClip = 500.0f;
    uint32_t  StateFrameIndex = 0;
};
bool     IsLargeCameraMovement(const FViewMatrices& Cur, const FViewMatrices& Prev, float RotationThresholdDeg, float TranslationThresholdM);
XMFLOAT4X4 ComputeClipToPrevClip(const FViewMatrices& Cur, const FViewMatrices& Prev); // §4.4: UE の Translated 行列と同じくカメラ相対・double で合成
XMFLOAT2 ComputeTemporalAASample(bool bTemporalUpsampling, int SamplesCVar, int SequenceLength, int Index, float FilterSize); // §4.4

// ======================= SceneVelocityData.h =======================
struct FComponentVelocityData
{
    XMFLOAT4X4 LocalToWorld;                         // 今フレーム描画値 (転置前)
    XMFLOAT4X4 PreviousLocalToWorld;                 // 前フレーム描画値
    uint64_t   LastFrameUpdated = 0;
    bool       bTeleportPending = true;              // 次の UpdateTransform で Prev = Current
    bool HasVelocity() const;                        // いずれかの要素で |L2W - Prev| > 1e-4 (UE FMatrix::Equals 許容誤差)
};
class FSceneVelocityData                            // UE FSceneVelocityData (FScene::VelocityData)
{
public:
    void StartFrame();                                                       // ++InternalFrameIndex; 全エントリ Prev = Current
    void Register(const UPrimitiveComponent* C, const XMFLOAT4X4& L2W);      // Prev = Current = L2W, bTeleportPending = true
    void UpdateTransform(const UPrimitiveComponent* C, const XMFLOAT4X4& L2W); // Current = L2W; 保留中なら Prev = L2W
    void MarkTeleported(const UPrimitiveComponent* C);                       // bTeleportPending = true (UE bTeleport / OverridePreviousTransform)
    void EndFrameUpdates();                                                  // 全 bTeleportPending = false
    void Remove(const UPrimitiveComponent* C);
    const FComponentVelocityData* Find(const UPrimitiveComponent* C) const;
    size_t Num() const { return m_ComponentData.size(); }
private:
    std::unordered_map<const UPrimitiveComponent*, FComponentVelocityData> m_ComponentData;  // キー = コンポーネント (プロキシ再生成を跨いで生存)
    uint64_t m_InternalFrameIndex = 0;
};

// ======================= VelocityRendering.h =======================
constexpr float kMotionBlurPerObjectSize = 0.5f;    // UE FPostProcessSettings::MotionBlurPerObjectSize 既定 [M]
bool PrimitiveHasVelocityForView(const FViewInfo& View, const FPrimitiveSceneProxy& Proxy, bool bDisableSmallObjectCull);
XMFLOAT2 EncodeVelocityToTextureCPU(XMFLOAT2 V);   // 自己テスト用 (HLSL と同式)
XMFLOAT2 DecodeVelocityFromTextureCPU(XMFLOAT2 E);

// ======================= PrimitiveSceneProxy.h (追加) =======================
virtual void DrawVelocity(RenderManager* RHI, const XMFLOAT4X4& PreviousLocalToWorld) const {}  // Opaque / Masked サブセットのみ
virtual void DrawResponsiveAA(RenderManager* RHI, bool bForceAll) const {}                     // Responsive 半透明サブセットのみ
virtual bool HasResponsiveAATranslucency(bool bForceAll) const { return false; }
virtual bool AlwaysHasVelocity() const { return false; }                                       // 将来のスキニング / WPO 用
protected: void UploadPrimitiveConstant(RenderManager* RHI, const XMFLOAT4X4* PreviousLocalToWorld = nullptr) const; // null = Prev に今の値

// ======================= RenderManager.h (追加) =======================
enum class EDepthStatePreset { DepthWrite, DepthRead, DepthReadEqual, None /* DepthEnable=FALSE, DSVFormat=UNKNOWN */ };
struct RENDER_TARGET { /* 既存 */ unsigned int UAVIndex = UINT_MAX; D3D12_GPU_DESCRIPTOR_HANDLE UAVHandle{};
                       unsigned int Width = 0, Height = 0; DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN; };
std::unique_ptr<RENDER_TARGET> CreateRenderTarget(unsigned int W, unsigned int H, DXGI_FORMAT F, unsigned int MipLevels = 1, bool bAllowUnorderedAccess = false);
void     ReleaseDepthBuffer();                        // DeferredRelease (DSV スロットは保持)
void     CreateDepthBuffer(unsigned int W, unsigned int H);   // R32_TYPELESS, DEPTH_WRITE, 同一 DSV スロットへ再作成
unsigned GetDepthBufferWidth() const;  unsigned GetDepthBufferHeight() const;
void     SetDefaultViewportSize(unsigned int W, unsigned int H); // m_Viewport / m_ScissorRect (BeginFrame / FlushAndReset が適用)
void     RestoreDefaultViewport();                     // 現在のコマンドリストへ即時適用
unsigned GetDefaultViewportWidth() const; unsigned GetDefaultViewportHeight() const;
size_t   GetNumFreeSRVDescriptors() const; size_t GetNumFreeRTVDescriptors() const; size_t GetDeferredReleaseQueueLength() const;
UINT64   QueryLocalVideoMemoryUsage();                 // m_Adapter.As(&IDXGIAdapter3) -> QueryVideoMemoryInfo(LOCAL).CurrentUsage
ComPtr<ID3D12PipelineState> CreatePipeline(/* 既存の引数 ... */, EDepthStatePreset DepthPreset = ..., bool bOptional = false);
                                                      // bOptional: .cso 欠落 / 空ならログのみで nullptr を返す (assert しない)。PS 無し PSO は作らない
bool     HasPipelineState(const char* PipelineName) const; // m_PipelineState に非 null で登録済みか (SetPipelineState 前の確認用)
```

`~RENDER_TARGET` additionally calls `ReleaseShaderResourceView(UAVIndex)` when `UAVIndex != UINT_MAX`. That release is deferred through the existing queue.

**`TemporalAA.h` declarations** (the CB layouts are in §3.4 and §3.5):

```cpp
enum ETAAFlags : uint32_t
{
    TAA_FLAG_UPSAMPLE_FILTERED     = 1u << 0,   // r.TemporalAAUpsampleFiltered (MainUpsampling)
    TAA_FLAG_RESPONSIVE_MASK_VALID = 1u << 1,   // マスクを今フレーム描いた
    TAA_FLAG_EYE_ADAPTATION_BUFFER = 1u << 2,   // t4 EyeAdaptation[0] を使う (無効時 ManualExposure)
    TAA_FLAG_HISTORY_HAS_ALPHA     = 1u << 3,   // 入力履歴が RGBA16F (R11G11B10 は a=1 を返すためクリア)
    TAA_FLAG_FTW_MODE_SHIFT        = 4,         // bits 4-5: FilteredTemporalWeight 定義 (0 総和 / 1 最近傍 / 2 = 1)
    TAA_FLAG_DOWNSAMPLE_OUTPUT     = 1u << 6,   // u1 へ書く (DOWNSAMPLE 順列内の安全ゲート)
};

struct FTAAPassParameters                           // UE FTAAPassParameters
{
    ETAAPassConfig Pass = ETAAPassConfig::Main;
    ETAAQuality    Quality = ETAAQuality::High;
    bool           bDownsample = false;
    bool           bUseR11G11B10History = false;
    bool           bUpsampleFiltered = true;
    RENDER_TARGET* SceneColorInput = nullptr;       // R, PSR
    unsigned int   SceneDepthSRVIndex = 0;          // LinearDepth (RG32F, R = view Z, 不透明のみ)
    unsigned int   SceneVelocitySRVIndex = 0;       // Velocity (R16G16_UNORM)
    unsigned int   ResponsiveMaskSRVIndex = 0;      // ResponsiveAAMask (R8_UNORM)
    bool           bResponsiveMaskValid = false;
    unsigned int   EyeAdaptationSRVIndex = 0;       // AutoExposure 結果 or ダミー
    bool           bUseEyeAdaptationBuffer = false;
    float          ManualExposure = 1.0f;           // PostProcess.Exposure
    XMUINT2        InputExtent{};                   // InputViewRect サイズ = R
    XMUINT2        OutputExtent{};                  // OutputViewRect サイズ = H
    float          CurrentFrameWeight = 0.04f, FilterSize = 1.0f;
    bool           bCatmullRom = false;
    bool           bForceHistoryBypass = false;     // CB bCameraCut = 1 (カット扱いはしない)
    ETemporalAADebugView DebugView = ETemporalAADebugView::Off;
    float          DebugScale = 1.0f;
    int            FilteredTemporalWeightMode = 0;
    float          NearClip = 0.1f, FarClip = 500.0f;
};
struct FTAAOutputs { FTAATexture* SceneColor = nullptr; FTAATexture* DownsampledSceneColor = nullptr; };

class ITemporalUpscaler                             // UE ITemporalUpscaler (4.26 形)
{
public:
    struct FPassInputs
    {
        bool           bAllowDownsampleSceneColor = false;
        RENDER_TARGET* SceneColorTexture = nullptr;
        unsigned int   SceneDepthSRVIndex = 0, SceneVelocitySRVIndex = 0, ResponsiveMaskSRVIndex = 0;
        bool           bResponsiveMaskValid = false;
        unsigned int   EyeAdaptationSRVIndex = 0;
        bool           bUseEyeAdaptationBuffer = false;
        float          ManualExposure = 1.0f;
        bool           bForceHistoryBypass = false;
    };
    struct FPassOutputs
    {
        RENDER_TARGET* SceneColor = nullptr;        XMUINT2 SceneColorExtent{};   // S, PSR
        RENDER_TARGET* HalfResSceneColor = nullptr; XMUINT2 HalfResExtent{};      // PSR or null
        FTemporalAAHistory NewHistory;                                          // CommitViewState へ渡す
    };
    virtual ~ITemporalUpscaler() = default;
    virtual const char* GetDebugName() const = 0;
    virtual bool  IsReady(const FViewFamilyInfo& Family) const = 0;   // 必要な PSO が全て存在するか
    virtual FPassOutputs AddPasses(const FViewInfo& View, const FViewFamilyInfo& Family, FSceneViewState& ViewState,
                                   const FAntiAliasingParams& Params, const FTemporalAADebugSettings& Debug, const FPassInputs& Inputs) = 0;
    virtual float GetMinUpsampleResolutionFraction() const = 0;       // 0.5
    virtual float GetMaxUpsampleResolutionFraction() const = 0;       // 2.0
};

class FDefaultTemporalUpscaler final : public ITemporalUpscaler
{
public:
    explicit FDefaultTemporalUpscaler(RenderManager* RHI);
    void Init();                                   // RS / PSO (Try) / CB リング / ダミー / R11G11B10 UAV 対応確認
    const char* GetDebugName() const override { return "Gen4 TAAU"; }
    bool  IsReady(const FViewFamilyInfo& Family) const override;
    FPassOutputs AddPasses(...) override;          // §4.8
    float GetMinUpsampleResolutionFraction() const override { return kMinTAAUpsampleResolutionFraction; }
    float GetMaxUpsampleResolutionFraction() const override { return kMaxTAAUpsampleResolutionFraction; }
    FTAAOutputs  AddTemporalAAPass(const FViewInfo& View, const FTAAPassParameters& P, const FTemporalAAHistory& InputHistory,
                                   FTemporalAAHistory* OutputHistory, FSceneViewState& ViewState);   // UE AddTemporalAAPass(View, Inputs, InputHistory, OutputHistory)
    FTAATexture* ComputeMitchellNetravaliDownsample(FTAATexture* Input, XMUINT2 OutputExtent);
    FTAATexture* GetDebugOutput() { return m_DebugOutput.RT ? &m_DebugOutput : nullptr; }
    bool IsR11G11B10HistorySupported() const { return m_bR11G11B10Supported; }
    bool RunGPUSelfTest(std::vector<float>& OutValues);   // BeginFrame 先頭からのみ呼ぶ (FlushAndReset を使う)
    bool HasSelfTestPSO() const { return m_PSOSelfTest != nullptr; }
    unsigned int GetDummySRVIndex() const;                // 1x1 黒 RGBA16F (RD 常駐)。未書き込みベロシティ / マスクの代替
    unsigned int GetDummyEyeAdaptationSRVIndex() const { return m_DummyEyeAdaptationSRV; }
private:
    RenderManager* m_RHI;
    ComPtr<ID3D12RootSignature> m_RootSignature;
    ComPtr<ID3D12PipelineState> m_PSO[3][4][2];     // [Pass][Quality][Downsample]; 存在しない組合せは null
    ComPtr<ID3D12PipelineState> m_PSOMitchellNetravali, m_PSOSelfTest;
    ComPtr<ID3D12Resource> m_ParamBuffer[2];        // フレーム毎 4 スロット x 512 B (0 TAA, 1 MN, 2 SelfTest, 3 予備)
    uint8_t* m_ParamPtr[2] = {};
    FTAATexture m_HalfRes, m_MNOutput, m_DebugOutput;
    std::unique_ptr<RENDER_TARGET> m_DummyTex;      // 1x1 RGBA16F 黒, RD 常駐 (SRV 用)
    std::unique_ptr<RENDER_TARGET> m_DummyUAVTex;   // 1x1 RGBA16F, UNORDERED_ACCESS 常駐 (未使用 u1/u2 用)
    ComPtr<ID3D12Resource> m_DummyEyeAdaptation;    // UPLOAD, float[2] = {1,1}, GENERIC_READ
    unsigned int m_DummyEyeAdaptationSRV = 0;
    ComPtr<ID3D12Resource> m_SelfTestBuffer, m_SelfTestReadback; unsigned int m_SelfTestUAV = 0;
    bool m_bR11G11B10Supported = false;
    bool m_bLoggedMissingPSO = false;
};

void     ComputeTemporalAASampleWeights(XMFLOAT2 JitterPixels, float FilterSize, bool bCatmullRom, float OutSampleWeights[9], float OutPlusWeights[5]);
XMFLOAT3 ComputePixelFormatQuantizationError(DXGI_FORMAT Format);  // RGBA16F: (2^-10)x3, R11G11B10: (2^-6, 2^-6, 2^-5)
```

### 2.5 `FSceneRenderer` members

| Added | Removed / changed |
|---|---|
| `FAntiAliasingParams m_AAParams` + `GetAntiAliasingParams()` | `m_PrevViewProjectionT`, `m_PrevInvViewProjectionT`, `m_PrevViewOrigin`, `m_bHistoryValid` (**deleted**; the compiler then catches old readers) |
| `FTemporalAADebugSettings m_TAADebug` + getter | `InitDOF()` → `InitDOF(W,H)` |
| `FViewFamilyInfo m_ViewFamily` + getter | `InitBloom()` → `InitBloom(PW,PH)`; `RenderBloom()` → `RenderBloom(RENDER_TARGET* Input, XMUINT2 RestoreExtent)` |
| `FSceneViewState m_ViewState` + getter | `CopySceneColorHistory()` copies textures only |
| `FViewInfo m_ViewInfo` + getter | `ComputeViewVisibility` uses `m_ViewInfo.ViewMatrices.ViewProjectionNoAAMatrix`, and rebuilds `m_ViewFrustum` only when `m_ViewInfo.bValid` (§4.4, "Culling on invalid frames") |
| `std::unique_ptr<FDefaultTemporalUpscaler> m_TemporalUpscaler` | `ResolvePostProcessSettings` → `SetTexelSize(R)` |
| `std::unique_ptr<RENDER_TARGET> m_TonemapOutput` (lazy, sized to the actual post input) | |
| `XMUINT2 m_AllocatedRenderExtent, m_AllocatedPostExtent` | |
| `bool m_bResponsiveMaskValid`, `bool m_bVelocityValid`: per-frame validity flags. Both are reset to `false` as the **first statement of `BeginFrame`** (§4.1 step 1), before any pass that can return early. Only the pass that cleared and wrote the texture in this frame sets its flag (`RenderVelocities`, `RenderResponsiveAAMask`). A freshly (re)allocated texture is therefore never read as valid before it has been cleared. | |
| `FTemporalAAStats m_TAAStats` + getter: `NumVelocityDraws`, `NumResizes`, `bTAARanThisFrame`, `bHistoryValidThisFrame`, `HistoryExtent`, `HistoryFormat`, `PostExtent`, `bUpscaleMerged`, `bFallbackMissingPSO`, `bLumenHistoryValid`, `bFogHistoryValid`, `LastSelfTestFailures`. `bLumenHistoryValid` is written by `RenderLumenScene` and `bFogHistoryValid` by `RenderLighting`, from the input structs they build. `MakeLumenFrameInputs` stays `const` and writes nothing (§9.3). | |
| `std::unique_ptr<FScreenshotCapture> m_Screenshot` + `RequestScreenshot(const std::string& Path)` | |
| Methods: `PrepareViewRectsForRendering()`, `ResizeRenderTargets(const FViewFamilyInfo&)`, `PrepareViewStateForVisibility(const FSceneView&)`, `RenderVelocities(FScene*)`, `RenderResponsiveAAMask(...)` (private, called inside `RenderTranslucency`), `CommitViewState(const FTemporalAAHistory&)`, `ApplyRenderViewport()`, `DrawTonemap(RENDER_TARGET* Input)`, `EnsureTonemapOutput(XMUINT2)`, `SelectPrimaryUpscalePipeline()`, `AddPrimaryUpscalePass(In, InExtent, O, const char* PSOName)`, `AddVisualizeTemporalAAPass(...)`, `IsPreTAADebugViewActive()` | |

`FSceneRenderer` constructor order (additions marked ★):
1. `m_SceneTextures.Init(m_RHI, BBW, BBH)`
2. `InitScreenQuad`, `InitIBL`, `InitLightBuffer`, `InitPostProcess`. `InitPostProcess` now does LUT and AE only. Bloom/DOF are created through `InitDOF(BBW,BBH)` and `InitBloom(BBW,BBH)` right after it.
3. `m_LightGrid->Init(2·BBW, 2·BBH)` (capacity) ★, then `SetViewSize(BBW,BBH)` ★
4. Shadow renderer, Lumen `Init()`, Fog `Init()`: unchanged, allocated at BB.
5. ★ `m_TemporalUpscaler = std::make_unique<FDefaultTemporalUpscaler>(m_RHI); m_TemporalUpscaler->Init();`
6. ★ `m_Screenshot = std::make_unique<FScreenshotCapture>(m_RHI);`
7. ★ `m_AllocatedRenderExtent = m_AllocatedPostExtent = {BBW, BBH};`

---

## 3. Exact data layouts

### 3.1 `VIEW_CONSTANT` (b0): 256 → 448 B (fits the 512 B ring slot)

The first 256 bytes keep their offsets and meaning. `Projection` and `InvViewProjection` now **include the jitter** (UE `ViewToClip` / `ClipToTranslatedWorld`).

```cpp
struct VIEW_CONSTANT                        // RenderManager.h
{
    XMFLOAT4X4 View;                        //   0  ワールド -> ビュー
    XMFLOAT4X4 Projection;                  //  64  ジッタ込み
    XMFLOAT4X4 InvViewProjection;           // 128  ジッタ込み (深度 + UV からのワールド復元)
    XMFLOAT4   WorldCameraOrigin;           // 192  xyz, w=1
    XMFLOAT4   NearFar;                     // 208  x=Near, y=Far, zw=0
    XMFLOAT4   DirectionalLightDirection;   // 224
    XMFLOAT4   DirectionalLightColor;       // 240
    // ---- Temporal AA / TAAU (FViewUniformShaderParameters の同名メンバ) ----
    XMFLOAT4X4 PrevViewProjection;          // 256  前フレーム View*Projection (前フレームのジッタ込み = UE PrevTranslatedWorldToClip)
    XMFLOAT4X4 ClipToPrevClip;              // 320  InvVP_NoAA(cur) * VP_NoAA(prev) (UE ClipToPrevClip)
    XMFLOAT4   TemporalAAJitter;            // 384  xy = 今フレーム NDC ジッタ, zw = 前フレーム NDC ジッタ
    XMFLOAT4   TemporalAAParams;            // 400  x = SampleIndex, y = SampleCount, zw = TemporalJitterPixels (レンダー px)
    XMFLOAT4   ViewSizeAndInvSize;          // 416  (R.x, R.y, 1/R.x, 1/R.y) (exact-size なので BufferSize と同一)
    float      MaterialTextureMipBias;      // 432
    float      MaterialTextureDerivativeMultiply; // 436  = 2^MipBias (予約。SampleGrad 用で現状未使用)
    uint32_t   StateFrameIndexMod8;         // 440  TAA 有効時 FrameIndex & 7, それ以外 0 [PORT]
    uint32_t   StateFrameIndex;             // 444
};
static_assert(sizeof(VIEW_CONSTANT) == 448, "VIEW_CONSTANT must mirror HLSL ViewConstantBuffer (b0)");
static_assert(offsetof(VIEW_CONSTANT, PrevViewProjection) == 256, "");
static_assert(offsetof(VIEW_CONSTANT, ClipToPrevClip) == 320, "");
static_assert(offsetof(VIEW_CONSTANT, TemporalAAJitter) == 384, "");
static_assert(offsetof(VIEW_CONSTANT, MaterialTextureMipBias) == 432, "");
static_assert(sizeof(VIEW_CONSTANT) <= 512, "one constant ring slot");
```

The HLSL mirror goes in `ConstantBuffers.hlsl`, in the same order. The fields appended after `DirectionalLightColor` are:

```hlsl
    float4x4 PrevViewProjection;   // 256 前フレーム (ジッタ込み)
    float4x4 ClipToPrevClip;       // 320 NoAA
    float4   TemporalAAJitter;     // 384 xy cur, zw prev (NDC)
    float4   TemporalAAParams;     // 400
    float4   ViewSizeAndInvSize;   // 416
    float    MaterialTextureMipBias;            // 432
    float    MaterialTextureDerivativeMultiply; // 436
    uint     StateFrameIndexMod8;               // 440
    uint     StateFrameIndex;                   // 444
```

**Writers:**
- `FSceneRenderer::PrepareViewStateForVisibility` writes every camera and temporal field (§4.4). `SetupLightConstants` still writes the two light fields afterwards.
- The camera b0 is re-uploaded at the existing sites (`RenderBasePass`, `RenderLighting`, `RenderTranslucency`) and in `RenderVelocities`.
- Shadow views, Lumen card capture and Polygon2D keep uploading a zero-initialised `VIEW_CONSTANT{}`. Every new field is therefore 0 there: bias 0, no temporal data. That is correct.

`StateFrameIndexMod8` is written as `m_ViewFamily.bTemporalAA ? (FrameIndex & 7) : 0`. UE always rotates it. **[PORT]:** we freeze it at 0 when AA is off, so that the S8 DeferredPS IGN change keeps the AA-off image identical to the baseline.

### 3.2 `PRIMITIVE_CONSTANT` (b1): 64 → 128 B

```cpp
struct PRIMITIVE_CONSTANT
{
    XMFLOAT4X4 LocalToWorld;           //  0 (転置)
    XMFLOAT4X4 PreviousLocalToWorld;   // 64 (転置。ベロシティパス以外は LocalToWorld と同値)
};
static_assert(sizeof(PRIMITIVE_CONSTANT) == 128, "PRIMITIVE_CONSTANT must mirror HLSL PrimitiveConstantBuffer (b1)");
```

- HLSL: add `float4x4 PreviousLocalToWorld;` after `LocalToWorld`.
- `UploadPrimitiveConstant(RHI, Prev)` writes both fields. With `Prev == nullptr` it writes `m_LocalToWorld` into both.
- The inline writers set `PreviousLocalToWorld = identity`: `DrawCardCapture` (StaticMeshComponent.cpp:249-252) and `Polygon2D.cpp:48-51`.

### 3.3 `PP_SETTINGS` (b4): size unchanged at 176 B; pads renamed

The renamed pads are renderer-owned and never persisted.

| Offset | Old | New | Type | Written by |
|---|---|---|---|---|
| 164 | `_pp_pad0` | `UpscaleUnsharpAmount` | float | the renderer before the upscale pass: `r.Upscale.Softness × max(0, 1 − (R.x·R.y)/(O.x·O.y))` [L] |
| 168 | `_pp_pad1` | `VisualizeMode` | uint | the renderer before the visualize pass (`ETemporalAADebugView`) |
| 172 | `_pp_pad2` | `VisualizeScale` | float | the renderer (`FTemporalAADebugSettings::VisualizeScale`) |

`WritePostProcess`/`ReadPostProcess` are unchanged, because they never touch the pads.

`SceneTexelSizeX/Y` (offsets 120/124) keep their "renderer-owned" meaning:
- `1/R` from `ResolvePostProcessSettings` through translucency.
- DOF sets its half-res value and then restores `1/R`.
- Bloom sets per-mip values and then restores `1/P`.
- The upscale pass sets `1/input`.
- The visualize pass sets `1/O`.

### 3.4 TAA constant buffer (root CBV b0 of the TAA root signature): 336 B in a 512 B slot

```cpp
struct FTemporalAAParameters                   // HLSL: cbuffer TemporalAAParameters : register(b0) (TemporalAA.hlsl)
{
    XMFLOAT4   InputSceneColorSize;            //   0 (R.x, R.y, 1/R.x, 1/R.y)
    XMINT4     InputMinMaxPixelCoord;          //  16 (0, 0, R.x-1, R.y-1)
    XMFLOAT4   OutputViewportSize;             //  32 (H.x, H.y, 1/H.x, 1/H.y)
    XMFLOAT4   HistoryBufferSize;              //  48 (Hp.x, Hp.y, 1/Hp.x, 1/Hp.y) 入力履歴の実寸
    XMFLOAT4   HistoryBufferUVMinMax;          //  64 (0.5/Hp.x, 0.5/Hp.y, (Hp.x-0.5)/Hp.x, (Hp.y-0.5)/Hp.y)
    XMFLOAT4   ScreenPosToHistoryBufferUV;     //  80 (0.5, -0.5, 0.5, 0.5) exact-size なので定数
    XMFLOAT4X4 ClipToPrevClip;                 //  96 転置済み (NoAA x NoAA)
    XMFLOAT2   TemporalJitterPixels;           // 160 レンダー px (+y 下)
    XMFLOAT2   ScreenPosAbsMax;                // 168 (1-1/Hp.x, 1-1/Hp.y) (AA_BICUBIC=0 経路用。現状未使用)
    float      ScreenPercentage;               // 176 R.x / H.x
    float      UpscaleFactor;                  // 180 H.x / R.x
    float      CurrentFrameWeight;             // 184
    float      HistoryPreExposureCorrection;   // 188 = 1
    uint32_t   bCameraCut;                     // 192 View.bCameraCut || !InputHistory.IsValid() || bForceHistoryBypass
    uint32_t   Flags;                          // 196 ETAAFlags
    float      ManualExposure;                 // 200 PostProcess.Exposure
    uint32_t   DebugMode;                      // 204 ETemporalAADebugView (5..13 のみ書く。それ以外 0)
    XMFLOAT4   SampleWeights[3];               // 208 [0..8] (i>>2, i&3) Main のみ
    XMFLOAT4   PlusWeights[2];                 // 256 [0..4] Main のみ
    XMFLOAT4   OutputQuantizationError;        // 288 xyz = ComputePixelFormatQuantizationError(出力フォーマット), w = 最大有限値 (RGBA16F 65504 / R11G11B10 64512)
    XMFLOAT4   DepthParams;                    // 304 x = Q = f/(f-n), y = -Q*n (d = x + y/viewZ), z = 0.999*f (遠方判定), w = 0
    uint32_t   StateFrameIndexMod8;            // 320 FrameIndex & 7 (常に回す)
    float      DebugScale;                     // 324
    float      SampleDistanceThreshold;        // 328 = 1.51 + (1.3 - 1.51) * (UpscaleFactor - 1) (クランプ無し) [H]
    uint32_t   Pad0;                           // 332
};
static_assert(sizeof(FTemporalAAParameters) == 336, "");
static_assert(offsetof(FTemporalAAParameters, ClipToPrevClip) == 96, "");
static_assert(offsetof(FTemporalAAParameters, SampleWeights) == 208, "");
static_assert(offsetof(FTemporalAAParameters, DepthParams) == 304, "");
```

The HLSL mirror declares the same fields in the same order: `float4, int4, float4 ×4, float4x4, float2, float2, float ×4, uint, uint, float, uint, float4[3], float4[2], float4, float4, uint, float, float, uint`.
- The two `float2`s share register c10.
- Each `float4` array element starts a register, so `SampleWeights[i >> 2][i & 3]` in HLSL matches the C++ `XMFLOAT4[3]` exactly.

### 3.5 Mitchell-Netravali constant buffer (slot 1 of the same ring, root CBV b0)

```cpp
struct FMitchellNetravaliParameters            // HLSL: cbuffer MitchellNetravaliParameters : register(b0)
{
    XMFLOAT4 InputSize;                        //  0 (H.x, H.y, 1/H.x, 1/H.y)
    XMFLOAT4 OutputSize;                       // 16 (S.x, S.y, 1/S.x, 1/S.y)
    XMFLOAT2 InputPerOutputPixel;              // 32 (H.x/S.x, H.y/S.y) (1..2)
    XMFLOAT2 Pad;                              // 40
};                                             // 48 B
static_assert(sizeof(FMitchellNetravaliParameters) == 48, "");
```

### 3.6 Graphics root signature additions

Append to `TEXTURE_TYPE`, just before `COUNT` (the header rule at RenderManager.h:15-17):

```cpp
VELOCITY,            // t35 (Texture2D<float2>: SceneVelocity R16G16_UNORM エンコード済み, 0 = 未書き込み)
TEMPORAL_AA_DEBUG,   // t36 (Texture2D<float4>: TAA DebugOutput / TAA 出力 (TemporalUpscalerIO))
COUNT,               // = 45 (static_assert(COUNT <= 64) は既存)
```

`Resources.hlsl`, appended after t34:

```hlsl
// ---- Temporal AA (t35-t36, TemporalAA.h / VelocityRendering.h) ----
Texture2D<float2> SceneVelocityTexture   : register(t35); // 0 = 未書き込み (カメラ運動へフォールバック)
Texture2D<float4> TemporalAADebugTexture : register(t36);
```

Nothing else in `InitRootSignature` changes: the SRV loop covers `CBV_COUNT..COUNT`. The new tables are pixel-visible (43 → 45 DWORDs). The static samplers are unchanged.

### 3.7 TAA compute root signature (`FDefaultTemporalUpscaler::Init`, version 1.0, flags NONE)

| Root idx | Type | Register | TAA | Mitchell-Netravali | Self-test |
|---|---|---|---|---|---|
| 0 | root CBV | b0 | `FTemporalAAParameters` | `FMitchellNetravaliParameters` | slot 2 (unused content) |
| 1 | table, 1 SRV | t0 | `InputSceneColor` (R) | history (H) | dummy tex |
| 2 | table, 1 SRV | t1 | `SceneLinearDepth` (LinearDepth, RG32F) | dummy tex | dummy tex |
| 3 | table, 1 SRV | t2 | `SceneVelocity` (R16G16_UNORM), or the dummy tex when the velocity pass did not run (reads 0 = not written) | dummy tex | dummy tex |
| 4 | table, 1 SRV | t3 | `HistoryBuffer` (Hp) or dummy tex | dummy tex | dummy tex |
| 5 | table, 1 SRV | t4 | `EyeAdaptationBuffer` (`Buffer<float>`) or dummy EA | dummy EA | dummy EA |
| 6 | table, 1 SRV | t5 | `ResponsiveAAMask` (R8_UNORM) when drawn this frame, otherwise the dummy tex (the flag is clear, so it is not read) | dummy tex | dummy tex |
| 7 | table, 1 UAV | u0 | `OutComputeTex` (H) | `OutputTexture` (S) | `RWStructuredBuffer<float>` (64 floats) |
| 8 | table, 1 UAV | u1 | `OutComputeTexDownsampled` or dummy UAV | dummy UAV | dummy UAV |
| 9 | table, 1 UAV | u2 | `DebugOutput` or dummy UAV | dummy UAV | dummy UAV |

- Static samplers: `s0` = `MIN_MAG_MIP_POINT`, CLAMP. `s1` = `MIN_MAG_MIP_LINEAR`, CLAMP. Both have MipLODBias 0, visibility ALL, space 0.
- Size: 2 + 9 = 11 DWORDs.
- One descriptor per table, because the free-list heap does not guarantee contiguity (the repo convention).
- Every table is bound on every dispatch; dummies stand in where a resource is absent (keeps GBV silent).
- Before binding: `SetDescriptorHeaps(1, {SRV heap})` (the AutoExposure pattern). The graphics root bindings are not disturbed.

### 3.8 Pipeline states

| Name | VS / PS or CS | RTV formats | Blend / Cull / Depth | Notes |
|---|---|---|---|---|
| `LinearDepth`, `DeferredLighting`, `HeightFog`, `PostProcessTonemap`, `PostProcessBloom{Threshold,Downsample,Upsample}`, `PostProcessDOF{CoC,Blur,Composite}` | unchanged | unchanged | **Depth = None** | [FIX #615] Behaviour-preserving: none of them binds a DSV. |
| `Velocity` | VelocityVS / VelocityPS | `{R16G16_UNORM}` | Opaque / Back / DepthRead (LESS_EQUAL, no write), DepthBias 0 | DSV bound. Fallback DepthBias −4 (risk R1). |
| `VelocityTwoSided` | same | same | Opaque / None / DepthRead | |
| `VelocityMasked` | VelocityVS / VelocityMaskedPS | same | Opaque / Back / DepthRead | Used only when masked **and** BaseColor exists (the shadow rule). |
| `VelocityMaskedTwoSided` | same | same | Opaque / None / DepthRead | |
| `ResponsiveAA` | GeometryVS / ResponsiveAAPS | `{R8_UNORM}` | Opaque / Back / DepthRead | DSV bound; holds translucent depth. |
| `ResponsiveAATwoSided` | same | same | Opaque / None / DepthRead | |
| `PostProcessUpscale0..5` | DeferredVS / `PostProcessUpscale_{Nearest,Bilinear,Directional,CatmullRom,Lanczos,Gaussian}_PS` | `{R8G8B8A8_UNORM}` | Opaque / Back / **None** | Back-buffer target. The index equals `r.Upscale.Quality`. Created **optional** (`bOptional = true`). |
| `VisualizeTemporalAA` | DeferredVS / VisualizeTemporalAAPS | `{R8G8B8A8_UNORM}` | Opaque / Back / **None** | Back-buffer target. Created **optional**. |
| TAA CS ×11 | `TemporalAA_*_CS.cso` | – | – | compute RS (§3.7) |
| `TemporalAAMitchellNetravali` | CS | – | – | compute RS |
| `TemporalAASelfTest` | CS | – | – | compute RS |

- `PostProcessTonemap` is reused unchanged for `m_TonemapOutput`, which has the same RGBA8 format, and for the split view.
- Compute PSOs use Lumen's `TryCreateComputePipeline` pattern: a missing `.cso` logs and leaves the PSO null. `IsReady()` then returns false and the frame renders without TAA (logged once). This avoids the known assert-only crash class.
- **Optional graphics PSOs.** Today `LoadShaderBytecode` asserts in Debug and, in Release, hands `{nullptr, 0}` to `CreateGraphicsPipelineState`. A null PS is legal there, so a missing pixel shader still yields a **non-null** PSO that draws nothing, and a null check cannot detect it. The new path is `CreatePipeline(..., bool bOptional)`:
  - When `bOptional` is set and either `.cso` is missing or empty, it logs once and returns `nullptr` without asserting and without creating a PSO. `LoadShaderBytecode` gains a `bool bAssertOnMissing` parameter for this.
  - `RenderManager::HasPipelineState(name)` is true only for a registered, non-null PSO.
  - `PostProcessUpscale0..5` and `VisualizeTemporalAA` are created optional. Their callers check `HasPipelineState` before `SetPipelineState` (§4.7, §6.7, §6.8). Every other graphics PSO stays required, with today's behaviour.

### 3.9 Resources

| Resource | Owner | Format | Extent | Flags | Rest state | (Re)created |
|---|---|---|---|---|---|---|
| Scene depth | RenderManager | R32_TYPELESS (DSV D32 in the existing 1-slot heap; SRV R32F) | R | DS | DEPTH_WRITE between frames | R changes |
| GBufferC/A/B, Substrate0/1, LinearDepth, SceneColor, SceneColorCopy, PrevSceneColor, PrevLinearDepth | FSceneTextures | unchanged | **R** | RT | unchanged | R changes |
| **Velocity** | FSceneTextures | R16G16_UNORM, clear value (0,0,0,1) | R | RT | RD | R changes |
| **ResponsiveAAMask** | FSceneTextures | R8_UNORM | R | RT | RD | R changes |
| DOFPrep/Ping/Blur | FSceneRenderer | RGBA16F | ((R.x+1)/2, (R.y+1)/2) | RT | PSR | R changes |
| DOFSharp | FSceneRenderer | RGBA16F | R | RT | PSR | R changes |
| TAA history pool ×2 | `FSceneViewState` | RGBA16F or R11G11B10_FLOAT | H | RT+UAV | tracked (§4.11) | lazily per slot on extent/format mismatch; released while AA is off (`!bTemporalAA`) |
| TAA half-res | FDefaultTemporalUpscaler | RGBA16F | (ceil(H.x/2), ceil(H.y/2)) | RT+UAV | PSR | on mismatch; released when the Downsample permutation is not used or AA is off |
| MN output | FDefaultTemporalUpscaler | RGBA16F | S | RT+UAV | PSR | on mismatch; released outside MainSuperSampling or when AA is off |
| TAA debug | FDefaultTemporalUpscaler | RGBA16F | H | RT+UAV | RD | CS debug view active, on mismatch; released when no CS debug view is active or AA is off |
| Dummy tex | FDefaultTemporalUpscaler | RGBA16F 1×1, cleared to (0,0,0,1) at Init, which is the optimized clear value of `CreateRenderTarget` (no #820); RG = 0 still means "velocity not written" and R = 0 "not responsive" | 1 | RT | RD | once |
| Dummy UAV tex | FDefaultTemporalUpscaler | RGBA16F 1×1 | 1 | RT+UAV | UNORDERED_ACCESS | once |
| Dummy eye adaptation | FDefaultTemporalUpscaler | UPLOAD buffer, float[2] = {1,1}; typed SRV R32_FLOAT, 2 elements | 8 B | – | GENERIC_READ | once |
| Self-test buffer + readback | FDefaultTemporalUpscaler | 64 floats DEFAULT (UAV) + READBACK | 256 B | UAV | COMMON: created COMMON (a buffer ignores any other initial state, #1328), promoted to UAV by the dispatch, UAV → COPY_SOURCE → UAV around the copy, and decays to COMMON again after each `ExecuteCommandLists` | once |
| TAA CB ring | FDefaultTemporalUpscaler | UPLOAD | 2 × 4 × 512 B | – | GENERIC_READ | once |
| Bloom mips/ups | FSceneRenderer | RGBA16F | from `O/2`, halved per level (floor, ≥1) | RT | PSR | once (constructor; O never changes) |
| TonemapOutput | FSceneRenderer | R8G8B8A8_UNORM | actual post input size | RT | PSR | lazily when upscale is needed and the size differs |
| Lumen screen textures | FLumenSceneData | unchanged | from R (probes `ceil(R/16)`) | UAV | unchanged | R changes |
| Fog volumes ×5 | FVolumetricFog | RGBA16F 3D | ceil(R/8) × 64 | UAV | unchanged | R changes |
| Light-grid buffers | FLightGridInjection | unchanged | **capacity** `ceil(2·O/64)` × 32 cells = 60×34×32 | UAV | unchanged | once (Init) |
| AutoExposure result | AutoExposure | raw R32 UAV / R32F SRV, 2 floats | 8 B | UAV | **COMMON between command lists** (buffer decay); **RD** within a frame after `PrepareResultForRead()` (§4.12) | – |
| Screenshot readback | FScreenshotCapture | READBACK buffer | O footprint (7680 × 1080 B) | – | – | first capture |

### 3.10 Descriptor and memory budget

- **Shader-visible heap:** 10000 slots, of which 6001 are used by the constant ring. The new permanent descriptors number about 32: 2 history × (SRV + UAV), half-res, MN, debug, 2 dummies (SRV + UAV), EA dummy, velocity, mask, self-test UAV.
- A resize frees and reallocates about 60 SRV/UAV and 20 RTV slots through the deferred queue. The leak check (§9.4) requires the free counts to return to their baseline.
- **New VRAM at the defaults** (TAAU 100 %, 1080p): history 2 × 15.8 MB + velocity 8.3 MB + mask 2.1 MB + LightGrid capacity +19 MB ≈ 61 MB. The debug view adds 15.8 MB when active.
- At HSP 200 the history is 2 × 63 MB. At SP 200 the render-resolution set is about 1.6 GB, which is why the spatial range stops at 200 %.


---

## 4. Frame flow

### 4.1 Pass order with extents, matrices and transitions

Notation:
- `O`, `R`, `S`, `H`, `P` are the extents of §0.1 decision 2.
- "jit" means jittered.
- **RD** = `PIXEL|NON_PIXEL_SHADER_RESOURCE`, **PSR** = `PIXEL_SHADER_RESOURCE`, **NPSR** = `NON_PIXEL_SHADER_RESOURCE`.

| # | Function | Work | Extent / viewport | b0 | New or changed transitions |
|---|---|---|---|---|---|
| 1 | `FSceneRenderer::BeginFrame` (top) | (0) Per-frame validity reset: `m_bVelocityValid = false; m_bResponsiveMaskValid = false;`. This comes first because `RenderTranslucency` returns early without calling `RenderResponsiveAAMask` when `Scene == nullptr` or no translucency is visible (`SceneRenderer.cpp:1018, 1085-1086`), and because a resize in (c) reallocates both textures without clearing them. (a) `m_Screenshot->ResolvePending()`: if a capture was recorded in the previous frame, `WaitGPU` and write the BMP (§9.3). (b) If `m_TAADebug.bRequestSelfTest`: `RunTemporalAASelfTests` (§9.2; the GPU part uses `FlushAndResetCommandList`). (c) `PrepareViewRectsForRendering()` (§4.3): `ComputeViewFamilyInfo`, PSO fallback, `ResizeRenderTargets` if `R` or `P` changed (§5), `m_LightGrid->SetViewSize(R)`, `m_RHI->SetDefaultViewportSize(R)`. | – | – | only on resize: the initial barriers of the new targets |
| 2 | `BeginFrame` | `m_RHI->BeginFrame()` (heap, RS, **viewport R**, ring reset). G-buffer RD→RT, clear, depth clear. ImGui NewFrame. | R | – | unchanged |
| 3 | `RenderBasePass` | `PrepareViewStateForVisibility(View)` (§4.4), which fills b0. Then `SetupLightConstants`, `InitFogConstants`, upload b0/b3, `ResolvePostProcessSettings` (**`SetTexelSize(R)`**), upload b4, `ComputeViewVisibility` with **`ViewProjectionNoAAMatrix`**, then the base-pass draws (GeometryPS uses `SampleBias`). | R (default) | jit cur, jit prev, NoAA C2P | – |
| 4 | **`RenderVelocities`** (new; `GameManager::Draw` calls it right after `RenderBasePass`) | §4.5 | R (inherited) | same b0 | Velocity RD→RT, clear (0,0,0,1), draws (DSV bound, LESS_EQUAL, no write), RT→RD |
| 5 | `RenderShadowDepths` | unchanged, except that the viewport restore at `ShadowRendering.cpp:689-694` becomes `m_RHI->RestoreDefaultViewport()` | shadow maps, then R | light views (`VIEW_CONSTANT{}`) | – |
| 6 | `RenderLumenScene` | unchanged, except that the card-capture restore at `LumenScene.cpp:1123-1129` becomes `m_RHI->RestoreDefaultViewport()`. `MakeLumenFrameInputs` (§4.9): `ScreenWidth/Height = R`, **jittered** previous VP, validity rule | atlas tiles, then R | card views | – |
| 7 | `RenderLighting` | b0 re-upload. LightGrid (dimensions from `SetViewSize`, `ScreenWidth/Height = R`). Fog (`ScreenSize = R`, previous VP **NoAA**, validity rule). LinearDepth, Lumen screen GI (R), Deferred, HeightFog. | R | jit | unchanged |
| 8 | `RenderTranslucency` | `ApplyRenderViewport()` at the top (R). b4 texel 1/R. Draws. Then **`RenderResponsiveAAMask`** (§4.6, S7) while the DSV is still bound. | R | jit | Mask RD→RT, clear, draws, RT→RD |
| 9 | `RenderPostProcessing` | §4.7: `m_AutoExposure->PrepareResultForRead()` (COMMON → RD, §4.12), `CopySceneColorHistory` (textures only), DOF at R, **TAA** (+ MN), AutoExposure, Bloom, LUT, Tonemap (+ spatial upscale or merged), split view, visualize, screenshot copy, depth → DEPTH_WRITE, **`CommitViewState`** | R → H → S = P → O | – | §4.11 |
| 10 | `ImGuiManager::Draw` | UI build only. **Skipped in `-taatest` mode.** | – | – | – |
| 11 | `EndFrame` | **New first step:** `SetDescriptorHeaps(SRV heap)`, `OMSetRenderTargets(back-buffer RTV)`, `RSSetViewports/ScissorRects(O)`. Then ImGui, BB RT→PRESENT, `Present`. | O | – | unchanged |
| 12 | `GameManager::Draw` end | `m_TestDriver.PostFrame()` (§9.3) | – | – | – |

**Viewport rule.**
- `RenderManager`'s default viewport becomes the **render extent**. It is set every frame in step 1, so `BeginFrame`, `FlushAndResetCommandList`, and the Shadow/Lumen restores all return to `R`.
- The passes that never set a viewport (base, velocity, LinearDepth, Deferred, HeightFog, translucency) therefore run at `R` automatically.
- Every pass after the TAA sets its viewport explicitly: Bloom per mip, Tonemap at the post extent, Upscale/Visualize at `O`.
- `ApplyRenderViewport()` is a private `FSceneRenderer` helper that calls `m_RHI->RestoreDefaultViewport()`.

**Texel-size rule (b4 `SceneTexelSizeX/Y`):**

| When | Value |
|---|---|
| `ResolvePostProcessSettings` (base pass and translucency upload) | 1/R |
| DOF | 1/half (existing), then restored to 1/R |
| Bloom | per mip (existing), then restored to 1/postExtent |
| Tonemap upload | 1/postExtent (TonemapPS does not read it) |
| Upscale pass | 1/input extent |
| Visualize pass | 1/O |

### 4.2 `ComputeViewFamilyInfo` (UE `FLegacyScreenPercentageDriver` + `PrepareViewRectsForRendering` + `FDefaultTemporalUpscaler` config selection)

```cpp
// ScreenPercentage.cpp
FViewFamilyInfo ComputeViewFamilyInfo(const FAntiAliasingParams& p, XMUINT2 O, bool bR11G11B10Supported)
{
    const FTAAStageGateValues g = { TAAStageGates::kScreenPercentage, TAAStageGates::kTemporalAA,
                                    TAAStageGates::kTemporalUpsampling, TAAStageGates::kTAAExtras };
    return ComputeViewFamilyInfoEx(p, O, bR11G11B10Supported, g);   // 自己テスト T12 は全 true で Ex を直接呼ぶ
}

FViewFamilyInfo ComputeViewFamilyInfoEx(const FAntiAliasingParams& p, XMUINT2 O, bool bR11G11B10Supported, const FTAAStageGateValues& g)
{
    FViewFamilyInfo F; F.OutputExtent = O;

    // ---- 1. AA メソッド (未実装値の丸め: 1 FXAA / 3 MSAA -> None, 4 TSR -> TemporalAA) ----
    const int m = p.AntiAliasingMethod;
    EAntiAliasingMethod method = (m == 2 || m == 4) ? EAntiAliasingMethod::TemporalAA : EAntiAliasingMethod::None;
    if (!g.bTemporalAA) method = EAntiAliasingMethod::None;                        // [GATE] S5 まで常に None
    F.AntiAliasingMethod = method;
    F.bTemporalAA = (method == EAntiAliasingMethod::TemporalAA);

    // ---- 2. 一次スクリーンパーセンテージ方式 (UE: TAA でなければ Spatial へ自動フォールバック) ----
    const bool bTAAU = F.bTemporalAA && p.bTemporalAAUpsampling && g.bTemporalUpsampling;  // [GATE] S6
    F.PrimaryScreenPercentageMethod = bTAAU ? EPrimaryScreenPercentageMethod::TemporalUpscale
                                            : EPrimaryScreenPercentageMethod::SpatialUpscale;

    // ---- 3. 解像度率と ViewRect (UE ApplyResolutionFraction = CeilToInt) ----
    float f = std::isfinite(p.ScreenPercentage) ? p.ScreenPercentage / 100.0f : 1.0f;
    if (!g.bScreenPercentage) f = 1.0f;                                              // [GATE] S3
    f = bTAAU ? std::clamp(f, kMinTAAUpsampleResolutionFraction, kMaxTAAUpsampleResolutionFraction)
              : std::clamp(f, kMinSpatialResolutionFraction,     kMaxSpatialResolutionFraction);
    F.ResolutionFraction = f;
    // 浮動小数誤差で 1 増えないよう -1e-6 (1920 * 0.5 = 960 を 961 にしない)
    F.RenderExtent.x = std::max(1u, (unsigned)std::ceil((double)O.x * (double)f - 1e-6));
    F.RenderExtent.y = std::max(1u, (unsigned)std::ceil((double)O.y * (double)f - 1e-6));
    F.EffectivePrimaryResolutionFraction = (float)F.RenderExtent.x / (float)O.x;     // UE と同じく X で定義
    const XMUINT2 R = F.RenderExtent;

    if (!F.bTemporalAA)
    {
        F.SecondaryExtent = F.HistoryExtent = F.PostProcessExtent = R;
        F.bSpatialUpscale = (R.x != O.x || R.y != O.y);
        return F;
    }

    // ---- 4. TAA パス構成 (FDefaultTemporalUpscaler::AddPasses と同じ判定) ----
    F.SecondaryExtent = bTAAU ? O : R;                        // UE: TAAParameters.OutputViewRect (SetupViewRect)
    F.TAAPass    = bTAAU ? ETAAPassConfig::MainUpsampling : ETAAPassConfig::Main;
    F.TAAQuality = (ETAAQuality)std::clamp(p.TemporalAAQuality, 0, 3);
    F.HistoryUpscaleFactor = g.bTemporalUpsampling ? GetTemporalAAHistoryUpscaleFactor(p) : 1.0f;   // [GATE] S6
    if (F.HistoryUpscaleFactor > 1.0f)
    {
        // UE: Pass = MainSuperSampling, bUseFast = false, OutputViewRect = SecondaryRect * Factor (float -> int32 切り捨て)
        F.TAAPass = ETAAPassConfig::MainSuperSampling;
        F.TAAQuality = ETAAQuality::High;
        F.HistoryExtent = { (unsigned)((float)F.SecondaryExtent.x * F.HistoryUpscaleFactor),
                            (unsigned)((float)F.SecondaryExtent.y * F.HistoryUpscaleFactor) };
    }
    else
    {
        F.HistoryExtent = F.SecondaryExtent;
    }
    F.PostProcessExtent = F.SecondaryExtent;                 // SuperSampling は MN で S へ戻す

    const bool bLowOrMedium = (F.TAAQuality == ETAAQuality::Low || F.TAAQuality == ETAAQuality::Medium);
    const bool bSS = (F.TAAPass == ETAAPassConfig::MainSuperSampling);
    F.bTAADownsample    = g.bTAAExtras && F.TAAQuality == ETAAQuality::Low
                       && p.bTemporalAAAllowDownsampling && !bSS;                    // UE 5.x: bAllowDownsample && Quality == Low
    F.bR11G11B10History = g.bTAAExtras && p.bTemporalAAR11G11B10History && bR11G11B10Supported
                       && bLowOrMedium && !bSS;                                      // アンチゴースト (alpha) 不要の品質のみ [M]
    F.bSpatialUpscale   = (F.PostProcessExtent.x != O.x || F.PostProcessExtent.y != O.y);
    return F;
}

float GetTemporalAAHistoryUpscaleFactor(const FAntiAliasingParams& p)   // UE 同名関数 (Main にも適用 [H])
{
    const float v = std::isfinite(p.TemporalAAHistoryScreenPercentage) ? p.TemporalAAHistoryScreenPercentage : 100.0f;
    return std::clamp(v / 100.0f, 1.0f, 2.0f);
}
```

Resulting values at `O` = 1920×1080. These are checked in self-test T12, which uses the vectors of Appendix A.3.

| Method | SP | HSP | R | S | H | P | Pass | UF = H.x/R.x | N (jitter) | Mip bias | SAMPLE_DISTANCE threshold |
|---|---|---|---|---|---|---|---|---|---|---|---|
| None | 100 | – | 1920×1080 | = R | = R | 1920×1080 | – | – | – | 0 | – |
| None | 50 | – | 960×540 | = R | = R | 960×540 → spatial upscale | – | – | – | 0 | – |
| TAA, Upsampling off | 100 | 100 | 1920×1080 | 1920×1080 | 1920×1080 | 1920×1080 | Main | 1 | 8 (Gaussian) | 0 | – |
| TAA, Upsampling off | 71 | 100 | 1364×767 | 1364×767 | 1364×767 | 1364×767 → spatial | Main | 1 | 8 | 0 | – |
| TAA, Upsampling off | 100 | 200 | 1920×1080 | 1920×1080 | 3840×2160 | 1920×1080 | MainSuperSampling | 2 | 8 | 0 | – (VARIANCE) |
| TAAU | 100 | 100 | 1920×1080 | 1920×1080 | 1920×1080 | 1920×1080 | MainUpsampling | 1 | 8 (uniform) | −0.30 | 1.5100 |
| TAAU | 50 | 100 | 960×540 | 1920×1080 | 1920×1080 | 1920×1080 | MainUpsampling | 2 | 32 | −1.30 | 1.3000 |
| TAAU | 67 | 100 | 1287×724 | 1920×1080 | 1920×1080 | 1920×1080 | MainUpsampling | 1.4918 | 17 | −0.8771 | 1.4067 |
| TAAU | 71 | 100 | 1364×767 | 1920×1080 | 1920×1080 | 1920×1080 | MainUpsampling | 1.4076 | 15 | −0.7933 | 1.4244 |
| TAAU | 75 | 100 | 1440×810 | 1920×1080 | 1920×1080 | 1920×1080 | MainUpsampling | 1.3333 | 14 | −0.7150 | 1.4400 |
| TAAU | 150 | 100 | 2880×1620 | 1920×1080 | 1920×1080 | 1920×1080 | MainUpsampling | 0.6667 | 8 | −0.30 | 1.5800 |
| TAAU | 200 | 100 | 3840×2160 | 1920×1080 | 1920×1080 | 1920×1080 | MainUpsampling | 0.5 | 8 | −0.30 | 1.6150 |
| TAAU | 100 | 200 | 1920×1080 | 1920×1080 | 3840×2160 | 1920×1080 | MainSuperSampling | 2 | 8 | −0.30 | – (VARIANCE) |
| TAAU | 50 | 150 | 960×540 | 1920×1080 | 2880×1620 | 1920×1080 | MainSuperSampling | 3 | 32 | −1.30 | – (VARIANCE) |
| TAAU | 30 (clamped to 50) | 100 | 960×540 | 1920×1080 | 1920×1080 | 1920×1080 | MainUpsampling | 2 | 32 | −1.30 | 1.3000 |
| None | 300 (clamped to 200) | – | 3840×2160 | = R | = R | 3840×2160 → spatial downscale | – | – | – | 0 | – |

`SampleDistanceThreshold = 1.51 + (1.3 − 1.51)·(UF − 1)`, unclamped. It is only used by MainUpsampling.

### 4.3 `PrepareViewRectsForRendering()` (top of `BeginFrame`, before `m_RHI->BeginFrame()`)

```cpp
void FSceneRenderer::PrepareViewRectsForRendering()
{
    const XMUINT2 O = { (unsigned)m_RHI->GetBackBufferWidth(), (unsigned)m_RHI->GetBackBufferHeight() };
    FViewFamilyInfo F = ComputeViewFamilyInfo(m_AAParams, O, m_TemporalUpscaler->IsR11G11B10HistorySupported());

    // TAA の PSO (.cso) が揃っていなければ AA 無しの構成へフォールバック (クラッシュさせない)
    m_TAAStats.bFallbackMissingPSO = false;
    if (F.bTemporalAA && !m_TemporalUpscaler->IsReady(F))
    {
        FAntiAliasingParams q = m_AAParams; q.AntiAliasingMethod = 0;
        F = ComputeViewFamilyInfo(q, O, false);
        m_TAAStats.bFallbackMissingPSO = true;      // IsReady 内で初回のみログ出力
    }
    m_ViewFamily = F;

    if (m_TAADebug.bRequestReallocate ||
        F.RenderExtent.x != m_AllocatedRenderExtent.x || F.RenderExtent.y != m_AllocatedRenderExtent.y ||
        F.PostProcessExtent.x != m_AllocatedPostExtent.x || F.PostProcessExtent.y != m_AllocatedPostExtent.y)
    {
        ResizeRenderTargets(F);                     // §5.2 (GPU 同期を含む。稀なイベント)
    }

    m_LightGrid->SetViewSize(F.RenderExtent.x, F.RenderExtent.y);   // 容量確保済みグリッドの今フレーム次元
    m_RHI->SetDefaultViewportSize(F.RenderExtent.x, F.RenderExtent.y);
}
```

`FDefaultTemporalUpscaler::IsReady(F)`:
- It returns true when these PSOs are non-null: `m_PSO[F.TAAPass][F.TAAQuality][F.bTAADownsample]`; plus `m_PSOMitchellNetravali` when the pass is MainSuperSampling.
- On the first false it logs `"[TemporalAA] missing PSO (pass/quality/downsample) -> AA disabled"` through `OutputDebugStringA`, once per combination.

### 4.4 `PrepareViewStateForVisibility(const FSceneView& View)` (first call in `RenderBasePass`)

It replaces the b0 fill at `SceneRenderer.cpp:549-563`.

```cpp
namespace
{
    // 転置して格納 (C++ は転置前で保持し、HLSL へは転置して渡す規約)
    void StoreT(XMFLOAT4X4& Dst, const XMFLOAT4X4& Src) { XMStoreFloat4x4(&Dst, XMMatrixTranspose(XMLoadFloat4x4(&Src))); }
}

void FSceneRenderer::PrepareViewStateForVisibility(const FSceneView& View)
{
    FViewInfo& V = m_ViewInfo; FSceneViewState& S = m_ViewState;
    const FViewFamilyInfo& F = m_ViewFamily; const FAntiAliasingParams& p = m_AAParams;
    const XMUINT2 R = F.RenderExtent;

    V.ViewRectSize = R; V.UnscaledViewRectSize = F.OutputExtent;
    V.AntiAliasingMethod = F.AntiAliasingMethod;
    V.PrimaryScreenPercentageMethod = F.PrimaryScreenPercentageMethod;

    if (!View.bValid)
    {
        // 従来挙動: b0 は据え置き (前回値)。次の有効フレームは強制カメラカット
        V.bValid = false; V.bPrevViewInfoValid = false; V.bPrevTransformsReset = false;
        S.bForceCameraCut = true;
        return;
    }
    V.bValid = true;
    V.NearClip = View.NearClip; V.FarClip = View.FarClip;
    V.ViewMatrices.Init(View.ViewMatrix, View.ProjectionMatrix, View.ViewOrigin);   // NoAA (ジッタ 0)

    // ---- カメラカット (UE FSceneView::bCameraCut + レンダラ側の規則; §4.4 表) ----
    V.bCameraCut = View.bCameraCut || S.bForceCameraCut || !S.bPrevFrameViewInfoValid
                || (S.PrevAntiAliasingMethod != F.AntiAliasingMethod) || m_TAADebug.bRequestHistoryReset;
    S.bForceCameraCut = false;
    m_TAADebug.bRequestHistoryReset = false;

    // ---- テンポラルジッタ (UE 4.26 PreVisibilityFrameSetup / 5.x PrepareViewStateForVisibility) ----
    V.TemporalJitterPixels = { 0.0f, 0.0f }; V.TemporalJitterIndex = 0; V.TemporalJitterSequenceLength = 1;
    const bool bJitter = (F.bTemporalAA || m_TAADebug.bForceJitterWithoutTAA) && !m_TAADebug.bDisableJitter;
    if (bJitter)
    {
        const bool  bTAAU = (F.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale);
        const float f     = F.EffectivePrimaryResolutionFraction;
        const int   CVar  = p.TemporalAASamples;
        int N = CVar;
        if (bTAAU)          N = (int)((float)N * std::max(1.0f, 1.0f / (f * f)));  // 出力画素あたりのサンプル密度一定 (int32 代入 = 切り捨て)
        else if (CVar == 5) N = 4;                                                  // 圧縮プラス 4 サンプル
        N = std::clamp(N, 1, 255);

        int Index = (int)S.TemporalAASampleIndex + 1;
        if (Index >= N || V.bCameraCut) Index = 0;                                  // [M] UE 4.26/5.x はカットで 0 へ戻す
        if (m_TAADebug.OverrideTemporalIndex >= 0)
            Index = m_TAADebug.OverrideTemporalIndex % N;                           // r.TemporalAA.Debug.OverrideTemporalIndex (凍結: 状態は進めない)
        else
            S.TemporalAASampleIndex = (uint32_t)Index;

        const XMFLOAT2 s = ComputeTemporalAASample(bTAAU, CVar, N, Index, p.TemporalAAFilterSize);
        V.TemporalJitterPixels = s; V.TemporalJitterIndex = Index; V.TemporalJitterSequenceLength = N;
        // レンダー (入力) 解像度でクリップ空間へ (UE: SampleX * 2 / ViewRect.W, SampleY * -2 / ViewRect.H)
        V.ViewMatrices.HackAddTemporalAAProjectionJitter({ s.x * 2.0f / (float)R.x, s.y * -2.0f / (float)R.y });
    }

    // ---- 前フレーム情報 (フレーム中は不変のスナップショット) ----
    V.PrevViewInfo = S.PrevFrameViewInfo;
    V.bPrevViewInfoValid = S.bPrevFrameViewInfoValid && !V.bCameraCut;
    V.bPrevTransformsReset = false;
    if (V.bCameraCut)
    {
        V.PrevViewInfo.ViewMatrices = V.ViewMatrices;          // 静止画素のカメラモーション = 0 (ジッタ込みの今フレーム)
        V.PrevViewInfo.TemporalAAHistory.SafeRelease();         // TAA 履歴を読まない
    }
    else if (IsLargeCameraMovement(V.ViewMatrices, V.PrevViewInfo.ViewMatrices, p.CameraRotationThreshold, p.CameraTranslationThreshold))
    {
        V.PrevViewInfo.ViewMatrices = V.ViewMatrices;          // UE bPrevTransformsReset: 履歴は保持しクランプに任せる [M]
        V.bPrevTransformsReset = true;
    }

    // ---- ClipToPrevClip (NoAA x NoAA, row-vector: PrevClip = ThisClip * C2P) ----
    // ワールド絶対座標の VP を float で逆行列 x 積にすると、静止カメラでも |カメラ位置| に比例した
    // 再投影誤差が残る。UE と同じくカメラ相対 (Translated) で double 合成する (下記 ComputeClipToPrevClip)
    V.ClipToPrevClip = ComputeClipToPrevClip(V.ViewMatrices, V.PrevViewInfo.ViewMatrices);

    // ---- Automatic View Mip Bias (TemporalUpscale 時のみ; UE 4.26 は TAAU 分岐内で計算) ----
    float bias = 0.0f;
    if (F.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale)
    {
        bias = -std::max(-std::log2(F.EffectivePrimaryResolutionFraction), 0.0f) + p.ViewTextureMipBiasOffset;
        bias = std::max(bias, p.ViewTextureMipBiasMin);
        if (!std::isfinite(bias)) bias = 0.0f;
    }
    V.MaterialTextureMipBias = bias;
    V.StateFrameIndex = S.FrameIndex;

    // ---- b0 (§3.1)。光源 2 フィールドは直後の SetupLightConstants が書く ----
    VIEW_CONSTANT& c = m_ViewConstant;
    StoreT(c.View, V.ViewMatrices.ViewMatrix);
    StoreT(c.Projection, V.ViewMatrices.ProjectionMatrix);                  // ジッタ込み
    StoreT(c.InvViewProjection, V.ViewMatrices.InvViewProjectionMatrix);    // ジッタ込み
    c.WorldCameraOrigin = { View.ViewOrigin.x, View.ViewOrigin.y, View.ViewOrigin.z, 1.0f };
    c.NearFar = { View.NearClip, View.FarClip, 0.0f, 0.0f };
    StoreT(c.PrevViewProjection, V.PrevViewInfo.ViewMatrices.ViewProjectionMatrix);   // 前フレームのジッタ込み
    StoreT(c.ClipToPrevClip, V.ClipToPrevClip);
    const XMFLOAT2 jc = V.ViewMatrices.TemporalAAProjectionJitter, jp = V.PrevViewInfo.ViewMatrices.TemporalAAProjectionJitter;
    c.TemporalAAJitter = { jc.x, jc.y, jp.x, jp.y };
    c.TemporalAAParams = { (float)V.TemporalJitterIndex, (float)V.TemporalJitterSequenceLength, V.TemporalJitterPixels.x, V.TemporalJitterPixels.y };
    c.ViewSizeAndInvSize = { (float)R.x, (float)R.y, 1.0f / (float)R.x, 1.0f / (float)R.y };
    c.MaterialTextureMipBias = bias;
    c.MaterialTextureDerivativeMultiply = std::exp2(bias);
    c.StateFrameIndexMod8 = F.bTemporalAA ? (S.FrameIndex & 7u) : 0u;       // [PORT] AA 無効時は 0 (基準画像を保つ)
    c.StateFrameIndex = S.FrameIndex;
}
```

`FViewMatrices` (header-only, `ViewMatrices.h`):
- `Init` stores `View` and `ProjNoAA` into both `ProjectionNoAAMatrix` and `ProjectionMatrix`, sets the jitter to 0 and calls `RecomputeDerivedMatrices()`.
- `RecomputeDerivedMatrices` computes `VP = XMMatrixMultiply(View, Projection)`, `VPNoAA = XMMatrixMultiply(View, ProjectionNoAA)`, `InvVP = XMMatrixInverse(nullptr, VP)` and `InvVPNoAA = XMMatrixInverse(nullptr, VPNoAA)`.
  - This is the **same call sequence** as today's `RenderBasePass`. With jitter 0, b0 is therefore bit-identical to the baseline (S1 acceptance).
- `HackAddTemporalAAProjectionJitter(J)`: `assert(TemporalAAProjectionJitter == 0)`, then `ProjectionMatrix._31 += J.x; ProjectionMatrix._32 += J.y;`, store `J`, then `RecomputeDerivedMatrices()`.
- `HackRemoveTemporalAAProjectionJitter()` subtracts the jitter and recomputes.

`ComputeTemporalAASample(bTAAU, CVar, N, Index, FilterSize)` in `SceneViewState.cpp`:

```cpp
XMFLOAT2 ComputeTemporalAASample(bool bTAAU, int CVar, int N, int Index, float FilterSize)
{
    if (N == 1) return { 0.0f, 0.0f };                       // [PORT] UE は Gaussian #0 の定数オフセット。0 にして AA Off と厳密比較可能にする
    if (bTAAU)                                                // 一様分布 (入出力画素の整列が無いため) — パターン分岐より先に判定
        return { Halton(Index + 1, 2) - 0.5f, Halton(Index + 1, 3) - 0.5f };
    switch (CVar)                                             // UE 4.26: CVarTemporalAASamplesValue で分岐 (添字は % 長さで保護)
    {
    case 2: { static const float X[] = { -4/16.f, 4/16.f },               Y[] = { -4/16.f, 4/16.f };               return { X[Index % 2], Y[Index % 2] }; }
    case 3: { static const float X[] = { -2/3.f, 2/3.f, 0.f },            Y[] = { -2/3.f, 0.f, 2/3.f };            return { X[Index % 3], Y[Index % 3] }; }
    case 4: { static const float X[] = { -2/16.f, 6/16.f, 2/16.f, -6/16.f }, Y[] = { -6/16.f, -2/16.f, 6/16.f, 2/16.f }; return { X[Index % 4], Y[Index % 4] }; }
    case 5: { static const float X[] = { 0.f, 1/2.f, 0.f, -1/2.f },       Y[] = { -1/2.f, 0.f, 1/2.f, 0.f };       return { X[Index % 4], Y[Index % 4] }; } // 圧縮プラス (N = 4)
    default: break;
    }
    // 窓付きガウス (Box-Muller)。σ = 0.47 * FilterSize, 半径 0.5 で窓掛け
    const float u1 = Halton(Index + 1, 2), u2 = Halton(Index + 1, 3);
    const float Sigma = 0.47f * std::max(FilterSize, 0.01f);
    const float InWindow = std::exp(-0.5f * (0.5f / Sigma) * (0.5f / Sigma));
    const float Theta = 2.0f * XM_PI * u2;
    const float r = Sigma * std::sqrt(-2.0f * std::log((1.0f - u1) * InWindow + u1));
    return { r * std::cos(Theta), r * std::sin(Theta) };
}
```

`ComputeClipToPrevClip(Cur, Prev)` in `SceneViewState.cpp` builds the matrix camera-relative, in double (UE builds `ClipToPrevClip` from translated matrices for the same reason):
- Background: the `ViewMatrix` of `XMMatrixLookToLH` is `Translation(−O)·Rot` (row vectors; `CameraComponent.cpp:31`). The product `InvVP_NoAA(cur)·VP_NoAA(prev)` in float therefore inverts and multiplies matrices that hold the absolute camera position.
- For a **static** camera that product leaves a residual `C2P ≠ I` that grows with |O|. Our float32 emulation (FOV 45°, n 0.1, f 500, yaw 17°, pitch 10°; `calc/c2p_precision.py`) gives BackN ≈ 0.008 px at the default pose (0, 7, −15), 0.017 px at +150 m and 0.13 px at 1 km. The reviewer's emulation gives larger values; the exact size depends on the inverse algorithm, but the growth with |O| holds in both.
- The TAA turns a constant reprojection bias δ into a directional smear of roughly δ·(1−α)/α ≈ 24·δ at α = 0.04, until the neighbourhood clamp limits it.
- Algebraically, `C2P = InvProjNoAA(cur) · InvRot(cur) · Translation(O_cur − O_prev) · Rot(prev) · ProjNoAA(prev)`. Only the origin **difference** appears, so a static camera gives exactly `I`.

```cpp
// SceneViewState.cpp
namespace
{
    using FMatrix44d = std::array<std::array<double, 4>, 4>;         // row-vector, 転置前
    FMatrix44d ToDouble(const XMFLOAT4X4& M);                         // m[i][j] をそのまま double へ
    FMatrix44d Mul(const FMatrix44d& A, const FMatrix44d& B);         // A * B
    FMatrix44d Inverse(const FMatrix44d& M);                          // ガウス・ジョルダン (部分ピボット), double
    FMatrix44d RotationPart(const XMFLOAT4X4& View)                   // 上 3x3 のみ (平行移動行 = 0, _44 = 1)
    {
        FMatrix44d R{}; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R[i][j] = View.m[i][j];
        R[3][3] = 1.0; return R;
    }
}
XMFLOAT4X4 ComputeClipToPrevClip(const FViewMatrices& Cur, const FViewMatrices& Prev)
{
    // UE: ClipToPrevClip = InvTranslatedViewProj(cur) * Translation(PreViewTranslation 差) * TranslatedViewProj(prev)
    // 絶対座標を行列に入れず原点差分のみを使う -> 静止カメラで厳密に単位行列 (float 逆行列の |O| 比例誤差を除去)
    FMatrix44d T{}; for (int i = 0; i < 4; ++i) T[i][i] = 1.0;
    T[3][0] = (double)Cur.ViewOrigin.x - (double)Prev.ViewOrigin.x;
    T[3][1] = (double)Cur.ViewOrigin.y - (double)Prev.ViewOrigin.y;
    T[3][2] = (double)Cur.ViewOrigin.z - (double)Prev.ViewOrigin.z;
    const FMatrix44d C2P =
        Mul(Mul(Mul(Mul(Inverse(ToDouble(Cur.ProjectionNoAAMatrix)), Inverse(RotationPart(Cur.ViewMatrix))), T),
                RotationPart(Prev.ViewMatrix)), ToDouble(Prev.ProjectionNoAAMatrix));
    XMFLOAT4X4 Out;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) Out.m[i][j] = (float)C2P[i][j];
    return Out;                                                        // 転置前 (b0 / TAA CB へは従来どおり転置してアップロード)
}
```

- The result is still row-vector and untransposed, and it is still uploaded transposed to b0 (`StoreT`) and to the TAA CB. Every consumer is unchanged.
- The far rule still works: `d = Q` makes `(s, Q, 1)·InvProj` a point at infinity (w = 0), which the translation leaves alone, so the reprojection is rotation-only.
- On a cut or a large movement, `Prev = Cur`, so the result is exactly `I`.
- Self-test T4b (§9.2) checks it away from the origin.

**Culling on invalid frames.** `ComputeViewVisibility` calls `GetViewFrustumBounds(m_ViewFrustum, ViewProjectionNoAAMatrix, ...)` only when `m_ViewInfo.bValid`. On a `!bValid` frame, `m_ViewFrustum` keeps its last valid planes, which is the old behaviour through the stale b0. Before the first valid frame it is default-constructed with no planes, and `FConvexVolume::IntersectBounds` then accepts everything. Nothing is rasterized anyway, because b0 is still zero. `FViewMatrices` and `FViewInfo::ClipToPrevClip` are value-initialized to identity (§2.4), so no path reads indeterminate memory.

`IsLargeCameraMovement(Cur, Prev, RotDeg, TransM)` is the UE formula with row-vector DirectXMath matrices:
- Let `c = cos(RotDeg°)`.
- Columns 0/1/2 of the upper 3×3 of `ViewMatrix` are the camera right/up/forward axes in world space, exactly as UE's `GetColumn(j)`.
- The result is `dot(col0) < c || dot(col1) < c || dot(col2) < c || |Origin_cur − Origin_prev|² > TransM²`.

**Camera-cut sources** (every one ends up in `V.bCameraCut`):

| Source | Where it is set | Consumed |
|---|---|---|
| Game request `UWorld::RequestCameraCut()` (ImGui "Camera Cut Now", test driver) | `m_bCameraCutRequested` | `CalcSceneView` ORs it into `FSceneView::bCameraCut` and clears it |
| Camera-component latch `UCameraComponent::NotifyCameraCut()` (SettingsManager Apply/Reset of a camera, test driver `cut` scenario) | `m_bCameraCutPending` | `ACameraActor::Tick` **reads** it (`IsCameraCutPending()`) and calls `m_CameraController.ResetVelocity()`. `CalcSceneView` **consumes** it (`ConsumeCameraCut()`) |
| Active camera changed (including null → camera on the first frame) | `UWorld::m_LastViewCamera` compared in `CalcSceneView` | same frame |
| First frame, and first frame after a `ViewState` reset | `!S.bPrevFrameViewInfoValid` | renderer |
| `View.bValid` false → true | `S.bForceCameraCut` set on the invalid frame | renderer |
| AA-method change (None ↔ TAA) | `S.PrevAntiAliasingMethod != F.AntiAliasingMethod` | renderer |
| Debug "Reset History" button | `m_TAADebug.bRequestHistoryReset` (one-shot) | renderer |

A screen-percentage change is **not** a cut: the TAA history is resampled. Lumen and fog invalidate their own history for one frame through the `ViewRectSize` rule (§4.9).

### 4.5 Velocity

#### 4.5.1 Game side: `FSceneVelocityData` (in `FScene`, keyed by component)

```cpp
// VelocityRendering.cpp (FSceneVelocityData の実装)
bool FComponentVelocityData::HasVelocity() const
{
    const float* a = &LocalToWorld._11; const float* b = &PreviousLocalToWorld._11;
    for (int i = 0; i < 16; ++i) if (std::fabs(a[i] - b[i]) > 1.0e-4f) return true;   // UE FMatrix::Equals(…, 0.0001)
    return false;
}
void FSceneVelocityData::StartFrame()
{
    ++m_InternalFrameIndex;
    for (auto& kv : m_ComponentData) kv.second.PreviousLocalToWorld = kv.second.LocalToWorld;   // 前フレームの描画値 -> Prev
}
void FSceneVelocityData::Register(const UPrimitiveComponent* C, const XMFLOAT4X4& L2W)
{
    FComponentVelocityData& d = m_ComponentData[C];
    d.LocalToWorld = d.PreviousLocalToWorld = L2W; d.LastFrameUpdated = m_InternalFrameIndex; d.bTeleportPending = true;
}
void FSceneVelocityData::UpdateTransform(const UPrimitiveComponent* C, const XMFLOAT4X4& L2W)
{
    auto it = m_ComponentData.find(C); if (it == m_ComponentData.end()) { Register(C, L2W); return; }
    it->second.LocalToWorld = L2W; it->second.LastFrameUpdated = m_InternalFrameIndex;
    if (it->second.bTeleportPending) it->second.PreviousLocalToWorld = L2W;    // テレポート: 速度を出さない
}
void FSceneVelocityData::MarkTeleported(const UPrimitiveComponent* C)
{ auto it = m_ComponentData.find(C); if (it != m_ComponentData.end()) it->second.bTeleportPending = true; }
void FSceneVelocityData::EndFrameUpdates() { for (auto& kv : m_ComponentData) kv.second.bTeleportPending = false; }
void FSceneVelocityData::Remove(const UPrimitiveComponent* C) { m_ComponentData.erase(C); }
const FComponentVelocityData* FSceneVelocityData::Find(const UPrimitiveComponent* C) const
{ auto it = m_ComponentData.find(C); return (it != m_ComponentData.end()) ? &it->second : nullptr; }
```

Hooks in `Scene.cpp`:

| Call site | Operation |
|---|---|
| `FScene::AddPrimitive`, after the proxy is created | `m_VelocityData.Register(Primitive, info.Proxy->GetLocalToWorld())`. The spawn-time snapshot never produces velocity, because the first push is a teleport. |
| `FScene::RemovePrimitive` | `m_VelocityData.Remove(Primitive)` |
| `UpdateAllPrimitiveSceneInfos`, first statement | `m_VelocityData.StartFrame()`. This runs every frame, even when nothing is pushed, which fixes the stale-Prev hazard. |
| `UpdateAllPrimitiveSceneInfos`, after `SendRenderTransform()` in **both** loops (proxy re-create and transform-dirty) | `m_VelocityData.UpdateTransform(component, component->GetSceneProxy()->GetLocalToWorld())`. Read the proxy through `it->Proxy` in the first loop. The key is the component, so proxy re-creation keeps the history. |
| `UpdateAllPrimitiveSceneInfos`, last statement | `m_VelocityData.EndFrameUpdates()` |
| New `FScene::MarkPrimitiveTeleported(UPrimitiveComponent* C)` | `m_VelocityData.MarkTeleported(C)` |
| New `FScene::GetVelocityData() const` | returns `m_VelocityData` |

**Teleport marking.** `SettingsManager::ApplyComponent` is static (SettingsManager.h:299), so it reaches the world through the component:

```cpp
if (auto* primitive = dynamic_cast<UPrimitiveComponent*>(Component))
{
    if (Snap.bPrimitive) { /* 既存の Set* */ }
    if (UWorld* world = primitive->GetWorld()) world->GetScene()->MarkPrimitiveTeleported(primitive);   // UE bTeleport
}
if (auto* camera = dynamic_cast<UCameraComponent*>(Component))
{
    if (Snap.bCamera) { /* 既存の FOV / Near / Far */ }
    camera->NotifyCameraCut();                  // ラッチ: ACameraActor::Tick がコントローラ速度をリセットし、CalcSceneView が消費
}
```

`ApplyComponent` runs from `LoadAndApply` (startup), the Reset menus and the Details "Reset Actor" button. All of them are teleports.

#### 4.5.2 `PrimitiveHasVelocityForView` (UE 4.x `VelocityRendering.cpp`)

```cpp
bool PrimitiveHasVelocityForView(const FViewInfo& View, const FPrimitiveSceneProxy& Proxy, bool bDisableSmallObjectCull)
{
    if (View.bCameraCut) return false;                              // UE: カット時は速度を描かない
    if (bDisableSmallObjectCull || kMotionBlurPerObjectSize <= 0.0f) return true;
    const FBoxSphereBounds& b = Proxy.GetBounds();
    const XMFLOAT3 o = View.ViewMatrices.ViewOrigin;
    const float dx = b.Origin.x - o.x, dy = b.Origin.y - o.y, dz = b.Origin.z - o.z;
    const float LODFactorDistanceSquared = dx * dx + dy * dy + dz * dz;   // LODDistanceFactor = 1
    const float MinR = kMotionBlurPerObjectSize * 2.0f / 100.0f;           // = 0.01
    return b.SphereRadius * b.SphereRadius > MinR * MinR * LODFactorDistanceSquared;   // 画面上で小さ過ぎる物体はカメラモーションに任せる
}
```

- The skip is UE's default behaviour. An object smaller than 1 % of its distance falls back to camera motion (risk R10).
- `FTemporalAADebugSettings::bDisableVelocitySmallObjectCull` turns the skip off for comparison.

#### 4.5.3 `FSceneRenderer::RenderVelocities(FScene* Scene)` (`VelocityRendering.cpp`)

```cpp
void FSceneRenderer::RenderVelocities(FScene* Scene)
{
    m_TAAStats.NumVelocityDraws = 0;
    const bool bNeeded = m_ViewFamily.bTemporalAA || m_TAADebug.bForceVelocityPass
        || m_TAADebug.DebugView == ETemporalAADebugView::MotionVectors || m_TAADebug.DebugView == ETemporalAADebugView::VelocityMask
        || m_TAADebug.DebugView == ETemporalAADebugView::TemporalUpscalerIO;    // 可視化 1 / 2 / 4 は t35 を読む
    m_bVelocityValid = false;                                        // BeginFrame 先頭でもリセット済み (二重化)
    if (!Scene || !bNeeded || !m_ViewInfo.bValid) return;          // TAA / 可視化はこのフレーム ダミーを読む

    ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
    RENDER_TARGET* vel = m_SceneTextures.Velocity.get();
    TransitionReadToRenderTarget(cl, vel);                          // RD -> RT
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_RHI->GetDepthStencilViewHandle();   // 深度はベースパス直後の DEPTH_WRITE (テストのみ)
    cl->OMSetRenderTargets(1, &vel->RTVHandle, FALSE, &dsv);
    const FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };              // RG = 0 = 未書き込み。最適化クリア値 (0,0,0,1) と一致 (#820 回避)
    cl->ClearRenderTargetView(vel->RTVHandle, clear, 0, nullptr);

    if (!m_ViewInfo.bCameraCut)
    {
        const std::vector<FPrimitiveSceneInfo>& prims = Scene->GetPrimitives();
        const FSceneVelocityData& vd = Scene->GetVelocityData();
        for (size_t i = 0; i < prims.size(); ++i)
        {
            if (m_PrimitiveVisibilityMap[i] == 0) continue;
            const FPrimitiveSceneProxy* proxy = prims[i].Proxy.get();
            if (!proxy || !proxy->GetViewRelevance().HasOpaqueRelevance()) continue;
            const FComponentVelocityData* d = vd.Find(prims[i].Component);
            if (!d || !(d->HasVelocity() || proxy->AlwaysHasVelocity())) continue;          // PrimitiveHasVelocityForFrame
            if (!PrimitiveHasVelocityForView(m_ViewInfo, *proxy, m_TAADebug.bDisableVelocitySmallObjectCull)) continue;
            proxy->DrawVelocity(m_RHI, d->PreviousLocalToWorld);
            ++m_TAAStats.NumVelocityDraws;
        }
    }
    TransitionRenderTargetToRead(cl, vel);                          // RT -> RD
    m_bVelocityValid = true;
}
```

- `TransitionReadToRenderTarget` and `TransitionRenderTargetToRead` (RD ↔ RT) are `inline` functions in the header-only `SceneRenderingUtils.h` (§2.1), together with the existing `TransitionToRenderTarget`, `TransitionToShaderResource` and `SetViewportAndScissor`. S0 moves those out of the `SceneRenderer.cpp` anonymous namespace (`:27-57`). They cannot stay there, because `VelocityRendering.cpp` and `PostProcessUpscale.cpp` are separate translation units. All three `.cpp` files include the header.
- b0 is still the camera view uploaded by `RenderBasePass`. The viewport is still `R`. Depth is `DEPTH_WRITE`.
- New member `bool m_bVelocityValid`. When it is false, TAA binds the dummy texture at t2: its RG reads 0, which means "not written".

`FStaticMeshSceneProxy::DrawVelocity(RM, Prev)` follows `DrawShadowDepth`:

```cpp
void DrawVelocity(RenderManager* RM, const XMFLOAT4X4& PreviousLocalToWorld) const override
{
    if (!IsMeshValid()) return;
    UploadPrimitiveConstant(RM, &PreviousLocalToWorld);                 // b1 = LocalToWorld + PreviousLocalToWorld
    for (unsigned int i = 0; i < m_Mesh->GetSubsetCount(); ++i)
    {
        const FSlot& slot = ResolveSlot(i);
        const EBlendMode blend = slot.Mat.GetBlendMode();
        if (IsTranslucentBlendMode(blend)) continue;                    // UE 既定: 半透明は速度を書かない
        const bool bTwoSided = slot.Mat.IsTwoSided();
        if (IsMaskedBlendMode(blend) && slot.BaseColor != nullptr)      // シャドウ深度と同じ判定
        {
            RM->SetPipelineState(bTwoSided ? "VelocityMaskedTwoSided" : "VelocityMasked");
            RM->SetTexture(RenderManager::TEXTURE_TYPE::BASE_COLOR, slot.BaseColor.get());
            slot.Mat.Bind(RM);                                          // b2 (BlendMode / OpacityMaskClipValue)
        }
        else
        {
            RM->SetPipelineState(bTwoSided ? "VelocityTwoSided" : "Velocity");
        }
        m_Mesh->DrawSubset(i);
    }
}
```

- `FFieldQuadSceneProxy::DrawVelocity` has the same structure, following its `DrawShadowDepth`: the VB, topology, t0 + b2 for masked, and `DrawInstanced(4,1,0,0)`. It skips the draw when the material is translucent.
- `FPolygon2DSceneProxy` keeps the empty base implementation.

**Sky dome.** `ASky::Tick` copies the camera location every frame, so the dome's `LocalToWorld` changes whenever the camera translates.
- The dome opts out of the velocity pass: `UPrimitiveComponent::m_bRenderVelocity` (`SetRenderVelocity(false)` in the `ASky` constructor), snapshotted into `FPrimitiveSceneProxy::RendersVelocity()`, and `RenderVelocities` skips proxies that return false. UE's sky never writes velocity either.
- Because the dome translates with the camera, its true screen motion is rotation-only (UE's infinitely distant sky). Its depth clamps to 1 (`DepthClipEnable = FALSE`, dome far beyond `f`), so LinearDepth = f ≥ 0.999·f and the TAA derives exactly that rotation-only motion from depth through the far-pixel rule (`d = Q`, §6.5), under translation and rotation alike.
- Writing velocity for the dome (the earlier design) made every sky pixel store anti-ghost alpha = 1: skyline pixels were rejected every frame while the camera translated, and the whole sky history was discarded on the first frame the camera stopped (final review).

### 4.6 Responsive AA mask (S7; end of `RenderTranslucency`, before its final barriers)

```cpp
void FSceneRenderer::RenderResponsiveAAMask(const std::vector<const FPrimitiveSceneProxy*>& SortedTranslucent)
{
    m_bResponsiveMaskValid = false;                                  // 念のため。正規のリセットは BeginFrame 先頭 (早期 return 対策)
    if (!m_ViewFamily.bTemporalAA || !TAAStageGates::kTAAExtras) return;
    const bool bForce = m_TAADebug.bForceResponsiveAA;
    bool bAny = false;
    for (const FPrimitiveSceneProxy* p : SortedTranslucent) bAny |= p->HasResponsiveAATranslucency(bForce);
    if (!bAny) return;

    ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
    RENDER_TARGET* mask = m_SceneTextures.ResponsiveAAMask.get();
    TransitionReadToRenderTarget(cl, mask);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_RHI->GetDepthStencilViewHandle();   // 半透明プリパス深度を含む DEPTH_WRITE
    cl->OMSetRenderTargets(1, &mask->RTVHandle, FALSE, &dsv);
    const FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cl->ClearRenderTargetView(mask->RTVHandle, clear, 0, nullptr);
    for (const FPrimitiveSceneProxy* p : SortedTranslucent) p->DrawResponsiveAA(m_RHI, bForce);   // PSO ResponsiveAA[TwoSided]
    TransitionRenderTargetToRead(cl, mask);
    cl->OMSetRenderTargets(1, &m_SceneTextures.SceneColor->RTVHandle, TRUE, &dsv);   // 以降の既存処理が期待する OM へ戻す
    m_bResponsiveMaskValid = true;
}
```

- `DrawResponsiveAA` uploads b1 and draws only the translucent/additive subsets whose material has `bEnableResponsiveAA`, or all of them when `bForce`.
- The pass uses the same `GeometryVS`, b0 and b1 as the translucency draws, so LESS_EQUAL against the translucent prepass depth passes bit-exactly. That marks the front-most `BLEND_Translucent` layer, plus additive surfaces in front of it, which is the coverage UE gets from stencil bit 3.
- `RenderTranslucency` has two early returns before this call: `Scene == nullptr` (`SceneRenderer.cpp:1018`) and no visible translucency (`:1085-1086`). The reset inside this function therefore does not run on those frames. The flag is valid only because `BeginFrame` resets it every frame (§4.1 step 1). Without that reset, a frame with no translucency would keep last frame's `true`, bind a stale mask with `TAA_FLAG_RESPONSIVE_MASK_VALID`, and force BlendFinal = 0.25 where translucency used to be. After a resize, it would bind an uncleared texture.

### 4.7 `RenderPostProcessing` (new body)

```cpp
void FSceneRenderer::RenderPostProcessing()
{
    ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
    const XMUINT2 O = m_ViewFamily.OutputExtent, R = m_ViewFamily.RenderExtent;

    if (m_AutoExposure) m_AutoExposure->PrepareResultForRead(); // 結果バッファ COMMON (減衰) -> RD。TAA の t4 読みより前 (§4.12)
    CopySceneColorHistory();                                   // Lumen 用テクスチャコピーのみ (行列の確定は CommitViewState へ移動)
    if (m_FinalSettings.Flags & PP_FLAG_DOF) RenderDOF();      // R (§5.3: MaxBlurSize を R.x/O.x 倍)

    // ---- Temporal upscaler (UE: DOF の後, 目の順応 / Bloom の前) ----
    RENDER_TARGET* postInput = m_SceneTextures.SceneColor.get(); XMUINT2 postExtent = R;   // AA 無し / TAA 不実行時
    RENDER_TARGET* halfRes = nullptr; XMUINT2 halfExtent{};
    FTemporalAAHistory outHistory; bool bTAARan = false;
    if (m_ViewFamily.bTemporalAA && m_ViewInfo.bValid)
    {
        ITemporalUpscaler::FPassInputs in;
        in.bAllowDownsampleSceneColor = m_ViewFamily.bTAADownsample;
        in.SceneColorTexture      = m_SceneTextures.SceneColor.get();
        in.SceneDepthSRVIndex     = m_SceneTextures.LinearDepth->SRVIndex;              // 不透明のみ (§0.1 decision 9)
        in.SceneVelocitySRVIndex  = m_bVelocityValid ? m_SceneTextures.Velocity->SRVIndex : m_TemporalUpscaler->GetDummySRVIndex();
        in.bResponsiveMaskValid   = m_bResponsiveMaskValid;
        in.ResponsiveMaskSRVIndex = m_bResponsiveMaskValid ? m_SceneTextures.ResponsiveAAMask->SRVIndex : m_TemporalUpscaler->GetDummySRVIndex();
        in.bUseEyeAdaptationBuffer = (m_FinalSettings.Flags & PP_FLAG_AUTO_EXPOSURE) && m_AutoExposure && m_AutoExposure->IsResultValid();
        in.EyeAdaptationSRVIndex  = in.bUseEyeAdaptationBuffer ? m_AutoExposure->GetExposureSRVIndex() : m_TemporalUpscaler->GetDummyEyeAdaptationSRVIndex();
        in.ManualExposure         = m_FinalSettings.Exposure;                            // = 2^EV (手動露出)
        in.bForceHistoryBypass    = IsPreTAADebugViewActive();                           // Lumen / LightGrid DebugMode 中は履歴を使わない
        const ITemporalUpscaler::FPassOutputs out =
            m_TemporalUpscaler->AddPasses(m_ViewInfo, m_ViewFamily, m_ViewState, m_AAParams, m_TAADebug, in);
        if (out.SceneColor)
        {
            postInput = out.SceneColor; postExtent = out.SceneColorExtent;
            halfRes = out.HalfResSceneColor; halfExtent = out.HalfResExtent;
            outHistory = out.NewHistory; bTAARan = true;
        }
    }
    m_TAAStats.bTAARanThisFrame = bTAARan; m_TAAStats.PostExtent = postExtent;

    // ---- 目の順応 (入力 = TAA 出力 or ハーフ解像度。入口 PSR 契約) ----
    RENDER_TARGET* aeIn = halfRes ? halfRes : postInput; const XMUINT2 aeExt = halfRes ? halfExtent : postExtent;
    if (m_AutoExposure) m_AutoExposure->Dispatch(aeIn->Resource.Get(), aeIn->SRVIndex, aeExt.x, aeExt.y, Time::GetDeltaTime());

    // ---- Bloom (チェーンは O/2 固定。入力は UV 参照 + しきい値パスの 4 タップボックス前置フィルタ) ----
    if (m_FinalSettings.Flags & PP_FLAG_BLOOM) RenderBloom(halfRes ? halfRes : postInput, postExtent);
    if (m_ColorGradingLUTBaker) m_ColorGradingLUTBaker->UpdateIfDirty(m_FinalSettings);

    // ---- Tonemap (+ 一次空間アップスケール)。サイズは実テクスチャから (§0.1 decision 12) ----
    const bool bNeedsUpscale = (postExtent.x != O.x || postExtent.y != O.y);
    const char* upscalePSO = bNeedsUpscale ? SelectPrimaryUpscalePipeline() : nullptr;   // 欠落時 1 (bilinear) へ, それも無ければ null
    const bool bMerge = bNeedsUpscale && (ShouldMergeTonemapWithUpscale(m_AAParams, postExtent, O) || upscalePSO == nullptr);
    SetTexelSize((int)postExtent.x, (int)postExtent.y);
    UploadPostProcessConstant();
    TransitionBackBuffer(cl, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE bb = m_RHI->GetCurrentBackBufferRTV();
    const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!bNeedsUpscale || bMerge)
    {
        cl->OMSetRenderTargets(1, &bb, TRUE, nullptr); cl->ClearRenderTargetView(bb, black, 0, nullptr);
        SetViewportAndScissor(cl, (int)O.x, (int)O.y);           // 結合時は TonemapPS の UV サンプリング (s1) が bilinear アップスケールを兼ねる
        DrawTonemap(postInput);
    }
    else
    {
        EnsureTonemapOutput(postExtent);                          // RGBA8_UNORM, 実サイズ不一致時のみ再確保 (PSR)
        TransitionToRenderTarget(cl, m_TonemapOutput.get());
        cl->OMSetRenderTargets(1, &m_TonemapOutput->RTVHandle, TRUE, nullptr);
        cl->ClearRenderTargetView(m_TonemapOutput->RTVHandle, black, 0, nullptr);
        SetViewportAndScissor(cl, (int)postExtent.x, (int)postExtent.y);
        DrawTonemap(postInput);
        TransitionToShaderResource(cl, m_TonemapOutput.get());
        cl->OMSetRenderTargets(1, &bb, TRUE, nullptr); cl->ClearRenderTargetView(bb, black, 0, nullptr);
        AddPrimaryUpscalePass(m_TonemapOutput.get(), postExtent, O, upscalePSO); // §6.7 (ビューポート O)
    }
    m_TAAStats.bUpscaleMerged = bMerge;

    // ---- デバッグ表示 ----
    if (m_TAADebug.DebugView == ETemporalAADebugView::InputOutputSplit && bTAARan)
    {
        // 左半分に TAA 入力 (ジッタ込み SceneColor) を同じトーンマップで描き直す (Bloom / LUT / 露出は共通 = 厳密比較)
        const D3D12_RECT left = { 0, 0, (LONG)(O.x / 2), (LONG)O.y };
        SetViewportAndScissor(cl, (int)O.x, (int)O.y); cl->RSSetScissorRects(1, &left);
        DrawTonemap(m_SceneTextures.SceneColor.get());
        SetViewportAndScissor(cl, (int)O.x, (int)O.y);
    }
    if (m_TAADebug.DebugView != ETemporalAADebugView::Off) AddVisualizeTemporalAAPass(postInput, bTAARan);   // §6.8, BB, ビューポート O

    // ---- スクリーンショット (UI 無しのバックバッファ) ----
    if (m_Screenshot->IsRequested()) { m_Screenshot->RecordCopy(cl, m_RHI->GetCurrentBackBufferResource(), O); cl->OMSetRenderTargets(1, &bb, TRUE, nullptr); }

    // ---- 深度: SRV -> DEPTH_WRITE (既存。TAA / 可視化が読み終えた後) ----
    cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_RHI->GetDepthBufferResource(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE));

    CommitViewState(bTAARan ? outHistory : FTemporalAAHistory{});      // 最後: 次フレームの PrevViewInfo を確定
}
```

Helper semantics:

| Helper | Definition |
|---|---|
| `DrawTonemap(RENDER_TARGET* In)` | The existing tonemap block (`SceneRenderer.cpp:1299-1321`) with `t0 = In`: PSO `PostProcessTonemap`, t9 `m_BloomUp[0]`, t10 LUT, t11 AutoExposure result, `DrawScreenPass()`. |
| `TransitionBackBuffer(cl, before, after)` | A one-line wrapper around the existing back-buffer barrier (`SceneRenderer.cpp:1286-1290`). |
| `ShouldMergeTonemapWithUpscale(p, In, O)` | UE `r.Tonemapper.MergeWithUpscale`. Mode 0 → false; 1 → true; 2 → `(In.x·In.y)/(O.x·O.y) > Threshold`. A merged upscale is always bilinear, whatever `r.Upscale.Quality` says (UE). |
| `SelectPrimaryUpscalePipeline()` | Returns `"PostProcessUpscale<q>"` with `q = clamp(r.Upscale.Quality, 0, 5)` when `m_RHI->HasPipelineState` says it exists. Otherwise it returns `"PostProcessUpscale1"` (bilinear) if that exists, or `nullptr`. Each fallback logs once. With `nullptr` the frame takes the merged path, where the tonemap's bilinear `s1` does the upscale, so a missing `.cso` never leaves the back buffer black (§3.8). |
| `RenderBloom(RENDER_TARGET* Input, XMUINT2 RestoreExtent)` | The existing body with the threshold pass reading `t0 = Input`. The final viewport/texel restore goes to `RestoreExtent`. The chain is `m_BloomMip/Up` fixed at `O/2` (§5.3). |
| `IsPreTAADebugViewActive()` | `m_LumenScene->GetParams().DebugMode != 0` or `m_LightGrid->GetParams().DebugMode != 0` |

**Why sizes come from the actual texture.** `postExtent` is always the extent of the texture that the next pass reads, and the planned `m_ViewFamily.PostProcessExtent` is only used for allocation (Bloom). A frame where the TAA does not run therefore still upscales correctly. That happens when the camera is invalid or `AddPasses` returns no output.

### 4.8 Temporal upscaler: `FDefaultTemporalUpscaler`

#### 4.8.1 `Init()`

1. **Root signature** of §3.7. There are 10 parameters. The static samplers are `s0` point clamp and `s1` linear clamp, both with MipLODBias 0.
2. **PSOs** use the Lumen `TryCreateComputePipeline` pattern: a missing file is logged and leaves the PSO null. The `m_PSO[Pass][Quality][Downsample]` table maps to these wrappers:

   | Pass (index) | Low [0] | Medium [1] | High [2] | MediumHigh [3] |
   |---|---|---|---|---|
   | Main [0], Downsample 0 | `TemporalAA_Main_Low_CS` | `…_Main_Medium_CS` | `…_Main_High_CS` | `…_Main_MediumHigh_CS` |
   | Main [0], Downsample 1 | `TemporalAA_Main_Low_Downsample_CS` | – | – | – |
   | MainUpsampling [1], Downsample 0 | `TemporalAA_Upsampling_Low_CS` | `…_Upsampling_Medium_CS` | `…_Upsampling_High_CS` | `…_Upsampling_MediumHigh_CS` |
   | MainUpsampling [1], Downsample 1 | `TemporalAA_Upsampling_Low_Downsample_CS` | – | – | – |
   | MainSuperSampling [2] | – | – | `TemporalAA_SuperSampling_CS` | – |

   Plus `TemporalAAMitchellNetravali_CS` and `TemporalAASelfTest_CS`.
3. **CB ring.** `m_ParamBuffer[2]` are UPLOAD buffers of 4 × 512 B each, persistently mapped. Slot 0 is TAA, 1 is MN, 2 is the self-test, 3 is spare. The frame index comes from `m_RHI->GetCurrentFrameIndex()`.
4. **Dummies:**
   - `m_DummyTex` = `CreateRenderTarget(1,1,RGBA16F)`, cleared to **(0,0,0,1)** through its RTV at Init (PSR → RT → clear → RD), then resting in RD. `GetDummySRVIndex()` returns its SRV. (0,0,0,1) is `CreateRenderTarget`'s optimized clear value (`RenderManager.cpp:966-970`), so the clear raises no WARNING #820, as for Velocity (judge #36). RG = 0 still reads as "velocity not written" and R = 0 as "not responsive". A = 1 is never read: the dummy is bound as history only when `bCameraCut = 1`, and then `HISTORY_HAS_ALPHA` is clear.
   - `m_DummyUAVTex` = `CreateRenderTarget(1,1,RGBA16F,1,true)` transitioned PSR → UNORDERED_ACCESS once, then resting in UAV forever.
   - `m_DummyEyeAdaptation`: an UPLOAD buffer of 8 B holding `{1,1}`, with a typed SRV `R32_FLOAT`, 2 elements. `GetDummyEyeAdaptationSRVIndex()` returns it.
5. **Self-test buffer:** 64 floats in a DEFAULT buffer (UAV, structured stride 4), plus a READBACK buffer.
6. **R11G11B10 capability:**

   ```cpp
   D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{ DXGI_FORMAT_R11G11B10_FLOAT };
   m_bR11G11B10Supported = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))
                        && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
   ```

   It is required on FL11, but the check is cheap. A failure means `ComputeViewFamilyInfo` never selects R11G11B10.

#### 4.8.2 `AddPasses` (UE `FDefaultTemporalUpscaler::AddPasses`, 4.26 shape)

```cpp
ITemporalUpscaler::FPassOutputs FDefaultTemporalUpscaler::AddPasses(const FViewInfo& View, const FViewFamilyInfo& Family,
    FSceneViewState& ViewState, const FAntiAliasingParams& Params, const FTemporalAADebugSettings& Debug, const FPassInputs& In)
{
    FTAAPassParameters P;
    P.Pass = Family.TAAPass;  P.Quality = Family.TAAQuality;              // SuperSampling は High 強制済み
    P.bDownsample = In.bAllowDownsampleSceneColor && P.Quality == ETAAQuality::Low && P.Pass != ETAAPassConfig::MainSuperSampling;
    P.bUseR11G11B10History = Family.bR11G11B10History;
    P.bUpsampleFiltered = Params.bTemporalAAUpsampleFiltered || P.Pass != ETAAPassConfig::MainUpsampling;   // UE: TAA_UPSAMPLE_FILTERED = CVar || Pass != MainUpsampling
    P.SceneColorInput = In.SceneColorTexture;
    P.SceneDepthSRVIndex = In.SceneDepthSRVIndex; P.SceneVelocitySRVIndex = In.SceneVelocitySRVIndex;
    P.ResponsiveMaskSRVIndex = In.ResponsiveMaskSRVIndex; P.bResponsiveMaskValid = In.bResponsiveMaskValid;
    P.EyeAdaptationSRVIndex = In.EyeAdaptationSRVIndex; P.bUseEyeAdaptationBuffer = In.bUseEyeAdaptationBuffer;
    P.ManualExposure = In.ManualExposure;
    P.InputExtent = Family.RenderExtent; P.OutputExtent = Family.HistoryExtent;   // SetupViewRect (+ SuperSampling 拡大)
    P.CurrentFrameWeight = Params.TemporalAACurrentFrameWeight; P.FilterSize = Params.TemporalAAFilterSize;
    P.bCatmullRom = Params.bTemporalAACatmullRom;
    P.bForceHistoryBypass = In.bForceHistoryBypass;
    P.DebugView = Debug.DebugView; P.DebugScale = Debug.VisualizeScale; P.FilteredTemporalWeightMode = Debug.FilteredTemporalWeightMode;
    P.NearClip = View.NearClip; P.FarClip = View.FarClip;

    FPassOutputs Out;
    const FTAAOutputs o = AddTemporalAAPass(View, P, View.PrevViewInfo.TemporalAAHistory, &Out.NewHistory, ViewState);
    if (!o.SceneColor) return FPassOutputs{};                              // PSO 欠落 (IsReady 済みなので通常起きない)
    if (P.Pass == ETAAPassConfig::MainSuperSampling)
    {
        FTAATexture* mn = ComputeMitchellNetravaliDownsample(o.SceneColor, Family.SecondaryExtent);
        Out.SceneColor = mn->RT.get(); Out.SceneColorExtent = Family.SecondaryExtent;
    }
    else
    {
        Out.SceneColor = o.SceneColor->RT.get(); Out.SceneColorExtent = o.SceneColor->Extent;
        if (o.DownsampledSceneColor) { Out.HalfResSceneColor = o.DownsampledSceneColor->RT.get(); Out.HalfResExtent = o.DownsampledSceneColor->Extent; }
    }
    return Out;
}
```

`FDefaultTemporalUpscaler` gains two getters, `GetDummySRVIndex()` and `GetDummyEyeAdaptationSRVIndex()`. They are additions to the §2.4 declaration.

#### 4.8.3 `AddTemporalAAPass`

```cpp
FTAAOutputs FDefaultTemporalUpscaler::AddTemporalAAPass(const FViewInfo& View, const FTAAPassParameters& P,
    const FTemporalAAHistory& In, FTemporalAAHistory* Out, FSceneViewState& VS)
{
    ID3D12PipelineState* pso = m_PSO[(int)P.Pass][(int)P.Quality][P.bDownsample ? 1 : 0].Get();
    if (!pso) return {};
    const DXGI_FORMAT fmt = P.bUseR11G11B10History ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
    const XMUINT2 R = P.InputExtent, H = P.OutputExtent;

    // ---- 入力履歴 (UE: !InputHistory.IsValid() || bCameraCut ならダミー黒) ----
    FTAATexture* inTex = VS.GetHistoryTexture(In);
    const bool bInValid = In.IsValid() && inTex && inTex->RT
        && inTex->Extent.x == In.ReferenceBufferSize.x && inTex->Extent.y == In.ReferenceBufferSize.y && inTex->Format == In.Format;
    const bool bHistory = bInValid && !View.bCameraCut && !P.bForceHistoryBypass;
    const XMUINT2 Hp = bHistory ? In.ReferenceBufferSize : H;

    // ---- 出力スロット (入力と別のピンポン枠。サイズ / フォーマット不一致なら遅延解放 + 再確保) ----
    const int outSlot = bInValid ? (In.RTSlot ^ 1) : 0;
    FTAATexture& outTex = VS.TemporalAAHistoryPool[outSlot];
    if (!outTex.Matches(H, fmt)) outTex.Allocate(m_RHI, H, fmt, L"TemporalAA");

    // ---- 定数 (§3.4) ----
    FTemporalAAParameters cb{};
    cb.InputSceneColorSize   = { (float)R.x, (float)R.y, 1.0f / R.x, 1.0f / R.y };
    cb.InputMinMaxPixelCoord = { 0, 0, (int)R.x - 1, (int)R.y - 1 };
    cb.OutputViewportSize    = { (float)H.x, (float)H.y, 1.0f / H.x, 1.0f / H.y };
    cb.HistoryBufferSize     = { (float)Hp.x, (float)Hp.y, 1.0f / Hp.x, 1.0f / Hp.y };
    cb.HistoryBufferUVMinMax = { 0.5f / Hp.x, 0.5f / Hp.y, (Hp.x - 0.5f) / Hp.x, (Hp.y - 0.5f) / Hp.y };   // Off = 0, Ext = Buf (exact-size)
    cb.ScreenPosToHistoryBufferUV = { 0.5f, -0.5f, 0.5f, 0.5f };                                        // = (Ext*0.5/Buf, -Ext*0.5/Buf, (Ext*0.5+Off)/Buf ...)
    XMStoreFloat4x4(&cb.ClipToPrevClip, XMMatrixTranspose(XMLoadFloat4x4(&View.ClipToPrevClip)));
    cb.TemporalJitterPixels  = View.TemporalJitterPixels;
    cb.ScreenPosAbsMax       = { 1.0f - 1.0f / Hp.x, 1.0f - 1.0f / Hp.y };
    cb.ScreenPercentage      = (float)R.x / (float)H.x;
    cb.UpscaleFactor         = (float)H.x / (float)R.x;
    cb.CurrentFrameWeight    = P.CurrentFrameWeight;
    cb.HistoryPreExposureCorrection = 1.0f;                                                               // プリエクスポージャ無し
    cb.bCameraCut            = bHistory ? 0u : 1u;
    cb.Flags = (P.bUpsampleFiltered ? TAA_FLAG_UPSAMPLE_FILTERED : 0u)
             | (P.bResponsiveMaskValid ? TAA_FLAG_RESPONSIVE_MASK_VALID : 0u)
             | (P.bUseEyeAdaptationBuffer ? TAA_FLAG_EYE_ADAPTATION_BUFFER : 0u)
             | ((bHistory && inTex->Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? TAA_FLAG_HISTORY_HAS_ALPHA : 0u)
             | ((uint32_t)(P.FilteredTemporalWeightMode & 3) << TAA_FLAG_FTW_MODE_SHIFT)
             | (P.bDownsample ? TAA_FLAG_DOWNSAMPLE_OUTPUT : 0u);
    cb.ManualExposure = P.ManualExposure;
    cb.DebugMode = IsTemporalAADebugViewFromCS(P.DebugView) ? (uint32_t)P.DebugView : 0u;
    if (P.Pass == ETAAPassConfig::Main)
    {
        float sw[9], pw[5]; ComputeTemporalAASampleWeights(View.TemporalJitterPixels, P.FilterSize, P.bCatmullRom, sw, pw);
        for (int i = 0; i < 9; ++i) (&cb.SampleWeights[i >> 2].x)[i & 3] = sw[i];
        for (int i = 0; i < 5; ++i) (&cb.PlusWeights[i >> 2].x)[i & 3] = pw[i];
    }
    const XMFLOAT3 qe = ComputePixelFormatQuantizationError(fmt);
    cb.OutputQuantizationError = { qe.x, qe.y, qe.z, (fmt == DXGI_FORMAT_R11G11B10_FLOAT) ? 64512.0f : 65504.0f };   // w = 出力フォーマットの最大有限値
    const float n = P.NearClip, f = P.FarClip, Q = f / (f - n);
    cb.DepthParams = { Q, -Q * n, 0.999f * f, 0.0f };
    cb.StateFrameIndexMod8 = View.StateFrameIndex & 7u;            // 量子化ノイズは常に回す
    cb.DebugScale = P.DebugScale;
    cb.SampleDistanceThreshold = 1.51f + (1.3f - 1.51f) * (cb.UpscaleFactor - 1.0f);
    const unsigned frame = m_RHI->GetCurrentFrameIndex();
    std::memcpy(m_ParamPtr[frame] + 0 * 512, &cb, sizeof(cb));

    // ---- 補助出力 ----
    FTAATexture* half = nullptr;
    if (P.bDownsample) { const XMUINT2 he = { (H.x + 1) / 2, (H.y + 1) / 2 };
                         if (!m_HalfRes.Matches(he, DXGI_FORMAT_R16G16B16A16_FLOAT)) m_HalfRes.Allocate(m_RHI, he, DXGI_FORMAT_R16G16B16A16_FLOAT, L"TemporalAAHalfRes");
                         half = &m_HalfRes; }
    FTAATexture* dbg = nullptr;
    if (cb.DebugMode != 0u) { if (!m_DebugOutput.Matches(H, DXGI_FORMAT_R16G16B16A16_FLOAT)) m_DebugOutput.Allocate(m_RHI, H, DXGI_FORMAT_R16G16B16A16_FLOAT, L"TemporalAADebug");
                              dbg = &m_DebugOutput; }

    // ---- バリア (1 回のバッチ。同一リソースの重複 / before == after は除去) ----
    ID3D12GraphicsCommandList* cl = m_RHI->GetGraphicsCommandList();
    FBarrierBatch pre;
    pre.Add(P.SceneColorInput->Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (bHistory) pre.Track(*inTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre.Track(outTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (half) pre.Track(*half, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (dbg)  pre.Track(*dbg,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre.Flush(cl);

    // ---- ディスパッチ ----
    ID3D12DescriptorHeap* heap = m_RHI->GetSRVDescriptorHeap(); cl->SetDescriptorHeaps(1, &heap);
    cl->SetComputeRootSignature(m_RootSignature.Get());
    cl->SetPipelineState(pso);
    cl->SetComputeRootConstantBufferView(0, m_ParamBuffer[frame]->GetGPUVirtualAddress() + 0 * 512);
    auto t = [&](UINT root, unsigned idx) { cl->SetComputeRootDescriptorTable(root, m_RHI->GetGPUDescriptorHandle(idx)); };
    t(1, P.SceneColorInput->SRVIndex);
    t(2, P.SceneDepthSRVIndex);
    t(3, P.SceneVelocitySRVIndex);
    t(4, bHistory ? inTex->RT->SRVIndex : m_DummyTex->SRVIndex);
    t(5, P.EyeAdaptationSRVIndex);
    t(6, P.ResponsiveMaskSRVIndex);
    t(7, outTex.RT->UAVIndex);
    t(8, half ? half->RT->UAVIndex : m_DummyUAVTex->UAVIndex);
    t(9, dbg ? dbg->RT->UAVIndex : m_DummyUAVTex->UAVIndex);
    cl->Dispatch((H.x + 7) / 8, (H.y + 7) / 8, 1);

    // ---- 戻しバリア ----
    FBarrierBatch post;
    post.Track(outTex, P.Pass == ETAAPassConfig::MainSuperSampling ? (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                                                                   : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);   // Main/Upsampling: AutoExposure の入口 PSR 契約
    if (half) post.Track(*half, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (dbg)  post.Track(*dbg, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    post.Add(P.SceneColorInput->Resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    post.Flush(cl);
    // 入力履歴は NPSR のまま (次フレームの出力スロット。追跡状態から UAV へ遷移する)

    *Out = FTemporalAAHistory{};
    Out->RTSlot = outSlot; Out->ReferenceBufferSize = H; Out->ViewportSize = H; Out->Format = fmt;
    return { &outTex, half };
}
```

**`FBarrierBatch`** is a private helper in `TemporalAA.cpp`:
- It holds up to 8 `D3D12_RESOURCE_BARRIER`s.
- `Add(res, before, after)` skips `before == after` and skips a resource already in the batch.
- `Track(FTAATexture&, after)` uses and updates `FTAATexture::State`.
- `Flush(cl)` issues one `ResourceBarrier` call.

This is the de-duplication graft. It matters wherever the same `ID3D12Resource` could appear twice, for example in the visualize pass, where `postInput` can be SceneColor.

**Resampling across resolution changes (UE parity).**
- `HistoryBufferSize`, `HistoryBufferUVMinMax` and `ScreenPosAbsMax` use the **previous** history extent `Hp`. `ScreenPosToHistoryBufferUV` is constant because the history always fills its texture.
- A screen-percentage change therefore reads the old history correctly: under Main, `Hp ≠ H`; under TAAU, `H = O` is unchanged.
- Nothing resets the history for an input-fraction change.

#### 4.8.4 `ComputeMitchellNetravaliDownsample(Input, S)`

1. Ensure `m_MNOutput` (RGBA16F, extent `S`).
2. Fill `FMitchellNetravaliParameters` in slot 1: `InputSize = (H, 1/H)`, `OutputSize = (S, 1/S)`, `InputPerOutputPixel = (H.x/S.x, H.y/S.y)`.
3. Barriers: `Input` is already RD from the TAA pass; `m_MNOutput` goes PSR → UAV.
4. Bindings: t0 = `Input->RT->SRVIndex`; t1..t3 = dummy tex; t4 = dummy EA; t5 = dummy tex; u0 = `m_MNOutput` UAV; u1/u2 = dummy UAV.
5. `Dispatch(ceil(S.x/8), ceil(S.y/8), 1)`, then `m_MNOutput` UAV → PSR. Return `&m_MNOutput`.

#### 4.8.5 GPU self-test dispatch (`RunGPUSelfTest`, called only from the top of `BeginFrame`)

1. `m_RHI->FlushAndResetCommandList()`.
2. Record `SetDescriptorHeaps`, the root signature, `m_PSOSelfTest`, the root CBV slot 2 (unused content), and all tables: dummies, with u0 = the self-test buffer UAV.
3. `Dispatch(1,1,1)`, then UAV → COPY_SOURCE, `CopyBufferRegion` into the readback buffer, then COPY_SOURCE → UAV.
4. `m_RHI->FlushAndResetCommandList()`. This waits, and restores the graphics state with the viewport at the default size.
5. `Map` the readback buffer and copy 64 floats out. Compare them in `TemporalAASelfTest.cpp` (§9.2).

#### 4.8.6 `ComputeTemporalAASampleWeights` (Main configuration only; UE `TemporalAA.cpp`)

UE evaluates the Catmull-Rom cubic even outside its support (`ax ≥ 2`), where it has no zero: CR(2.6) = −0.288 and CR(15) = −1183. `r.TemporalAAFilterSize` goes down to 0.1 (§7.1), and offsets divided by FS reach |x| = 15. With `r.TemporalAACatmullRom=1` and FS below about 0.75, that gives huge negative weights and a near-zero or negative sum, so the filter produces garbage or NaN.

Zeroing the tails is not enough. Our scan over realistic jitters (|J| ≤ 0.5 plus the CVar 2–5 patterns; `calc/cr_scan.py`) still finds normalized weights up to 91× at FS 0.4–0.5, because the positive and negative lobes cancel. The port therefore adds a conditioning guard **[PORT]**:

```cpp
// TemporalAA.cpp
namespace
{
    constexpr float kMinWeightSum        = 1.0e-6f;
    constexpr float kMinCRConditioning   = 0.25f;   // Σw >= 0.25·Σ|w| (負ローブの相殺で正規化重みが発散するのを防ぐ)
    float CatmullRom(float x)                       // [PORT] |x| >= 2 は 0 (UE は台の外でも 3 次式を評価する潜在不具合)
    {
        const float ax = std::fabs(x);
        if (ax >= 2.0f) return 0.0f;
        if (ax > 1.0f)  return ((-0.5f * ax + 2.5f) * ax - 4.0f) * ax + 2.0f;
        return (1.5f * ax - 2.5f) * ax * ax + 1.0f;
    }
    // 正規化。総和が極小 / 負、または相殺で悪条件なら「J に最も近いサンプル = 1」(同距離は中心) へフォールバック
    void NormalizeOrNearest(const float* W, const float* D2, const int* Idx, int Count, int CentreSlot, float* Out)
    {
        float Sum = 0.0f, AbsSum = 0.0f; int Nearest = CentreSlot;
        for (int k = 0; k < Count; ++k) { Sum += W[Idx[k]]; AbsSum += std::fabs(W[Idx[k]]); if (D2[Idx[k]] < D2[Idx[Nearest]]) Nearest = k; }
        const bool bIllConditioned = !(Sum > kMinWeightSum) || Sum < kMinCRConditioning * AbsSum;
        for (int k = 0; k < Count; ++k) Out[k] = bIllConditioned ? (k == Nearest ? 1.0f : 0.0f) : W[Idx[k]] / Sum;
    }
}
void ComputeTemporalAASampleWeights(XMFLOAT2 J, float FilterSize, bool bCatmullRom, float OutSampleWeights[9], float OutPlusWeights[5])
{
    static const int kOff[9][2] = { {-1,-1},{0,-1},{1,-1},{-1,0},{0,0},{1,0},{-1,1},{0,1},{1,1} };
    static const int kAll[9] = { 0,1,2,3,4,5,6,7,8 };
    static const int kPlus[5] = { 1, 3, 4, 5, 7 };
    const float FS = std::max(FilterSize, 0.01f);
    float W[9], D2[9];
    for (int i = 0; i < 9; ++i)
    {
        const float dx = ((float)kOff[i][0] - J.x) / FS, dy = ((float)kOff[i][1] - J.y) / FS;   // UE: SampleOffsets - Jitter
        D2[i] = dx * dx + dy * dy;
        W[i]  = bCatmullRom ? CatmullRom(dx) * CatmullRom(dy) : std::exp(-2.29f * D2[i]);       // ガウス: Sigma = 0.47
    }
    NormalizeOrNearest(W, D2, kAll,  9, 4, OutSampleWeights);   // 中心 = 添字 4
    NormalizeOrNearest(W, D2, kPlus, 5, 2, OutPlusWeights);     // 中心 = プラス内の添字 2
}
```

- The Gaussian weights are all positive, so `Sum == AbsSum`. There only the `kMinWeightSum` test can fire, at very small FS, and its one-hot result equals the normalized limit.
- For the Gaussian and uniform jitters (|J| < 0.5) the nearest sample is always the centre. A CVar-3 jitter such as (−2/3, 0) selects the neighbour it is closest to, which is also the correct limit of a shrinking kernel.
- For FS ≥ 0.75, which includes the default of 1, neither guard fires: the smallest Σw/Σ|w| is 0.54 (`calc/cr_guard_check.py`). The result there equals UE exactly (A.7).
- With the guard, the largest normalized weight over the scan is 2.46. T7 has the vectors (A.7).

### 4.9 `CommitViewState` and the previous-frame consumers (fixes the ordering hazard)

```cpp
void FSceneRenderer::CommitViewState(const FTemporalAAHistory& OutputHistory)   // RenderPostProcessing の最後
{
    if (!m_ViewInfo.bValid) return;                         // カメラ不在フレームは確定しない (次の有効フレームはカット)
    FPreviousViewInfo& P = m_ViewState.PrevFrameViewInfo;
    P.ViewMatrices          = m_ViewInfo.ViewMatrices;      // ジッタ込み + NoAA + ジッタ値
    P.TemporalAAHistory     = OutputHistory;                // TAA 未実行フレームは無効 -> 次の TAA は bCameraCut 扱い
    P.ViewRectSize          = m_ViewInfo.ViewRectSize;
    P.SceneColorPreExposure = 1.0f;
    m_ViewState.bPrevFrameViewInfoValid = true;
    m_ViewState.PrevAntiAliasingMethod  = m_ViewInfo.AntiAliasingMethod;
    m_ViewState.FrameIndex++;
}
```

`CopySceneColorHistory` keeps its texture copies (`SceneRenderer.cpp:728-801`). Its matrix block (803-810) is **deleted**, together with the members `m_PrevViewProjectionT`, `m_PrevInvViewProjectionT`, `m_PrevViewOrigin` and `m_bHistoryValid`. The compiler then finds every old reader.

During frame N every consumer reads the immutable snapshot `m_ViewInfo.PrevViewInfo` (frame N−1):

| Consumer | Uses | Why |
|---|---|---|
| VelocityVS / VelocityPS | b0 `PrevViewProjection` (previous, jittered) and `TemporalAAJitter.zw` | UE formulation. Subtracting both jitters gives the unjittered velocity (Appendix A.4). |
| TAA CS | `ClipToPrevClip` (NoAA × NoAA); history `Hp` | UE |
| Lumen, in `MakeLumenFrameInputs` | `PrevViewProjectionT = T(Prev.ViewProjectionMatrix)` and `PrevInvViewProjectionT = T(Prev.InvViewProjectionMatrix)`, both **jittered**; `PrevCameraOrigin = (Prev.ViewOrigin, 1)`; `ScreenWidth/Height = R`; `bHistoryValid = V.bPrevViewInfoValid && !V.bPrevTransformsReset && Prev.ViewRectSize == R` | `PrevSceneColor` and `PrevLinearDepth` are rasters with the previous jitter, so the reprojection stays exact (temporal_systems §2.5). The `ViewRectSize` term invalidates exactly one frame after a resize. |
| VolumetricFog, through `FComputeInputs` | `PrevViewProjectionT = T(Prev.ViewProjectionNoAAMatrix)` (**NoAA**); the same `bHistoryValid` rule, ANDed inside the fog with its own `m_bHistoryValid` (cleared by `CreateVolumes`) | The froxel grid is unjittered (`_11/_22` only), so the jittered previous VP would offset the history by up to 1/16 froxel (temporal_systems §3.5). UE uses `UnjitteredPrevWorldToClip`. |

`MakeLumenFrameInputs` keeps its current-frame fields, which already read the jittered b0. `T(M)` means `XMStoreFloat4x4(XMMatrixTranspose(XMLoadFloat4x4(&M)))`.

### 4.10 Which matrix each consumer uses

| Consumer | Matrix | Result |
|---|---|---|
| GeometryVS (base, translucency, responsive mask) and VelocityVS current clip | b0 `Projection` (jit) through `GetBasePassClipPosition` | Rasterization jitter. The positions are bit-identical between passes. |
| VelocityVS previous clip | b0 `PrevViewProjection` (previous jit); the PS subtracts `TemporalAAJitter.zw` | UE `PrevTranslatedWorldToClip` |
| DeferredPS / HeightFogPS / Lumen world reconstruction | b0 `InvViewProjection` (jit) | Exact, because the depth was rasterized with the jitter. No shader change. |
| LinearDepthPS | `NearFar` only | Jitter-invariant |
| LightGrid tiles, fog froxels, refraction offset scale | `Projection._11/_22` only | Unjittered, as in UE. The error is ≤ 0.5 render px against 64-px and 8-px cells. |
| `ComputeViewVisibility` | `ViewProjectionNoAAMatrix` | Unjittered culling (UE `ViewFrustum` is built before the jitter) |
| CSM fitting | `FSceneView` scalars | Unaffected |
| Shadow / Lumen card / Polygon2D views | their own `VIEW_CONSTANT{}` | Unjittered, with mip bias 0 |
| TAA camera motion, VisualizeMotionVectors | `ClipToPrevClip` (NoAA × NoAA) | UE |

### 4.11 Resource state timeline (new and changed resources)

| Resource | Frame start | Writer | TAA dispatch | After TAA | End of frame |
|---|---|---|---|---|---|
| Velocity | RD | `RenderVelocities`: RT → RD | read (t2, NPSR ⊂ RD) | read (visualize PS, ImGui) | RD |
| ResponsiveAAMask | RD | mask pass: RT → RD | read (t5) | read (ImGui thumbnail) | RD |
| LinearDepth | RD | existing | read (t1) | read | RD |
| SceneColor (R) | PSR | existing | **NPSR** | PSR (split-view tonemap, visualize t0) | PSR |
| History, output slot (Main / MainUpsampling) | tracked (NPSR from its use as input last frame, or PSR when new) | TAA: → UAV | UAV | **PSR**: AutoExposure's entry contract; AE toggles PSR ↔ NPSR internally | PSR |
| History, output slot (MainSuperSampling) | tracked | TAA: → UAV | UAV | **RD** (MN t0, ImGui) | RD |
| History, input slot | tracked (PSR or RD) | – | **NPSR** | NPSR | NPSR; next frame it is the output slot → UAV |
| TAA half-res | PSR | TAA: → UAV | UAV | PSR (AE input, Bloom threshold) | PSR |
| MN output | PSR | MN: → UAV → PSR | – | PSR (AE, Bloom, Tonemap) | PSR |
| TAA debug | tracked | TAA: → UAV | UAV | **RD** (visualize PS t36) | RD |
| Dummy tex | RD | – | read | read | RD |
| Dummy UAV tex | UAV | – | bound, never accessed | – | UAV |
| AutoExposure result (a **buffer**) | **COMMON**: buffers decay to COMMON when each `ExecuteCommandLists` completes. `PrepareResultForRead()` at the top of `RenderPostProcessing` issues COMMON → RD once `IsResultValid()`. Before the first dispatch it is COMMON (created UAV, ignored with #1328) and is promoted by the first clear/dispatch. | AE: RD → UAV → COPY_SOURCE → **RD** | read (t4, previous frame's value) when `IsResultValid()`, otherwise the dummy EA | read (Tonemap t11, visualize) | RD, decaying to COMMON at submission |
| TonemapOutput | PSR | Tonemap: → RT → PSR | – | PSR (Upscale t0) | PSR |
| Back buffer | PRESENT | Tonemap / Upscale: → RT; screenshot RT → COPY_SOURCE → RT | – | RT (visualize, ImGui) | → PRESENT in `EndFrame` |
| Scene depth | DEPTH_WRITE | existing | RD (not read by the TAA) | RD | DEPTH_WRITE |

The ImGui "TAA History" thumbnail shows `PrevFrameViewInfo.TemporalAAHistory`'s texture after `CommitViewState`. That is this frame's output, in PSR or RD, so it is pixel-readable at `EndFrame`.

### 4.12 Exposure for HDR weighting, and the `AutoExposure` edits

- `FrameExposureScale E` = `EyeAdaptationBuffer[0]` when `TAA_FLAG_EYE_ADAPTATION_BUFFER` is set, otherwise `ManualExposure`.
  - The flag is set from `(PP_FLAG_AUTO_EXPOSURE) && m_AutoExposure->IsResultValid()`.
  - AutoExposure dispatches **after** the TAA, so `E` is the **previous** frame's value, as in UE.
  - Frame 0 uses the manual exposure, and t4 is bound to the dummy.
- The shader guards the value: `E = (E > 0 && E < 1e30) ? E : ManualExposure` (§6.5).
- There is no pre-exposure, so `HistoryPreExposureCorrection = 1`.

**Buffer-state rule.** `m_Result` is a buffer (`AutoExposure.cpp:159-161`). Under legacy barriers a buffer decays to COMMON when each `ExecuteCommandLists` completes, so no cross-frame "rests in PSR/RD" contract can hold. In frame N+1 the buffer starts in COMMON, and there are two ways it goes wrong:
- If something reads it first, such as the TAA's t4, that read promotes it implicitly to NPSR. The pass-2 barrier's `StateBefore` then no longer matches.
- If nothing reads it, it stays COMMON, and the barrier mismatches again.

Today's `PSR → UAV` barrier (`:380-386`) has the same latent defect. The fix makes the start state explicit every frame.

`AutoExposure.h/.cpp` changes (the only edits to that class; **[FIX]**, done in S0 so that the debug-layer baseline D0 is measured with them):
1. New `void PrepareResultForRead()`, called as the **first statement of `RenderPostProcessing`**, before anything can read t4 or t11:
   ```cpp
   void AutoExposure::PrepareResultForRead()
   {
       // バッファは ExecuteCommandLists 完了ごとに COMMON へ減衰する (レガシーバリア)。前フレーム結果を読む前に明示遷移
       if (!m_ResultInitialised) return;                 // 初回 Dispatch 前: COMMON のまま (初回のクリア / UAV 書き込みで暗黙昇格)
       CommandList()->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_Result.Get(),
           D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
   }
   ```
2. The pass-2 barrier guarded by `m_ResultInitialised` becomes `PIXEL|NON_PIXEL → UNORDERED_ACCESS` (`:380-386`). Its `StateBefore` now matches, because step 1 ran earlier in the same command list.
3. The end-of-dispatch barrier becomes `COPY_SOURCE → PIXEL|NON_PIXEL` (was `→ PIXEL_SHADER_RESOURCE`, `:408-412`). The tonemap (t11) and the visualize pass read it in that state within the frame.
4. Add `bool IsResultValid() const { return m_ResultInitialised; }` to `AutoExposure.h`.
5. The input contract is unchanged: PSR on entry and on exit.

- `Dispatch` runs every frame (`SceneRenderer.cpp:1250-1258`), so steps 1–3 form a closed cycle: COMMON → RD → UAV → COPY_SOURCE → RD, then decay.
- A mid-frame `FlushAndResetCommandList` (resize, self-test, ImGui asset load) only adds another decay, which step 1 of the next frame absorbs. None of them runs between step 1 and the AE dispatch.
- The S0 GBV run confirms that no `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH` involves `AutoExposureResult` (§9.6).

---

## 5. Resolution changes

### 5.1 Triggers

| Change | Detected in | Action |
|---|---|---|
| `R`: ScreenPercentage, AA method, Upsampling toggle, or PSO fallback | `PrepareViewRectsForRendering` compares with `m_AllocatedRenderExtent` | Full render-resolution reallocation (§5.2). The TAA history is **kept** and resampled. |
| `P`: the planned post extent | compared with `m_AllocatedPostExtent` | Reset `m_TonemapOutput`, which is recreated lazily at the actual size (the Bloom chain is fixed at `O/2`) |
| `H` or the history format | `AddTemporalAAPass` (`FTAATexture::Matches`) | Lazy reallocation of the **output** slot only. The input history (old size and format) is still read correctly. |
| Actual post-input size ≠ `m_TonemapOutput` size | `EnsureTonemapOutput` | Lazy reallocation (deferred release) |
| `O` | never (fixed back buffer) | – (§1.2) |

**Startup ordering.**
- The `FSceneRenderer` constructor allocates everything at `R = P = O`.
- `SettingsManager::Initialize` applies the INI later, in `GameManager::Begin`.
- The first `BeginFrame` therefore detects any mismatch and reallocates, using the lazy compare-with-last-applied pattern (`ColorGradingLUTBaker::ParamsChanged` precedent). Neither ImGui nor SettingsManager needs a hook.

### 5.2 `ResizeRenderTargets(const FViewFamilyInfo& F)`: the order is normative

```cpp
void FSceneRenderer::ResizeRenderTargets(const FViewFamilyInfo& F)
{
    const XMUINT2 R = F.RenderExtent, P = F.PostProcessExtent;
    const bool bForce = m_TAADebug.bRequestReallocate;             // S3a 検証用ワンショット (同一サイズでも再確保)
    m_TAADebug.bRequestReallocate = false;
    const bool bR = bForce || (R.x != m_AllocatedRenderExtent.x || R.y != m_AllocatedRenderExtent.y);
    const bool bP = bForce || (P.x != m_AllocatedPostExtent.x || P.y != m_AllocatedPostExtent.y);
    if (!bR && !bP) return;

    // (0) 記録済みで未実行のコマンド (コンストラクタの初期バリア等) を実行し GPU をアイドルにする。
    //     リストはこの時点で空か、初回フレームならコンストラクタの記録のみ。
    m_RHI->FlushAndResetCommandList();

    // (1) 旧リソースを遅延削除キューへ (フェンス値 = 現在値)
    if (bR)
    {
        m_SceneTextures.Release(m_RHI);                   // 10 RT + Velocity + Mask + DepthSRV + LinearDepthDisplaySRV
        m_RHI->ReleaseDepthBuffer();                      // リソースのみ遅延解放 (DSV 枠は保持)
        m_DOFPrep.reset(); m_DOFPing.reset(); m_DOFBlur.reset(); m_DOFSharp.reset();
        m_LumenScene->ReleaseScreenTextures();            // Probe*, ProbeSH[2], DiffuseIndirect[2], Reflection (GlobalSDF / RCSH / アトラスは保持)
        m_FogRenderer->GetVolumetricFog()->ReleaseVolumes();
    }
    if (bP)
    {
        for (int i = 0; i < BLOOM_MIPS; ++i) { m_BloomMip[i].reset(); m_BloomUp[i].reset(); }
        m_TonemapOutput.reset();
    }

    // (2) 待機 + 遅延削除の実解放 (新規確保の前に旧メモリを返す = VRAM ピーク抑制)
    m_RHI->WaitGPU();

    // (3) 再確保 (SRV / UAV / RTV は全て新しいヒープ枠。DSV だけは同一 CPU 枠へ再作成)
    if (bR)
    {
        m_RHI->CreateDepthBuffer(R.x, R.y);               // DEPTH_WRITE 開始 (BeginFrame のクリアと整合)
        m_SceneTextures.Init(m_RHI, R.x, R.y);            // 読み取り状態への初期バリアを記録 (既存と同じ)
        InitDOF(R.x, R.y);
        m_LumenScene->CreateScreenTextures(R.x, R.y);     // ジッタ位相 (m_ProbeJitterIndex) は保持。履歴は下の ViewRectSize 規則で無効化
        m_FogRenderer->GetVolumetricFog()->CreateVolumes(R.x, R.y);   // m_bHistoryValid = false のみ, 状態 UAV (ジッタ位相 m_FrameNumber は保持)
        m_ViewState.PrevFrameViewInfo.ViewRectSize = { 0u, 0u };      // PrevSceneColor / PrevLinearDepth は未定義 -> Lumen / Fog 履歴を 1 フレーム無効化 (同一サイズ再確保でも)
        m_AllocatedRenderExtent = R;
    }
    if (bP)
    {
        m_AllocatedPostExtent = P;                        // Bloom チェーンは O/2 固定 (再確保しない)
    }
    ++m_TAAStats.NumResizes;
    char msg[160]; sprintf_s(msg, "[ScreenPercentage] resize R=%ux%u P=%ux%u freeSRV=%zu\n", R.x, R.y, P.x, P.y, m_RHI->GetNumFreeSRVDescriptors());
    OutputDebugStringA(msg);
}
```

**Why this is safe:**
- Step (0) executes the commands recorded since the last `Present`. On the first frame after construction these are the constructor's initial barriers, which reference the resources about to be released (judge-log #39).
- After step (0), `WaitGPU` has signalled and waited on the single direct queue, so every earlier frame has completed. Nothing in flight can observe a released resource or a rewritten descriptor.
- In step (1), each release is queued with the fence value `m_Frame[m_RTIndex]`, which step (0) incremented. Step (2) signals exactly that value, waits and flushes, so the old memory is freed **before** the new allocation. Peak VRAM is therefore the old set or the new set, never both.
- The depth DSV is rewritten in place in its private 1-slot CPU heap (`m_DepthBufferHandle`). DSV descriptors are consumed when `OMSetRenderTargets` is recorded, and the list is empty.
- Every SRV/UAV/RTV of a new resource gets a **new** heap slot, so no shader-visible descriptor is ever rewritten in place.
- `FlushAndResetCommandList` re-applies the old default viewport. `m_RHI->BeginFrame()` right after applies the new one (`PrepareViewRectsForRendering` sets it before).

### 5.3 Per-subsystem changes

| Subsystem | Today (fact) | Change |
|---|---|---|
| RHI depth buffer | BB-sized, created once in `InitDepthBuffer` (`RenderManager.cpp:295-350`) | Split into `CreateDepthBuffer(W,H)`, which holds the resource creation code plus `CreateDepthStencilView` into the existing `m_DepthBufferHandle`, and `ReleaseDepthBuffer()`, which does `DeferredRelease(std::move(m_DepthBuffer))`. `InitDepthBuffer` creates the heap and calls `CreateDepthBuffer(BBW,BBH)`. New getters `GetDepthBufferWidth/Height()`. |
| `FSceneTextures` | `Init(RHI)` from BB (`SceneTextures.cpp:8-9`) | `Init(RHI, W, H)` stores `Extent = {W,H}`. It creates the 10 existing targets, **`Velocity` (R16G16_UNORM)** and **`ResponsiveAAMask` (R8_UNORM)**, with the existing initial-barrier list extended by Velocity and Mask (PSR → RD). The depth SRV and the LinearDepth display SRV are allocated as before; the display SRV index is kept in the new member `LinearDepthDisplaySRVIndex`. `Release(RHI)` resets every `unique_ptr` (deferred through `~RENDER_TARGET`) and calls `ReleaseShaderResourceView(DepthSRVIndex)` and `ReleaseShaderResourceView(LinearDepthDisplaySRVIndex)`. `GBuffers` is rebuilt in `Init`. |
| DOF | `(BB+1)/2` targets; DOFSharp at BB; composite viewport and restore at BB (`SceneRenderer.cpp:1476, 1500-1503`) | `InitDOF(W,H)`: half = `((W+1)/2, (H+1)/2)`, Sharp `W×H`. `RenderDOF` uses `R` for the composite viewport and the restore (viewport **and** `SetTexelSize(R)`). **Radius invariance:** at entry `const float savedMaxBlur = m_FinalSettings.MaxBlurSize; m_FinalSettings.MaxBlurSize *= (float)R.x / (float)O.x;`, restored before the final upload. `DOFBlurPS` radius = CoC × MaxBlurSize in half-res texels (`DOFBlurPS.hlsl:47`), so the output-pixel radius is preserved: at 50 %, MaxBlurSize 8 → 4 half-res texels of a 480-px-wide texture = the same output-pixel radius. |
| Bloom | BB/2 chain; restore to BB (`:170-194`, `:1566-1568`) | Chain fixed at `O/2` (`InitBloom(O)` in the constructor only): mip0 = `(max(1,O.x/2), max(1,O.y/2))`, halved per level, floor, ≥ 1. The bloom kernels are fixed-tap filters in mip texels, so tying the chain to `O` keeps the halo's on-screen size invariant to the screen percentage (like the DOF radius). The threshold pass sets `SceneTexelSize = 1/mip0` and box-prefilters the input with 4 bilinear taps at ±¼ mip0 texel: P = O gives the exact 2×2 box (the old single bilinear tap), P = 2·O the full 4×4 footprint, P < O a magnifying read; the half-res TAA input is read by UV as well. The restore goes to the post extent (§4.7). |
| Tonemap | inherits the BB viewport | explicit viewport at the post extent or at `O` (§4.7) |
| `m_TonemapOutput` | – | `EnsureTonemapOutput(E)`: `CreateRenderTarget(E, R8G8B8A8_UNORM)` when null or of a different size (the old one is released through its destructor), state PSR |
| AutoExposure | BB dimensions (`SceneRenderer.cpp:1255-1256`) | the dimensions of the texture it is given (`aeExt`) |
| Light grid | dimensions from BB in `Init` (`LightGridInjection.cpp:103-108`); `ScreenWidth/Height` from BB (`:351-352`) | `Init(CapacityW, CapacityH)` is called with `(2·BBW, 2·BBH)`. It computes `m_CapacityGridX/Y = ceil(Cap/64)` (60×34 at 1080p) and `m_CapacityCells = X·Y·32` = 65 280, and sizes every buffer with the capacity (`maxLinks = m_CapacityCells·32`). New `SetViewSize(W,H)` sets `m_ViewWidth/Height = W,H`, `m_GridSizeX/Y = ceil(W/64), ceil(H/64)` (asserted ≤ capacity; clamped in Release) and `m_NumCells = X·Y·32`. `Dispatch` uses `m_ViewWidth/Height` for `ScreenWidth/Height` and keeps `MaxCulledLightLinks = CulledLightDataCapacity = m_NumCells·32`. `FillForwardLightData` already uses `m_GridSizeX/Y`. The shaders index cells with the per-frame `CulledGridSizeX/Y` (b3 and the grid b0), so a capacity allocation with per-frame dimensions is valid, and `LightGridInjection_CS.cso` stays byte-identical. There is no LightGrid reallocation path at all. |
| Volumetric fog | grid from BB (`VolumetricFog.cpp:178-182`); `ScreenSize` from BB (`:439-441`) | `Init()` keeps the RS, PSOs and parameter buffers and calls `CreateVolumes(BBW,BBH)`. `CreateVolumes(W,H)` computes the grid (`ceil(W/8)`, `ceil(H/8)`, 64), updates `m_Stats`, stores `m_ViewWidth/Height`, creates the 5 volumes (state UAV), and sets `m_bHistoryValid = false`. It does **not** touch `m_LightScatteringFrame` or the jitter phase `m_FrameNumber` (`VolumetricFog.cpp:446-451`), so a reallocated run stays frame-aligned with a run that never reallocated. `ReleaseVolumes()` is the destructor's `release` lambda promoted to a member, and the destructor calls it. `Dispatch` uses `m_ViewWidth/Height` for `ScreenSize`. `FogRendering.cpp:199-203` needs no change, because it follows `GetGridSizeX/Y()`. |
| Lumen | `InitScreenTextures()` mixes GlobalSDF/RCSH with screen textures (`LumenScene.cpp:393-455`); `ScreenWidth/Height` = BB (`SceneRenderer.cpp:701-702`); viewport restore at BB (`LumenScene.cpp:1123-1129`) | `InitScreenTextures()` is split into `InitGlobalTextures()` (GlobalSDF ×2, RCSH ×3) and `CreateScreenTextures(W,H)`, which holds lines 409-444 parameterized by `W,H` and updates `m_NumProbesX/Y` and `m_Stats`. `CreateScreenTextures` keeps the existing `m_DiffuseIndirectFrame = m_DiffuseIndirectCurrent = 0` of the moved block (`:439-440`; ping-pong indices only, harmless). It does **not** reset `m_ProbeSHFrame` or the jitter phase `m_ProbeJitterIndex` (`:1587-1592`), so a reallocated run keeps the same probe-jitter sequence as a run without reallocation. History invalidation goes only through `Inputs.bHistoryValid`, the `ViewRectSize` rule of §4.9. `Init()` calls `InitGlobalTextures(); CreateScreenTextures(BBW,BBH);`. `ReleaseScreenTextures()` releases ProbeGeo, TraceRadiance, FilteredRadiance, ProbeSH[2].{SHR,SHG,SHB,Aux}, DiffuseIndirect[2] and Reflection through the new private member `ReleaseComputeTexture(FLumenComputeTexture&)`, which is the destructor lambda promoted (the destructor now calls it too). The card-capture restore calls `m_RHI->RestoreDefaultViewport()`. `MakeLumenFrameInputs` passes `R`. |
| Shadows | restore to BB (`ShadowRendering.cpp:686-694`) | `m_RHI->RestoreDefaultViewport()` |
| `RenderManager` viewport | `InitViewport` sets BB once (`:81-94`) | `SetDefaultViewportSize(W,H)` writes `m_Viewport` and `m_ScissorRect`, which `SetDefaultGraphicsState` applies. `RestoreDefaultViewport()` records `RSSetViewports/ScissorRects` with them immediately. Getters `GetDefaultViewportWidth/Height()`. |
| b4 texel | `SetTexelSize(BB)` in `ResolvePostProcessSettings` (`:360`) | `SetTexelSize(R)`. `InitPostProcess`'s initial `SetTexelSize(BB)` stays, because it is overwritten every frame. |
| ImGui thumbnails | SRV handles read every frame | unchanged; they pick up the new SRVs automatically |
| TAA history / aux targets | – | lazy per-slot reallocation (§4.8.3); never an in-place descriptor rewrite |

### 5.4 History invalidation matrix

| Event | Render-res resources | Lumen history | Fog history | TAA history | Velocity pass | Previous view matrices |
|---|---|---|---|---|---|---|
| Screen-percentage change | reallocated | invalid for 1 frame (`ViewRectSize` rule) | invalid (`CreateVolumes`, plus the rule) | **kept, resampled** (Main: the output slot is reallocated at the new `H`, the input is read with `Hp`) | runs | kept |
| `HistoryScreenPercentage` change (Main ↔ SuperSampling, or a different `H`) | – | – | – | output slot reallocated; input resampled | – | – |
| History format change (R11G11B10 ↔ RGBA16F through Quality or setting) | – | – | – | output slot reallocated; input read; `HISTORY_HAS_ALPHA` cleared for an R11 input | – | – |
| Quality change without a format change | – | – | – | kept | – | – |
| AA method change (None ↔ TAA) | possibly reallocated (the clamp range differs) | invalid (cut) | invalid (cut) | cut | cleared, no draws | Prev = Current |
| Upsampling toggle | reallocated if `R` changes | per the rule | per the rule | kept, resampled (UE) | runs | kept |
| Camera cut (any §4.4 source) | – | invalid | invalid | not read (dummy bound, `bCameraCut = 1`) | cleared, no draws | Prev = Current |
| Large camera movement (`bPrevTransformsReset`) | – | invalid | invalid | kept (clamp + off-screen test handle it) [M] | runs (object motion only) | Prev = Current |
| Pre-TAA debug view active (Lumen or LightGrid DebugMode) | – | – | – | bypassed: CB `bCameraCut = 1` only; the view state is unaffected | runs | kept |
| PSO fallback (a missing `.cso`) | reallocated at the spatial clamp if it differs | per the rule | per the rule | cut (the AA method changes) | – | – |

### 5.5 UI behaviour

- The ScreenPercentage and HistoryScreenPercentage sliders edit a local copy. They commit on `ImGui::IsItemDeactivatedAfterEdit()`, so one drag causes one reallocation, not one per tick.
- The AA window shows `Resizes`, free SRV/RTV descriptors, the deferred-release queue length and VRAM `CurrentUsage`, so the leak check (§9.4) can be read directly.

---

## 6. Shader specifications

All new HLSL targets SM 5.0 (fxc), is UTF-8 and carries Japanese comments. The identifiers below are final.

### 6.1 Shared headers

**`Shader/VelocityCommon.hlsl`** is register-free, so compute shaders can include it.

```hlsl
#ifndef VELOCITY_COMMON_HLSL
#define VELOCITY_COMMON_HLSL
// UE VelocityCommon.ush (Gen4: xy のみ)。0 (クリア値) = 未書き込み -> TAA は深度からカメラモーションを再構築
static const float VELOCITY_ENCODE_SCALE = 0.499f * 0.5f;           // 0.2495
static const float VELOCITY_ENCODE_BIAS  = 32767.0f / 65535.0f;     // 0.49999237
float2 EncodeVelocityToTexture(float2 V)
{
    V = clamp(V, -2.0f, 2.0f);                   // [PORT] |V| > 2.0038 は UNORM 0 (未書き込み) と衝突するため
    return V * VELOCITY_ENCODE_SCALE + VELOCITY_ENCODE_BIAS;
}
float2 DecodeVelocityFromTexture(float2 E) { return (E - VELOCITY_ENCODE_BIAS) * (1.0f / VELOCITY_ENCODE_SCALE); }
bool   IsVelocityWritten(float2 E)         { return E.x > 0.0f; }   // UE: EncodedVelocity.x > 0

struct VELOCITY_VS_OUTPUT
{
    float4 Position        : SV_POSITION;   // 今フレーム clip (ジッタ込み)。ベースパスと同一式
    float4 PackedVelocityA : TEXCOORD1;     // 今フレーム clip (ジッタ込み, = Position)
    float4 PackedVelocityC : TEXCOORD2;     // 前フレーム clip (前フレームのジッタ込み)
    float2 TexCoord        : TEXCOORD0;
    float4 Color           : COLOR;
};
#endif
```

**`Shader/BasePassVertexCommon.hlsl`** requires `Common.hlsl` (b0/b1).

```hlsl
#ifndef BASE_PASS_VERTEX_COMMON_HLSL
#define BASE_PASS_VERTEX_COMMON_HLSL
// GeometryVS / VelocityVS 共通 (UE INVARIANT 相当)。ベロシティ / Responsive マスクの LESS_EQUAL が
// ベースパス / 半透明プリパス深度とビット一致するよう、SV_Position は必ずこの関数で求めること。
float4 GetBasePassClipPosition(float3 LocalPosition)
{
    precise float4x4 wvp = mul(mul(LocalToWorld, View), Projection);   // 既存 GeometryVS と同じ式・同じ順序
    precise float4 clip = mul(float4(LocalPosition, 1.0f), wvp);
    return clip;
}
#endif
```

**`Shader/TemporalAACommon.hlsl`** is **register-free**. It holds helpers only. The TAA, MN and self-test compute shaders each declare their own cbuffer and resources.

```hlsl
#ifndef TEMPORAL_AA_COMMON_HLSL
#define TEMPORAL_AA_COMMON_HLSL

static const int2 kOffsets3x3[9] = { int2(-1,-1), int2(0,-1), int2(1,-1), int2(-1,0), int2(0,0), int2(1,0), int2(-1,1), int2(0,1), int2(1,1) };
static const uint kPlusIndexes3x3[5] = { 1, 3, 4, 5, 7 };

// ---- YCoCg (非正規化: Y = R + 2G + B = Luma4) ----
float3 RGBToYCoCg(float3 c) { return float3(dot(c, float3(1, 2, 1)), dot(c, float3(2, 0, -2)), dot(c, float3(-1, 2, -1))); }
float3 YCoCgToRGB(float3 c) { const float Y = c.x * 0.25f, Co = c.y * 0.25f, Cg = c.z * 0.25f; return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg); }

// ---- HDR 重み (Karis 1/(1+L)。Y = 4L なので +4 は HdrWeight4 と同じ膝; 定数倍は相殺) ----
float  HdrWeightY(float Y, float Exposure) { return rcp(Y * Exposure + 4.0f); }
float2 WeightedLerpFactors(float WeightA, float WeightB, float Blend)
{
    const float A = (1.0f - Blend) * WeightA, B = Blend * WeightB;
    const float R = rcp(A + B);                   // 両重みとも正 (HdrWeightY > 0) なので 0 除算しない
    return float2(A * R, B * R);
}

// ---- TAAU 空間重み (Blackman-Harris 近似, 半径 1 出力 px, 下限 0.005) ----
float ComputeSampleWeigth(float2 PixelDelta /*入力 px*/, float UpscaleFactor)   // UE の綴り (Weigth) を踏襲
{
    const float x2 = saturate(UpscaleFactor * UpscaleFactor * dot(PixelDelta, PixelDelta));
    return (0.905f * x2 - 1.9f) * x2 + 1.0f;
}

// ---- NaN / 負 / half 上限ガード ----
float3 SanitizeColor(float3 c, float MaxValue) { return min(-min(-c, 0.0f), MaxValue); }

// ---- 標準 Z: ビュー Z -> デバイス Z (LinearDepthPS の厳密逆)。遠方 (>= DepthParams.z) は無限遠 d = Q ----
//      DepthParams = (Q, -Q*n, 0.999*f, 0)
float ViewZToDeviceZ(float ViewZ, float4 DepthParams)
{
    return (ViewZ >= DepthParams.z) ? DepthParams.x : (DepthParams.x + DepthParams.y / ViewZ);
}

// ---- 最近傍深度 (X パターン ±Cross)。標準 Z なので「手前 = 小さい」: UE の max/> を min/< へ反転 ----
//      Z = (x:(-C,-C), y:(+C,-C), z:(-C,+C), w:(+C,+C)) のビュー Z。Offset = (0,0) なら中心が最近傍。
void SelectClosestDepthCross(float Z0, float4 Z, int Cross, out int2 Offset, out float ClosestZ)
{
    int2 DepthOffset = int2(Cross, Cross); int DepthOffsetXx = Cross;
    if (Z.x < Z.y) DepthOffsetXx = -Cross;                      // UE: >
    if (Z.z < Z.w) DepthOffset.x = -Cross;                      // UE: >
    const float ZXY = min(Z.x, Z.y), ZZW = min(Z.z, Z.w);       // UE: max
    if (ZXY < ZZW) { DepthOffset.y = -Cross; DepthOffset.x = DepthOffsetXx; }
    const float ZXYZW = min(ZXY, ZZW);
    Offset = int2(0, 0); ClosestZ = Z0;
    if (ZXYZW < Z0) { Offset = DepthOffset; ClosestZ = ZXYZW; }
}

// ---- 5 タップ Catmull-Rom (角除去)。UV と重みのみ返す (サンプリングは呼び出し側) ----
void CatmullRom5Taps(float2 UV, float4 BufferSize, out float2 TapUV[5], out float TapW[5])
{
    const float2 UVp = UV * BufferSize.xy;
    const float2 tc  = floor(UVp - 0.5f) + 0.5f;
    const float2 f = UVp - tc, f2 = f * f, f3 = f2 * f;
    const float2 w0 = f2 - 0.5f * (f3 + f);
    const float2 w1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
    const float2 w3 = 0.5f * (f3 - f2);
    const float2 w2 = 1.0f - w0 - w1 - w3;
    const float2 W0 = w0, W1 = w1 + w2, W2 = w3;
    const float2 S0 = (tc - 1.0f) * BufferSize.zw;
    const float2 S1 = (tc + w2 / W1) * BufferSize.zw;
    const float2 S2 = (tc + 2.0f) * BufferSize.zw;
    TapUV[0] = float2(S1.x, S0.y); TapW[0] = W1.x * W0.y;
    TapUV[1] = float2(S0.x, S1.y); TapW[1] = W0.x * W1.y;
    TapUV[2] = float2(S1.x, S1.y); TapW[2] = W1.x * W1.y;
    TapUV[3] = float2(S2.x, S1.y); TapW[3] = W2.x * W1.y;
    TapUV[4] = float2(S1.x, S2.y); TapW[4] = W1.x * W2.y;
}

// ---- Mitchell-Netravali (B = C = 1/3) [L] ----
float MitchellNetravali(float x)
{
    x = abs(x);
    if (x < 1.0f) return (7.0f * x * x * x - 12.0f * x * x + 16.0f / 3.0f) / 6.0f;
    if (x < 2.0f) return (-7.0f / 3.0f * x * x * x + 12.0f * x * x - 20.0f * x + 32.0f / 3.0f) / 6.0f;
    return 0.0f;
}

// ---- UE Random.ush ----
uint3 Rand3DPCG16(int3 p)
{
    uint3 v = uint3(p);
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v >> 16u;
}
float2 Hammersley16(uint Index, uint NumSamples, uint2 Random)
{
    const float E1 = frac((float)Index / (float)NumSamples + float(Random.x) * (1.0f / 65536.0f));
    const float E2 = float((reversebits(Index) >> 16) ^ Random.y) * (1.0f / 65536.0f);
    return float2(E1, E2);
}

// ---- 確率的量子化 (UE QuantizeForFloatRenderTarget; E in [0,1), 誤差 = 出力フォーマットの 1 ULP を 2 の冪へ切り下げ) ----
float3 QuantizeForFloatRenderTarget(float3 Color, float E, float3 QuantizationError)
{
    float3 Error = Color * QuantizationError;
    Error = asfloat(asuint(Error) & ~0x007FFFFFu);
    return Color + Error * E;
}

// ---- UE InterleavedGradientNoise (S8 の DeferredPS フレーム項と自己テスト用) ----
float InterleavedGradientNoise(float2 uv, float FrameId)
{
    uv += FrameId * (float2(47.0f, 17.0f) * 0.695f);
    return frac(52.9829189f * frac(dot(uv, float2(0.06711056f, 0.00583715f))));
}

// ---- デバッグ用ヒートマップ (0->黒, 0.04->暗青, 0.2->緑, 0.25->黄, 1->赤) ----
float3 BlendFinalHeat(float b)
{
    if (b <= 0.04f) return lerp(float3(0, 0, 0), float3(0, 0, 0.5f), b / 0.04f);
    if (b <= 0.2f)  return lerp(float3(0, 0, 0.5f), float3(0, 1, 0), (b - 0.04f) / 0.16f);
    if (b <= 0.25f) return lerp(float3(0, 1, 0), float3(1, 1, 0), (b - 0.2f) / 0.05f);
    return lerp(float3(1, 1, 0), float3(1, 0, 0), saturate((b - 0.25f) / 0.75f));
}
#endif
```

### 6.2 Velocity shaders

**`Shader/VelocityVS.hlsl`**

```hlsl
#include "Common.hlsl"
#include "BasePassVertexCommon.hlsl"
#include "VelocityCommon.hlsl"
VELOCITY_VS_OUTPUT main(VS_INPUT input)
{
    VELOCITY_VS_OUTPUT o;
    o.Position        = GetBasePassClipPosition(input.Position);                  // ベースパスとビット一致
    o.PackedVelocityA = o.Position;
    const float4 prevWorld = mul(float4(input.Position, 1.0f), PreviousLocalToWorld);
    o.PackedVelocityC = mul(prevWorld, PrevViewProjection);                        // 前フレームのジッタ込み (UE PrevTranslatedWorldToClip)
    o.TexCoord = input.TexCoord;
    o.Color    = input.Color;
    return o;
}
```

**`Shader/VelocityPixelShader.hlsl`** is a header, `ExcludedFromBuild`. `VelocityPS.hlsl` is `#define VELOCITY_MASKED 0` followed by `#include "VelocityPixelShader.hlsl"`, and `VelocityMaskedPS.hlsl` is the same with `1`.

```hlsl
#include "Common.hlsl"
#include "VelocityCommon.hlsl"
float4 main(VELOCITY_VS_OUTPUT input) : SV_TARGET0
{
#if VELOCITY_MASKED
    // GeometryPS と同一の被覆判定 (同じ SampleBias / 同じ頂点カラー乗算 / 同じしきい値)。穴に速度を書かない
    const float4 baseColor = TextureBaseColor.SampleBias(Sampler, input.TexCoord, MaterialTextureMipBias) * input.Color;
    if (Material.BlendMode == BLEND_MASKED)
    {
        clip(baseColor.a - Material.OpacityMaskClipValue);
    }
#endif
    if (input.PackedVelocityC.w <= 1.0e-4f)                                        // [PORT] 前フレーム位置がカメラ背後
        return float4(EncodeVelocityToTexture(float2(2.0f, 2.0f)), 0.0f, 0.0f);    // -> HSP = s - 2 が画面外 -> 履歴棄却
    const float2 ScreenPos     = input.PackedVelocityA.xy / input.PackedVelocityA.w - TemporalAAJitter.xy;
    const float2 PrevScreenPos = input.PackedVelocityC.xy / input.PackedVelocityC.w - TemporalAAJitter.zw;
    return float4(EncodeVelocityToTexture(ScreenPos - PrevScreenPos), 0.0f, 0.0f); // RG のみ格納 (R16G16_UNORM)
}
```

Sign check (Appendix A.4): an object moving +x gives `V.x > 0`, and the history position `s − V` lies where the object was.

### 6.3 `Shader/ResponsiveAAPS.hlsl`

```hlsl
#include "Common.hlsl"
float4 main(PS_INPUT input) : SV_TARGET0 { return float4(1.0f, 0.0f, 0.0f, 1.0f); }   // R8_UNORM に 1 (UE: ステンシル bit 3)
```

### 6.4 Changes to existing shaders

| File | Change | Stage |
|---|---|---|
| `ConstantBuffers.hlsl` | b0 per §3.1 | S2 |
| `ConstantBuffers.hlsl` | b4 pads renamed per §3.3 | S3 |
| `ConstantBuffers.hlsl` | b1 gains `float4x4 PreviousLocalToWorld;` | S4 |
| `Resources.hlsl` | t35 and t36 per §3.6 | S4 |
| `GeometryVS.hlsl` | `#include "BasePassVertexCommon.hlsl"`; `output.Position = GetBasePassClipPosition(input.Position);`. The `wvp` lines go; the WorldPosition/Normal/Tangent lines stay. | S4 |
| `GeometryPS.hlsl:18, 62, 72` | `.Sample(Sampler, uv)` → `.SampleBias(Sampler, uv, MaterialTextureMipBias)`. The Unlit sky path goes through line 18 and gets the bias (UE parity). | S2 |
| `TranslucentPS.hlsl:179, 230, 235` | same `SampleBias` change. The Radiance-Cache `SampleLevel` at 139-141 stays. Refraction keeps using b4 `SceneTexelSize`, which is 1/R. | S2 |
| `DeferredPS.hlsl:31-33` | UE frame term in the Lumen GatherMode-1 IGN: `const float2 p = float2(PixelPos) + (float)StateFrameIndexMod8 * (float2(47.0f, 17.0f) * 0.695f); float ign = frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));`. `StateFrameIndexMod8` is 0 while AA is off, so the AA-off image stays identical. | S8 |
| `ShadowDepthMaskedPS.hlsl`, `LumenCardCapturePS.hlsl`, `Structs.hlsl`, `DeferredVS.hlsl` | **unchanged**. These are views without mip bias, and `PS_INPUT` stays shared. | – |

### 6.5 `Shader/TemporalAA.hlsl` (header; the body of all 11 permutations)

#### 6.5.1 Permutation defines

Each wrapper defines `TAA_PASS_CONFIG`, `TAA_QUALITY` and `TAA_DOWNSAMPLE`, then includes `TemporalAA.hlsl`.

| Wrapper | TAA_PASS_CONFIG | TAA_QUALITY | TAA_DOWNSAMPLE | AA_UPSAMPLE | Box | AA_FILTERED | AA_SAMPLES | AA_DYNAMIC_ANTIGHOST | AA_ROUND | AA_UPSAMPLE_ADAPTIVE_FILTERING |
|---|---|---|---|---|---|---|---|---|---|---|
| `TemporalAA_Main_Low_CS` | 0 | 0 | 0 | 0 | MIN_MAX | 0 | 5 [L] | 0 | 1 | 0 |
| `TemporalAA_Main_Low_Downsample_CS` | 0 | 0 | 1 | 0 | MIN_MAX | 0 | 5 [L] | 0 | 1 | 0 |
| `TemporalAA_Main_Medium_CS` | 0 | 1 | 0 | 0 | MIN_MAX | 1 | 5 | 0 | 1 | 0 |
| `TemporalAA_Main_High_CS` | 0 | 2 | 0 | 0 | MIN_MAX | 1 | 9 | 1 | 1 | 0 |
| `TemporalAA_Main_MediumHigh_CS` | 0 | 3 | 0 | 0 | MIN_MAX | 1 | 5 | 1 | 1 | 0 |
| `TemporalAA_Upsampling_Low_CS` | 1 | 0 | 0 | 1 | SAMPLE_DISTANCE | 0 | 5 [L] | 0 | 0 | 0 |
| `TemporalAA_Upsampling_Low_Downsample_CS` | 1 | 0 | 1 | 1 | SAMPLE_DISTANCE | 0 | 5 [L] | 0 | 0 | 0 |
| `TemporalAA_Upsampling_Medium_CS` | 1 | 1 | 0 | 1 | SAMPLE_DISTANCE | 1 | 5 | 0 | 0 | 0 |
| `TemporalAA_Upsampling_High_CS` | 1 | 2 | 0 | 1 | SAMPLE_DISTANCE | 1 | 9 | 1 | 0 | 0 |
| `TemporalAA_Upsampling_MediumHigh_CS` | 1 | 3 | 0 | 1 | SAMPLE_DISTANCE | 1 | 5 | 1 | 0 | 0 |
| `TemporalAA_SuperSampling_CS` | 2 | 2 (forced) | 0 | 1 | VARIANCE | 1 | 9 | 1 | 0 | 1 [L] |

- Common to all [M]: `AA_CROSS 2`, `AA_BICUBIC 1`, `AA_MANUALLY_CLAMP_HISTORY_UV 1`, `AA_YCOCG 1`, `AA_CLAMP 1`, `AA_NAN 1`, `AA_DYNAMIC 1`.
- `r.TemporalAAUpsampleFiltered=0` is a **runtime** flag (`TAA_FLAG_UPSAMPLE_FILTERED` cleared means "center sample"), which keeps the count at 11 permutations. As in UE (`TAA_UPSAMPLE_FILTERED = CVar || Pass != MainUpsampling`), the CVar only applies to MainUpsampling; MainSuperSampling always runs with the flag set (`AddPasses`, §4.8.2).
- `TAA_SCREEN_PERCENTAGE_RANGE` is folded away (§1.2).

```hlsl
// TemporalAA.hlsl (冒頭)
#define HISTORY_CLAMPING_BOX_MIN_MAX         0
#define HISTORY_CLAMPING_BOX_VARIANCE        1
#define HISTORY_CLAMPING_BOX_SAMPLE_DISTANCE 2
#define AA_CROSS 2
#if TAA_PASS_CONFIG == 0
  #define AA_UPSAMPLE 0
  #define AA_HISTORY_CLAMPING_BOX HISTORY_CLAMPING_BOX_MIN_MAX
  #define AA_ROUND 1
  #define AA_UPSAMPLE_ADAPTIVE_FILTERING 0
#elif TAA_PASS_CONFIG == 1
  #define AA_UPSAMPLE 1
  #define AA_HISTORY_CLAMPING_BOX HISTORY_CLAMPING_BOX_SAMPLE_DISTANCE
  #define AA_ROUND 0
  #define AA_UPSAMPLE_ADAPTIVE_FILTERING 0
#else
  #define AA_UPSAMPLE 1
  #define AA_HISTORY_CLAMPING_BOX HISTORY_CLAMPING_BOX_VARIANCE
  #define AA_ROUND 0
  #define AA_UPSAMPLE_ADAPTIVE_FILTERING 1
#endif
#if TAA_QUALITY == 0
  #define AA_FILTERED 0
  #define AA_SAMPLES 5
  #define AA_DYNAMIC_ANTIGHOST 0
#elif TAA_QUALITY == 1
  #define AA_FILTERED 1
  #define AA_SAMPLES 5
  #define AA_DYNAMIC_ANTIGHOST 0
#elif TAA_QUALITY == 2
  #define AA_FILTERED 1
  #define AA_SAMPLES 9
  #define AA_DYNAMIC_ANTIGHOST 1
#else
  #define AA_FILTERED 1
  #define AA_SAMPLES 5
  #define AA_DYNAMIC_ANTIGHOST 1
#endif
#define TAA_FLAG_UPSAMPLE_FILTERED     (1u << 0)
#define TAA_FLAG_RESPONSIVE_MASK_VALID (1u << 1)
#define TAA_FLAG_EYE_ADAPTATION_BUFFER (1u << 2)
#define TAA_FLAG_HISTORY_HAS_ALPHA     (1u << 3)
#define TAA_FLAG_FTW_MODE_SHIFT        4u
#define TAA_FLAG_DOWNSAMPLE_OUTPUT     (1u << 6)
```

#### 6.5.2 Constant buffer and resources (compute root signature §3.7)

```hlsl
#include "TemporalAACommon.hlsl"
#include "VelocityCommon.hlsl"

cbuffer TemporalAAParameters : register(b0)       // C++ FTemporalAAParameters (§3.4, 336 B)
{
    float4   InputSceneColorSize;          //   0
    int4     InputMinMaxPixelCoord;        //  16
    float4   OutputViewportSize;           //  32
    float4   HistoryBufferSize;            //  48
    float4   HistoryBufferUVMinMax;        //  64
    float4   ScreenPosToHistoryBufferUV;   //  80
    float4x4 ClipToPrevClip;               //  96
    float2   TemporalJitterPixels;         // 160
    float2   ScreenPosAbsMax;              // 168
    float    ScreenPercentage;             // 176
    float    UpscaleFactor;                // 180
    float    CurrentFrameWeight;           // 184
    float    HistoryPreExposureCorrection; // 188
    uint     bCameraCut;                   // 192
    uint     Flags;                        // 196
    float    ManualExposure;               // 200
    uint     DebugMode;                    // 204
    float4   SampleWeights[3];             // 208
    float4   PlusWeights[2];               // 256
    float4   OutputQuantizationError;      // 288 (w = 最大有限値)
    float4   DepthParams;                  // 304 (Q, -Q*n, 0.999*f, 0)
    uint     StateFrameIndexMod8;          // 320
    float    DebugScale;                   // 324
    float    SampleDistanceThreshold;      // 328
    uint     Pad0;                         // 332
};
Texture2D<float4>   InputSceneColor          : register(t0);   // R (PSR -> NPSR)
Texture2D<float2>   SceneLinearDepth         : register(t1);   // R: ビュー Z [m] (不透明のみ)
Texture2D<float2>   SceneVelocity            : register(t2);   // R16G16_UNORM エンコード (0 = 未書き込み)
Texture2D<float4>   HistoryBuffer            : register(t3);   // Hp (カット時はダミー。読まない)
Buffer<float>       EyeAdaptationBuffer      : register(t4);   // [0] = 前フレーム露出
Texture2D<float>    ResponsiveAAMask         : register(t5);   // R8_UNORM
RWTexture2D<float4> OutComputeTex            : register(u0);   // H (新しい履歴 = 後段の SceneColor)
RWTexture2D<float4> OutComputeTexDownsampled : register(u1);   // ceil(H/2)
RWTexture2D<float4> DebugOutput              : register(u2);   // H
SamplerState        PointClampSampler        : register(s0);
SamplerState        LinearClampSampler       : register(s1);

int2  ClampInputPixel(int2 p) { return clamp(p, InputMinMaxPixelCoord.xy, InputMinMaxPixelCoord.zw); }
float3 LoadInputYCoCg(int2 p) { return RGBToYCoCg(SanitizeColor(InputSceneColor.Load(int3(ClampInputPixel(p), 0)).rgb, 65504.0f)); }  // [PORT] 入力もサニタイズ
float  LoadViewZ(int2 p)      { return SceneLinearDepth.Load(int3(ClampInputPixel(p), 0)).r; }
bool   IsDynamicAt(int2 p)    { return IsVelocityWritten(SceneVelocity.Load(int3(ClampInputPixel(p), 0))); }
```

#### 6.5.3 Current-frame filter and neighbourhood box

```hlsl
// 現フレームの再構成フィルタ。InvFilterScale < 1 でカーネルを広げる (適応フィルタのみ)
void FilterCurrentFrame(float3 C[9], float2 dKO, float E, float InvFilterScale, out float3 Filtered, out float FTW)
{
#if AA_FILTERED
  #if AA_UPSAMPLE
    if ((Flags & TAA_FLAG_UPSAMPLE_FILTERED) == 0u)                       // r.TemporalAAUpsampleFiltered = 0
    {
        Filtered = C[4];
        FTW = ComputeSampleWeigth(-dKO * InvFilterScale, UpscaleFactor);
        return;
    }
  #endif
    float3 Acc = 0.0f; float WAcc = 0.0f, WS = 0.0f;
    [unroll] for (uint s = 0; s < AA_SAMPLES; ++s)
    {
  #if AA_SAMPLES == 9
        const uint i = s;
  #else
        const uint i = kPlusIndexes3x3[s];
  #endif
  #if AA_UPSAMPLE
        const float ws = ComputeSampleWeigth((float2(kOffsets3x3[i]) - dKO) * InvFilterScale, UpscaleFactor);   // dPP = o - dKO (入力 px)
  #elif AA_SAMPLES == 9
        const float ws = SampleWeights[i >> 2][i & 3];                         // CPU: exp(-2.29|o - J|^2 / FS^2) 正規化
  #else
        const float ws = PlusWeights[s >> 2][s & 3];
  #endif
        const float wh = HdrWeightY(C[i].x, E);
        Acc += C[i] * (ws * wh); WAcc += ws * wh; WS += ws;
    }
    Filtered = Acc * rcp(WAcc);
  #if AA_UPSAMPLE
    FTW = WS;                                                                  // [L] 非正規化空間重みの総和 (出力画素あたりの時間サンプル密度を一定に)
  #else
    FTW = 1.0f;
  #endif
#else
    Filtered = C[4];
  #if AA_UPSAMPLE
    FTW = ComputeSampleWeigth(-dKO * InvFilterScale, UpscaleFactor);          // [L] 最近傍サンプルの重み
  #else
    FTW = 1.0f;
  #endif
#endif
}

void ComputeNeighborhoodBoundingbox(float3 C[9], float2 dKO, float3 Filtered, out float3 NeighborMin, out float3 NeighborMax)
{
#if AA_HISTORY_CLAMPING_BOX == HISTORY_CLAMPING_BOX_MIN_MAX
    float3 PlusMin = C[1], PlusMax = C[1];
    [unroll] for (uint p = 1; p < 5; ++p) { PlusMin = min(PlusMin, C[kPlusIndexes3x3[p]]); PlusMax = max(PlusMax, C[kPlusIndexes3x3[p]]); }
  #if AA_SAMPLES == 9
    const float3 SquareMin = min(PlusMin, min(min(C[0], C[2]), min(C[6], C[8])));
    const float3 SquareMax = max(PlusMax, max(max(C[0], C[2]), max(C[6], C[8])));
    #if AA_ROUND
    NeighborMin = 0.5f * (SquareMin + PlusMin); NeighborMax = 0.5f * (SquareMax + PlusMax);   // 丸めた箱 (UE AA_ROUND)
    #else
    NeighborMin = SquareMin; NeighborMax = SquareMax;
    #endif
  #else
    NeighborMin = PlusMin; NeighborMax = PlusMax;
  #endif
#elif AA_HISTORY_CLAMPING_BOX == HISTORY_CLAMPING_BOX_VARIANCE
    float3 m1 = 0.0f, m2 = 0.0f;
    [unroll] for (uint s = 0; s < AA_SAMPLES; ++s) { m1 += C[s]; m2 += C[s] * C[s]; }          // SuperSampling は 9 サンプル
    m1 *= (1.0f / AA_SAMPLES); m2 *= (1.0f / AA_SAMPLES);
    const float3 StdDev = sqrt(abs(m2 - m1 * m1));
    NeighborMin = m1 - 1.25f * StdDev; NeighborMax = m1 + 1.25f * StdDev;                        // [H] 1.25 sigma
  #if !AA_UPSAMPLE_ADAPTIVE_FILTERING
    NeighborMin = min(NeighborMin, Filtered); NeighborMax = max(NeighborMax, Filtered);
  #endif
#else // SAMPLE_DISTANCE
    NeighborMin = C[4]; NeighborMax = C[4];                                     // K は |dKO| <= 0.707 なので常に内側
    const float Thr2 = SampleDistanceThreshold * SampleDistanceThreshold;      // lerp(1.51, 1.3, UF - 1) [H], 入力 px [M]
    [unroll] for (uint s = 0; s < AA_SAMPLES; ++s)
    {
  #if AA_SAMPLES == 9
        const uint i = s;
  #else
        const uint i = kPlusIndexes3x3[s];
  #endif
        const float2 dPP = float2(kOffsets3x3[i]) - dKO;
        if (dot(dPP, dPP) < Thr2) { NeighborMin = min(NeighborMin, C[i]); NeighborMax = max(NeighborMax, C[i]); }
    }
#endif
}
```

#### 6.5.4 Main body

```hlsl
#if TAA_DOWNSAMPLE
groupshared float4 GSDownsample[64];
#endif

[numthreads(8, 8, 1)]
void main(uint2 GroupThreadId : SV_GroupThreadID, uint2 DispatchThreadId : SV_DispatchThreadID, uint GroupIndex : SV_GroupIndex)
{
    const uint2 PixelPos = DispatchThreadId;                                   // OutputViewportRect.Min = 0 (exact-size)
    const bool  bInside  = all(PixelPos < (uint2)OutputViewportSize.xy);       // 早期 return 禁止 (DOWNSAMPLE の同期のため)

    // ---- 1. 出力画素中心 (未ジッタ) ----
    const float2 ViewportUV = (float2(PixelPos) + 0.5f) * OutputViewportSize.zw;
    const float2 ScreenPos  = float2(2.0f * ViewportUV.x - 1.0f, 1.0f - 2.0f * ViewportUV.y);

    // ---- 2. 最近接入力画素 K と dKO (入力 px) ----
#if AA_UPSAMPLE
    const float2 PPCo = ViewportUV * InputSceneColorSize.xy + TemporalJitterPixels;  // 出力画素中心が写るジッタ済み入力座標
    const float2 PPCk = floor(PPCo) + 0.5f;
    const float2 dKO  = PPCo - PPCk;                                           // [-0.5, 0.5)
    const int2   K    = ClampInputPixel(int2(floor(PPCo)));
#else
    const float2 dKO  = TemporalJitterPixels;                                  // Main: K = 出力画素。重みは CPU (o - J)
    const int2   K    = ClampInputPixel(int2(PixelPos));
#endif

    // ---- 3. 前フレーム露出 / Responsive ----
    float E = (Flags & TAA_FLAG_EYE_ADAPTATION_BUFFER) ? EyeAdaptationBuffer[0] : ManualExposure;
    E = (E > 0.0f && E < 1.0e30f) ? E : ManualExposure;
    const bool bResponsive = ((Flags & TAA_FLAG_RESPONSIVE_MASK_VALID) != 0u) && ResponsiveAAMask.Load(int3(K, 0)) > 0.5f;

    // ---- 4. 最近傍深度 (X パターン ±AA_CROSS 入力 px, 標準 Z = min) ----
    const float Z0 = LoadViewZ(K);
    const float4 Zc = float4(LoadViewZ(K + int2(-AA_CROSS, -AA_CROSS)), LoadViewZ(K + int2(AA_CROSS, -AA_CROSS)),
                             LoadViewZ(K + int2(-AA_CROSS,  AA_CROSS)), LoadViewZ(K + int2(AA_CROSS,  AA_CROSS)));
    int2 VelocityOffset; float ClosestZ;
    SelectClosestDepthCross(Z0, Zc, AA_CROSS, VelocityOffset, ClosestZ);      // PosN.xy は動かさない (UE)
    const float DeviceZ = ViewZToDeviceZ(ClosestZ, DepthParams);               // 遠方 = d = Q (回転のみ再投影)

    // ---- 5. カメラ運動 (NoAA x NoAA) とオブジェクト運動 ----
    const float4 PrevClip = mul(float4(ScreenPos, DeviceZ, 1.0f), ClipToPrevClip);
    bool   bPrevBehind = PrevClip.w <= 1.0e-6f;                                // [PORT] 大移動 / 背面ガード
    float2 BackN = ScreenPos - PrevClip.xy / max(PrevClip.w, 1.0e-6f);
    const float2 EncodedVelocity = SceneVelocity.Load(int3(ClampInputPixel(K + VelocityOffset), 0));
    if (IsVelocityWritten(EncodedVelocity)) { BackN = DecodeVelocityFromTexture(EncodedVelocity); bPrevBehind = false; }
    const float2 BackTemp = BackN * OutputViewportSize.xy;                     // 単位 = 出力 px の 2 倍 (UE)
    const float  Velocity = sqrt(dot(BackTemp, BackTemp));
    const float2 HistoryScreenPosition = ScreenPos - BackN;
    const bool   OffScreen = max(abs(HistoryScreenPosition.x), abs(HistoryScreenPosition.y)) >= 1.0f || bPrevBehind;

    // ---- 6. 近傍 3x3 (YCoCg) ----
    float3 C[9];
    [unroll] for (uint i = 0; i < 9; ++i) C[i] = LoadInputYCoCg(K + kOffsets3x3[i]);   // 未使用分は fxc が除去

    // ---- 7. 現フレームフィルタ (適応フィルタでなければクランプ前) ----
    float3 Filtered; float FTW;
    FilterCurrentFrame(C, dKO, E, 1.0f, Filtered, FTW);

    // ---- 8. 近傍ボックス ----
    float3 NeighborMin, NeighborMax;
    ComputeNeighborhoodBoundingbox(C, dKO, Filtered, NeighborMin, NeighborMax);

    // ---- 9. 履歴 (Catmull-Rom 5 タップ, UV 手動クランプ。カット時は読まない) ----
    float3 HistoryY = Filtered; float HistoryAlpha = 0.0f;
    if (bCameraCut == 0u)
    {
        const float2 HistoryUV = HistoryScreenPosition * ScreenPosToHistoryBufferUV.xy + ScreenPosToHistoryBufferUV.zw;
        float2 TapUV[5]; float TapW[5];
        CatmullRom5Taps(HistoryUV, HistoryBufferSize, TapUV, TapW);
        float4 Acc = 0.0f; float WSum = 0.0f;
        [unroll] for (uint t = 0; t < 5; ++t)
        {
            const float2 uv = clamp(TapUV[t], HistoryBufferUVMinMax.xy, HistoryBufferUVMinMax.zw);   // AA_MANUALLY_CLAMP_HISTORY_UV
            Acc += HistoryBuffer.SampleLevel(LinearClampSampler, uv, 0.0f) * TapW[t]; WSum += TapW[t];
        }
        float4 History = Acc * rcp(WSum);                                     // 角除去で総和 != 1 のため正規化
        History.rgb = SanitizeColor(History.rgb * HistoryPreExposureCorrection, 65504.0f);   // [PORT] リンギング / NaN
        HistoryY = RGBToYCoCg(History.rgb);
        HistoryAlpha = (Flags & TAA_FLAG_HISTORY_HAS_ALPHA) ? History.a : 0.0f;              // [PORT] R11G11B10 は a = 1 を返す
    }

    // ---- 10. 履歴棄却 ----
    bool IgnoreHistory = OffScreen || (bCameraCut != 0u);
    const bool DynamicCenter = IsDynamicAt(K);
    bool AntiGhostReject = false;
#if AA_DYNAMIC_ANTIGHOST
    const bool Dynamic = IsDynamicAt(K + int2(0, -1)) || IsDynamicAt(K + int2(-1, 0)) || DynamicCenter
                      || IsDynamicAt(K + int2(1, 0))  || IsDynamicAt(K + int2(0, 1));
    AntiGhostReject = !Dynamic && HistoryAlpha > 0.0f;                         // 動的だった履歴が静的背景に残るのを消す
    IgnoreHistory = IgnoreHistory || AntiGhostReject;
#endif

    // ---- 11. クランプ (YCoCg AABB) ----
    const float3 HistoryPreClamp = HistoryY;
    HistoryY = clamp(HistoryY, NeighborMin, NeighborMax);
#if AA_UPSAMPLE_ADAPTIVE_FILTERING
    {   // [L] クランプで大きく動いた (棄却された) 画素ほどカーネルを最大 1 入力 px まで広げる
        const float Rejection = saturate(length(HistoryPreClamp - HistoryY) / max(length(NeighborMax - NeighborMin), 1.0e-4f));
        FilterCurrentFrame(C, dKO, E, lerp(1.0f, rcp(max(UpscaleFactor, 1.0f)), Rejection), Filtered, FTW);
    }
#endif

    // ---- 12. FilteredTemporalWeight の定義切替 (デバッグ: 0 総和 / 1 最近傍 / 2 = 1) ----
    const uint FTWMode = (Flags >> TAA_FLAG_FTW_MODE_SHIFT) & 3u;
#if AA_UPSAMPLE
    if (FTWMode == 1u) FTW = ComputeSampleWeigth(-dKO, UpscaleFactor);
    else if (FTWMode == 2u) FTW = 1.0f;
#endif

    // ---- 13. BlendFinal (= 現フレームの重み) ----
    const float LumaFiltered = Filtered.x, LumaHistory = HistoryY.x;          // YCoCg Y = Luma4
    float BlendFinal = FTW * CurrentFrameWeight;
    BlendFinal = lerp(BlendFinal, 0.2f, saturate(Velocity / 40.0f));
    const float BlendFinalPreFloor = BlendFinal;                               // デバッグ view 5 用 (下限適用前。収束した平坦画素は下限で 1 になるため)
    BlendFinal = max(BlendFinal, saturate(0.01f * LumaHistory * rcp(max(abs(LumaFiltered - LumaHistory), 1.0e-8f))));   // UE の停滞防止下限 (そのまま)
    if (bResponsive)   BlendFinal = 0.25f;
    if (IgnoreHistory) BlendFinal = 1.0f;

    // ---- 14. HDR 加重ブレンド + ガード + 確率的量子化 ----
    const float2 Wl = WeightedLerpFactors(HdrWeightY(LumaHistory, E), HdrWeightY(LumaFiltered, E), BlendFinal);
    float3 OutRGB = YCoCgToRGB(HistoryY * Wl.x + Filtered * Wl.y);
    OutRGB = SanitizeColor(OutRGB, OutputQuantizationError.w);                 // AA_NAN: NaN / 負 -> 0, 上限
    {
        const uint2 Rnd = Rand3DPCG16(int3(PixelPos, StateFrameIndexMod8)).xy;
        const float Eq  = Hammersley16(0u, 1u, Rnd).x;                         // [0, 1) (UE Gen4)
        OutRGB = min(QuantizeForFloatRenderTarget(OutRGB, Eq, OutputQuantizationError.xyz), OutputQuantizationError.w);
    }
    float OutAlpha = 0.0f;
#if AA_DYNAMIC_ANTIGHOST
    OutAlpha = DynamicCenter ? 1.0f : 0.0f;                                    // 次フレームのアンチゴースト用
#endif
    if (bInside) OutComputeTex[PixelPos] = float4(OutRGB, OutAlpha);

    // ---- 15. デバッグ出力 (DebugMode = ETemporalAADebugView 5..13) ----
    if (DebugMode != 0u && bInside)
    {
        const float  Bg   = 0.2f * saturate(0.25f * Filtered.x * E);          // 背景 (表示用輝度)
        float3 d = Bg.xxx;
        switch (DebugMode)
        {
        case 5u:  d = BlendFinalHeat(IgnoreHistory ? 1.0f : (bResponsive ? 0.25f : BlendFinalPreFloor)); break; // BlendFinal (下限前, §6.8)
        case 6u:  d = (OffScreen || AntiGhostReject || bCameraCut != 0u)
                      ? float3(OffScreen ? 1.0f : 0.0f, AntiGhostReject ? 1.0f : 0.0f, bCameraCut != 0u ? 1.0f : 0.0f) : Bg.xxx; break; // Rejection
        case 7u:  d = saturate(abs(HistoryPreClamp.x - HistoryY.x) * E * 0.25f * DebugScale).xxx; break;      // HistoryClamp
        case 8u:  d = saturate(abs(HistoryPreClamp.x - Filtered.x) * E * 0.25f * DebugScale).xxx; break;      // ReprojectionError
        case 9u:  d = saturate(FTW / 1.2f).xxx; break;                                                        // FilteredTemporalWeight
        case 10u: d = all(VelocityOffset == 0) ? float3(0, 0, 0)
                    : (VelocityOffset.x < 0 ? (VelocityOffset.y < 0 ? float3(1, 0, 0) : float3(0, 0, 1))
                                            : (VelocityOffset.y < 0 ? float3(0, 1, 0) : float3(1, 1, 0))); break; // ClosestDepthOffset
        case 11u: d = bResponsive ? float3(1, 1, 0) : Bg.xxx; break;                                          // ResponsiveMask
        case 12u: d = float3(saturate(HistoryAlpha), DynamicCenter ? 1.0f : 0.0f, AntiGhostReject ? 1.0f : 0.0f); break; // DynamicAntiGhost
        case 13u: d = float3(dKO.x + 0.5f, dKO.y + 0.5f, saturate(FTW / 1.2f)); break;                         // InputSampleAlignment
        default:  break;
        }
        DebugOutput[PixelPos] = float4(d, 1.0f);
    }

#if TAA_DOWNSAMPLE
    // ---- 16. ハーフ解像度 (2x2 ボックス, 有効画素のみで重み付け) ----
    GSDownsample[GroupIndex] = bInside ? float4(OutRGB, 1.0f) : float4(0.0f, 0.0f, 0.0f, 0.0f);
    GroupMemoryBarrierWithGroupSync();
    if (((GroupThreadId.x | GroupThreadId.y) & 1u) == 0u && (Flags & TAA_FLAG_DOWNSAMPLE_OUTPUT) != 0u)
    {
        const float4 s = GSDownsample[GroupIndex] + GSDownsample[GroupIndex + 1] + GSDownsample[GroupIndex + 8] + GSDownsample[GroupIndex + 9];
        const uint2 HalfPos = PixelPos >> 1;
        const uint2 HalfExtent = ((uint2)OutputViewportSize.xy + 1u) >> 1;     // ceil(H/2) = 確保サイズ
        if (s.w > 0.0f && all(HalfPos < HalfExtent)) OutComputeTexDownsampled[HalfPos] = float4(s.rgb / s.w, 1.0f);
    }
#endif
}
```

- **Standard-Z porting marks.** Every depth comparison that differs from UE carries a `// UE: >` or `// UE: max` comment. These are in `SelectClosestDepthCross` above. The far-pixel rule in `ViewZToDeviceZ` is new code: UE's `HAS_INVERTED_Z_BUFFER == 0` branch was only an `#error`.

### 6.6 `Shader/TemporalAAMitchellNetravali_CS.hlsl` (MainSuperSampling, H → S) [L]

```hlsl
#include "TemporalAACommon.hlsl"
cbuffer MitchellNetravaliParameters : register(b0)
{
    float4 InputSize;             // (H.x, H.y, 1/H.x, 1/H.y)
    float4 OutputSize;            // (S.x, S.y, 1/S.x, 1/S.y)
    float2 InputPerOutputPixel;   // (H.x/S.x, H.y/S.y) (1..2)
    float2 Pad;
};
Texture2D<float4>   InputTexture  : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid >= (uint2)OutputSize.xy)) return;                            // LDS 不使用なので早期 return 可
    const float2 InPos  = (float2(DTid) + 0.5f) * InputPerOutputPixel;       // 入力 (履歴) の連続座標
    const float2 Radius = 2.0f * InputPerOutputPixel;                          // 台 = 出力 2 px
    const int2   First  = int2(floor(InPos - Radius - 0.5f)) + 1;              // 中心 (i + 0.5) > InPos - Radius
    float wx[9], wy[9];
    [unroll] for (int k = 0; k < 9; ++k)
    {
        wx[k] = MitchellNetravali(((float)(First.x + k) + 0.5f - InPos.x) / InputPerOutputPixel.x);
        wy[k] = MitchellNetravali(((float)(First.y + k) + 0.5f - InPos.y) / InputPerOutputPixel.y);
    }
    float3 acc = 0.0f; float wsum = 0.0f;
    [loop] for (int y = 0; y < 9; ++y)
    {
        [unroll] for (int x = 0; x < 9; ++x)
        {
            const float w = wx[x] * wy[y];
            if (w == 0.0f) continue;
            const int2 p = clamp(First + int2(x, y), int2(0, 0), int2(InputSize.xy) - 1);
            acc += InputTexture.Load(int3(p, 0)).rgb * w; wsum += w;
        }
    }
    const float3 c = SanitizeColor(acc / wsum, 65504.0f);                      // 負ローブ (-0.035) と NaN のガード
    OutputTexture[DTid] = float4(c, 1.0f);
}
```

The 1D taps sum to `ratio` and are normalized; Appendix A.8 has the vectors.

### 6.7 Primary spatial upscale: `Shader/PostProcessUpscale.hlsl` (header) + 6 wrappers (UE `PostProcessUpscale.usf`, `r.Upscale.Quality`)

Common setup:
- Graphics root signature; the PS includes `Common.hlsl` and `TemporalAACommon.hlsl` (for `CatmullRom5Taps`).
- Input `t0 = TextureBaseColor` = `m_TonemapOutput`, which is display-referred LDR, as in UE: the primary upscale runs after the tonemap.
- `uv = input.TexCoord`, the output viewport UV, which equals the input UV (exact-size).
- `InSize = float4(1/PostProcess.SceneTexelSizeX, 1/PostProcess.SceneTexelSizeY, SceneTexelSizeX, SceneTexelSizeY)`.
- The output is `float4(saturate(rgb), 1)`.
- The wrappers are `PostProcessUpscale_{Nearest,Bilinear,Directional,CatmullRom,Lanczos,Gaussian}_PS.hlsl`, each `#define UPSCALE_METHOD n` (0..5) followed by `#include "PostProcessUpscale.hlsl"`.

| Method | Name (UE) | Implementation |
|---|---|---|
| 0 | Nearest | `TextureBaseColor.Load(int3(min(int2(uv * InSize.xy), int2(InSize.xy) - 1), 0))` |
| 1 | Bilinear | `TextureBaseColor.SampleLevel(Sampler2, uv, 0)` |
| 2 | Directional blur + unsharp mask [M] | Four bilinear taps at `uv ± 0.5·InSize.zw` (NW, NE, SW, SE); `C` = their mean. `L = dot(rgb, float3(0.299, 0.587, 0.114))` per tap. `dSWmNE = L_SW − L_NE`, `dSEmNW = L_SE − L_NW`. `Dir = float2(dSWmNE + dSEmNW, dSWmNE − dSEmNW)`, then `Dir *= 0.125 · rsqrt(dot(Dir, Dir) + 6e-8)`. `N = Sample(uv − Dir·InSize.zw)`, `P = Sample(uv + Dir·InSize.zw)`. Output `(N + P)·((0.25 + 1)·0.5) − C·0.25`. |
| 3 | 5-tap Catmull-Rom (default) | `CatmullRom5Taps(uv, InSize, TapUV, TapW)`. Clamp each tap to `[0.5·InSize.zw, 1 − 0.5·InSize.zw]`, `SampleLevel(Sampler2)`, and normalize by `ΣTapW`. |
| 4 | Lanczos-3, 13 taps [M] | `Pix = uv·InSize.xy`, `tc = floor(Pix − 0.5) + 0.5`, `f = Pix − tc + 2`. `w_k = L3(f − k)` for k = 0..5, with `L3(x) = sinc(x)·sinc(x/3)` for `|x| < 3`, else 0 (the constant cancels). Merge the centre pair: `W2 = w2 + w3`, `S2 = tc + w3/W2`. The positions per axis are `{tc − 2, tc − 1, S2, tc + 2, tc + 3}` with weights `{w0, w1, W2, w4, w5}`. Sample the **diamond** `|i − 2| + |j − 2| ≤ 2` of that 5×5 grid (13 taps) and normalize by the weight sum. |
| 5 | Gaussian unsharp, up to 36 taps [L] | `Pix = uv·InSize.xy`; `first = floor(Pix − 0.5) − 2`. For `k, l ∈ [0, 5]`: `center = first + (k, l) + 0.5`, `o = Pix − center`, `r2 = dot(o, o)`. Skip if `r2 > 9`. `w = exp(−r2)` (σ² = 0.5). `Acc += w·s`, `Lap += w·s·(2·r2 − 2)`, `W += w`. `Out = Acc/W − 0.5·PostProcess.UpscaleUnsharpAmount·(Lap/W)`. `UpscaleUnsharpAmount = r.Upscale.Softness·max(0, 1 − (In.x·In.y)/(O.x·O.y))`, set by the renderer. |

`FSceneRenderer::AddPrimaryUpscalePass(In, InExtent, O, PSOName)`:
1. `m_FinalSettings.UpscaleUnsharpAmount = …; SetTexelSize(InExtent); UploadPostProcessConstant();`
2. `SetPipelineState(PSOName)`, `SetTexture(BASE_COLOR, In)`, `SetViewportAndScissor(O)`, `DrawScreenPass()`.

`PSOName` comes from `SelectPrimaryUpscalePipeline()` (§4.7), which checks `m_RHI->HasPipelineState`:
- It returns the `r.Upscale.Quality` PSO when it exists.
- Otherwise it returns `PostProcessUpscale1` (bilinear), with a one-time log.
- If that is missing too, it returns `nullptr`, and `RenderPostProcessing` takes the merged tonemap path instead, so this pass is not called.

The upscale PSOs are created **optional** (§3.8). A null check alone would not work, because a missing PS in Release still yields a non-null PSO that draws nothing.

### 6.8 `Shader/VisualizeTemporalAAPS.hlsl` (graphics; DeferredVS; back buffer; viewport O)

**Bindings, set by `AddVisualizeTemporalAAPass(postInput, bTAARan)`.** The pass first checks `m_RHI->HasPipelineState("VisualizeTemporalAA")`. The PSO is optional (§3.8); when it is missing, the pass logs once and returns, leaving the normal image.

| Slot | Resource |
|---|---|
| b0 | camera, re-uploaded with `SetConstant(VIEW, m_ViewConstant)` |
| b4 | `VisualizeMode = (uint)DebugView`, `VisualizeScale`, `SceneTexelSize = 1/O` |
| t0 | render-res SceneColor (PSR) |
| t4 | LinearDepth |
| t11 | AutoExposure result (RD) or its SRV, used only when the flag is set |
| t35 | `m_bVelocityValid ? Velocity : m_TemporalUpscaler->GetDummySRVIndex()`. The velocity pass may not have run: TAA PSO fallback, an invalid view, the frame after a resize, or a debug view outside §4.5.3's `bNeeded`. The dummy then reads RG = 0, meaning "not written". Loads outside its 1×1 extent also return 0, which D3D defines for out-of-bounds `Load`. The views therefore fall back to camera motion instead of reading stale or uninitialized memory, just like the CS views. |
| t36 | the TAA debug texture (modes 5–13); `postInput` (mode 4); the dummy tex otherwise |

**Display of HDR values:** `DisplayHDR(c) = LinearToSRGB(saturate(c·Ex / (1 + c·Ex)))`, where `Ex = (PostProcess.Flags & PP_FLAG_AUTO_EXPOSURE) ? AutoExposureBuffer[0] : PostProcess.Exposure`. `LinearToSRGB` comes from `ColorSpace.hlsl`.

**Camera motion at render pixel `k`** (shared by modes 1, 2 and 4): the same as TAA step 5 without dilation.
- `z = TextureLinearDepth.Load(k).r`
- `d = (z ≥ 0.999·NearFar.y) ? Q : Q − Q·NearFar.x/z`, with `Q = NearFar.y/(NearFar.y − NearFar.x)`
- `s = ((k + 0.5)·ViewSizeAndInvSize.zw)` mapped to ScreenPos
- `Prev = mul(float4(s, d, 1), ClipToPrevClip)`, `V = s − Prev.xy/Prev.w`
- If `IsVelocityWritten(t35.Load(k))`, then `V = Decode(...)`.
- Output pixels: `Δ = (V.x·O.x/2, −V.y·O.y/2)`.

| View (`ETemporalAADebugView`) | Computation | What a correct image looks like / what a bug looks like |
|---|---|---|
| 1 MotionVectors | `k = floor(uv·R)`. Background: `hsv(hue = atan2(−Δ.y, Δ.x)/2π, sat = written ? 1 : 0.5, val = saturate(|Δ|·VisualizeScale/16))` plus `0.15·DisplayHDR(luma(t0))`. Per 24-px cell, a white line of width < 1 px from the cell centre to `centre − Δ(centre)`. | A static camera is **black** everywhere, including the sky, with no arrows. Per-frame flicker means the jitter was not removed. A rotating camera gives a smooth field that is continuous across object/sky boundaries. |
| 2 VelocityMask | written → green (0,1,0); otherwise `0.3·luma` | Only moving primitives are green; the sky is **never** green (it writes no velocity, §4.5). The green disappears the frame after motion stops (the stale-Prev check). |
| 3 InputOutputSplit | This PS draws only the 2-px red divider (`abs(SVPos.x − O.x/2) < 1`, else `discard`). The left half was drawn by the second tonemap of §4.7. | The left half (input) aliases and shimmers; the right half (TAA) is stable. An offset between the halves of ≥ ½ px means a jitter-sign or Main-weight error. |
| 4 TemporalUpscalerIO | A 2×2 grid. TL = `DisplayHDR(t0)` input (render res, bilinear by quadrant UV). TR = depth `t4.G`. BL = the mode-1 colours without arrows. BR = `DisplayHDR(t36)` output. Labels ("Input WxH", "Output WxH", "Pass / Quality") are drawn by the ImGui AA window with `GetForegroundDrawList()->AddText` at the quadrant corners (UE `VisualizeTemporalUpscaler`). | The quadrants show the correct resolutions; the output is sharper than the input at < 100 %. |
| 5 BlendFinal (CS) | `t36` as-is: `BlendFinalHeat(IgnoreHistory ? 1 : (bResponsive ? 0.25 : BlendFinalPreFloor))`. `BlendFinalPreFloor` is FTW·CFW after the velocity lerp (§6.5.4 step 13). The view deliberately **excludes** UE's anti-stall floor `max(.., saturate(0.01·Lh/\|Lf−Lh\|))`. That floor is ≥ 0.06 whenever \|ΔL\| < Lh/6 and exactly 1 when \|ΔL\| ≤ 1 % of Lh (A.9, near-equal case), which is most converged flat pixels, so it would paint a correct static image red. The blend itself still uses the floor. | A static view is **dark blue**: pre-floor ≤ 0.06, `imgstat` dark-blue fraction. Red everywhere means the history is always rejected. Red for exactly one frame after a cut is correct. Yellow marks responsive pixels. |
| 6 Rejection (CS) | R = OffScreen, G = anti-ghost reject, B = camera cut; otherwise `0.2·luma` | Red appears only at screen edges while panning. Full blue appears for exactly one frame on a cut. |
| 7 HistoryClamp (CS) | gray `|PreClamp.Y − Clamped.Y|` | Bright only at edges and disocclusions |
| 8 ReprojectionError (CS) | gray `|PreClamp.Y − Filtered.Y|` | Near black on static geometry for a static or panning camera. Uniform brightness means a C2P or UV-mapping error. |
| 9 FilteredTemporalWeight (CS) | gray `FTW/1.2` | 100 %: uniform ≈ 0.9. 50 %: a moiré with a mean ≈ 0.26. |
| 10 ClosestDepthOffset (CS) | black when there is no offset; (−,−) red, (+,−) green, (−,+) blue, (+,+) yellow | A 2-px coloured rim **outside** object silhouettes. Rims inside the silhouettes mean max was used instead of min. |
| 11 ResponsiveMask (CS) | yellow where responsive | Only on responsive translucency |
| 12 DynamicAntiGhost (CS) | R = history alpha, G = DynamicCenter, B = reject | A blue trail behind a moving object for one frame, then clean |
| 13 InputSampleAlignment (CS) | (dKO.x + 0.5, dKO.y + 0.5, FTW/1.2) | A smooth gradient pattern that moves every frame with the jitter |

For the CS views (5–13) the PS reads `TemporalAADebugTexture.Load(int3(uv·DebugExtent, 0))`, where `DebugExtent` comes from `GetDimensions`.

### 6.9 `Shader/TemporalAASelfTest_CS.hlsl` (GPU helper parity)

- It is a single 1×1×1 group and writes `RWStructuredBuffer<float> Out : register(u0)`.
- It includes `TemporalAACommon.hlsl` and `VelocityCommon.hlsl` and evaluates the **same** helpers as the TAA CS.
- The C++ side compares with tolerance 1e-5. Entries marked † depend on `rcp` and use 2e-3.

| idx | Expression | Expected |
|---|---|---|
| 0,1 | `EncodeVelocityToTexture(float2(-0.01357995, 0))` | 0.49660417, 0.49999237 |
| 2,3 | `DecodeVelocityFromTexture(float2(32545, 32767) / 65535.0)` | −0.0135772, 0.0 |
| 4 | `EncodeVelocityToTexture(float2(3, 0)).x` (±2 clamp) | 0.99899237 |
| 5 | `ComputeSampleWeigth(float2(0.5, 0), 1)` | 0.5815625 |
| 6 | `ComputeSampleWeigth(float2(0, -0.0833333), 2)` | 0.9479205 |
| 7–9 | `RGBToYCoCg(float3(1, 0.5, 0.25))` | 2.25, 1.5, −0.25 |
| 10–12 | `YCoCgToRGB(float3(2.25, 1.5, -0.25))` | 1.0, 0.5, 0.25 |
| 13 † | `HdrWeightY(2.25, 1)` | 0.16 |
| 14,15 † | `WeightedLerpFactors(1.0/6.0, 0.1, 0.04)` | 0.9756098, 0.0243902 |
| 16 † | `ViewZToDeviceZ(10, float4(1.00020004, -0.100020004, 499.5, 0))` | 0.99019804 |
| 17 | the same with `ViewZ = 500` (far rule) | 1.00020004 |
| 18 † | the same with `ViewZ = 499` | 0.99999960 |
| 19–21 | `SelectClosestDepthCross(7, float4(5, 3, 4, 6), 2)` → (Offset.x, Offset.y, Z) | 2, −2, 3 |
| 22–24 | `SelectClosestDepthCross(7, float4(9, 2, 8, 1), 2)` | 2, 2, 1 |
| 25–27 | `SelectClosestDepthCross(7, float4(9, 9, 9, 9), 2)` | 0, 0, 7 |
| 28,29 | `CatmullRom5Taps(float2(10.75, 10.75) / 100, float4(100, 100, 0.01, 0.01))` → (`TapW[2]`, `TapUV[2].x·100 − 10.5`) | 1.1962891, 0.2071429 |
| 30,31 | `MitchellNetravali(0.5)`, `MitchellNetravali(1.5)` | 0.5347222, −0.0347222 |
| 32 | `QuantizeForFloatRenderTarget(1.0.xxx, 0.5, (1.0/64).xxx).x` | 1.0078125 |
| 33 | `QuantizeForFloatRenderTarget(1.0.xxx, 0.5, (1.0/1024).xxx).x` | 1.00048828 |
| 34 | `Hammersley16(0, 1, Rand3DPCG16(int3(0, 0, 0)).xy).x` | 0.10681152 |
| 35 | the same for `int3(10, 20, 3)` | 0.78860474 |
| 36 | `InterleavedGradientNoise(float2(1, 0), 0)` | 0.5557134 |
| 37 | `IsVelocityWritten(float2(0, 0)) ? 1 : 0` | 0 |
| 38 | `IsVelocityWritten(float2(1.0/65535, 0)) ? 1 : 0` | 1 |

### 6.10 Shader file list and project entries

| File | Type | Stage |
|---|---|---|
| `VelocityCommon.hlsl`, `BasePassVertexCommon.hlsl`, `VelocityPixelShader.hlsl` | header (`ExcludedFromBuild`) | S4 |
| `PostProcessUpscale.hlsl` | header | S3 |
| `TemporalAACommon.hlsl` | header | S3 (Upscale mode 3 uses `CatmullRom5Taps`) |
| `TemporalAA.hlsl` | header | S5 |
| `VelocityVS.hlsl` | Vertex 5.0 | S4 |
| `VelocityPS.hlsl`, `VelocityMaskedPS.hlsl`, `VisualizeTemporalAAPS.hlsl` | Pixel 5.0 | S4 |
| `ResponsiveAAPS.hlsl` | Pixel 5.0 | S7 |
| `PostProcessUpscale_Nearest_PS.hlsl` … `_Gaussian_PS.hlsl` (6) | Pixel 5.0 | S3 |
| `TemporalAA_Main_{Low,Medium,High,MediumHigh}_CS.hlsl` (4), `TemporalAASelfTest_CS.hlsl` | Compute 5.0 | S5 |
| `TemporalAA_Upsampling_{Low,Medium,High,MediumHigh}_CS.hlsl` (4), `TemporalAA_SuperSampling_CS.hlsl`, `TemporalAAMitchellNetravali_CS.hlsl` | Compute 5.0 | S6 |
| `TemporalAA_Main_Low_Downsample_CS.hlsl`, `TemporalAA_Upsampling_Low_Downsample_CS.hlsl` | Compute 5.0 | S7 |

That is **6 headers and 24 compiled shaders: 13 compute and 11 graphics**. The graphics shaders are 1 VS, VelocityPS, VelocityMaskedPS, VisualizeTemporalAAPS, ResponsiveAAPS and the 6 upscale PS. The compute shaders are the 11 TAA wrappers, Mitchell-Netravali and SelfTest.

`FxCompile` items added per stage. Every file is one `<FxCompile>` item: compiled files carry the two configuration conditions, and headers carry `ExcludedFromBuild` for both. Each stage's vcxproj diff must match this row exactly:

| Stage | Compiled (graphics + compute) | Headers (`ExcludedFromBuild`) | FxCompile items added |
|---|---|---|---|
| S3b | 6 + 0 (upscale PS) | 2 (`PostProcessUpscale.hlsl`, `TemporalAACommon.hlsl`) | 8 |
| S4 | 4 + 0 (VelocityVS, VelocityPS, VelocityMaskedPS, VisualizeTemporalAAPS) | 3 (`VelocityCommon`, `BasePassVertexCommon`, `VelocityPixelShader`) | 7 |
| S5 | 0 + 5 (4 Main wrappers, SelfTest) | 1 (`TemporalAA.hlsl`) | 6 |
| S6 | 0 + 6 (4 Upsampling wrappers, SuperSampling, Mitchell-Netravali) | 0 | 6 |
| S7 | 1 + 2 (ResponsiveAAPS, 2 Downsample wrappers) | 0 | 3 |
| **Total** | **11 + 13 = 24** | **6** | **30** |

- Every compiled file gets a `<FxCompile Include="Shader\X.hlsl">` item with `ObjectFileOutput` = `Shader/cso/%(Filename).cso`, `ShaderType` and `<ShaderModel>5.0</ShaderModel>`. Each of those needs a separate `Condition` for **both** `Debug|x64` and `Release|x64`: copy the `GeometryVS.hlsl` block at `DirectX12.vcxproj:355-362`.
- Headers get `ExcludedFromBuild=true` for both configurations (the `AutoExposureCommon.hlsl` block at `:227-230`).
- Filters:
  - TAA, upscale and visualize shaders under `Shader\PostProcess`.
  - Velocity and responsive shaders under `Shader`.
  - Headers under `Shader\Header`.
  - New C++ files: `TemporalAA*`, `PostProcessUpscale*` and `ScreenPercentage*` under `ソース ファイル\System\Render\PostProcess`; the rest under `ソース ファイル\System\Render`.
- Compiler rules:
  - No name ends in `RT_CS`, so fxc SM 5.0 is used for all of them.
  - No compute shader uses implicit-derivative `Sample` (fxc X4532); all use `Load`/`SampleLevel`.
  - Groupshared use is 64 × 16 B = 1 KB.
  - `reversebits` is valid in cs_5_0.
- The wrapper content is exactly:

  ```hlsl
  // TemporalAA_Upsampling_High_CS.hlsl
  #define TAA_PASS_CONFIG 1
  #define TAA_QUALITY 2
  #define TAA_DOWNSAMPLE 0
  #include "TemporalAA.hlsl"
  ```

---

## 7. Settings, UI and INI

### 7.1 Persisted settings (`FAntiAliasingParams`, INI section `[AntiAliasing]`, key = field name)

- `ReadAntiAliasing` uses the NaN-safe EditorViewport form for floats: `v = (v == v) ? std::clamp(v, lo, hi) : Default.v` (`SettingsManager.cpp:1768-1780` pattern). Ints use `std::clamp`.
- The clamp ranges are exactly the slider ranges. Every slider passes `ImGuiSliderFlags_AlwaysClamp`.
- The "Stage" column says when the control becomes live. Until then it is drawn disabled with `TextDisabled("(S<n>)")`. The field itself is persisted from S1.

| UE CVar | Field | Default | Range / values | ImGui widget ("Anti-Aliasing" window) | Live from |
|---|---|---|---|---|---|
| `r.AntiAliasingMethod` | `AntiAliasingMethod` | 2 | {0 None, 2 TemporalAA}. On INI load: 1→0, 3→0, 4→2; anything else outside {0,2} → 2 | `BeginCombo "Method"`: None / FXAA (n/a) / Temporal AA / MSAA (n/a) / TSR (n/a). The n/a entries use `ImGuiSelectableFlags_Disabled`. | S5 |
| `r.ScreenPercentage` | `ScreenPercentage` | 100 | [10, 200] | `SliderFloat "Screen Percentage" "%.0f %%"` on a local copy, committed on `IsItemDeactivatedAfterEdit()`; `SameLine + TextDisabled` shows the effective clamp ("TAAU: 50–200") and the resulting R | S3 |
| `r.TemporalAA.Upsampling` | `bTemporalAAUpsampling` | true **[PORT]** (UE4 default 0; the port's purpose is TAAU) | bool | `Checkbox "Temporal Upsampling (TAAU)"` | S6 |
| `r.TemporalAA.Quality` | `TemporalAAQuality` | 2 | [0, 3] | `Combo "Quality"`: 0 Low / 1 Medium / 2 High / 3 Medium High | S5 |
| `r.TemporalAASamples` | `TemporalAASamples` | 8 | [1, 64] | `SliderInt "Samples"`; `TextDisabled` shows the effective N and index | S5 |
| `r.TemporalAACurrentFrameWeight` | `TemporalAACurrentFrameWeight` | 0.04 | [0, 1] | `SliderFloat "%.3f"` | S5 |
| `r.TemporalAAFilterSize` | `TemporalAAFilterSize` | 1.0 | [0.1, 2] | `SliderFloat`. Below about 0.75 with Catmull-Rom, the §4.8.6 guards may reduce a frame's weights to the nearest sample. | S5 |
| `r.TemporalAACatmullRom` | `bTemporalAACatmullRom` | false | bool | `Checkbox` (effective for Main only; `TextDisabled` otherwise) | S5 |
| `r.TemporalAAUpsampleFiltered` | `bTemporalAAUpsampleFiltered` | true | bool | `Checkbox` (MainUpsampling) | S6 |
| `r.TemporalAA.HistoryScreenPercentage` | `TemporalAAHistoryScreenPercentage` | 100 | [100, 200] | `SliderFloat "%.0f %%"`, commit on release; shows `H` | S6 |
| `r.TemporalAA.R11G11B10History` | `bTemporalAAR11G11B10History` | true | bool | `Checkbox` + `TextDisabled("(Quality 0/1, not SuperSampling)")`; "unsupported" when the capability check failed | S7 |
| `r.TemporalAA.AllowDownsampling` | `bTemporalAAAllowDownsampling` | true | bool | `Checkbox` + `TextDisabled("(Quality 0)")` | S7 |
| `r.Upscale.Quality` | `UpscaleQuality` | 3 | [0, 5] | `Combo "Spatial Upscale"`: Nearest / Bilinear / Directional / Catmull-Rom / Lanczos-3 / Gaussian Unsharp | S3 |
| `r.Upscale.Softness` | `UpscaleSoftness` | 1.0 | [0, 1] | `SliderFloat` (mode 5) | S3 |
| `r.Tonemapper.MergeWithUpscale.Mode` | `TonemapperMergeWithUpscaleMode` | 0 | [0, 2] | `Combo`: Off / Always / Threshold | S3 |
| `r.Tonemapper.MergeWithUpscale.Threshold` | `TonemapperMergeWithUpscaleThreshold` | 0.49 | [0, 1] | `SliderFloat` | S3 |
| `r.ViewTextureMipBias.Offset` | `ViewTextureMipBiasOffset` | −0.3 | [−2, 1] | `SliderFloat`; shows the resulting bias | S6 (plumbed S2) |
| `r.ViewTextureMipBias.Min` | `ViewTextureMipBiasMin` | −2.0 | [−4, 0] | `SliderFloat` | S6 |
| engine `CameraRotationThreshold` | `CameraRotationThreshold` | 45 | [0, 180] ° | `SliderFloat "%.0f deg"` | S1 |
| engine `CameraTranslationThreshold` (10000 cm) | `CameraTranslationThreshold` | 100 | [0, 10000] m | `DragFloat "%.1f m"` + explicit clamp | S1 |

**Per-material setting.** `Material::bEnableResponsiveAA` corresponds to UE `UMaterial::bEnableResponsiveAA`, default false.
- It is a CPU-only member **outside** `MATERIAL Params`, so the b2 layout (224 B) is unchanged. Accessors: `ShouldEnableResponsiveAA()` and `SetEnableResponsiveAA(bool)`. `Material` is copied by value into `FSlot`, so proxies receive the flag.
- UI: in `ImGuiManager::DrawMaterialEditor`, a `Checkbox("Responsive AA")` goes right after "Two Sided" (`ImGuiManager.cpp:~1813`). It is enabled only for Translucent/Additive blend modes and is `TextDisabled` otherwise. A change returns `true`, so the caller calls `MarkRenderStateDirty` and the proxy is recreated.
- INI: `MaterialSnapshot::EnableResponsiveAA`, written and read as `mp + "EnableResponsiveAA"` next to `TwoSided` (`SettingsManager.cpp:1146`, `:1313`). Capture and Apply go through the accessors (`:727`, `:772`). Live from S7.

### 7.2 Not persisted (`FTemporalAADebugSettings`; the Lumen `DebugMode` convention)

Each control shows `SameLine(); TextDisabled("(not saved)")`.

| Field | Widget | Live from |
|---|---|---|
| `DebugView` | `Combo "Visualize"`: Off, 1 MotionVectors, 2 VelocityMask, 3 InputOutputSplit, 4 TemporalUpscalerIO, 5 BlendFinal, 6 Rejection, 7 HistoryClamp, 8 ReprojectionError, 9 FilteredTemporalWeight, 10 ClosestDepthOffset, 11 ResponsiveMask, 12 DynamicAntiGhost, 13 InputSampleAlignment | 1–2: S4; 3, 5–13: S5; 4: S6 |
| `VisualizeScale` | `SliderFloat` [0.1, 64], log | S4 |
| `OverrideTemporalIndex` | `SliderInt` [−1, N−1] (−1 = off) | S2 |
| `FilteredTemporalWeightMode` | `Combo`: Sum / Nearest / One | S6 |
| `bForceJitterWithoutTAA`, `bDisableJitter` | `Checkbox` | S2 |
| `bForceVelocityPass`, `bDisableVelocitySmallObjectCull` | `Checkbox` | S4 |
| `bForceResponsiveAA` | `Checkbox` | S7 |
| `bRequestHistoryReset` | `Button "Reset History"` | S1 (cut) |
| `bRequestSelfTest` | `Button "Run Self Test"`. It sets the flag; the tests run at the next `BeginFrame` top, never inside the ImGui build. | S1 (CPU), S5 (GPU) |
| `bRequestReallocate` | `Button "Reallocate Render Targets"` | S3a |
| – | `Button "Camera Cut Now"` → `m_World->RequestCameraCut()` | S1 |
| – | `Button "Capture Screenshot (F9)"` → `m_SceneRenderer->RequestScreenshot("")` | ST |

### 7.3 ImGui layout

**Settings ▸ Anti-Aliasing** opens `ImGuiManager::AntiAliasingWindow()`.
- New layout flag `bool bShowAntiAliasing = false;` in `FLayoutSettings`. It is persisted in `[ImGui]` by `Write/ReadImGuiLayout` and included in Debug ▸ Show All / Hide All. Update the comment at `ImGuiManager.h:18-20` ("Settings : Lumen / Anti-Aliasing").
- `Draw()` gains `if (m_Layout.bShowAntiAliasing) AntiAliasingWindow();`. `SettingsMenu()` gains `ImGui::MenuItem("Anti-Aliasing", nullptr, &m_Layout.bShowAntiAliasing);`.

```cpp
void ImGuiManager::AntiAliasingWindow()
{
    ImGui::Begin("Anti-Aliasing", &m_Layout.bShowAntiAliasing);
    FAntiAliasingParams& p = m_SceneRenderer->GetAntiAliasingParams();
    FTemporalAADebugSettings& d = m_SceneRenderer->GetTemporalAADebugSettings();
    const FViewFamilyInfo& F = m_SceneRenderer->GetViewFamily();
    const FViewInfo& V = m_SceneRenderer->GetViewInfo();
    const FTemporalAAStats& s = m_SceneRenderer->GetTemporalAAStats();

    // ---- 統計 (Text 行 + Separator) ----
    // Method / ScreenPercentageMethod / Pass / Quality ; O / R / S / H / P (実 PostExtent) ; UF
    // Jitter index/N, jitter px ; Mip bias ; CameraCut / PrevTransformsReset (このフレーム)
    // History valid / format / extent ; Velocity draws ; Merge ; TAA ran ; PSO fallback
    // Resizes ; Free SRV / RTV ; Deferred queue ; VRAM CurrentUsage (MB) ; Self test failures
    ImGui::Separator();
    // Method combo, Screen Percentage (commit-on-release), Temporal Upsampling
    if (ImGui::CollapsingHeader("Temporal AA (r.TemporalAA*)", ImGuiTreeNodeFlags_DefaultOpen)) { /* §7.1 の TAA 行 */ }
    if (ImGui::CollapsingHeader("Spatial Upscale (r.Upscale*, r.Tonemapper.MergeWithUpscale*)")) { /* Upscale 行 */ }
    if (ImGui::CollapsingHeader("Texture Mip Bias (r.ViewTextureMipBias*)")) { /* Offset / Min */ }
    if (ImGui::CollapsingHeader("Camera (Cut / Large Movement)")) { /* 閾値 + "Camera Cut Now" */ }
    if (ImGui::CollapsingHeader("Debug (not saved)")) { /* §7.2 */ }
    if (ImGui::Button("Reset to Default")) m_Settings->ResetAntiAliasing();
    // TemporalUpscalerIO の 4 象限ラベル: DebugView == 4 のとき GetForegroundDrawList()->AddText (出力座標 = クライアント座標)
    ImGui::End();
}
```

- `FSceneRenderer` gains the getters `GetAntiAliasingParams()`, `GetTemporalAADebugSettings()`, `GetViewFamily()`, `GetViewInfo()`, `GetViewState()` and `GetTemporalAAStats()`.
- The VRAM figure comes from `m_RHI->QueryLocalVideoMemoryUsage()` once per second.

**G-Buffer window** (`BufferWindow`, after LinearDepth at `ImGuiManager.cpp:214`) gains three `200×100` images:
- "Velocity": the raw SRV. Static-but-written pixels are olive (0.5, 0.5); unwritten pixels are black. From S4.
- "TAA History": `m_SceneRenderer->GetViewState().GetHistoryTexture(PrevFrameViewInfo.TemporalAAHistory)->RT->SRVHandle`, shown when valid. From S5.
- "Responsive AA": the mask SRV. From S7.

**Edit ▸ Reset to Default** gains `"Anti-Aliasing"` → `m_Settings->ResetAntiAliasing()`.

**F9** (`GameManager::Update`, only when `!ImGui::GetIO().WantCaptureKeyboard`) calls `m_SceneRenderer.RequestScreenshot("")`, which writes to an automatic path in `Saved/Screenshots/`.

### 7.4 SettingsManager (the Lumen template; the params are reached through `m_SceneRenderer`, so there is no new cached pointer and no `m_VolumetricFog` bug class)

| Function | Change |
|---|---|
| Header | `#include "AntiAliasingSettings.h"`; `FAntiAliasingParams m_DefaultAntiAliasing{};`; `static void WriteAntiAliasing(ConfigFile&, const FAntiAliasingParams&)`; `static void ReadAntiAliasing(const ConfigFile&, FAntiAliasingParams&)`; public `void ResetAntiAliasing();`; `void SetSaveEnabled(bool b) { m_bSaveEnabled = b; }` with `bool m_bSaveEnabled = true;`. Update the doc block at `SettingsManager.h:14-69`. |
| `CaptureDefaults` | `if (m_SceneRenderer) m_DefaultAntiAliasing = m_SceneRenderer->GetAntiAliasingParams();` |
| `LoadAndApply` | `if (m_SceneRenderer && ini.HasSection("AntiAliasing")) ReadAntiAliasing(ini, m_SceneRenderer->GetAntiAliasingParams());`, placed after `[Translucency]` |
| `SaveCurrent` | first line: `if (!m_bSaveEnabled) return false;`. Then `WriteAntiAliasing`, after `[Translucency]` and before `[Lumen]`. |
| `ResetAntiAliasing` | `m_SceneRenderer->GetAntiAliasingParams() = m_DefaultAntiAliasing;` (the debug settings live in another struct and are untouched) |
| `ResetAll` | add `ResetAntiAliasing();` |
| `Write/ReadImGuiLayout` | `bShowAntiAliasing` |
| Material snapshot | `EnableResponsiveAA` (§7.1) |
| `ApplyComponent` (static) | Primitive: `if (UWorld* w = primitive->GetWorld()) w->GetScene()->MarkPrimitiveTeleported(primitive);` (S4). Camera: `camera->NotifyCameraCut();` (S1). §4.5.1 has the code. |

`ReadAntiAliasing` validation:
- `AntiAliasingMethod`: map 1→0, 3→0, 4→2, and anything outside {0,2} → 2.
- Clamp every other field as in §7.1.
- A `nan` value falls back to the default.

### 7.5 Camera-cut API (game side)

```cpp
// SceneView.h (FSceneView に追加)
bool        bCameraCut = false;       // UE FSceneView::bCameraCut
const void* CameraId   = nullptr;     // 統計表示用 (アクティブカメラ)

// CameraComponent.h (UCameraComponent に追加)
void NotifyCameraCut() { m_bCameraCutPending = true; }            // テレポート / Reset / Load
bool IsCameraCutPending() const { return m_bCameraCutPending; }    // ACameraActor::Tick が読む (消費しない)
bool ConsumeCameraCut() { const bool b = m_bCameraCutPending; m_bCameraCutPending = false; return b; }
private: bool m_bCameraCutPending = false;

// World.h (UWorld に追加)
void RequestCameraCut() { m_bCameraCutRequested = true; }
FSceneView CalcSceneView(float AspectRatio);                         // const を外す
private: bool m_bCameraCutRequested = false; const UCameraComponent* m_LastViewCamera = nullptr;
```

```cpp
// World.cpp
FSceneView UWorld::CalcSceneView(float AspectRatio)
{
    FSceneView view;
    UCameraComponent* camera = m_Scene.GetActiveCamera();
    if (camera) { camera->GetSceneView(view, AspectRatio); view.bCameraCut = camera->ConsumeCameraCut(); }
    view.bCameraCut = view.bCameraCut || m_bCameraCutRequested;       // ゲーム要求
    m_bCameraCutRequested = false;
    if (camera != m_LastViewCamera) { view.bCameraCut = true; m_LastViewCamera = camera; }   // アクティブカメラ変更
    view.CameraId = camera;
    /* 既存のポストプロセス解決 */
    return view;
}

// Camera.cpp (ACameraActor::Tick の先頭)
if (m_CameraComponent && m_CameraComponent->IsCameraCutPending()) m_CameraController.ResetVelocity();   // テレポート後に慣性を残さない
if (!m_bInputEnabled) { /* テストドライバ: 入力インパルス 0 のまま UpdateSimulation へ */ }
```

`ACameraActor::SetInputEnabled(bool)` gates the whole input block of `Tick`: the mouse, wheel and keyboard impulses stay zero. The controller then only releases its pending motion, which `ResetVelocity()` clears at test start.

---

## 8. Staged implementation plan

**Rules for every stage:**
- **Build.** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" DirectX12.vcxproj /p:Configuration=Release /p:Platform=x64 /m /fl /flp:logfile=<scratch>\build_<stage>.log`. Zero errors, and no new warnings: compare `warning` counts with the previous stage's log. Also build Debug|x64 from S0 on.
- **Run from the repo root.** Before any interactive run, back up `Saved/Config/EngineSettings.ini` and `imgui.ini` and restore them afterwards. Driver runs (`-taatest`) write neither file.
- **Pass the stage's acceptance checks** (§9 procedures), plus the `.cso` rule of §9.1 and the debug-layer diff of §9.6. Run the debug-layer diff from S0 on.
- **Report.** The stage report lists every changed `.cso` with a one-line justification, the captures and their metrics, the debug-layer ID set and the free-descriptor counts.
- **Order.** The stages run strictly in this order: **ST → S0 → S1 → S2 → S3a → S3b → S4 → S5 → S6 → S7 → S8**. They all touch `SceneRenderer.cpp`, `RenderManager.cpp` or the vcxproj, so none of them runs concurrently. References to "S3" elsewhere in this document mean S3b, which completes the screen-percentage stage.
- **Images.** "Baseline-equal" means `imgdiff` mean |Δ| ≤ max(0.5/255, 1.5·N0) and 99.9th percentile ≤ max(8/255, 1.5·P0), where N0 and P0 are the run-to-run noise floor measured in ST (§9.5).

### ST: verification harness (no rendering change)

**New:** `ScreenshotCapture.h/.cpp`, `TemporalAATestDriver.h/.cpp`.

**Changed:**
- `Main.cpp`: passes `lpCmdLine` to `GameManager`; the loop also breaks when `gameManager.ShouldExit()`; `wWinMain` returns `gameManager.GetExitCode()`, saved before the `GameManager` scope ends.
- `GameManager.h/.cpp`: the constructor becomes `GameManager(HWND, const wchar_t* CmdLine)`. Owns `FTemporalAATestDriver m_TestDriver`. `Begin` calls `m_TestDriver.OnBegin(...)` after `SettingsManager::Initialize`. `Update` calls `m_TestDriver.PreWorldTick(...)` before `m_World.Tick`, and handles F9. `Draw` skips `m_ImGuiManager.Draw()` when `m_TestDriver.IsActive()` and calls `m_TestDriver.PostFrame(...)` after `EndFrame`.
- `Time.h/.cpp`: `static void SetFixedDeltaTime(float Seconds)`. When non-zero, `Update` uses `unscaledDeltaTime = Seconds` and `deltaTime = Seconds · timeScale` instead of the measured value (`lastFrameTime` is still updated).
- `Camera.h/.cpp`: `SetInputEnabled(bool)`.
- `SettingsManager.h/.cpp`: `SetSaveEnabled`.
- `SceneRenderer.h/.cpp`:
  - `std::unique_ptr<FScreenshotCapture> m_Screenshot` and `RequestScreenshot(const std::string&)`.
  - `m_Screenshot->ResolvePending()` at the top of `BeginFrame`.
  - `RecordCopy` at the end of `RenderPostProcessing`, before the depth barrier, followed by re-binding the back-buffer RTV.
  - `FlushScreenshots()` (a `WaitGPU` plus write), called by the driver before exit.
- vcxproj and filters.

**`FScreenshotCapture`:**
- `RecordCopy`: back buffer RT → COPY_SOURCE; `CopyTextureRegion` into a READBACK buffer using `GetCopyableFootprints` (row pitch 7680 B, 256-aligned); COPY_SOURCE → RT.
- `ResolvePending` / `FlushScreenshots`: `WaitGPU`, `Map`, then write a 24-bit bottom-up BMP (BGR swap, rows padded to 4 B).
- Paths: `-taaout/<scenario>_f<frame>.bmp` for the driver, and `Saved/Screenshots/<YYYYMMDD_HHMMSS>_<frame>_<SP>_<method>_<pass>_Q<q>.bmp` for F9 (the local-time prefix keeps names from different sessions apart).

**Driver:** §9.3 defines the command line and scenarios. In ST only `static`, `pan` and `rotate` (camera only), `-taaframes`, `-taacapture`, `-taaout`, `-grain` and the log are functional. Each later stage adds its keys and scenarios, listed in that stage.

**Acceptance:**
1. `x64\Release\DirectX12.exe -taatest=static -taaframes=120 -taacapture=60,120 -taaout=<scratch>\st` exits with 0 and writes two 1920×1080 BMPs and a 120-line `taatest_log.txt`. `EngineSettings.ini` and `imgui.ini` have the same SHA-256 before and after.
2. Run it twice. Measure the noise floor N0 (mean) and P0 (99.9th percentile) between the two frame-120 captures, and record them. The frame-120 capture is **B0**, the reference image. Also capture `rotate` and `pan` at frame 120 (B0-rotate, B0-pan).
3. Interactive: F9 writes a BMP. Exiting normally still saves the INI.
4. `.cso` byte-identical, because no HLSL changed.

### S0: RHI groundwork (no visible change)

**New:** `Halton.h`, `SceneRenderingUtils.h`.

**Changed:** `RenderManager.h/.cpp`, `SceneRenderer.cpp` (`EndFrame` rebind; the screen-pass helpers move out; the `PrepareResultForRead` call), `ShadowRendering.cpp`, `LumenScene.cpp` (viewport restores; the probe-jitter lambda → `Halton.h`), `VolumetricFog.cpp` (file-static `Halton` → `Halton.h`), `ColorGradingLUTBaker.cpp` and `AutoExposure.h/.cpp` (state fixes), vcxproj.

**Tasks:**
1. `EDepthStatePreset::None`: `DepthEnable = FALSE`, `DepthWriteMask = ZERO`, `DepthFunc = ALWAYS`. `CreatePipeline` uses `DSVFormat = (DepthPreset == None) ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_D32_FLOAT`. Pass `None` for the 10 full-screen PSOs of §3.8 **[FIX #615]**.
2. `CreateRenderTarget(..., bAllowUnorderedAccess)`: `ALLOW_UNORDERED_ACCESS` flag, a UAV descriptor (`UAVIndex`/`UAVHandle`), and `Width`/`Height`/`Format` stored for every target. `~RENDER_TARGET` releases the UAV index through `ReleaseShaderResourceView`.
3. `CreateDepthBuffer(W,H)` / `ReleaseDepthBuffer()` / `GetDepthBufferWidth/Height()` (§5.3); `InitDepthBuffer` → heap + `CreateDepthBuffer(BB)`.
4. `SetDefaultViewportSize`, `RestoreDefaultViewport`, `GetDefaultViewportWidth/Height`. The default stays BB in S0. The Shadow and Lumen restores call `RestoreDefaultViewport()`.
5. `GetNumFreeSRVDescriptors()`, `GetNumFreeRTVDescriptors()`, `GetDeferredReleaseQueueLength()`, `QueryLocalVideoMemoryUsage()`, where `m_Adapter`, or the adapter used at device creation, is queried via `As(&IDXGIAdapter3)`.
6. `SetConstant`: `assert(Size <= CONSTANT_BUFFER_SIZE)` plus a runtime `if (Size > CONSTANT_BUFFER_SIZE) { OutputDebugStringA(...); return; }`. `AllocateRTVSlot` gets the same empty check as `AllocateSRVSlot`.
7. Debug-only environment switches in `InitDevice`, read with `GetEnvironmentVariableA`:
   - `DX12_DEBUG_GBV=1` calls `ID3D12Debug1::SetEnableGPUBasedValidation(TRUE)` right after `EnableDebugLayer`, before device creation.
   - `DX12_DEBUG_NO_BREAK=1` skips both `SetBreakOnSeverity` calls (`RenderManager.cpp:188-189`).
8. `EndFrame` first rebinds the SRV heap, the back-buffer RTV and viewport/scissor `O` (§4.1 step 11).
9. `Halton.h` holds `inline float Halton(uint32_t Index, uint32_t Base)`, the UE formula. VolumetricFog and Lumen switch to it: identical math, identical output.
10. **[FIX] ColorGradingLUT states** (§0.4 #38). The bake CS reads t0 (`ColorGradingLUTBaker.cpp:318-321`), but both possible t0 textures sit in PSR:
    - `m_FallbackTex`, bound whenever no artist LUT is loaded (including the constructor's bake at `:200`), is created in PSR at `:222`. Create it in `PIXEL|NON_PIXEL` instead.
    - `LoadArtistLUT` leaves the `LoadTexture` result in PSR (`:245`). The committed `EngineSettings.ini` loads `Asset/Texture/LUTs\Adventure.DDS` on every run. After `LoadTexture`, record one transition PSR → `PIXEL|NON_PIXEL` into the open command list.
11. **Screen-pass helpers.** Move `TransitionToRenderTarget`, `TransitionToShaderResource` and `SetViewportAndScissor` from the `SceneRenderer.cpp` anonymous namespace (`:27-57`) into `SceneRenderingUtils.h` as `inline` functions, unchanged, and add the unused `TransitionReadToRenderTarget` / `TransitionRenderTargetToRead` (§2.1). This is a pure move.
12. **[FIX] AutoExposure result-buffer states** (§4.12, edits 1–5): `PrepareResultForRead()` as the first statement of `RenderPostProcessing`; RD in the pass-2 and end barriers; `IsResultValid()`.

**Acceptance:**
1. `.cso` byte-identical.
2. `static`, `rotate` and `pan` captures are baseline-equal to B0. The LUT and AE fixes only change barrier states.
3. Debug|x64 **without** `DX12_DEBUG_NO_BREAK`: `-taatest=static -taaframes=300` exits 0, so #615 is gone.
   - If another pre-existing ERROR still breaks, record it in the report, then use `DX12_DEBUG_NO_BREAK=1`.
   - **D0** is the debug-layer baseline (§9.6). It has two parts:
     - **D0-plain**: the DBWIN histograms of `static`, `rotate` and `pan`.
     - **D0-GBV**: the same three scenarios with `DX12_DEBUG_GBV=1`.
4. D0-GBV contains no `RESOURCE_STATE`, `INVALID_DESCRIPTOR` or barrier-mismatch message that names `ArtistLUTFallback`, the artist LUT or `AutoExposureResult`. That shows fixes 10 and 12 worked. Any remaining pre-existing ID of those classes is recorded by name in the report.

### S1: view framework, previous-frame commit, camera cut, settings skeleton (no visible change)

**New:** `AntiAliasingSettings.h`, `ViewMatrices.h`, `SceneViewState.h/.cpp`, `ScreenPercentage.h/.cpp` (all gates false, so `R = O`), `TemporalAASelfTest.h/.cpp` (CPU part).

**Changed:**
- `SceneRenderer.h/.cpp`:
  - members (§2.5), with `m_Prev*` and `m_bHistoryValid` **deleted**
  - `PrepareViewRectsForRendering` (family only; until S5 there is no `m_TemporalUpscaler`, so pass `bR11G11B10Supported = false` and skip the `IsReady` fallback — the gates keep TAA off)
  - `PrepareViewStateForVisibility`: no b0 extension yet; it writes the first 256 B exactly as before
  - `CommitViewState`
  - `CopySceneColorHistory`: matrix block deleted
  - `MakeLumenFrameInputs` and the fog inputs per §4.9
  - `ComputeViewVisibility` builds the frustum from `m_ViewInfo.ViewMatrices.ViewProjectionNoAAMatrix`, untransposed, and only when `m_ViewInfo.bValid` (§4.4, "Culling on invalid frames"); this replaces `m_ViewConstant.View*Projection` at `:450-452`
  - the per-frame flag reset at the top of `BeginFrame` (§4.1 step 1; the flags exist from S1, and their writers arrive in S4/S7)
  - self-test request at the top of `BeginFrame`
- `SceneView.h`, `World.h/.cpp`, `CameraComponent.h/.cpp`, `Camera.h/.cpp`: §7.5.
- `SettingsManager.h/.cpp`: the `[AntiAliasing]` group with every §7.1 field; the camera latch in `ApplyComponent`.
- `ImGuiManager.h/.cpp`: the AA window with stats and every control, disabled until its stage; the menu entries; the layout flag.
- `TemporalAATestDriver.cpp`: `-sp`, `-aa`, `-taaq` and the other overrides now write `FAntiAliasingParams`, without persisting; the `cut` and `largemove` scenarios; the log columns `bCameraCut`, `bPrevTransformsReset`, Lumen/fog `bHistoryValid`.
- vcxproj.

**`ComputeViewFamilyInfo` is split** into `ComputeViewFamilyInfoEx(p, O, bR11Supported, const FTAAStageGateValues& g)` and the gated wrapper. Self-test T12 calls `Ex` with all gates true.

**Acceptance:**
1. `.cso` byte-identical.
2. `static`, `rotate` and `pan` captures are baseline-equal to S0. The math is identical: `XMMatrixMultiply` / `XMMatrixInverse` in the same order, with zero jitter.
3. `cut`: the log shows `bCameraCut = 1` at exactly frame 60, and Lumen/fog `bHistoryValid = 0` only at frame 60. `largemove`: `bPrevTransformsReset = 1` only at frame 60.
4. INI round trip: the `[AntiAliasing]` section is written on exit (interactive run). Hand-edited `ScreenPercentage=nan` and `AntiAliasingMethod=7` reload as 100 and 2.
5. `-taaselftest`: T1, T3, T13, T15 pass; T12 passes with `Ex` (all gates on).

### S2: jitter, b0 extension, mip-bias plumbing (jitter gated off by default)

**Changed:**
- `RenderManager.h`: `VIEW_CONSTANT` 448 B with the static_asserts.
- `ConstantBuffers.hlsl`: b0.
- `SceneRenderer.cpp`: the full b0 fill and the jitter of §4.4, active only under `bTemporalAA` (gated false) or `bForceJitterWithoutTAA`.
- `SceneViewState.cpp`: `ComputeTemporalAASample`, `ComputeClipToPrevClip` (camera-relative, double; §4.4).
- `GeometryPS.hlsl`, `TranslucentPS.hlsl`: `SampleBias`.
- `TemporalAASelfTest.cpp`: T2, T4, T4b, T5.
- `ImGuiManager.cpp`: jitter debug controls.
- Driver: `-forcejitter`, `-nojitter`, `-samples`, `-overrideindex`.

**Acceptance:**
1. `.cso`: `fxc /dumpbin` instruction streams (§9.1) are identical for every pre-existing shader except GeometryPS and TranslucentPS. In those two, the 3 material `sample` instructions each become `sample_b` with the bias operand, and a `dcl_constantbuffer CB0[…]` declaration may appear or grow because b0 is now read. Every compute `.cso` is byte-identical.
2. The default `static`/`rotate` captures are baseline-equal to S1 (bias 0, jitter off).
3. `-forcejitter=1 -samples=1`: baseline-equal to S1 (N = 1 → zero offset). `-forcejitter=1 -samples=8`: the frame-to-frame (F2F) diff exceeds 2·N0 (the image moves).
4. `-taaselftest`: T1–T5 (including T4b), T12, T13, T15 pass.

### S3a: resizable render targets (no visible change)

**Changed:**
- `SceneTextures.h/.cpp`: `Init(RHI,W,H)`, `Release`, `Extent`, `LinearDepthDisplaySRVIndex`.
- `SceneRenderer.h/.cpp`: `ResizeRenderTargets` exactly as §5.2, including `bRequestReallocate`; `InitDOF(W,H)` and `InitBloom(PW,PH)`; the constructor order of §2.5.
- `LumenScene.h/.cpp`: `InitGlobalTextures`, `CreateScreenTextures`, `ReleaseScreenTextures`, `ReleaseComputeTexture`. No jitter-phase reset (§5.3).
- `VolumetricFog.h/.cpp`: `CreateVolumes` (clears `m_bHistoryValid` only), `ReleaseVolumes`, `m_ViewWidth/Height`.
- `LightGridInjection.h/.cpp`: capacity `Init(CapW,CapH)` + `SetViewSize`.
- `ImGuiManager.cpp`: the "Reallocate" button.
- Driver: the `realloc` scenario (sets `bRequestReallocate` every `-reallocperiod` frames, default 90).

**Acceptance:**
1. `.cso` byte-identical, because no HLSL changed.
2. `static` baseline-equal to S2.
3. **Image check after reallocation.** Run `realloc -taaframes=660 -taacapture=150,330,510`, which reallocates at 90, 180, …, 630. Its captures are frames realloc + 60, and each must be baseline-equal to the `static -taacapture=150,330,510` capture at the **same frame index**.
   - The 60-frame settle covers the volumetric-fog history (HistoryWeight 0.9, `VolumetricFog.h:83`, enabled by `GameManager.cpp:178`; 0.9⁶⁰ ≈ 0.002) and the Lumen DiffuseIndirect/ProbeSH accumulation.
   - Comparing equal frame indices keeps the TAA, fog and Lumen jitter phases aligned. N0 is measured at equal frame indices, so it covers only run-to-run noise.
4. **Validity flags, log only.** Lumen and fog show `bHistoryValid = 0` at exactly the reallocation frames, and 1 on every other frame after the first.
5. **Leak check** (counters only, no image criterion): `realloc -reallocperiod=30 -taaframes=650`, which reallocates 21 times, at 30, 60, …, 630. The free SRV and RTV counts and the deferred-queue length at frame 29 equal those at frame 650 (±0), per the §9.4 rule. VRAM is within 5 %.
6. Debug layer (§9.6): the plain ID set is ⊆ D0-plain.

### S3b: screen percentage, primary spatial upscale, runtime resolution change

**New:** `PostProcessUpscale.h/.cpp`; shaders `PostProcessUpscale.hlsl`, the 6 wrappers and `TemporalAACommon.hlsl`.

**Changed:**
- `ScreenPercentage.h`: `kScreenPercentage = true`.
- `SceneRenderer.h/.cpp`:
  - default viewport = R; `ApplyRenderViewport`
  - texel rules
  - DOF at R with the `MaxBlurSize` scale
  - `RenderBloom(Input, Restore)`
  - `DrawTonemap`, `EnsureTonemapOutput`, the merge logic, `SelectPrimaryUpscalePipeline`, `AddPrimaryUpscalePass`
  - AutoExposure sizes
  - `MakeLumenFrameInputs` R; `SetViewSize(R)`
  - the rest of the §4.7 post body **without** the TAA block
- `RenderManager.h/.cpp`: the optional-PSO path, `CreatePipeline(..., bOptional)` + `HasPipelineState` (§3.8); the `PostProcessUpscale0..5` PSOs, created optional.
- `PostProcessSettings.h` + `ConstantBuffers.hlsl`: b4 pad renames.
- `ImGuiManager.cpp`: Screen Percentage (commit on release), the Upscale rows, the stats.
- Driver: `-upscaleq`, `-merge`; the `resize` scenario.

**Acceptance:**
1. AA None at SP 100 is baseline-equal to S3a: no upscale pass, no merge.
2. `-sp=50 / 71 / 150 / 200` (AA None, `-upscaleq=3`):
   - The stats and log show R = 960×540, 1364×767, 2880×1620, 3840×2160.
   - The mean |Δ| vs B0 is ≤ 8/255. A framing or scale error produces a strong edge-shaped diff far above this.
   - `imgdiff`'s `diff.bmp` shows no uniform offset.
   - Lumen, fog and the light grid show no tile or grid seams (visual check of the SP 50 capture).
3. All `-upscaleq=0..5` run at SP 50. Mode 0 is visibly blocky; mode 3 is the default look. `-merge=1` and `-merge=2` run, and merged captures differ from unmerged ones only by the upscale filter. Missing-`.cso` check, Release: temporarily rename `PostProcessUpscale_CatmullRom_PS.cso`. The run logs the fallback once, and the SP 50 capture equals `-upscaleq=1` within the noise floor, not black. Restore the file afterwards.
4. `resize` (AA None) under the Debug build: the ID set is ⊆ D0-plain. The free SRV/RTV counts are equal at frame 20 and at the end. VRAM is within 5 %.
5. DOF on (PP flag via `-dof=1`) at SP 50 vs SP 100: the blur radius looks the same (visual A/B of the defocused background crop).
6. `.cso`: instruction streams unchanged for existing shaders; only the new upscale shaders are added.

### S4: velocity

**New:** `SceneVelocityData.h`, `VelocityRendering.h/.cpp`; shaders `VelocityCommon.hlsl`, `BasePassVertexCommon.hlsl`, `VelocityVS.hlsl`, `VelocityPixelShader.hlsl`, `VelocityPS.hlsl`, `VelocityMaskedPS.hlsl`, `VisualizeTemporalAAPS.hlsl` (modes 1 and 2).

**Changed:**
- `RenderManager.h/.cpp`: `PRIMITIVE_CONSTANT` 128 B; `TEXTURE_TYPE::VELOCITY` / `TEMPORAL_AA_DEBUG`; the `Velocity*` PSOs and the optional `VisualizeTemporalAA` PSO.
- `ConstantBuffers.hlsl` (b1), `Resources.hlsl` (t35/t36), `GeometryVS.hlsl`.
- `Scene.h/.cpp`; `PrimitiveSceneProxy.h/.cpp`; `StaticMeshComponent.cpp`, `Field.cpp` (`DrawVelocity`; the card-capture b1 writes an identity `PreviousLocalToWorld`); `Polygon2D.cpp` (b1 identity).
- `SceneTextures.h/.cpp`: `Velocity` and `ResponsiveAAMask`, both created in S4.
- `SceneRenderer.h/.cpp`: `RenderVelocities`, `AddVisualizeTemporalAAPass` (t35 = Velocity or dummy per `m_bVelocityValid`; `HasPipelineState` check), the stats. `GameManager.cpp` calls `RenderVelocities` after `RenderBasePass`.
- `SettingsManager.cpp`: teleport marking.
- `ImGuiManager.cpp`: Velocity thumbnail; Visualize 1/2; `VisualizeScale`; `bForceVelocityPass`; the small-object toggle.
- `TemporalAASelfTest.cpp`: T6, T16.
- Driver: `moveobj` (motion stops at frame 90), `sky`, `-debugvis`.

**Acceptance:**
1. The default (AA None) `static`/`rotate` captures are baseline-equal to S3b. `precise` may change GeometryVS codegen by at most 1 ULP.
2. `static -debugvis=1`: every pixel is gray (|R−G| ≤ 2/255 and |G−B| ≤ 2/255), which means no motion colour anywhere, sky included.
3. `rotate -debugvis=1`: a smooth field, continuous across the sky/geometry boundary (visual). `rotate -debugvis=2`: the sky is **not** green (no object velocity under pure yaw).
4. `pan -debugvis=2`: the sky dome is **not** green (it writes no velocity, §4.5). `moveobj -debugvis=2 -taacapture=60,90,91`: only the Table is green at 60 and 90, and **nothing** is green at 91 (the stale-Prev hazard).
5. `moveobj -debugvis=2`: no non-green pixel inside the moving Table's silhouette. Check a crop, visually; this is depth invariance. If it fails, apply the risk-R1 fallback: `DepthBias = −4` on the `Velocity*` PSOs.
6. Reset Actor on the moving Table (interactive) gives no velocity spike on the next frame.
7. Self-tests T6 and T16 pass. The debug-layer IDs are ⊆ D0-plain, with no #820 clear-value warning.
8. `.cso`: GeometryVS may change. All shaders referencing b1 change only in RDEF (dumpbin identical), except GeometryVS. Compute shaders are byte-identical.

### S5: Temporal AA core (Main config)

**New:** `TemporalAA.h/.cpp`; shaders `TemporalAA.hlsl`, `TemporalAA_Main_{Low,Medium,High,MediumHigh}_CS.hlsl`, `TemporalAASelfTest_CS.hlsl`.

**Changed:**
- `ScreenPercentage.h`: `kTemporalAA = true`. **The default becomes TAA Main at 100 %.**
- `AutoExposure` is not edited here. Its state fixes and `IsResultValid()` landed in S0 (task 12); S5 only binds the result at t4.
- `SceneRenderer.h/.cpp`: `m_TemporalUpscaler`, the TAA block of §4.7, `IsPreTAADebugViewActive`, the split view, the visualize modes 3 and 5–13, `CommitViewState` with the history, the PSO fallback in `PrepareViewRectsForRendering`.
- `VisualizeTemporalAAPS.hlsl`: modes 3 and 5–13.
- `TemporalAASelfTest.cpp`: the GPU part, T7 (including the Catmull-Rom FS 0.5 / 0.4 / 0.1 guard vectors), T14.
- `ImGuiManager.cpp`: the TAA controls live; the "TAA History" thumbnail.
- Driver: `-cfw`, `-catmullrom`; the `aatoggle` scenario (AA off→on at frame 60).
- vcxproj.

**Acceptance** (all with `-aa=2 -upsampling=0 -sp=100` unless stated):
1. `-taaselftest`: CPU and GPU all pass. No PSO-null log line.
2. `static` convergence: the F2F diff (frames 119 vs 120) has mean ≤ F2F_off + 0.3/255 and 99.9th percentile ≤ 6/255. F2F_off is the same measurement with `-aa=0`.
3. PSNR vs the SSAA reference (§9.5) ≥ PSNR(AA off) + 1.5 dB.
4. Mean luma of the converged TAA `static` capture vs B0: |Δmean| ≤ 1/255. This checks the HDR weighting and the quantization bias.
5. `pan` and `rotate` with `-debugvis=8` (ReprojectionError): mean ≤ 25/255, bright only at disocclusions and screen edges.
6. `moveobj` Q2 and Q3: no trail longer than 2 px behind the Table at maximum speed (visual crop). Q1 may trail (UE behaviour).
7. `cut -debugvis=5 -taacapture=59,60,61`: ≥ 95 % of the pixels of frame 60 are red (IgnoreHistory → 1); frames 59 and 61 are < 5 % red. View 5 shows the **pre-floor** weight (§6.8), so converged flat pixels are dark blue, not red. `aatoggle` gives the same result at frame 60. Also `static -debugvis=5` at frame 120: dark-blue fraction ≥ 95 % (V2).
8. `-taaq=0..3` all run. Switching quality at runtime (interactive) causes no hitch beyond the first use.
9. Debug layer and GBV (§9.6, pass rule): the plain IDs are ⊆ D0-plain, the GBV IDs are ⊆ D0-GBV, and there is no `RESOURCE_STATE`, `INVALID_DESCRIPTOR` or barrier-mismatch ID that is not already in D0-GBV.
10. `.cso`: new files, plus `VisualizeTemporalAAPS.cso` (modes 3 and 5–13, §9.1). Every other pre-existing `.cso` is byte-identical to S4.

### S6: TAAU (MainUpsampling), SuperSampling + Mitchell-Netravali, active mip bias

**New:** `TemporalAA_Upsampling_{Low,Medium,High,MediumHigh}_CS.hlsl`, `TemporalAA_SuperSampling_CS.hlsl`, `TemporalAAMitchellNetravali_CS.hlsl`.

**Changed:**
- `ScreenPercentage.h`: `kTemporalUpsampling = true`. **The default becomes TAAU at 100 %.**
- `TemporalAA.cpp`: MN, and IsReady for SS.
- `SceneRenderer.cpp`: the TemporalUpscalerIO view.
- `VisualizeTemporalAAPS.hlsl`: mode 4.
- `ImGuiManager.cpp`: Upsampling, UpsampleFiltered, HSP, mip bias, FTW mode; the IO labels.
- `TemporalAASelfTest.cpp`: T8–T11.
- Driver: `-upsampling`, `-hsp`, `-mipoffset`, `-ftwmode`.

**Acceptance:**
1. `-sp=50/71/100/150` (TAAU): the log shows N = 32 / 15 / 8 / 8 and a mip bias of −1.30 / −0.79 / −0.30 / −0.30.
2. PSNR vs reference: TAAU 50 ≥ spatial (Catmull-Rom) 50 + 1 dB; TAAU 71 ≥ spatial 71.
3. `resize` (TAAU) with `-debugvis=5` (pre-floor view, §6.8) captures at each change frame: < 5 % red, which shows the TAA history is kept. The log shows Lumen/fog `bHistoryValid = 0` at exactly the change frames. Lumen and fog are checked **through the log only**; their image needs about 60 frames to re-converge (S3a #3).
4. `-hsp=200`: the log shows H = 3840×2160 and pass MainSuperSampling. No pixel is pure black where B0 is lit (no NaN). The first and last 2 columns and rows are within 4/255 of their neighbours (no border artefact).
5. TAAU 100 vs Main 100, converged: mean |Δ| ≤ 2/255.
6. `-sp=50 -mipoffset=0 -mipmin=0` vs the default: `edge_metric` on a Field texture crop is higher with the default bias.
7. The self-tests pass. Debug layer and GBV per the §9.6 pass rule (⊆ D0-plain / D0-GBV, and no new state, descriptor or barrier ID). `.cso`: new files, plus `VisualizeTemporalAAPS.cso` (mode 4, §9.1). Every other pre-existing `.cso` is byte-identical to S5.

### S7: Responsive AA, R11G11B10 history, half-resolution output

**New:** `ResponsiveAAPS.hlsl`, `TemporalAA_Main_Low_Downsample_CS.hlsl`, `TemporalAA_Upsampling_Low_Downsample_CS.hlsl`.

**Changed:**
- `ScreenPercentage.h`: `kTAAExtras = true`.
- `Material.h/.cpp` (§7.1).
- `StaticMeshComponent.cpp`, `Field.cpp`: `DrawResponsiveAA`, `HasResponsiveAATranslucency`.
- `SceneRenderer.cpp`: `RenderResponsiveAAMask` wired in `RenderTranslucency`; the half-res routing.
- `RenderManager.cpp`: `ResponsiveAA[TwoSided]` PSOs.
- `TemporalAA.cpp`: R11 and downsample paths.
- `ImGuiManager.cpp`: the material checkbox, the mask thumbnail, the R11/AllowDownsampling rows, `bForceResponsiveAA`.
- `SettingsManager.cpp`: the material key.
- Driver: `translucent`, `qswitch` (Q1 → Q2 at frame 60), `-responsive`, `-r11`, `-allowdown`.

**Acceptance:**
1. `translucent -responsive=1 -debugvis=11`: the mask covers exactly the Cat's translucent pixels. `-debugvis=5` shows yellow (0.25) there. The trail behind the Cat is visibly shorter than with `-responsive=0`.
2. `-taaq=0` (downsample on): the AutoExposure readback exposure is within ±2 % of `-taaq=2`. The bloom brightness in a crop is within ±3 %.
3. `-taaq=1 -r11=1 static -taaframes=1200 -taacapture=120,1200`: mean |Δ| ≤ 1/255 and per-channel mean drift ≤ 0.5/255, so there is no hue drift. Compared with `-r11=0`: mean ≤ 1/255.
4. `qswitch -debugvis=6`: at frame 60 fewer than 1 % of the pixels show the green anti-ghost rejection (the `HISTORY_HAS_ALPHA` guard).
5. Debug layer and GBV per the §9.6 pass rule (⊆ D0-plain / D0-GBV, and no new state, descriptor or barrier ID). `.cso`: only new files. `VisualizeTemporalAAPS.cso` is unchanged: mode 11 is a CS view and needs no PS edit.

### S8: finalization

**Tasks:**
- Fold the gates: delete `TAAStageGates`, keeping the S7 behaviour unconditional.
- `DeferredPS.hlsl` IGN frame term (§6.4).
- The Japanese documentation headers:
  - `TemporalAA.h` lists every deviation of Appendix B.
  - The `SettingsManager.h` doc block.
- Run the full matrix §9.7 and record timings: the average frame time over 300 frames from `Time` at the defaults, at SP 50/100/150 and at HSP 200.
- Write a known-issues list: Appendix B items and pre-existing bugs observed.

**Acceptance:**
1. All of §9.7 passes.
2. `.cso`: only `DeferredPS` changes, and its dumpbin diff is the IGN term only.
3. AA-off captures are baseline-equal to S7 AA-off captures, because `StateFrameIndexMod8 = 0` when AA is off.

---

## 9. Verification plan

All scripts live in the session scratchpad (`<scratch>/tools/`), never in the repository. The verification scripts live outside the repository.

### 9.1 Build and `.cso` checks

**Baseline.** Before ST:
1. Build Release `/t:Rebuild`.
2. Copy `Shader/cso` to `<scratch>/cso/<stage>_before`.

Debug and Release both write `Shader/cso`, so always compare Release against Release, as in the memory note.

**Byte check.** `Get-FileHash -Algorithm SHA256` over both folders, then compare file names and hashes.

**Instruction check.** This is B's graft. It applies whenever a cbuffer grows (S2 for b0, S3b for b4 names, S4 for b1):
1. Run `"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe" /dumpbin <file>.cso > <file>.asm` on the before and after builds.
2. Drop every line that starts with `//`: that is the reflection block, which lists cbuffer sizes and names.
3. Diff what remains. Identical means the code is provably unchanged even though RDEF grew.

**Allowed changes per stage.** Any other change fails the stage.

| Stage | Byte-identical | Instruction-identical (RDEF may differ) | May change |
|---|---|---|---|
| ST, S0, S1, S3a | all | – | – |
| S2 | all compute shaders; DeferredVS | every pre-existing shader that reads b0 | GeometryPS, TranslucentPS (`sample` → `sample_b`, plus the b0 declaration) |
| S3b | all compute shaders | shaders that read b4 | new upscale PS |
| S4 | all compute shaders | shaders that read b1 | GeometryVS (`precise` / helper); new velocity and visualize shaders |
| S5 | all pre-existing `.cso` except `VisualizeTemporalAAPS` | – | new TAA shaders; `VisualizeTemporalAAPS` (created in S4; modes 3 and 5–13 added) |
| S6 | all pre-existing `.cso` except `VisualizeTemporalAAPS` | – | new TAA shaders; `VisualizeTemporalAAPS` (mode 4 added) |
| S7 | all pre-existing `.cso`, including `VisualizeTemporalAAPS` | – | new shaders only (`ResponsiveAAPS`, the 2 Downsample wrappers) |
| S8 | all except DeferredPS | – | DeferredPS (IGN term) |

The compute shaders that must never change are: `AutoExposure*_CS`, `ColorGradingLUT_CS`, `LightGrid*_CS`, `Lumen*_CS` (fxc), `VolumetricFog*_CS`, `DistanceField*`, `IBL*`, and every `*RT_CS` (dxc). `Structs.hlsl` is never modified, so `LightGridInjection_CS` qualifies.

### 9.2 Self-tests (`TemporalAASelfTest.cpp`)

`RunTemporalAASelfTests(FSceneRenderer&, bool bIncludeGPU)` runs:
- at the first `BeginFrame` in Debug builds (the constructor sets `bRequestSelfTest` under `#ifdef _DEBUG`);
- when the command line contains `-taaselftest`;
- when "Run Self Test" is pressed.

It always runs at the **top of `BeginFrame`**, never during the ImGui build. It logs `"[TAA SelfTest] <id> PASS|FAIL got=… exp=…"` through `OutputDebugStringA` and appends the same lines to the driver log. A summary line `"[TAA SelfTest] n/m PASS"` follows, and `m_TAAStats.LastSelfTestFailures` is set.

**CPU tests** (vectors in Appendix A):

| ID | Test | Tolerance | From stage |
|---|---|---|---|
| T1 | `Halton(i, 2/3)` for i = 1..8 | 1e-6 | S1 |
| T2 | Uniform and Gaussian sample tables, index 0..7; the CVar 2/3/4/5 tables with `Index % len` at Index = 7; CVar 5 → N = 4 outside TAAU | 1e-4 | S2 |
| T3 | `HackAddTemporalAAProjectionJitter`: the point (0.5, 0.3, 7) moves by exactly (0, −1/6) px at R = 1920×1080; `InvVP·VP = I` after `RecomputeDerivedMatrices` | 1e-4 px; 1e-5 | S1 |
| T4 | `ComputeClipToPrevClip` for a 0.1 m translation at the origin: the matrix entries, and (−0.01357995, 0, 0.99019804) → (0, 0); the far pixel (0.3, 0.2, Q) → BackN = 0 | 1e-5 | S2 |
| T4b | `ComputeClipToPrevClip` **away from the origin**, with `ViewMatrix = XMMatrixLookToLH` in float and forward = (sin 17°, 0, cos 17°). (a) Static camera at (0, 7, −15), (150, 7, −15) and (1000, 7, −15): pixel (0.3, 0.2, d(z = 10) = 0.99019804) gives \|BackN\| < 1e-6 NDC. The float emulation gives exactly 0. The old world-space float product gives up to 0.13 px at 1 km. (b) Previous camera at (150, 7, −15), current camera moved 0.1 m along its right axis: (−0.01357995, 0, 0.99019804) → (0, 0), and the entries (2,0) and (3,0) are within 1e-4 of A.5's −1.35772353 and 1.35799513. The tolerance absorbs the float rounding of positions near 150 m (`calc/c2p_relative.py`). | (a) 1e-6; (b) 1e-5 | S2 |
| T5 | 1° yaw: the reprojected ndc.x is independent of device Z (0.5 / 0.99 / 1.0 / Q) | 1e-6 | S2 |
| T6 | Velocity encoding: −0.01357995 → 32545 → −0.0135772; 0 → 32767; ±2 → 65469 / 65; 3 → clamped to 65469 | exact integers / 1e-6 | S4 |
| T7 | `ComputeTemporalAASampleWeights` (§4.8.6): J = (−0.164, 0.284) Gaussian, J = 0, J = (0, −1/6); Catmull-Rom at FS = 1 and FS = 0.5 (normalized, no guard); Catmull-Rom at FS = 0.1 (sum 0 → one-hot centre), FS = 0.4 with J = (−0.35, 0.35) (Σw/Σ\|w\| = 0.121 < 0.25 → one-hot centre), and FS = 0.1 with J = (−2/3, 0) (one-hot on (−1, 0)); every output is finite. Vectors in A.7. | 1e-4 | S5 |
| T8 | Upsample weight curve at x² = (0, .25, .5, .75, 1) → (1, .5815625, .27625, .0840625, .005) | 1e-6 | S6 |
| T9 | TAAU mapping at p = (100,57), (101,57), (0,0), (1919,1079), 50 %, J = (0.25, −1/6): K, dKO, FTW | 1e-5 | S6 |
| T10 | Catmull-Rom 1D weights at f = 0, .25, .5, .75 | 1e-7 | S6 |
| T11 | Mitchell-Netravali kernel values and the normalized taps at ratio 2 and 1.5 | 1e-6 | S6 |
| T12 | `ComputeViewFamilyInfoEx` (all gates on) reproduces every row of the §4.2 table, including N, the bias and the threshold | exact / 1e-4 | S1 (S2 for N) |
| T13 | LinearDepth ↔ device Z round trip for z = 0.1, 1, 10, 100, 499; the far rule for 499.5 and 500 | 1e-6 relative | S1 |
| T14 | YCoCg round trip, `HdrWeightY`, `WeightedLerpFactors` (CPU mirrors of the HLSL helpers) | 1e-6 | S5 |
| T15 | `IsLargeCameraMovement`: 44° yaw → false, 46° → true; 99 m → false, 101 m → true | exact | S1 |
| T16 | `FSceneVelocityData` life cycle (below) | exact | S4 |

T16 steps:
1. `Register` at T0, then a frame with a push of T1 gives no velocity, because it is a teleport.
2. A push T1 → T2 gives velocity.
3. A frame with no push gives `HasVelocity() == false`.
4. `MarkTeleported`, then the next push, gives no velocity.
5. `Remove`, then `Find` returns null.

**GPU parity** (from S5): `TemporalAASelfTest_CS` (§6.9) is dispatched once and read back (§4.8.5). This proves that the HLSL helpers, including the standard-Z closest-depth branch and the far-pixel rule, equal the CPU reference.

### 9.3 Test driver (`FTemporalAATestDriver`)

**Command line.** Keys are case-sensitive, and `wWinMain` passes `lpCmdLine`. An unknown key is logged and gives exit code 2. A key whose feature arrives in a later stage is accepted and logged as "not yet available".

| Key | Effect | Default |
|---|---|---|
| `-taatest=<scenario>` | Test mode: overrides applied after `SettingsManager::Initialize`; `SetSaveEnabled(false)`; `ImGui::GetIO().IniFilename = nullptr`; `Time::SetFixedDeltaTime(1/60)`; `ACameraActor::SetInputEnabled(false)` + `ResetVelocity`; ImGui windows not built; film grain off | – |
| `-taaframes=N` | frames to run (frame 1 = the first `Draw`) | 120 |
| `-taacapture=a,b,…` | frames to capture (BMP) | the last frame |
| `-taaout=<dir>` | output folder (created) | `Saved/TAATest` |
| `-taaselftest` | set `bRequestSelfTest` for frame 1. With `-taatest`, failures give exit code 1. Alone (interactive), the results are logged only. | – |
| `-fixeddt=<s>` | fixed delta time | 1/60 |
| `-grain=0/1`, `-dof=0/1` | clear or set `PP_FLAG_GRAIN` / `PP_FLAG_DOF` on the post-process volume (not saved) | grain 0, DOF unchanged |
| `-aa=`, `-sp=`, `-upsampling=`, `-taaq=`, `-samples=`, `-cfw=`, `-catmullrom=`, `-hsp=`, `-r11=`, `-allowdown=`, `-upscaleq=`, `-merge=`, `-mipoffset=`, `-mipmin=` | write the matching `FAntiAliasingParams` field (not saved) | INI value |
| `-debugvis=`, `-visscale=`, `-forcejitter=`, `-nojitter=`, `-overrideindex=`, `-ftwmode=` | write `FTemporalAADebugSettings` | off |
| `-responsive=0/1` | `translucent` scenario: `SetEnableResponsiveAA` on the Cat's slot 0 | 0 |
| `-reallocperiod=N` | `realloc` scenario: frames between forced reallocations | 90 |

**Scenarios.**
- `C0` is the camera pose after the INI is applied.
- `t = (frame − 1)·dt`.
- `PreWorldTick(frame)` applies the transforms **before** `m_World.Tick`, so the sky copies this frame's camera location (judge-log #40).
- The actors are found by spawn index through `m_World.GetActors()`: 5 = Camera, 8 = Table, 9 = Cat. The driver asserts the expected class, `ACameraActor` / `AStaticMeshActor`.

| Scenario | Motion |
|---|---|
| `static` | none |
| `pan` | camera location = C0 + (2·t, 0, 0) m |
| `rotate` | camera yaw = C0.yaw + 45°·t |
| `moveobj` | Table location = base + (cos(2πt/2) − 1, 0, sin(2πt/2)) m, radius 1 m, period 2 s; frozen at its frame-90 position from frame 91 on |
| `sky` | camera pitch = −15° (looking up), yaw = C0.yaw + 90°·t |
| `translucent` | Cat slot 0: `SetBlendMode(BLEND_Translucent)`, Opacity 0.5, `MarkRenderStateDirty`; the Cat moves like `moveobj` without stopping |
| `cut` | static; at frame 60 the camera is set to C0 + (3, 0, 3) m with yaw +90°, then `NotifyCameraCut()` |
| `largemove` | static; at frame 60 the camera is set to C0 + (150, 0, 0) m, **no** cut |
| `resize` | SP 100 → 50 → 71 → 150 → 200 → 100 at frames 30 / 60 / 90 / 120 / 150; runs ≥ 180 frames |
| `realloc` | `bRequestReallocate = true` at every multiple of `-reallocperiod` (default 90, so each realloc + 60 capture lands before the next reallocation). The leak soak uses 30. |
| `aatoggle` | AA 0 for frames 1–59, AA 2 from frame 60 |
| `qswitch` | Quality 1 with `-r11=1` for frames 1–59, Quality 2 from frame 60 |

**Per-frame log** (`<out>/taatest_log.txt`, CSV, one line per frame):
- frame, R, S, H, P (actual), method, pass, quality, format
- jitter index/N, jitter px, mip bias
- `bCameraCut`, `bPrevTransformsReset`, Lumen/fog `bHistoryValid`, `bHistoryValidThisFrame`
- velocity draws, TAA ran, merge
- free SRV/RTV, deferred-queue length, VRAM MB, resizes, self-test failures

`FTemporalAAStats` gains `bLumenHistoryValid` and `bFogHistoryValid` for this. `MakeLumenFrameInputs()` is `const` (`SceneRenderer.h:151`), so it cannot write stats. Instead:
- `RenderLumenScene` builds its inputs once (`const FLumenFrameInputs in = MakeLumenFrameInputs();`), passes them on, and sets `m_TAAStats.bLumenHistoryValid = in.bHistoryValid`.
- `RenderLighting` sets `m_TAAStats.bFogHistoryValid = fogInputs.bHistoryValid` next to where it fills `fogInputs` (`SceneRenderer.cpp:852-856`).

Both writers are non-const. The fog's own `m_bHistoryValid` is cleared only by `CreateVolumes`, and that happens on the same frame the `ViewRectSize` rule fires, so the renderer-side value is the effective one.

**Exit.** After frame N:
1. `FlushScreenshots()`.
2. Set `ShouldExit = true`.
3. The exit code is 0 on success, 1 if any self-test failed, 2 for a bad command line, 3 if a BMP write failed.

### 9.4 Leak and resource counters

These are shown in the AA window and logged per frame:
- `GetNumFreeSRVDescriptors()` and `GetNumFreeRTVDescriptors()`.
- `GetDeferredReleaseQueueLength()`.
- `QueryLocalVideoMemoryUsage()` (`IDXGIAdapter3::QueryVideoMemoryInfo(0, LOCAL).CurrentUsage`).

Criterion for `realloc` and `resize`: two frames after the last change, the free counts and the queue length equal their values before the first change, ±0. VRAM is within 5 %.

### 9.5 Image tools and metrics (pure Python 3, no numpy: none is installed)

**`<scratch>/tools/imgdiff.py A.bmp B.bmp [--crop x,y,w,h] [--out diff.bmp]`** reads 24-bit BMPs and prints:
- the mean |Δ| per channel and overall;
- the max;
- the 99.9th percentile;
- PSNR (dB) over RGB;
- the fraction of pixels with |Δ| > 8/255.

It writes `diff.bmp` (|Δ|×8).

**`<scratch>/tools/imgstat.py A.bmp [--crop]`** prints:
- the mean per channel and the mean luma;
- the gray fraction (|R−G| ≤ 2 and |G−B| ≤ 2);
- the red fraction (R > 200, G < 60, B < 60);
- the green fraction (G > R + 60 and G > B + 60);
- the yellow fraction;
- the dark-blue fraction (R ≤ 8, G ≤ 34, B ≤ 136). In view 5 this is `BlendFinalHeat(b)` for b ≤ 0.06: (0, 32, 112) at 0.06, (0, 0, 128) at 0.04, black at 0. The back buffer is UNORM, and view 5 is displayed as-is.

**`<scratch>/tools/edge_metric.py A.bmp --crop x,y,w,h`** prints the mean absolute horizontal plus vertical luma gradient, a sharpness proxy.

**Noise floor (ST).** N0 and P0 are the mean and 99.9th percentile of `imgdiff` between two independent `static` runs at frame 120, AA off. They capture the non-determinism of LightGrid/Lumen atomics. **No criterion in this plan requires byte-identical images across runs** (judge-log #37).

**SSAA reference.**
- `-aa=0 -sp=200 -upscaleq=1 -merge=0`: bilinear 2:1 sampling at output-pixel centres is an exact 2×2 box, i.e. a 4-spp ordered grid.
- Capture at frame 120 of `static`.
- PSNR comparisons are always relative: TAA vs AA-off, or TAAU vs spatial, against the same reference.

**F2F.** Frames 119 vs 120 of the same run.

### 9.6 D3D12 debug-layer diff

- **Listener.** Reuse `<scratch>/dbgcap/dbwin.cpp`, a DBWIN listener that launches the process without attaching a debugger. Extend it to forward the remaining command-line arguments to the child process (`dbwin <seconds> <exe> <cwd> <args…>`) and rebuild it with `cl /EHsc dbwin.cpp`.
- **Run.**
  1. `set DX12_DEBUG_NO_BREAK=1` (plus `DX12_DEBUG_GBV=1` for GBV runs).
  2. `dbwin 120 x64\Debug\DirectX12.exe <repo> -taatest=<s> -taaframes=300 > <scratch>/d3d12_<stage>_<s>.log`.
- **Parse.** Extract `D3D12 (ERROR|CORRUPTION|WARNING): .*\[ (\w+) (?:ERROR|WARNING|CORRUPTION) #(\d+): (\w+)\]` and reduce it to the set of `(severity, id)` with counts.
- **Baseline D0** is taken in S0, **after** the S0 state fixes (LUT, task 10; AutoExposure, task 12). It has two parts:
  - **D0-plain**: the ID histogram of plain debug-layer runs of `static`, `rotate` and `pan`.
  - **D0-GBV**: the same three scenarios with `DX12_DEBUG_GBV=1`.
  
  D0-plain contains pre-existing IDs such as WARNING #1328 (`CREATERESOURCE_STATE_IGNORED`, buffers created in a non-COMMON state) and WARNING #820 (the DOF clear value). #615 is gone.
  
  No absolute "no state messages" rule is possible: messages that come from pre-existing code outside this feature, if any survive S0, are recorded in D0 by resource name.
- **Pass:**
  - the plain ID set of every stage and scenario is ⊆ D0-plain;
  - the GBV ID set is ⊆ D0-GBV;
  - there is **no new** `RESOURCE_STATE`, `INVALID_DESCRIPTOR`, `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH` or `INVALID_SUBRESOURCE_STATE` ID, meaning one not already in D0-GBV. A known ID that now names a new resource (Velocity, ResponsiveAAMask, TemporalAA*, the dummies, `AutoExposureResult`) also fails;
  - there is never any CORRUPTION.
  
  GBV runs are required in S5–S7 for the scenarios `static`, `moveobj`, `resize` and `translucent`.
- **Scenarios per stage:** `static`, `rotate`, plus the scenarios the stage adds.

### 9.7 Final scenario matrix (S8; every run is a driver run with `-taaframes ≥ 120`)

| # | Scenario | Setup | Pass criterion |
|---|---|---|---|
| V1 | Regression, AA off | `static -aa=0` | baseline-equal to B0 |
| V2 | Static convergence (Main) | `static -aa=2 -upsampling=0` | F2F mean ≤ F2F_off + 0.3/255 and p99.9 ≤ 6/255. PSNR vs reference ≥ AA-off + 1.5 dB. Converged mean luma within 1/255 of B0. `-debugvis=5` (pre-floor view, §6.8): `imgstat` dark-blue fraction ≥ 95 % (pre-floor BlendFinal ≤ 0.06). |
| V3 | Static convergence (TAAU default) | `static` | as V2, ±1 dB of V2's PSNR |
| V4 | Pan / rotate | `pan`, `rotate` with `-debugvis=8` | mean ≤ 25/255; no ghost outlines in the plain captures at frames 60/120 (visual) |
| V5 | Moving object | `moveobj -taaq=0..3` | Q2/Q3: no trail > 2 px (crop, visual); `-debugvis=2`: only the Table is green; frame 91 has none; `-debugvis=12`: rejection trail present |
| V6 | Sky | `sky`, `pan`, `largemove` | `rotate -debugvis=2` and `pan -debugvis=2`: the sky is not green; `pan -debugvis=6`: 0 anti-ghost rejects (pure green pixels); `largemove -debugvis=6`: 0 anti-ghost rejects at f61; `sky`: the dome is not blurred (edge_metric of a sky crop ≥ 90 % of the static value) |
| V7 | Translucency + responsive | `translucent -responsive=0/1` | `-debugvis=11` covers exactly the Cat; with 1 the trail is visibly shorter; the background is not smeared |
| V8 | Screen percentages | TAAU `-sp=50/71/100/150` vs spatial at the same SP | log matches the §4.2 table; PSNR(TAAU) > PSNR(spatial) at 50 and 71; F2F p99.9 ≤ 12/255 at 50 |
| V9 | History 200 % | `-hsp=200` | H = 3840×2160; no NaN (no black where B0 is lit); border check (S6 #4) |
| V10 | Quality 0–3 | `-taaq=0..3`, static and `moveobj` | all run; Q0 + downsample: AE within ±2 %; R11 drift test (S7 #3) |
| V11 | Methods | AA off / Main / TAAU at 100 % | TAAU vs Main converged mean ≤ 2/255; AA off vs TAA shows edge AA in crops |
| V12 | Camera cut | `cut -debugvis=5 -taacapture=59,60,61` (pre-floor view) | frame 60 ≥ 95 % red; 59 and 61 < 5 %; Lumen/fog invalid only at 60 (log); no ghost of the pre-cut view at 61 |
| V13 | Runtime resolution change | `resize` with TAAU and with `-upsampling=0` | no errors; leak criteria (§9.4); TAAU: < 5 % red at the change frames (debugvis 5, pre-floor); Main: history resampled (< 5 % red); Lumen/fog `bHistoryValid = 0` exactly at the change frames and 1 otherwise (log only; there is no image criterion for their re-convergence) |
| V14 | Large movement | `largemove` | `bPrevTransformsReset` at 60 only; no NaN; F2F at 90 back within V2 limits |
| V15 | AA toggle / quality switch | `aatoggle`, `qswitch` | S5 #7 and S7 #4 criteria |
| V16 | Self-tests | `-taaselftest` in Release and Debug | 100 % pass (CPU + GPU) |
| V17 | Unjittered culling | `static`, AA on | the log's visible-primitive count is constant over 120 frames (add `NumVisible` from `FCullingStats` to the log) |
| V18 | INI round trip | interactive: change every §7.1 field, Save, restart | all restored; debug settings not restored; hand-edited `nan` / out-of-range values clamp or fall back |

---

## 10. Risks and mitigations

| # | Risk | Mitigation |
|---|---|---|
| R1 | **Velocity/base-pass depth mismatch.** Different VS binaries under LESS_EQUAL can drop velocity on some pixels. | Both VS use `GetBasePassClipPosition` (`precise`, same expression order). S4 acceptance #5 checks coverage. The documented fallback is `DepthBias = −4` on the four `Velocity*` PSOs: a few D32 ULPs toward the camera. It is valid with `DepthClipEnable = FALSE`, because the sky still clamps to 1. |
| R2 | **Standard-Z porting errors** (a leftover `max`/`>`) | Every flip is marked `// UE: …` and centralized in `SelectClosestDepthCross`. GPU self-test entries 19–27. ClosestDepthOffset view: rims must be *outside* silhouettes. |
| R3 | **Jitter sign or scale errors** | T3, T7, T9; S2 acceptance #3; MotionVectors must be gray when static; InputOutputSplit shows no ½-px offset. |
| R4 | **Previous-frame ordering regressions** | The `m_Prev*` members are deleted, so the compiler catches old readers. `PrevViewInfo` is a per-frame snapshot. `CommitViewState` is the last call of post. |
| R5 | **Resize: descriptor leaks, VRAM spikes, dangling SRVs** | FlushAndReset → release → WaitGPU → create (§5.2); new slots for every view; the S3a `realloc` soak; the leak counters; ImGui reads handles every frame. |
| R6 | **Lumen/fog history garbage** after a resize, reallocation or cut | The `ViewRectSize` rule, `PrevFrameViewInfo.ViewRectSize = 0` on any render-target recreation, and the fog's `m_bHistoryValid = false`. Jitter phases are **not** reset, so reallocated runs stay frame-aligned (S3a #3). |
| R7 | **The AutoExposure state change** breaks the tonemap or frame 0 | Buffer decay is handled explicitly: `PrepareResultForRead` does COMMON → RD every frame, and the barriers use RD (S0 [FIX]). `IsResultValid()` plus the dummy EA on frame 0. D0-GBV and the S5–S7 GBV runs. |
| R8 | **Anti-ghost misfires on an R11G11B10 history** (alpha reads 1) | `TAA_FLAG_HISTORY_HAS_ALPHA`; R11 is restricted to Q0/Q1; the `qswitch` test. |
| R9 | **Translucency smearing** (no depth, no velocity) | Opaque LinearDepth for reprojection; the per-material responsive mask with a force-all debug toggle. |
| R10 | **Small distant movers ghost** (the UE `MotionBlurPerObjectSize` skip) | UE-faithful default; `bDisableVelocitySmallObjectCull` shows the effect; one constant (`kMotionBlurPerObjectSize`) disables it. |
| R11 | **Low-confidence UE details** ([L]/[M]): FTW semantics, adaptive filter, MN kernel, upscale modes 2/4/5, SAMPLE_DISTANCE units, Q0 sample set, index reset on cut, R11 conditions | Each is isolated in one named function or define; Appendix C records the decision and how to revisit it. The FTW mode switch, the debug views (FTW, BlendFinal, HistoryClamp) and PSNR make alternatives measurable. |
| R12 | **Stochastic-quantization bias.** UE's E ∈ [0,1) is unbiased only if the float conversion truncates; with round-to-nearest it biases the FP16 history by about +0.5 ULP per frame, ≲ 1 % after accumulation. | S5 acceptance #4 (converged mean luma within 1/255 of B0). If it fails, change one line: `E − 0.5` (centred) in §6.5 step 14. That is a documented [PORT] switch, not a design change. |
| R13 | **Static noise that TAA cannot resolve** (DeferredPS GatherMode-1 IGN) | S8 adds UE's frame term; it is frozen at 0 with AA off, so the AA-off image is unchanged. |
| R14 | **Correlated low-discrepancy sequences** (fog Halton 2/3/5 over 8 frames, Lumen probe Halton 2/3 over 16, TAA Halton 2/3) | Accepted: the Lumen offsets are integer pixels, so the correlation is weak. If S8 shows structured patterns, offset the Lumen probe index by 7 (`(m_ProbeJitterIndex + 7) % 16`), a tuning knob outside UE. |
| R15 | **Memory at 200 %** (≈ 1.6 GB of render-res targets at 3840×2160; +126 MB of history at HSP 200) | Default 100 %; the spatial clamp stops at 200 % **[PORT]**; the AA window shows VRAM. |
| R16 | **Mid-frame `FlushAndResetCommandList`** (asset loads in ImGui) resets the viewport and drops the back-buffer binding | `EndFrame` rebinds heap, RTV and viewport (S0). The default viewport is `R`, and every post pass sets its own. Tracked resource states persist across the flush. |
| R17 | **A missing `.cso`** would crash (the known assert-only class), or, for a graphics PS in Release, silently create a PSO that draws nothing | Compute: `TryCreateComputePipeline` + `IsReady` fallback to AA None (§4.3). Graphics: `CreatePipeline(..., bOptional)` returns null without asserting, and `HasPipelineState` gates `SetPipelineState` (§3.8). The upscale falls back to `PostProcessUpscale1`, then to the merged tonemap; the visualize pass is skipped. All of them log once. S3b #3 exercises the path. |
| R18 | **Masked materials without a BaseColor texture** read a stale t0 in the base pass (pre-existing) | The velocity pass uses the masked PSO only when BaseColor exists (the shadow rule). The residual mismatch is pre-existing and listed as a known issue. |
| R19 | **Driver non-determinism** (LightGrid/Lumen atomics, vsync timing) | Fixed dt; frame-count based; every criterion uses the measured noise floor N0/P0; no byte-identical requirement across runs. |
| R20 | **Merge conflicts across stages** | Strict stage order; new logic in new files (`TemporalAA.cpp`, `VelocityRendering.cpp`, `PostProcessUpscale.cpp`, `SceneViewState.cpp`, `ScreenPercentage.cpp`); `SceneRenderer.cpp` gains call sites and routing only. |
| R21 | **Gate misuse** (a temporary value persisted to the INI) | Gates are compile-time constants in code paths, never defaults. `FAntiAliasingParams` defaults are final from S1. S8 deletes the gates. |

---

## Appendix A: Derivations and test vectors

The numbers were computed in double precision with `<scratch>/taau/calc/vectors*.py`. The projection is FOV 45°, aspect 16/9, n = 0.1, f = 500, so:
- `P11 = 1.35799513`, `P22 = 2.41421356`
- `P33 = Q = 1.00020004`, `P43 = −Q·n = −0.10002000`, `P34 = 1`

### A.1 UV ↔ ScreenPos ↔ pixels

- `s = (2u.x − 1, 1 − 2u.y)` and `u = (0.5s.x + 0.5, 0.5 − 0.5s.y)`.
- A displacement `Δs` is `(Δs.x·E.x/2, −Δs.y·E.y/2)` pixels.
- `HSP` (0.1, −0.2) → history UV (0.55, 0.6). `HSP` (−1.0, 0.5) → UV (0.0, 0.25), which is OffScreen (|s| ≥ 1).
- `HistoryBufferUVMinMax`:

  | Hp | Value |
  |---|---|
  | 1920×1080 | (0.00026042, 0.00046296, 0.99973958, 0.99953704) |
  | 1364×767 | (0.00036657, 0.00065189, 0.99963343, 0.99934811) |
  | 960×540 | (0.00052083, 0.00092593, 0.99947917, 0.99907407) |
  | 3840×2160 | (0.00013021, 0.00023148, 0.99986979, 0.99976852) |

### A.2 The jitter lives in `_31/_32` and shifts the image by exactly +J_px

For a row vector (x, y, z, 1): `ndc.x = x·P11/z + P31` and `ndc.y = y·P22/z + P32`. Adding `J_ndc` to (P31, P32) shifts the NDC by exactly `J_ndc` at every depth, and leaves the depth row and `_11/_22` untouched. With `J_ndc = (2sx/R.x, −2sy/R.y)` the pixel shift is (+sx, +sy).

**Vector.** R = 1920×1080, uniform index 0 → `J_px` = (0, −0.1666667), `J_ndc` = (0, 0.000308642). The point (0.5, 0.3, 7):
- unjittered pixel (1053.119666, 484.128200)
- jittered pixel (1053.119666, 483.961534)
- Δ = (0, −0.1666667) ✓

**Consequence for the weights.** A scene point that belongs to output-pixel centre c appears at `c + J`. The input sample at `p + o` is therefore `o − J` from it, which is UE's `SampleOffsets − Jitter`.

### A.3 Sequences, counts and mip bias

**Halton:**
- Halton(i, 2) for i = 1..8: 0.5, 0.25, 0.75, 0.125, 0.625, 0.375, 0.875, 0.0625.
- Halton(i, 3): 0.333333, 0.666667, 0.111111, 0.444444, 0.777778, 0.222222, 0.555556, 0.888889.

**Uniform (TAAU) samples:**
- Index 0..7: (0, −0.1667), (−0.25, 0.1667), (0.25, −0.3889), (−0.375, −0.0556), (0.125, 0.2778), (−0.125, −0.2778), (0.375, 0.0556), (−0.4375, 0.3889).
- Index 8..15: (0.0625, −0.4630), (−0.1875, −0.1296), (0.3125, 0.2037), (−0.3125, −0.3519), (0.1875, −0.0185), (−0.0625, 0.3148), (0.4375, −0.2407), (−0.4688, 0.0926).

**Gaussian samples** (FilterSize 1, σ = 0.47, window 0.5678676): (−0.1640, 0.2840), (−0.2080, −0.3603), (0.1722, 0.1445), (−0.4305, 0.1567), (0.0485, −0.2752), (0.0648, 0.3673), (−0.1472, −0.0536), (0.3670, −0.3079).

**Fixed patterns at Index 7** (`% len`):
- CVar 2 → (4/16, 4/16)
- CVar 3 → (2/3, 0)
- CVar 4 → (−6/16, 2/16)
- CVar 5 → (−1/2, 0), with N = 4

**Sample count** `N = int(8·max(1, 1/f²))` with `f = R.x/O.x`:

| SP | f | Raw | N |
|---|---|---|---|
| 50 | 0.5 | 32.0 | 32 |
| 67 | 0.6703125 | 17.80 | 17 |
| 71 | 0.7104167 | 15.85 | 15 |
| 75 | 0.75 | 14.22 | 14 |
| ≥ 100 | – | – | 8 |

**Mip bias** `max(min(log2 f, 0) − 0.3, −2)`:

| f | 0.5 | 0.6703 | 0.7104 | 0.75 | ≥ 1 |
|---|---|---|---|---|---|
| Bias | −1.3000 | −0.8771 | −0.7933 | −0.7150 | −0.3000 |
| `2^bias` | 0.406126 | 0.544463 | 0.577038 | 0.609189 | 0.812252 |

### A.4 Velocity

- `ClipCur.xy/w = s_cur + J_cur` and `ClipPrev.xy/w = s_prev + J_prev`. Subtracting `TemporalAAJitter.xy/.zw` gives exactly `V = s_cur − s_prev`.
- **Object moving +0.1 m along x, point (0, 0, 10):** V = +0.01357995 = +13.0368 px at 1920. `HSP = s − V` lies where the point was.

**Encoding:**
- V = −0.01357995 → E = 0.49660417 → UNORM16 32545. V = 0 → 32767.
- 32545 decodes to −0.01357717 (an error of 0.0027 px).
- One LSB = 6.1158e-5 NDC = 0.0587 px at 1920.
- V = +2 → 65469; V = −2 → 65. |V| = 2.004 → 0, a sentinel collision, hence the ±2 clamp (V = 3 → 0.99899237 → 65469).
- (2, 2) decodes to 2.0000021, so `HSP ≤ −1` is OffScreen, as intended for "previous position behind the camera".

### A.5 ClipToPrevClip (NoAA × NoAA; PrevClip = (s, d, 1)·C2P)

**Camera translated +0.1 m along x (prev x = 0, cur x = 0.1):**

```
C2P = |  1           0  0  0 |
      |  0           1  0  0 |
      | -1.35772353  0  1  0 |
      |  1.35799513  0  0  1 |
```

- The point (0, 0, 10) has current (s, d) = (−0.01357995, 0, 0.99019804). It maps to (0, 0), so BackN = −0.01357995 (−13.0368 px). The third row gives the depth-dependent parallax.
- **Far pixel.** At s = (0.3, 0.2):
  - With d = 1 (the far plane at 500 m), BackN.x = −0.00027160, which is −0.2607 px for 0.1 m and −2.6074 px for 1 m of translation.
  - With **d = Q** (infinity, the §6.5 far rule), BackN = 0 exactly.
  - This is why far pixels use `d = Q`: UE's infinite-far behaviour, rotation-only.
- **Camera yawed 1°:** the reprojected ndc.x = 0.02370389 for d = 0.5, 0.99, 1.0 and Q. It is depth-independent.
- **Construction.** The matrix above is `InvProjNoAA · Translation(O_cur − O_prev in view space) · ProjNoAA`, which is what `ComputeClipToPrevClip` (§4.4) produces: camera-relative, composed in double, stored as float.
- **Precision away from the origin (T4b).** At a static camera at (0, 7, −15), (150, 7, −15) and (1000, 7, −15) (yaw 17°, pixel (0.3, 0.2), z = 10), the camera-relative construction evaluated in float32 gives BackN = 0. The old world-space `InvVP·VP` float product gives 0.008 / 0.017 / 0.13 px in our emulation (`calc/c2p_precision.py`, yaw 17°, pitch 10°).
- **Moved away from the origin.** A 0.1 m move along the camera's right axis at (150, 7, −15) reproduces the matrix above to ≤ 5e-5 per entry and BackN to 5e-7. The residual is the float rounding of the two positions near 150 m (`calc/c2p_relative.py`).

**Device Z ↔ view Z** (`d = Q − Q·n/z`; far rule `z ≥ 0.999·f → Q`):

| view Z | 0.1 | 1 | 10 | 100 | 499 | 499.5 | 500 |
|---|---|---|---|---|---|---|---|
| device Z | 0.00000000 | 0.90018004 | 0.99019804 | 0.99919984 | 0.99999960 | 1.00020004 (far) | 1.00020004 (far) |

A float32 `LinearDepthPS` gives 499.969 for device Z = 1, which is above the 499.5 threshold.

### A.6 TAAU input mapping, weights and FilteredTemporalWeight

Weight curve `w(x²) = (0.905x² − 1.9)x² + 1` with `x² = saturate(UF²·|o − dKO|²)`: at x² = (0, .25, .5, .75, 1) it is (1, 0.5815625, 0.27625, 0.0840625, 0.005).

Setup: O = 1920×1080, R = 960×540 (UF = 2), J = (0.25, −1/6):

| Output p | PPCo | K | dKO | Weights | FTW |
|---|---|---|---|---|---|
| (100, 57) | (50.5, 28.58333) | (50, 28) | (0.0, 0.08333) | centre 0.94792, others 0.005 | 0.98792 |
| (101, 57) | (51.0, 28.58333) | (51, 28) | (−0.5, 0.08333) | all 0.005 | 0.045 |
| (0, 0) | (0.5, 0.08333) | (0, 0) | (0.0, −0.41667) | centre 0.11699, others 0.005 | 0.15699 |
| (1919, 1079) | (960.0, 539.58333) | (960, 539) → clamp (959, 539) | (−0.5, 0.08333) | all 0.005 | 0.045 |

- At UF = 1, J = (0.25, −1/6), p = (10, 10): K = (10, 10), dKO = (0.25, −0.16667), centre weight 0.83585, (1,0) = 0.1938, (0,−1) = 0.08034, FTW = 1.13999.
- Mean FTW over a uniform dKO: 1.1341 (UF 1), 0.5944 (UF 1.408), 0.3173 (UF 2).

### A.7 Main configuration: CPU sample weights

`w_i = exp(−2.29·|o_i − J|²/FS²)` (or `CatmullRom((o_i − J).x/FS)·CatmullRom((o_i − J).y/FS)`), normalized, with the §4.8.6 guards. The order is (−1,−1) … (1,1). FS = 1 unless stated.

| J (px) | SampleWeights | PlusWeights {1,3,4,5,7} |
|---|---|---|
| (−0.1640, 0.2840), Gaussian idx 0 | 0.0033, 0.0156, 0.0007, 0.1215, 0.5661, 0.0271, 0.0452, 0.2105, 0.0101 | 0.0166, 0.1291, 0.6018, 0.0288, 0.2238 |
| (0, 0) | 0.0071, 0.0700, 0.0071, 0.0700, 0.6915, 0.0700, 0.0071, 0.0700, 0.0071 | – |
| (0, −1/6) | 0.0145, 0.1429, 0.0145, 0.0666, 0.6577, 0.0666, 0.0031, 0.0310, 0.0031 | – |
| Catmull-Rom, Gaussian idx 0 | −0.0090, −0.0657, 0.0040, 0.1034, 0.7518, −0.0459, 0.0334, 0.2428, −0.0148 | −0.0666, 0.1048, 0.7622, −0.0465, 0.2462 |
| Catmull-Rom, FS 0.5, Gaussian idx 0 J (raw Σw = 0.298115, no guard) | 0, 0, 0, −0.0568, 1.2316, 0, 0.0084, −0.1833, 0 | 0, −0.0573, 1.2421, 0, −0.1848 |
| Catmull-Rom, FS 0.1, Gaussian idx 0 J (Σw = 0 → nearest) | 0, 0, 0, 0, 1, 0, 0, 0, 0 | 0, 0, 1, 0, 0 |
| Catmull-Rom, FS 0.4, J (−0.35, 0.35) (Σw = 0.0022, Σ\|w\| = 0.0182, ratio 0.121 < 0.25 → nearest) | 0, 0, 0, 0, 1, 0, 0, 0, 0 | 0, 0, 1, 0, 0 |
| Catmull-Rom, FS 0.1, J (−2/3, 0), CVar 3 (Σw = 0 → nearest = (−1, 0)) | 0, 0, 0, 1, 0, 0, 0, 0, 0 | 0, 1, 0, 0, 0 |

`CatmullRom(x)` with `ax = |x|`: 0 for ax ≥ 2 **[PORT]**, `((−0.5ax + 2.5)ax − 4)ax + 2` for 1 < ax < 2, else `(1.5ax − 2.5)ax² + 1`. Without the zero, UE's cubic gives −0.288 at 2.6 and −1183 at 15 (`calc/vectors7.py`, `calc/vectors8.py`, `calc/cr_scan.py`).

### A.8 Resampling kernels

**Catmull-Rom 1D** (taps −1, 0, +1, +2):

| f | w0 | w1 | w2 | w3 | Merged W | S1 offset | 5-tap sum (fx = fy) |
|---|---|---|---|---|---|---|---|
| 0 | 0 | 1 | 0 | 0 | (0, 1, 0) | 0 | 1.0 |
| 0.25 | −0.0703125 | 0.8671875 | 0.2265625 | −0.0234375 | (−0.0703125, 1.09375, −0.0234375) | 0.2071429 | 0.9912109 |
| 0.5 | −0.0625 | 0.5625 | 0.5625 | −0.0625 | (−0.0625, 1.125, −0.0625) | 0.5 | 0.984375 |
| 0.75 | −0.0234375 | 0.2265625 | 0.8671875 | −0.0703125 | (−0.0234375, 1.09375, −0.0703125) | 0.7928571 | 0.9912109 |

The centre 2D tap weight at f = 0.25 is 1.1962891.

**Mitchell-Netravali (B = C = 1/3):**
- k at (0, .25, .5, .75, 1, 1.25, 1.5, 1.75, 2) = 0.888889, 0.782118, 0.534722, 0.256076, 0.055556, −0.023438, −0.034722, −0.014757, 0.
- Ratio 2, output pixel 10 → input 21.0; normalized taps for i = 17..24: −0.007378, −0.011719, 0.128038, 0.391059, 0.391059, 0.128038, −0.011719, −0.007378 (raw sum 2.0).
- Ratio 1.5 → input 15.75; taps for i = 13..18: −0.023148, 0.116770, 0.559156, 0.356481, −0.004287, −0.004973 (raw sum 1.5).

**Lanczos-3** (`f = frac + 2`, k = 0..5):

| frac | w_k | Merged {w0, w1, W2, w4, w5} | S2 offset | Diamond-13 sum |
|---|---|---|---|---|
| 0 | (0, 0, 1, 0, 0, 0) | – | – | 1 |
| 0.25 | (0.030021, −0.132871, 0.890067, 0.270190, −0.067791, 0.007356) | (0.030021, −0.132871, 1.160257, −0.067791, 0.007356) | 0.232871 | 1.007556 |
| 0.5 | (0.024317, −0.135095, 0.607927, 0.607927, −0.135095, 0.024317) | (0.024317, −0.135095, 1.215854, −0.135095, 0.024317) | 0.5 | 1.012545 |

The taps are normalized by their sum.

### A.9 Colour space, HDR weight and blend

- YCoCg(1, 0.5, 0.25) = (2.25, 1.5, −0.25), and the inverse returns (1, 0.5, 0.25).
- `HdrWeightY(2.25, 1) = 0.16`; with E = 0.25 it is 0.219178.
- `WeightedLerpFactors(1/6, 0.1, 0.04)` = (0.97561, 0.02439).

BlendFinal chain (CFW 0.04). The blend uses the last column. Debug view 5 shows the pre-floor column (§6.8):

| Case | Pre-floor (FTW·CFW, velocity lerp) = view 5 | Floor `saturate(0.01·Lh/\|Lf−Lh\|)` | BlendFinal (blend) |
|---|---|---|---|
| static, FTW 1.04, Lf 1.0, Lh 1.5 | 0.0416 | 0.03 | 0.0416 |
| moving at 10 px/frame (Velocity 20), same lumas | 0.1208 | 0.03 | 0.1208 |
| near-equal, FTW 1.04 (Lf 1.0, Lh 1.005): a typical converged flat pixel | 0.0416 (dark blue) | 1.0 | **1.0** (red if the view showed it) |
| TAAU 50 % far sample (FTW 0.045), Lf 1.0, Lh 1.5 | 0.0018 | 0.03 | 0.03 (the floor dominates) |

### A.10 Closest-depth selection (standard Z, X pattern at ±2)

| Z (x, y, z, w) | Z0 | Offset | Closest Z |
|---|---|---|---|
| (5, 3, 4, 6) | 7 | (+2, −2) | 3 |
| (9, 2, 8, 1) | 7 | (+2, +2) | 1 |
| (9, 9, 9, 9) | 7 | (0, 0) | 7 |

### A.11 Stochastic quantization and noise

**`QuantizeForFloatRenderTarget`:**
- (1.0, E = 0.5, 1/64) = 1.0078125
- (3.0, 0.25, 1/64) = 3.0078125
- (0.3, 0.75, 1/32) = 0.305859375
- (1.0, 0.5, 2⁻¹⁰) = 1.00048828

**`Rand3DPCG16` → `Hammersley16(0, 1, ·).x`:**

| int3 | Rand3DPCG16 | E.x |
|---|---|---|
| (0, 0, 0) | (7000, 6616, 52874) | 0.10681152 |
| (1, 0, 0) | (8174, 37409, 60645) | 0.12472534 |
| (10, 20, 3) | (51682, 13070, 27103) | 0.78860474 |
| (1919, 1079, 7) | (1314, 56367, 28217) | 0.02005005 |

**IGN:** IGN((0,0),0) = 0; IGN((1,0),0) = 0.555713; IGN((0,1),0) = 0.309269; IGN((1,0),1) = 0.391269; IGN((10,20),3) = 0.266265.

**Formats:** R11G11B10 max finite = 65024 (R, G) and 64512 (B), so `OutputQuantizationError.w = 64512`. RGBA16F max = 65504.

### A.12 Other derived constants

| Quantity | Value |
|---|---|
| DOF `MaxBlurSize` scaling (8 at 100 %) | 50 % → 4.0; 71 % → 5.683; 150 % → 12.0 |
| `UpscaleUnsharpAmount` (Softness 1) | 960×540: 0.75; 1364×767: 0.4955; ≥ 100 %: 0 |
| LightGrid capacity at 1080p output | 60 × 34 × 32 = 65 280 cells (vs 16 320 today); ≈ 26 MB of grid buffers |
| HistoryScreenPercentage truncation | 1920×1080 × 1.37 → 2630×1479; 1364×767 × 2 → 2728×1534 |

---

## Appendix B: Deviations from UE

| Item | UE (confidence) | Port | Reason |
|---|---|---|---|
| Buffer model | ViewRect inside a quantized extent | exact-size textures, (0,0) origin | every existing screen shader stays unchanged |
| Spatial fraction range | 0.01–4.0 [H] | 0.10–2.00 | memory |
| TAA depth source | SceneDepth (opaque) | opaque LinearDepth → device Z | the depth buffer holds translucent depth after the translucency pass |
| Far pixels | infinite reverse-Z: rotation-only for free | view Z ≥ 0.999·f → d = Q | finite 500 m far plane; same result (A.5) |
| Responsive AA | stencil bit 3 | R8 mask pass | no stencil (D32_FLOAT) |
| Input sanitize | output-only NaN guard | input, history and output | robustness; no visible difference |
| w ≤ 0 guards | none known | camera-motion and velocity paths | large-movement / behind-camera robustness |
| Velocity clamp ±2 | none | clamp | avoids a collision with the 0 sentinel |
| Main-config Catmull-Rom sample weights | cubic evaluated outside its support; plain normalization | 0 for \|x\| ≥ 2; one-hot on the nearest sample when Σw ≤ 1e-6 or Σw < 0.25·Σ\|w\| (§4.8.6) | UE's latent defect gives huge negative weights for FS < ~0.75; identical to UE for FS ≥ 0.75 |
| ClipToPrevClip construction | translated (camera-relative) matrices | camera-relative, composed in double from `ViewOrigin` differences (§4.4) | same intent as UE; the engine has no translated-world matrices, so the translation is factored out explicitly |
| BlendFinal debug view (view 5) | – (port-specific view) | shows the pre-floor weight; the blend still uses the floor (§6.8) | the anti-stall floor is 1 on converged flat pixels by design, so a post-floor view would carry no signal |
| History alpha flag | – | `HISTORY_HAS_ALPHA` | R11G11B10 SRVs return a = 1 |
| R11G11B10 condition | low/fast, no alpha [M] | Q0/Q1, not SuperSampling, typed-store capability | consistent with there being no anti-ghost alpha |
| `N == 1` jitter | constant Gaussian sample #0 | zero offset | exact AA-off comparisons |
| `StateFrameIndexMod8` in b0 | always rotates | 0 while AA is off | keeps the S8 IGN change baseline-neutral |
| Lumen/fog history on large movement | not UE features | invalidated for one frame | their reprojection would use reset matrices |
| Velocity output variants | `r.VelocityOutputPass` 0/1/2 | 2 only | `PS_INPUT` and the MRT set stay unchanged |
| Mip-bias mechanism | per-material sampler state | `SampleBias` + b0 | the static samplers are baked into the root signature |
| Pre-exposure | always on (5.x) | none, correction = 1 | engine limitation; the hook `SceneColorPreExposure` is kept |
| LightGrid sizing | per-view allocation | capacity 2·O allocated once, per-frame dimensions | removes a reallocation path |
| Compute-only TAA | CS and PS paths | CS | #615 and simplicity |
| `TAA_SCREEN_PERCENTAGE_RANGE` | LDS permutations | folded | performance only; `Load` gives identical results |

---

## Appendix C: Resolved open questions

| # | Question (source) | Decision | Revisit if |
|---|---|---|---|
| C1 | FilteredTemporalWeight semantics [L] | Sum of unnormalized spatial weights (mode 0). Mode 1 (nearest) and mode 2 (1.0) are runtime switches. | TAAU at 50 % shows swimming or too-slow convergence in V8 → try mode 1 |
| C2 | Adaptive filtering formula in SuperSampling [L] | `Rejection = saturate(|Δclamp| / |box|)`, `InvFilterScale = lerp(1, 1/max(UF,1), Rejection)`, filter after clamp; box from moments only | ringing or aliasing at disocclusions in V9 |
| C3 | Mitchell-Netravali kernel [L] | Separable B = C = 1/3, support 2 output px, normalized, negative lobes clamped by the guard | – |
| C4 | SAMPLE_DISTANCE units [M] | Input pixels, `lerp(1.51, 1.3, UF − 1)` unclamped | – |
| C5 | Q0 box sample set [L] | Plus (5), for both MIN_MAX and SAMPLE_DISTANCE | – |
| C6 | AA_BICUBIC for Low (B claimed 0) | 1 for every quality | – |
| C7 | Jitter-index reset on cut (judges disagreed) | Reset (UE 4.26/5.x code) [M]. The effect is invisible, because a cut discards history. | – |
| C8 | 2/3/4/5 patterns key (A/B/C disagreed) | TAAU branch first, then switch on the **CVar value** with `% len` (UE 4.26) | – |
| C9 | Sample-count rounding | Truncation (UE `int32 = float`) | – |
| C10 | HistoryScreenPercentage under Main | Applies (UE 4.26 `GetTemporalAAHistoryUpscaleFactor`) | – |
| C11 | Stochastic quantization noise | UE: E ∈ [0,1) via `Hammersley16(Rand3DPCG16)`, error floored to a power of two; FP16 and R11 | S5 #4 fails → centred E (R12) |
| C12 | HDR weight constant | `1/(Y·E + 4)` with Y = Luma4 | – |
| C13 | Velocity small-object skip default | `MotionBlurPerObjectSize = 0.5` (UE `FPostProcessSettings` default [M]); C's "default 0" claim rejected | – |
| C14 | Upscale mode 4 footprint | Lanczos-3, merged centre pair, **diamond 13 taps**, normalized | – |
| C15 | Upscale mode 5 formulation [L] | Gaussian (σ² = 0.5) with a Laplacian unsharp term scaled by `Softness·max(0, 1 − area ratio)` | – |
| C16 | Tonemapper merge | UE modes 0/1/2 with Threshold 0.49; merged = bilinear | – |
| C17 | Where DOF runs and how big | R, `MaxBlurSize × R.x/O.x` | – |
| C18 | Bloom chain size | Fixed at `O/2` (SP-invariant halo); the threshold pass box-prefilters the input sampled by UV | – |
| C19 | Exposure for HDR weighting | The previous frame's AutoExposure result on the GPU (t4), or manual when invalid; no CPU readback | – |
| C20 | Depth invariance technique | A shared `precise` helper; DepthBias −4 only as a fallback | S4 #5 fails → fallback |
| C21 | Lumen previous matrices | Jittered (they match `PrevSceneColor`/`PrevLinearDepth` rasters); fog uses NoAA | – |
| C22 | Previous-frame commit point | End of `RenderPostProcessing` (`CommitViewState`); snapshot at `PrepareViewStateForVisibility` | – |
| C23 | Resize ordering | FlushAndReset → release → WaitGPU → create | – |
| C24 | History on SP change | Kept and resampled via `Hp` (UE) | – |
| C25 | Where the TAAU default goes | `r.TemporalAA.Upsampling = 1` and `r.AntiAliasingMethod = 2` from S1 (values), active from S5/S6 (gates) | – |
| C26 | Responsive AA mask coverage | Front-most translucent layer via LESS_EQUAL against the translucent prepass depth; additive included | – |
| C27 | R11G11B10 default | `true` (UE5 [M]); effective only for Q0/Q1 | – |
| C28 | Debug #615 | Fixed in S0 (`EDepthStatePreset::None`) | – |
| C29 | Test-driver camera control | Transforms set before `World.Tick`, with input disabled and the controller velocity reset | – |
| C30 | Captures | Back-buffer readback before ImGui, BMP; no `CopyFromScreen` | – |

## Revision log

Adversarial review, 2026-09-30. Each finding was checked against the source at `444dee0`, or against the math with the scripts in `<scratch>/taau/calc/`. All 14 are **accepted**. Two were refined beyond the proposed fix; nothing was rejected outright, and the parts of fixes that were not adopted are noted in their rows.

| # | Finding (severity) | Verdict and evidence | Resolution |
|---|---|---|---|
| 1 | View 5 and its criteria contradict the BlendFinal anti-stall floor (major) | **Valid.** `max(BF, saturate(0.01·Lh/\|ΔL\|))` is 1 when \|ΔL\| ≤ 1 % Lh, which is most converged flat pixels (A.9), so a correct image shows red. | The blend is unchanged. Step 13 saves `BlendFinalPreFloor`, and view 5 shows `IgnoreHistory ? 1 : (bResponsive ? 0.25 : BlendFinalPreFloor)` (§6.5.4, §6.8). A.9 gains a pre-floor column. `imgstat` gains a dark-blue fraction. S5 #7, S6 #3, V2, V12 and V13 now name the pre-floor view. The optional post-floor view was not added: it is red by design and has no pass or fail signature. |
| 2 | Float world-space `ClipToPrevClip` leaves a static-camera bias that grows with \|O\| (minor) | **Valid.** Our float32 emulation: 0.008 / 0.017 / 0.13 px at 0 / 150 / 1000 m. The reviewer's figures are larger; the magnitude depends on the inverse algorithm, but the growth holds. | New `ComputeClipToPrevClip`: `InvProj·InvRot·T(ΔO)·Rot·Proj` in double, stored as float (§4.4). New self-test T4b: static poses give BackN = 0, and a 0.1 m move at 150 m reproduces A.5 within 1e-5 (§9.2, A.5, S2). |
| 3 | Catmull-Rom weights blow up for FS < ~0.75 (minor) | **Valid.** CR(15) = −1183. **Refined:** zeroing `ax ≥ 2` plus a `Σw ≤ 1e-6` guard still leaves normalized weights up to 91× at FS 0.4–0.5 (`calc/cr_scan.py`). | New §4.8.6: CR is 0 for `ax ≥ 2`, and the weights fall back to one-hot on the nearest sample when `Σw ≤ 1e-6` **or** `Σw < 0.25·Σ\|w\|`. The largest weight is then 2.46, and for FS ≥ 0.75 the result is identical to UE. The fallback is the nearest sample rather than the centre; the two are the same for every Gaussian or uniform jitter. T7, A.7, §7.1 and App. B are updated. |
| 4 | `m_bResponsiveMaskValid` stays true across frames without translucency (major) | **Valid.** `RenderTranslucency` returns early at `SceneRenderer.cpp:1018` and `:1085-1086`, before the mask pass. | Both flags are reset as the first statement of `BeginFrame` (§4.1 step 1, §2.5, S1), which also covers textures reallocated without a clear. The wrong "stays false" text in §4.6 is corrected. |
| 5 | The absolute GBV "no state messages" rule is unmeetable because of the LUT bug (major) | **Valid.** `m_FallbackTex` is created in PSR (`ColorGradingLUTBaker.cpp:222`) and bound to the bake CS whenever there is no artist LUT, including the constructor bake at `:200`. The committed INI loads `Adventure.DDS`. | S0 task 10 fixes both LUT states. D0 is split into D0-plain and D0-GBV (static, rotate, pan), measured after the S0 fixes. The pass rule in §9.6, S5 #9, S6 #7 and S7 #5 is now "⊆ D0-GBV and no new state, descriptor or barrier ID". |
| 6 | "Frames realloc + 2 baseline-equal" is unmeetable (major) | **Valid.** Fog is enabled (`GameManager.cpp:178`) with HistoryWeight 0.9 (`VolumetricFog.h:83`), Lumen accumulates, and the design reset `m_ProbeJitterIndex` / `m_LightScatteringFrame`. | No jitter-phase or fog ping-pong reset in `CreateScreenTextures` / `CreateVolumes` (§5.2, §5.3, R6). New S3a criterion: realloc every 90 frames, and the realloc + 60 captures are baseline-equal to `static` at the same frame index. A separate leak soak uses `-reallocperiod=30`. Lumen/fog validity is checked through the log only (S3a #4, S6 #3, V13). |
| 7 | The AutoExposure buffer decays to COMMON, so the RD → UAV barrier mismatches (minor) | **Valid.** `m_Result` is a buffer (`AutoExposure.cpp:159-161`), and the existing PSR → UAV barrier at `:380-386` has the same latent defect. | New `PrepareResultForRead()` (COMMON → RD) as the first statement of `RenderPostProcessing`. Pass 2 is RD → UAV and the end barrier is COPY_SOURCE → RD. The whole fix moves to S0 (task 12) so that D0 is measured with it (§4.12, §4.11, §3.9, R7). |
| 8 | Screen-pass helpers are invisible to other TUs, and a `const` method writes stats (minor) | **Valid.** The helpers are in an anonymous namespace (`SceneRenderer.cpp:27-57`), and `MakeLumenFrameInputs() const` (`SceneRenderer.h:151`). | `SceneRenderingUtils.h` gets inline helpers (§2.1, S0 task 11, §4.5.3). The stats are written by `RenderLumenScene` and `RenderLighting` from the inputs they build (§9.3, §2.5). |
| 9 | `VisualizeTemporalAAPS` is edited in S5 and S6 but the stages say "only new files" (minor) | **Valid.** | §9.1 is split into S5 / S6 / S7 rows. S5 #10 and S6 #7 allow `VisualizeTemporalAAPS.cso`. S7 still requires it unchanged. |
| 10 | The shader count of 26 is wrong (minor) | **Valid.** The count is 24: 13 compute + 11 graphics, plus 6 headers. | §6.10 is corrected and gains a per-stage FxCompile table (8 / 7 / 6 / 6 / 3 = 30 items), checked against the vcxproj header form (`:227-230`). |
| 11 | Dummy clear (0,0,0,0) vs optimized clear (0,0,0,1) → #820 (minor) | **Valid** (`RenderManager.cpp:966-970`). | The dummy is cleared to (0,0,0,1). The RG = 0 and R = 0 meanings are unchanged, and A = 1 is never read (§4.8.1, §3.9). |
| 12 | A missing upscale PS still yields a non-null PSO in Release (minor) | **Valid** (`LoadShaderBytecode` at `RenderManager.cpp:1128-1166`; a null PS is legal). | New `CreatePipeline(..., bOptional)` and `HasPipelineState`. The upscale and visualize PSOs are optional. `SelectPrimaryUpscalePipeline` falls back to quality 1, then to the merged tonemap, and visualize is skipped (§3.8, §4.7, §6.7, §6.8, R17). S3b #3 tests it. |
| 13 | t35 is bound to a stale or uninitialized Velocity (minor) | **Valid.** | t35 is `m_bVelocityValid ? Velocity : dummy` (§6.8), and `TemporalUpscalerIO` is added to the velocity pass's `bNeeded` (§4.5.3). |
| 14 | Uninitialized matrices are read by culling on `!bValid` frames (minor) | **Valid.** The identity initializers were already in §2.4. | `ComputeViewVisibility` rebuilds the frustum only when `m_ViewInfo.bValid`. Otherwise it keeps the last valid planes, as the old stale-b0 behaviour did, and before the first valid frame it has no planes, so everything is accepted (§4.4, §2.5, S1). |


Final whole-diff review, 2026-10-01. The confirmed findings of the review of `300d7ff..working tree` were applied after S8 (checkpoint "final-review fixes"). Evidence is under `<scratch>/taau/review/fix/`.

| # | Finding (severity) | Resolution |
|---|---|---|
| 1 | The camera-following sky dome wrote velocity, so its anti-ghost alpha rejected skyline pixels while panning and discarded the whole sky history when the camera stopped (bug) | `UPrimitiveComponent::SetRenderVelocity(false)` for the dome, `FPrimitiveSceneProxy::RendersVelocity()`, skipped in `RenderVelocities` (§0.1 item 7, §4.5). The far-pixel rule reconstructs its motion exactly. `pan -debugvis=6` and `largemove -debugvis=6` f61 now show 0 anti-ghost rejects; `pan -debugvis=2` no longer shows green sky (S4 #4, V6, §6.8 view 2). |
| 2 | `r.TemporalAAUpsampleFiltered=0` also disabled filtering in MainSuperSampling (spec deviation) | `P.bUpsampleFiltered = CVar || Pass != MainUpsampling` (§4.8.2, §6.5.1); the ImGui checkbox says "(MainUpsampling only)" otherwise. |
| 3 | The driver read the AutoExposure READBACK buffer while this frame's copy could still be running (robustness) | `PostFrame` calls `WaitGPU` before `UpdateReadback` (test mode only); the descriptor / queue columns are sampled before it. `ae_exposure` / `ae_avg_luminance` are now this frame's values and deterministic. `frame_ms` loses CPU/GPU overlap in `-taatest` runs. |
| 4 | `FBarrierBatch::Track` committed a state even when `Add` dropped the barrier (quality) | `Add` merges chains / exact duplicates and returns whether the resource ends in `After`; `Track` commits only then. |
| 5 | The bloom halo scaled with the spatial screen percentage (behaviour change) | Chain fixed at `O/2` (constructor only; `P` changes no longer reallocate it); the threshold pass box-prefilters with 4 taps at ±¼ mip0 texel (§0.1 item 12, §3.9, §5.1, §5.2, §5.3, C18, judge #18). |
| 6 | TAA history / aux targets were never released (robustness) | History pool + half-res / MN / debug outputs are released while `!bTemporalAA`; half-res, MN and debug outputs also when their configuration is not used this frame (§3.9). |
| 7 | Source comments referenced a design doc that only existed in the session scratchpad (quality) | This document is copied into the repository as `Docs/TAAU/TAAU_Design.md` (not git-added); `TemporalAA.h` names it; the T7 comment no longer names a scratch script. |
| 8 | The tested IGN helper was not the one DeferredPS ran (quality) | `DeferredPS.hlsl` includes `TemporalAACommon.hlsl` and calls `InterleavedGradientNoise`; `DeferredPS.cso` is byte-identical. |
| 9 | Stage tags / gate history left in comments (quality) | Removed or rephrased; the self-test catalogue is grouped by subject. Comment-only. |
| 10 | Wrong Debug self-test comment (quality) | Comment says CPU + GPU parity; the `bIncludeGPU` signature is kept (§2). |
| 11 | Write-only state and unused API (quality) | Removed: screenshot `m_NumRows` / `m_RowSizeInBytes` / `HasPending` / `GetLastWrittenPath`, driver `GetFrame` and the unused `UWorld&` parameters, `GetCapacityCells`, `Time::GetFixedDeltaTime`, `IsSaveEnabled`, `IsInputEnabled`. Kept as spec-declared UE-mirroring API: `CameraId`, `LastFrameUpdated`, `m_InternalFrameIndex`, `UAVHandle`, `GetDefaultViewportWidth/Height`, `IsTAAUpsamplingConfig`, the `ITemporalUpscaler` getters. `GetFrameIndexMod8()` is used for b0. `GetPrimaryUpscalePipelineName` is now used (#12). |
| 12 | Upscale PSO / method / quality names and debug ranges defined in several places (quality) | `RenderManager` registers upscale PSOs through `GetPrimaryUpscalePipelineName` (+ `static_assert`); ImGui combos use `GetTAAQualityName` / `GetUpscaleMethodName`; `AntiAliasingRanges::kVisualizeScale*` / `kFilteredTemporalWeightMode*`. |
| 13 | Three log-once patterns / inline back-buffer barrier (quality) | `FSceneRenderer::m_UpscaleFallbackLoggedMask` (+ `kBilinearMissingBit`) and `m_bLoggedMissingVisualizePSO`; `EndFrame` uses `TransitionBackBuffer`. |
| 14 | Unnamed constants (quality) | `kTAATileSize` / `TAA_TILE_SIZE`, `kNumTAAPassConfigs` / `kNumTAAQualities`, `TWO_PI`. All TAA / visualize `.cso` byte-identical. |
| 15 | Non-ANSI `-taaout` paths split the log and the BMPs (robustness) | Capture paths are carried as UTF-8 (`path.u8string()` / `std::filesystem::u8path`). |
| 16 | `Saved/Screenshots` and `Saved/TAATest` were not git-ignored (quality) | `.gitignore` rules added. |
| 17 | F9 names repeated across sessions (robustness) | Automatic names start with `<YYYYMMDD_HHMMSS>_` (§ST path, `SceneRenderer.h`). |
| 18 | A recorded but unwritten F9 / button capture was lost on exit (robustness) | `GameManager::~GameManager` calls `FlushScreenshots` before the final `WaitGPU`. |

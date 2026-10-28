# プロシージャルツリー生成 設計書 (v2)

**対象ブランチ:** `Procedural` (基点 `4f34adf`。v2 と polish 1〜3 の変更は未コミットの作業ツリー)。
**スコープ:** パラメータから木 (幹 / 枝 / 小枝 / 葉 / 葉の房) のメッシュを実行時に生成し、既存のスタティックメッシュ描画経路 (ベースパス / シャドウ / Distance Field / Lumen / Velocity) にそのまま載せる。Details での編集と INI への保存を含む。v2 (`GeneratorVersion 2` / テクスチャ `Version 2`) は約 5 m の広葉樹 (Oak 5 m プリセット = 開けた場所で育ったオーク) を目標に、骨格 / 葉の陰影 / テクスチャを作り直したもの。その後の polish 1〜3 で、非対称な二又、太い主枝、屈曲、平たい丸屋根の樹冠、樹皮 v6 (溝の網)、樫の葉を入れ、葉 (Substrate TwoSidedWrap) のピクセルだけ照明を補正した (4.4)。ヘッダの型と `GeneratorVersion` / `Version` は固定のまま。生成規則の改訂は `ProceduralTree.cpp` の `GeneratorRevision` (= 3) をハッシュに混ぜて区別する。
**前提:** 単位はメートル、Y アップ、左手系。表面は時計回り、接線 = +U、`dP/dV = cross(N, dP/dU)` (ProceduralMeshGenerator.h の規約)。

---

## 0. 決定事項

1. **メッシュは FBX と同じ `FBXModel` に載せる。** `FBXModel::BuildFromProceduralMesh` (CPU 頂点列 → GPU バッファ + ローカル境界 + SDF ベイク + DXR BLAS) を通すので、描画系は木を特別扱いしない。この経路と `ProceduralMeshGenerator.h/.cpp` (基本形状 / ノイズ / 接線計算 / FNV ハッシュ) は、破棄済みコミット `2b051fd` (reflog) の基盤部分をそのまま再利用した。
2. **木は `ATree` (AStaticMeshActor 派生) 1 体 = `FProceduralTreeParams` 1 組。** メッシュは `FAssetManager::CreateProceduralMesh("Tree_<パラメータハッシュ>")` で共有する。同じパラメータの木は GPU バッファ / SDF スロット / BLAS を共有する。
3. **プロシージャルメッシュのキャッシュは弱参照。** パラメータ編集のたびに別名のメッシュが生まれるので、`FAssetManager` は `weak_ptr` で持ち、使うコンポーネント / プロキシが居なくなった時点で解放する。`FBXModel` のデストラクタが `FDistanceFieldAtlas::ReleaseMesh` でスロットを返却し、`AddMesh` は空きスロットを優先して再利用する (スロットは 16 個しかない)。
4. **再生成は描画の外 (Tick) で行う。** メッシュ構築は SDF ベイク / BLAS 構築でコマンドリストをフラッシュする (`FlushAndResetCommandList`) ため、ImGui のコールバック (フレーム内) では行わない。Details / INI からの変更は `ATree::RequestRebuild()` で予約し、次の `Tick` が `RebuildTree()` を呼ぶ。`RebuildTree` は全体の所要時間を `FProceduralTreeStats::RebuildMs` に記録する (キャッシュから取ったときは 0)。
5. **出力は常に 4 セクション (添字 = MaterialIndex = マテリアルスロット)。** 空のセクションも MaterialIndex 付きで存在する (`FStaticMeshSceneProxy::ResolveSlot` は未知の添字を最後のスロットに丸めるので、添字がずれると小枝が葉のマテリアルで描かれる)。

   | セクション | 内容 | マテリアル | SDF |
   |---|---|---|---|
   | 0 `BarkMaterialIndex` | 幹と太い枝 (確定した根元半径 >= `TwigRadiusThreshold`、Oak 既定 2.2 cm)。閉じたチューブ | Opaque | 寄与する |
   | 1 `LeafMaterialIndex` | 単葉カード (四角形 2 三角形) | Masked / TwoSided / Substrate TwoSidedWrap | 非寄与 |
   | 2 `ClusterMaterialIndex` | 葉の房カード (2x2 アトラスのセル 1 枚 = 葉 5〜6 枚) | 1 と同じ設定 + 房アトラス | 非寄与 |
   | 3 `TwigMaterialIndex` | 小枝 (根元半径 < `TwigRadiusThreshold`)。閉じたチューブ | Opaque + 小枝用樹皮 | 非寄与 |

   葉 / 房は開いた面で、レイ交差の偶奇で符号を決める SDF ベイクを乱すので入れない (`FProceduralMeshSection::bAffectDistanceField = false`、`FBXModel::CreateSubset` が三角形の蓄積を飛ばす)。小枝は閉じているが 64³ のボクセル (8〜9 cm) より細く、ベイク (全ボクセル x 全三角形の総当たり、同期) の時間だけ食うので除外する。`TwigRadiusThreshold 0` で v1 と同じく全ての枝が SDF に入る。DF シャドウ / SW Lumen からは小枝が消えるが、描画 / シャドウマップ / BLAS には全セクションが参加する。Oak では 2.2 cm 以上の主枝 / 主幹 (片 31 本) が樹皮セクションで、SDF 三角形は 4522 (< 10k)。
6. **テクスチャも生成する (`ProceduralTreeTextures.cpp`)。** 既存アセットに樹皮 / 葉のテクスチャが無く、テクスチャ未設定のスロットは直前のバインドを引きずる (`FStaticMeshSceneProxy::BindSlot` は存在するものだけバインド) ので、4 スロット全てに BaseColor / Normal / ARM の 3 枚を必ず割り当てる (無いファイルは 房 → 葉、小枝 → 樹皮、法線 → 平坦 で代替)。13 ファイルを RGBA8 + ミップの DDS として `Saved/ProceduralTexture/Tree*_v2.dds` に書き出し、既存の DDS ローダで読む。ファイルが揃っていれば生成しない。ヘッダの `Version` は 2 で固定なので、v2 の中で絵柄を変えたとき (樹皮 v5 / v6、樫の葉) は古い `Tree*_v2.dds` を消して作り直させる。初回起動のベイクは合計約 1.1〜1.2 s (樹皮 + 小枝 ~0.44 s、房アトラス ~0.54 s、葉 ~0.15 s) で再生成経路には乗らない。
7. **同じパラメータからは常に同じメッシュ。** 乱数はシード付き `mt19937` を自前の一様変換で使う (標準の分布クラスは処理系依存)。v2 は枝ごとの乱数サブストリーム (2.1) なので、ある枝のパラメータを変えても他の枝は変わらない。`FProceduralTreeParams::ComputeHash` (FNV-1a、`GeneratorVersion` と `GeneratorRevision` 込み、使っている階層分のみ、`LeafTint` 含む) がキャッシュ名 / SDF キャッシュ (`Saved/ProceduralMesh/Tree_<hash>_<ジオメトリハッシュ>.mdf`) のキーになる。生成アルゴリズム / 乱数の消費順を変えたら `GeneratorRevision` を上げる (ヘッダの `GeneratorVersion` は固定。古いキャッシュは孤立するだけで衝突しない)。
8. **生成は 2 パス。** (A) 骨格パス = 乱数を消費して骨格 (`FStem`) と葉の配置パラメータ (`FLeafSpec`) を決める。樹冠の天井 / 小枝の切り詰め (2.4) で縮めた枝は、同じ角度の予定で経路を作り直す (乱数は引き直さない)。(B) 出力パス = 半径の確定 → 葉の原点 / 向きの解決 → 樹冠の統計 (中心 / 葉密度グリッド) → ジオメトリ。出力パスは乱数を引かない (見た目の選択は `LatticeHash`)。樹冠全体の情報 (中心からの向き、露出度) を葉の法線 / 頂点カラーに焼けるのはこのため。
9. **葉のマテリアルは Substrate TwoSidedWrap (Masked / TwoSided)。** `ATree::ApplyDefaultMaterials()` が 4 スロットの既定値を入れる (4.3)。保存済み INI の `C0.M1.*` が新しい既定値を隠さないよう `ATree::MaterialPresetVersion` (= 3) を `Tree.MaterialPreset` として保存し、古ければ読み込み時に既定値で上書きする (4.2)。照明側は TWO_SIDED_WRAP のピクセルだけ、透過の影とスカイ可視率を補正する (4.4。他のマテリアルの出力は変わらない)。
10. **予算は `Sanitize()` が保証する。** 枝の片の総数 <= 20000 (`MaxTotalStems`、分岐のクローン込みで `EstimateStems()` が正確に数える)、単葉 + 房カード <= 8000 (`MaxLeafCards`、`EstimateCards()` は上界)、三角形 <= 500k (`MaxTreeTriangles`、`ProceduralTree.cpp` 内の定数、上界の見積り)。枝数は 最深階層の `SegSplits` → `BaseSplits` → 最深階層の `Branches`、カードは `LeavesPerBranch` (2 まで) → `ClustersPerTwig` → `LeavesOnParentLevel` → 最深階層の `Branches`、三角形は最深階層から `RadialSegments` (4 まで) → `Segments` (3 まで) → 幹の分割 (周 8 / 長さ 6 まで) → 最深階層の `Branches` の順に減らす。
11. **INI は後方互換。** `[Actor.N]` に `Tree.*` キーを足すだけ。既存キーは変えない。旧キー `Tree.Tropism` は明示的に読み替える (4.2)。木のアクターはフォグの後にスポーンし、既存の `[Actor.N]` の添字を変えない。保存済みの `Tree.*` は既定値より優先される (4.2)。

---

## 1. 構成

| ファイル | 役割 |
|---|---|
| `ProceduralTree.h` | `FProceduralTreeParams` (全体 + `FProceduralTreeLevelParams` x 4) / `ECrownShape` / `ETreePreset` / `FProceduralTreeStats` / `FProceduralTreeBuilder` (セクション定数、予算) / `FProceduralTreeTextures` (パス、`Version`、`LeafCoverageClip`、`AtlasCellInset`) の宣言。型の正本 (メンバ初期化子は名目値で、Oak の既定値はコンストラクタ) |
| `ProceduralTree.cpp` | `FTreeGenerator` (骨格パス `GenerateStem` / `PruneLength` / `CollectLeaves` / `CollectClusters`、出力パス `AssignRadii` / `ResolveLeafSpecs` / `ComputeCrownStats` / `EmitBark` / `EmitLeaves`)、コンストラクタ (= Oak5m) / `MakePreset` / `Sanitize` / `EstimateStems` / `EstimateCards` / `ComputeHash` / `Build`。形の定数 (`LimbKinkFactor` 等) と `GeneratorRevision` / `MaxTreeTriangles` はファイル冒頭 |
| `ProceduralTreeTextures.cpp` | 樹皮 (溝の網 `FFurrowNetwork`) / 葉 (`FLeafVariant` + `EvalLeaf`) / 房アトラス / 小枝 / 平坦法線の DDS 生成 (カバレッジ保存ミップ `WriteDDS`) |
| `Tree.h/.cpp` | `ATree`: パラメータ保持、ハッシュ比較、`FAssetManager` 経由のメッシュ差し替え、`RebuildMs` 計測、マテリアル 4 スロットの既定値 (`ApplyDefaultMaterials`)、`MaterialPresetVersion` |
| `ProceduralMeshGenerator.h/.cpp` | `FProceduralMeshSection` / `FProceduralMeshData` (2b051fd から再利用) + `bAffectDistanceField`、`CountInvertedTriangles` / `ComputeTangents` (テスト用) |
| `FBXModel.h/.cpp` | `BuildFromProceduralMesh` / `CreateSubset` / `FinalizeLoad` (2b051fd から再利用) + DF 除外フラグ + デストラクタでのスロット返却 |
| `AssetManager.h/.cpp` | `CreateProceduralMesh` / `FindProceduralMesh` (弱参照キャッシュ) |
| `DistanceFieldAtlas.h/.cpp` | `GetIfExists` / `ReleaseMesh` / 空きスロットの再利用、非キャッシュベイク (2b051fd) |
| `ImGuiManager.cpp` | Details の "Procedural Tree" セクション (`DrawProceduralTreeSection`) |
| `SettingsManager.h/.cpp` | `ActorSnapshot::Tree` / `TreeMaterialPreset`、`Tree.*` キーの読み書き (`WriteTreeParams` / `ReadTreeParams`、旧 Tropism の読み替え)、`ApplyActor` → `SetTreeParams` + マテリアル版の判定 |
| `Shader/SubstrateEvaluation.hlsl` / `ShadowFilteringCommon.hlsl` / `ForwardLightingCommon.hlsl` / `DeferredPS.hlsl` / `TranslucentPS.hlsl` | 葉 (TWO_SIDED_WRAP) の照明の補正: 裏面透過を別の項にして専用の影を掛ける、CSM の遮蔽物までの距離による透過の影、スカイ可視率の下限 (4.4) |
| `TemporalAATestDriver.h/.cpp` | `-taatest=tree` シナリオ (実行中の再生成、SDF スロット、再生成時間の上限 = 終了コード 5) |
| `GameManager.cpp` | `ATree` を (7, 0, 6) にスポーン (テーブルの外、地面クアッドの内側) |

---

## 2. 生成アルゴリズム (Weber & Penn の簡略版、v2)

### 2.1 2 パス構成と乱数の規約

- **骨格パス (A):** `GenerateStem(FChildSpec)` を幹から深さ優先で再帰する。1 本の枝 (片) ごとに `FTreeRandom(Spec.Seed)` を作り、その枝の乱数は全てそこから引く。`FStem` は `m_Stems` (添字 = 枝 ID、親が先)、葉 / 房の配置パラメータは `m_LeafSpecs` に積む。
- **サブストリームのシード:** 幹 = `HashCombine(Params.Seed, 0, 0)`、子 / クローン = `HashCombine(親の Seed, 子番号 + 1, 生まれる枝自身の Level)` (FNV-1a 32bit を 12 バイトに掛ける)。子番号は分岐のクローンを先に数える (クローンは親と同じ Level、通常の子は親 + 1)。木全体のストリーム `FTreeRandom(Params.Seed)` からは幹の長さだけ引く。
- **引く回数は整数パラメータだけで決まる (無条件に引く):** 枝 1 本につき、セグメントごとに 4 回 (揺れの振幅 / 方位、分岐の位相 / 分岐角と長さの揺れ。分岐しなくても引く) → 子の螺旋の開始位相 1 回 + 子 1 本につき 4 回 (位置 / 分岐角 / 下向き角 / 長さの揺れ。`bAlternate` でも位相は引く) → 葉: 裸枝判定 1 回 + 1 枚につき 6 回 (位置 / 方位 / 角度 / 大きさ / 捻り / 予備。出力しない葉の分も引く) → 房 (`ClustersPerTwig > 0` のときだけ): 1 枚につき 5 回。最終階層は単葉 `LeavesPerBranch` 枚 + 房 `ClustersPerTwig + TerminalClusters` 枚、1 つ上の階層 (側枝) は単葉 `LeavesOnParentLevel` 枚 + 房 2 枚 (`ClustersOnParentLevel`)、それより上の階層は枝先の葉 `LeafTerminalCount` 枚 + 枝先の房 `TerminalClusters` 枚 (切られた片は 0)。刈り込まれた切り株も裸枝も同じ回数を引き、子も必ず生やす (枝数 / 葉の枚数は刈り込みで減らない)。例外として、分岐した片ごとの子 / 葉の枚数は取り分区間 (2.3) 経由で `BaseOffset` / `SegSplits` (float) に依存するが、合計は保存される。
- **見た目だけの選択は `LatticeHash(X, Y, Seed)` で引かない:** 単葉 / 房の `U0 / U1 / U2` = `(Index, 0 / 1 / 2)`、縁の削れ α = `(Index, 3)`、房の左右反転 = `(Index, 4)`、枝の途中の房の面内の回転 = `(Index, 5)`、樹皮 V のオフセット = `(枝の Seed, 5)`、二又の脇の片の断面の取り分 = `(枝の Seed, 6)`、脇の片の長さ = `(枝の Seed, 7 + k)`、Level 1 の枝ごとの色相 = `(AncestorL1, 7)`、葉の螺旋の開始位相 = `(枝 ID, 9)`、房の螺旋の開始位相 = `(枝 ID, 10)`、房のアトラスセル = `(枝 ID, 11 + k)`、枝先の房を付けるか = `(枝 ID, 31)`、子の付け根の枠の揺れ = `(枝の Seed, 40 + k)`、屈曲の面 / 振幅 = `(KinkSeed, 49)` / `(KinkSeed, 50 + セグメント)`、房の大きさの Level 1 ごと / 小枝ごとのばらつき = `(AncestorL1, 61)` / `(枝の Seed, 62)`、幹の傾きの方位 = `(0, 77)`、根元の苔の方位 = `(0, 78)`。`Index = 枝 ID x 256 + スロット` (単葉 0..63、親階層の葉 64 + j、房 128 + k) なので、トグルで出力枚数が変わっても同じ葉は同じ色になる。葉の色の空間的なむらと苔のまだらは `FProceduralNoise(Params.Seed)` の FBM。
- **出力パス (B):** `AssignRadii` (2.5) → `ResolveLeafSpecs` (2.7) → `ComputeCrownStats` (2.8) → 枝ごとに `EmitBark` (確定した根元半径 < `TwigRadiusThreshold` ならセクション 3、それ以外は 0) → `EmitLeaves` (単葉はセクション 1、房は 2)。出力バッファは先に確保する。
- **巻き順:** `AddTriangle(Section, I0, I1, I2, Reference)` が幾何法線と Reference の内積で向きを決める (縮退三角形は捨てる)。葉カードは面法線 (曲げる前) を、樹皮はブレンド前の幾何法線の和を Reference に渡すので、頂点法線を曲げても巻き順は変わらない。`CountInvertedTriangles` (頂点法線の和との比較) が 0 になるよう、葉の頂点法線は面法線との内積 >= 0.12 に抑える (`ClampToHemisphere`)。

### 2.2 骨格 (セグメント列)

幹は根元から `Level[0].DownAngle` (鉛直からの角、30 deg まで、Oak 4 deg / Sapling 0) だけ傾け、方位は `LatticeHash(0, 77)`。枝は `Segments` 本のセグメント。各セグメントで接線 `t` を次の順に更新し、フレームの法線は回転最小フレーム (`TransportNormal`) で運ぶ。角度は全て元の枝 (分岐前) の `SegmentsTotal` で割り、分岐したクローンは元の枝の予定をそのまま続ける (2.3)。

1. **系統的な曲がり** (鉛直面内、+ で上へ。鉛直な接線ならフレームの法線まわり): `CurveBack == 0` なら `Curve / Segments` を全長に、非 0 なら前半 `Curve`、後半 `CurveBack` を各 `Segments / 2` で配る S 字 (奇数セグメントは中央が半分ずつ)。天頂 / 真下を越えては曲げない (残りの角 `AngleToVertical` = `atan2(水平成分, ±t.y)` で飽和。`acos(t.y)` は鉛直近くで桁落ちする)。クローンの最初のセグメントだけは分岐角の手前で止める (`KeepFromVertical`): 真下で飽和した小枝の兄弟クローンが同じ向きに重ならないように。
2. **低域通過した揺れ:** 先にセグメント分の `w = Signed(CurveVariance)`, `az = Range(0, 2π)` を引いてベクトル `v[s] = w (cos az, sin az)` (フレーム N / B 成分) にし、`v' = 0.25 v[s-1] + 0.5 v[s] + 0.25 v[s+1]` (端は折り返し) の向きを軸に `|v'| / Segments` [deg] 回す。
3. **屈光性:** `Tropism x TropismScale x (1 - t.y) / Segments` [deg] を曲がりと同じ軸 / 符号規則で (+ で上、- で垂れる)。`Tropism` の単位は「水平な枝が全長で曲がる角 [deg]」(直立で 0、水平で 1 倍、真下で 2 倍 = 垂れた枝ほど強い)。1 セグメント ±12 deg まで、鉛直で飽和。
4. **幹の脇の片の立ち上がり** (polish3、`UprightBend > 0` のときだけ): 二又の脇の片 (2.3) は 2 番目から `max(1, Segments / 2)` 番目のセグメントで合計 `0.5 x 分岐角` (`SideCloneUpright`) だけ鉛直へ戻す (最初のセグメントは分岐角いっぱいに開いてから立ち上がる壺形。天頂は越えない)。
5. **屈曲 (仮軸分枝のジグザグ、polish3):** Level 1 / 2 と幹の分岐より上の片 (Level 0 のクローン) で、`CurveVariance > 0` のとき。元の枝のセグメント番号 j で、j = 0 は `+k/2`、以降は奇数 `-k` / 偶数 `+k` を交互に折る (向きは +k/2 と -k/2 を行き来するので枝全体の向きは変わらない)。`k = min(0.3 x CurveVariance, 20 deg) x lerp(0.6, 1, LatticeHash(KinkSeed, 50 + j))` (`LimbKinkFactor`)、折る面は `LatticeHash(KinkSeed, 49) x 2π + 0.45 j` [rad] の方位 (N / B 平面) でゆっくり回る。`KinkSeed` は元の枝の Seed でクローンへ引き継ぐ (分岐しても屈曲の予定が続く)。乱数は引かない。Oak では幹の分岐より上 ±12、L1 ±13.5、L2 ±9 deg。
6. **合計の上限:** 1 セグメントの合計の曲がりを `min(40 deg, 2 asin(0.45 x セグメント長 / 襟込み根元半径))` に抑える (`LimitBend`。隣接リングが交差せず、フレームが捻れない)。

骨格点の接線は隣接セグメントの平均。骨格パスの半径はテーパ曲線 (出力パスのパイプモデルが置き換える): 元の枝の長さ比 x で `Base x (1 - (1 - Taper') x^p)` (`p = 1.5` (`PipeTaperPower`) はパイプモデルあり = 根元側は太いまま先で細る凸形、`PipeExponent 0` では 1 = 直線)。`Taper' = Taper x TipScale` (刈り込みで縮めた比。短くした枝が太い切り株で終わらない)。テーパは元の枝の全長で測る (クローンの先端 / 根元 = テーパ曲線の終点 / 始点の比)。`TipRadius = min(Base, max(..., MinTipRadius 2 mm))`、`MinStemRadius 1.5 mm`。

### 2.3 二又分岐 (BaseSplits / SegSplits、乱数を使わない)

- **分岐の予定 `FindSplitSegment`:** 幹の最初の片は `(s + 1) / SegmentsTotal >= BaseOffset` となる最初のセグメントで `BaseSplits` 本に分岐する (それまで `SegSplits` は数えない = 根元の分岐は常に幹の最初の分岐で、樹冠の根元は最初の片の中にある)。それ以外のセグメントは誤差拡散: `err += SegSplits; n = floor(err + 0.5); err -= n`。誤差はクローンへ引き継ぐ (経路に沿った分岐数 ≒ `SegSplits x Segments`)。`n > 0` で最後のセグメントでなければそこで切る。Oak は幹の 5 / 12 セグメント (y 1.96 m) で二又。
- **切られた片:** 骨格点は `owned = s + 1` セグメント分、長さ / 先端半径はそこでのテーパ曲線の値。先端の半球はクローンの中に埋まる。切られた片 (`IsCutPiece`) には枝先の葉 / 房を付けない。
- **クローン `n + 1` 本** (通常の子より先に積む、非対称): 分岐角 `clamp(SplitAngle + 揺れ x SplitAngleVariance - 0.5 x 鉛直からの傾き, 8, 90) deg`。k = 0 は主軸を継ぐ片で分岐角 x 0.25 (`MainSplitDeviation`) だけ逸れ、方位は脇の片の反対側。脇の片 (k >= 1) は分岐角いっぱいに開き、方位 = 位相 x 360 + (k - 1) x 360 / n (フレームの N / B 面内)。断面の取り分 (パイプモデル): 脇の片の合計 `S = lerp(0.30, 0.45, LatticeHash) x n` (0.75 まで) を等分、主軸は `1 - S`。半径 = `r x 取り分^(1 / max(PipeExponent, 1))` (出力パスで親の確定半径に合わせ直す)。長さ = 残りセグメント x (1 ± 0.1 揺れ)、脇の片はさらに `lerp(0.72, 1, LatticeHash)` 倍。根元 = 先端から `(0.3 + 0.15 k) r` 戻した点 (根元の円盤を親に埋める。片ごとに深さを変えるのは、半径が下限で揃った兄弟の根元リングが同じ中心で交わらないように)。`SegmentStart` / `DistanceOffset` (幹の接地ランプを続ける) / `AncestorL1` (同じ色相) / `ParentShade` (切った先端の明度) / `TipScale` / `KinkSeed` を引き継ぐ。幹の脇の片は `UprightBend = 0.5 x 分岐角` (2.2 の 4)。
- **主軸の先端 (polish3):** 刈り込みが有効なとき、幹のクローンの予測した先端 (原点 + 向き x 長さ) が `yT - 0.5 x ClusterSize` (`LeaderTopMargin`) より上なら、そこで止まる長さに縮める (元の半分より短くはしない)。主軸の先が葉の外へ裸で突き出ない。
- **子の数の保存 (取り分区間):** 元の枝の子を生やせる部分 (`BaseOffset` より先) に占める片の割合を区間 `[ShareLo, ShareEnd]` で持ち、`count = SharedCount(Branches, lo, hi) = round(n hi) - round(n lo)`。区間の端で丸めるので全ての片の合計はちょうど `Branches`。葉 / 房の枚数も同じ取り分 (片ごとに最低 1 枚)。
- **見積り:** `EstimateStems() = Σ_i stems_i x (1 本の枝の片の数)` を同じ `FindSplitSegment` で数えるので生成される `NumStems` と一致する。`EstimateCards()` は上界: 最終階層 = 枝の数 x 枚数 + (片の数 - 枝の数) (単葉、房の途中の分も同じ) + 片の数 x `TerminalClusters`、1 つ上の階層 = `LeavesOnParentLevel` と房 2 枚を同じ規則で、それより上 = 片の数 x (`LeafTerminalCount` + `TerminalClusters`)。三角形の上界 (Sanitize 用) = 片 1 本あたり `2 R x 持つセグメント + 8 R` (幹の最初の片は + 6 R) + カード x 2。

### 2.4 子枝の仕様 (樹冠の形 / 刈り込み / 頂芽優勢 / 左右交互)

子 1 本につき 4 回引く (位置 / 分岐角 / 下向き角 / 長さの揺れ)。比 `r = 1 - (t - baseOffset) / (1 - baseOffset)` (1 = 樹冠の根元、0 = 親の先端。分岐した片でも元の枝全体で測る)。

- **位置 (頂芽優勢 + 枠の揺れ):** `u = clamp((k + 0.5 + Signed(0.25) + j) / count, 0.25 / count, 1 - 0.25 / count)`、`j = ±0.4 x LatticeHash` (`LimbSlotJitter` / `ChildSlotJitter`。主枝が群れと隙間を作り、小枝が等間隔の櫛にならない)。両端のクランプは子が親の端 (分岐点 / 先端) にちょうど乗らないように (分岐直後の子はパイプモデルの窓でクローンの根元リングを細らせる)。Level 1 はさらに 3 段 (`LimbTiers`) の中心へ 0.45 (`LimbTierPull`) 寄せる (段の間に空が抜ける)。`t = baseOffset + (1 - baseOffset) u^(1 / (1 + TipBias))`。
- **二又の分岐点のまわり (Level 1、polish3):** 幹の片の上では、クローンの根元 / 切られた片の先端から 0.25 m (`ForkClearance`) の中に主枝を置かない (t の範囲をその内側へ写す。子の数は変わらない)。分岐点から 0.5 m 以内の主枝は下向き角を 70 deg までに抑える (分岐点から真横に出る細い枝にしない)。
- **方位:** 螺旋 (`phi += Rotate + 揺れ`)。`bAlternate` なら基準面法線 `planeN` (RMF 法線と「枝から見た上」`cross(T, cross(up, T))` を 0.6 で混ぜたもの、空寄り) から `±(180 - Rotate + 揺れ)` で左右交互 (平面的な小枝。`Rotate` は面からの開き角)。
- **下向き角:** `clamp(DownAngle + DownAngleShape x (1.6 r - 0.6) + 揺れ, 5, 175) deg` (根元側 +Shape で水平寄り、先端側 -0.6 Shape で立つ)。
- **長さ:** `親の名目長 x Length x [Level 1: ShapeRatio(CrownShape, r)] x lerp(1, LengthShape, 1 - r) x (1 + 揺れ)`。`ShapeRatio` は Weber & Penn の 8 種: Conical `0.2 + 0.8 r`、Spherical `0.2 + 0.8 sin(π r)`、Hemispherical `0.2 + 0.8 sin(π r / 2)`、Cylindrical `1`、TaperedCylindrical `0.5 + 0.5 r`、Flame `r <= 0.7 ? r / 0.7 : (1 - r) / 0.3`、InverseConical `1 - 0.8 r`、TendFlame `0.5 + 0.5 x Flame`。
- **樹冠包絡 (刈り込み、`PruneRatio > 0`):** 根元 `yB` = 幹の `BaseOffset` の位置の高さ (距離で測る)、上端 `yT` = 幹の根元 + 幹の名目長、`h = (y - yB) / (yT - yB)`、`R(h) = PruneWidth x (yT - yB) x max(h < PruneWidthPeak ? (h / Peak)^PrunePowerHigh : ((1 - h) / (1 - Peak))^PrunePowerLow, 0.25)` (`PruneWidthPeak` が最大半径の高さ、`PrunePowerHigh` が山より下、`PrunePowerLow` が山より上の形。形は最大半径の 0.25 倍 (`PruneEnvelopeFloor`) を下回らず、h は 0..1 に飽和、`-0.12 .. 1.12` (`PruneEnvelopeOvershoot`) の外は半径 0)。軸は幹の根元を通る鉛直線。予測した先端 `原点 + L x (向きを鉛直面内で (Curve + CurveBack) / 2 曲げたもの)` が包絡の外なら L を 8 回二分して交点 L' を求め、`長さ = max(0.15 L, lerp(L, L', PruneRatio))`、`TipScale = 長さ / L`。枝は消さず短くするだけ (切り株も子を持つ)。幹のクローン (Level 0) は刈らない (主軸の先端は 2.3)。
- **樹冠の天井 / 小枝の切り詰め (骨格パス、polish3):** 刈り込みが有効なとき、Level 1 以上の切られていない片で経路を作った後に:
  - **天井:** 実際の経路の最も高い点が `yB + (1 + 0.9 x 0.12) (yT - yB)` を越えたら、先端の高さがちょうど天井になるまでセグメント長を一様に縮めて (0.15 倍まで) 同じ角度の予定で作り直す。予測した先端で刈る `PruneLength` の取りこぼし (曲がり / 揺れ / 屈曲で上へ反った枝) を拾い、樹冠の頂が平らな丸屋根になる。
  - **小枝の切り詰め:** 最終階層とその 1 つ上 (側枝) の先端が葉の間引きの範囲 `1.05 R(h) + 0.5 / PruneRatio - 0.2 m` の外なら、先端が範囲に入る倍率 (8 回の二分、根元も外なら 0.15) で一様に縮めて作り直す。葉は後で長さ比で付くので短くなった枝に沿い、枝先のカードは新しい先端に付く (包絡の上で葉を全て間引かれた裸の鞭が突き出ない)。
  - どちらも 1 セグメントを 1.2 cm (`MinTrimSegment`) より短くはしない。判定は接合部の沈み込み (2.5) の前の位置で、骨格と包絡だけで決まる (葉のパラメータは枝を動かさない)。
- **半径:** `clamp(親の半径 x (刈り込み前の子の長さ / 親の名目長)^RatioPower, lo, 親の半径 x min(Radius, 0.9))` (`Radius` は上限、刈り込みは先端の Taper だけを細らせる)。`lo` は Level 1 (主枝) が `min(max(MinTipRadius, 0.45 x 付け根の親の半径), 上限)` (`LimbMinRadiusRatio`。樹冠の中に太い骨格の枝が見える)、それ以外は `MinTipRadius`。親の `ChildAttach` に (t, 半径) を記録する (パイプモデル用)。

### 2.5 半径の確定 (`AssignRadii`、出力パス)

親が先に並ぶ `m_Stems` の順に:

1. 子の根元半径を `0.85 x 親の確定半径 (付け根)` までに抑える (先端も同じ比)。クローンは `親の片の確定した先端半径 x CloneRadiusRatio` (= 断面の取り分^(1 / PipeExponent)、`MinStemRadius` を下回らない)。
2. **パイプモデル** (`PipeExponent n > 0`、子を持つ片と切られた片): `r_pipe(f)^n = Tip^n + (Base^n - Tip^n) x Σ_c w_c(f) r_c^n / Σ_c r_c^n`、`w_c = 1 - SmoothStep(hi - 0.6 / seg, hi, f)`、`hi = min(t_c + 0.5 / seg, 1)` (子の断面は付け根の直後で抜ける)。根元 = Base、先端 = Tip を保ったまま余剰の断面を子の比で配るので、子が細くても太くても根元 / 先端の半径は変わらず、子を出すたびに段階的に細る。`r(f) = lerp(テーパ曲線, r_pipe(f), 0.75)`。子の無い切られた片 (幹の根元の片) は `r_pipe = Base` を 0.4 (`CutPieceHold`) だけ混ぜる (幹が分岐の手前で約 15 % 細る)。先端は `min(MinTipRadius, Base)` を下回らない。`n = 2` でダ・ヴィンチ則、0 で無効 (テーパ曲線のまま)。
3. **接合部の沈み込み** (`JunctionSink > 0`): 子の根元を親の軸から `radialDir x d`、`d = max(0, R - JunctionSink x rc - rc |cos down|)`、さらに根元リング (襟込み) の全頂点が親の軸から `0.98 R` 以内に収まる量に抑える (`MaxSinkInside`、32 方位)。骨格は親の軸上に作ってあるので子孫ごと平行移動する (オフセットは祖先の分を累積)。
4. **襟の実効倍率** `Collar = min(JunctionCollar, 0.95 R / rc - 1)` (根元リングが親に収まる範囲)。4 mm 未満の細い小枝には襟を出さないが、沈み込みの判定は襟付き半径で行う (小枝セクションへの振り分けを変えても位置はビット単位で同じ)。
5. **樹皮 UV のタイル:** クローン以外は `tilesAround = max(1, round(2π Base x BarkUVTilesPerMeter x BarkUVScale))`、`vScale = tilesAround / (2π Base)`、V のオフセット `LatticeHash(枝の Seed, 5) / vScale` (タイル 1 枚分の中の決定的な位置)。分岐のクローンは親の片のタイル数 / V 密度を継ぎ、V は親の片の先端 (`親の長さ - 0.3 r + 親のオフセット`) から続ける (polish3: 樹皮の溝が分岐を通って続き、本数と位相が跳ばない。細い片では溝が少し詰まる)。

### 2.6 樹皮チューブ (`EmitBark`、閉じた面)

- **リング列** = 骨格点 + 挿入リングを距離順に: 幹の最初の片の根元の張り出し (`TrunkFlare > 0`、`0.03 / 0.06 / 0.10 x 幹長`)、襟 (`min(0.6 rc, 最初のセグメントの半分)`)。半径倍率は 張り出し `1 + TrunkFlare x (1 - s / (TrunkFlareHeight x 幹長))^2` x 襟 `1 + Collar x (1 - s / (2.5 rc))^2` (切られた片は先端までに襟を消す)。リングは `RadialSegments + 1` 頂点 (継ぎ目を複製)、法線は半径方向 + 先細りの傾き (隣のリングとの差分 `-dr/ds`。4 mm 未満の小枝は補正なし、周は `min(RadialSegments, 4)` 分割)。
- **先端:** `TipRound > 0` なら半球 (35 / 70 deg のリング + 極、高さ `TipRound x r_tip`。針のように光る先端を作らない)、0 なら平らな扇。**根元:** 法線 -T の平らな円盤 (側面の頂点は共有せず複製: 襟で傾いた側面法線と食い違わないように)。親の中に埋まる。
- **UV (正方テクセル):** U = 周方向 x tilesAround、`V = (距離 + UVOffsetV) x vScale` (2.5 の 5)。接線 (+U) は周方向。
- **頂点カラー (決定的):** 明度 = 幹は接地ランプ 0.72 → 1 (元の幹の根元からの距離、クローンにも続く) x 根元 0.3 m の接地の陰 0.82 → 1 / 子は `lerp(親の付け根の明度 x 0.75, 1, smoothstep(0, 3 rc, s))` (継ぎ目を隠す) / クローンは切った先端の明度、x 子の付け根まわりの暗化 (1 rc 以内 0.9 → 4 rc で 1、クローンは除く)、x 樹冠内の暗化 `lerp(BarkAOInner, 1, 露出度)` (樹冠の箱の中だけ。幹は最も低い葉の中心より下では暗くしない: `SmoothStep(minY - 0.1, minY + 0.3, y)` でなじませる)、x 空からの光 0.88 (下面) .. 1 (上面) (`BarkMoss > 0 || BarkAOInner < 1` のときだけ)。苔 = 上向きの面 `BarkMoss x smoothstep(0.3, 0.9, N.y) x smoothstep(0.45, 0.70, まだら)` + 根元 `min(1.6 BarkMoss, 1) x smoothstep(到達高さ, 0, y) x 方位 x smoothstep(0.35, 0.6, まだら)` (到達高さ = 1.1 m x lerp(0.55, 1, まだら)、方位 = `smoothstep(-0.25, 0.75, dot(N, 苔の側))`、苔の側は木ごとの水平方位 `LatticeHash(0, 78)`)。苔は R を 0.68 倍、B を 0.32 倍へ (G = 明度はそのまま、黄緑)。まだら = `FBM3D(P x 1.5)`。小枝 (セクション 3) は法線を `0.3 x LeafNormalBlend` だけ樹冠の外向き (`CrownDir`) へ寄せる (樹冠の中で葉と一緒に陰影が付く。半径の 4 倍より短い切り株は寄せない)。小枝の位置の項 (露出度 / まだら / 高さ) は端と 3 本おきのリングで評価して補間する (生成時間の節約)。
- **閉じている保証:** 位置で溶接すると全ての辺がちょうど 2 回使われる (半球 / 襟 / 張り出しリング / 埋まった円盤を含む)。親に埋め込んだ子の根元やクローンの円盤は SDF の偶奇判定で小さな「外」のポケットを作るが、64³ では影響は小さい (v1 と同じ)。

### 2.7 葉 / 房カードの配置

骨格パス `CollectLeaves` / `CollectClusters` は配置パラメータ (位置 t、方位 Phi、角度、捻り、大きさ) だけ決め、出力パス `ResolveLeafSpecs` が確定した半径で原点 / 向きを解く (骨格パスの仮の半径で焼き込むと、パイプモデルで太った枝に葉柄が埋まる)。

- **最終階層:** `LeavesPerBranch` 枚を `t >= 0.1` に。**その 1 つ上の階層 (側枝の胴):** `LeavesOnParentLevel` 枚を `t >= 0.35` (`ParentLeafTMin`) に (葉柄は一律 2 cm、裸枝にしない) + `ClustersPerTwig > 0` なら房 2 枚 (`ClustersOnParentLevel`) を `t >= 0.55` に (枝先の房は付けない)。**それより上の階層:** 切られていない片の先端 (主軸 / 主枝の先端) に枝先の葉 `LeafTerminalCount` 枚 (親階層の規則) + 枝先の房 `TerminalClusters` 枚 (`bTipBouquet`。主軸の先端が裸で終わらない)。位置 `t = tMin + (1 - tMin) u^(1 / (1 + LeafTipBias))`、`u = (j + 0.5 + Signed(0.3)) / Count`。最後の `LeafTerminalCount` 枚は枝先 (`t = 1`) に接線寄り (`lerp(T, radialDir, 0.35)`) の扇状で 1.15 倍。他は `lerp(1, 0.8, t^2)` 倍 (枝先ほど若く小さい)。
- **枚数:** 親の付け根側の枝ほど少なく `nominal = round(Count x (0.5 + 0.5 AttachT))`、付け根側のスロットから間引き、枝先の葉は常に残す。房と混ぜるとき (`ClustersPerTwig > 0`、最終階層) は枝先でない単葉を `round(nominal x 0.85)` (`LeafMixFactor`) 枚に減らす。`U_bare < LeafBareTwigFraction` の小枝 (最終階層のみ) は単葉も房も 1 枚も出さない (乱数は引く)。
- **方位 / 向き:** 黄金角の螺旋 (`137.5 ± 40 deg`、開始位相は `LatticeHash`)、基準面 `planeN` (2.4 と同じ、空寄り)。原点 = 枝の表面 + 葉柄 `min(LeafPetioleLength, max(1 cm, 2 r))` (r は出力される樹皮の半径。隙間はテクスチャの葉柄が覆う)。向き = 接線から `LeafAngle ± LeafAngleVariance`。葉面はおおむね上向きにしてから葉軸まわりに ±35 deg 捻る (出力パスで樹冠の外向きに反転することがある。2.8)。
- **房カード** (`CollectClusters`、単葉の後に同じストリームで 5 回 / 枚): 途中の `ClustersPerTwig` 枚は `t = clamp(TMin + (1 - TMin) (k + 0.5 ± 0.2) / n, TMin, 1)` (小枝は `TMin 0.12` (`ClusterTMin`)、側枝の胴は 0.55)、角度 `LeafAngle x 0.8`、同じ螺旋、原点は枝の表面 (葉柄なし。アトラスの小枝が付け根を覆う)、大きさ `ClusterSize x (1 ± LeafSizeVariance)`、幅 = 大きさ x `ClusterAspect`、セルは `LatticeHash`。枝先の `TerminalClusters` 枚 (切られていない片だけ、小枝は `LatticeHash(枝 ID, 31)` で 6 割 (`TerminalClusterKeep`) だけ。上位階層の枝先の房は常に) は枝の軸の先端から `normalize(lerp(T, up, 0.3))` の向き、j 枚目の +U は RMF 法線を接線まわりに `j x 90 deg` 回したもの。`ClustersPerTwig 0` で房は枝先 / 側枝の分も含めて全て無効。
- **房の大きさ / 向きのばらつき (出力パス、`LatticeHash`):** 大きさに 運ぶ片の長さの倍率 `lerp(0.4, 1, SmoothStep(4 cm, 25 cm, 長さ))` (数 cm の小枝に 30 cm の房を付けない) x Level 1 の枝ごと `1 ± 0.30` x 小枝ごと `1 ± 0.20` を掛ける (輪郭の房の大きさが枝ごとに違う)。枝の途中の房は付け根を中心に面内で ±25 deg 回す。半分の房は表裏を入れ替えて貼る (`bMirror`: アトラスの絵が左右反転する。+U / 法線を反転するので接線の規約と巻き順はそのまま)。
- **包絡による間引き:** `PruneRatio > 0` のとき、カードの中心が `1.1 R(h) + 0.5 m / PruneRatio` (`LeafCullMargin`) の外なら出力しない (`bPruned`、樹冠の統計にも入れない。頂点も出さない)。余裕は刈り込みが残すはみ出し (1 - PruneRatio) とカード 1 枚分。

### 2.8 葉の陰影 (`ComputeCrownStats` / `EmitLeaves`、頂点の法線と色)

- **樹冠の統計:** 中心 C = 葉面積で重み付けしたカード中心の平均 (房の実効面積 = `Size^2 x ClusterAspect x 0.6`)、軸ごとの半径 `R = max(1.15 x max|中心 - C|, 0.3 m)`。24³ の葉密度グリッド (範囲 C ± R): セルの光学的厚さ `tau = LeafAOFalloff x 葉面積 / セル断面`。露出度 `Exposure(P) = exp(-Σ tau / 6)`: 真上 + 仰角 50 deg x 方位 72 deg 刻み 5 本、各 1 セルずつ 5 歩。葉が無ければ幹の先端に 1 m の箱。
- **法線:** `nSph = CrownDir(中心)` (楕円体の外向き)。`dot(FaceN, nSph) < 0` なら FaceN と Side を反転 (+U 側が外)。`nBlend = normalize(lerp(FaceN, nSph, LeafNormalBlend) + up x LeafNormalUpBias)` を面法線との内積 >= 0.12 に抑える (巻き順の保証。`LeafNormalBlend` の上限 0.85)。葉先側の 2 頂点は Side まわりに `LeafDroop ± min(15, LeafDroop) (+20 枯れ葉)` deg 倒す (垂れ)、左右の列は Dir まわりに `∓LeafCup` deg 内側へ (皿状)。ジオメトリは平面のまま。接線は Side を各頂点の法線に直交化したもの。
- **頂点カラー:** `ao = clamp(lerp(LeafAOInner, 1, 露出度) x lerp(LeafAOBottom, 1, 0.5 + 0.5 nSph.y), 0.45, 1)` (樹冠の塊が黒い切り絵にならない下限)。カードごとの値は `u0 = 0.5 U0 + 0.5 x 空間ノイズ` (0.6 m 周期の FBM。隣の葉が明暗に飛ばず、色が樹冠の上をなだらかに移る)。色相: `yellow = saturate(0.5 露出度 + 0.5 u0 - 0.25 + 枝オフセット)`、`blue = saturate(0.5 (1 - 露出度) + 0.5 (1 - u0) - 0.25 - 枝オフセット)`、枝オフセット = `LeafBranchVariance x (hash(AncestorL1) - 0.5)` (同じ Level 1 の枝の葉は似た色)。`色 = LeafTint x ao x (1 + LeafTintVariance (u0 - 0.5)) x lerp(1, (1.10, 1.02, 0.70), LeafHueVariance x yellow) x lerp(1, (0.80, 0.98, 1.05), LeafHueVariance x blue)`、G にさらに `lerp(1, lerp(0.94, 1.04, 樹冠内の高さ), LeafHueVariance)`。枯れ葉 (`U2 < LeafDeadFraction`、単葉のみ): `LeafTint x ao x (0.62, 0.40, 0.16)`、大きさ 0.7 倍。1 を超える成分は全体を割って収め (Lumen カードの RGBA8 アルベドと一致させる)、各成分 0.1 以上。α = `clamp(1 - LeafErosion x hash, 0.7, 1)` (マスク = テクスチャ α x 頂点 α、カバレッジ保存ミップの前提 >= 0.7)。
- **UV:** 単葉はテクスチャ全面、房は 2x2 アトラスのセル `((c & 1), (c >> 1)) x 0.5` から `AtlasCellInset` (1/64) を除いた範囲 (`uv = セル原点 + inset + (u, v) x (0.5 - 2 inset)`、ベイカと共有)。どちらも V = 0 が葉先 / 房の先、V = 1 が付け根。

### 2.9 プリセットと既定値

コンストラクタ = **Oak 5 m** (約 5 m の開けた場所のオーク、4 階層: 4 deg 傾いた幹が約 2 m で非対称に二又、主枝 7 本は太く少数で段ごとに群れ、下の主枝は水平に張り出す、樹冠は幅約 6 m の平たい丸屋根)。グローバル値はヘッダのメンバ初期化子 (名目値) をコンストラクタが Oak 用に上書きする。`MakePreset(Sapling)` = v1 の既定値 (3 階層、14 枝、葉 0.22 m) で v2 のパラメータを全て中立にしたもの (ただし 2.2 の屈曲 / 2.4 の枠の揺れ・段・主枝の半径の下限はパラメータでない規則なので Sapling にも効く。葉 928 枚、v1 は 1176)。

| | Oak L0 (幹) | Oak L1 (主枝) | Oak L2 (側枝) | Oak L3 (小枝) | Sapling L0 / L1 / L2 / (L3) |
|---|---|---|---|---|---|
| Branches | - | 7 | 12 | 5 | - / 14 / 7 / (4) |
| Length / Variance | 4.7 m / 0.05 | 0.7 / 0.40 | 0.5 / 0.2 | 0.55 / 0.25 | 4.0 m / 0 ・ 0.55 / 0.15 ・ 0.5 / 0.2 ・ (0.5 / 0.2) |
| LengthShape | 1 | 1 (CrownShape が決める) | 1 | 0.8 | 1 / 0.45 / 0.7 / (0.8) |
| Radius | 0.17 m | 0.8 (上限) | 0.7 | 0.8 | 0.16 m / 0.6 / 0.55 / (0.55) |
| RatioPower | 1.0 | 0.5 (+ 下限 0.45 x 親) | 1.3 | 1.3 | 1.0 |
| Taper | 0.16 | 0.15 | 0.2 | 0.3 | 0.55 / 0.3 / 0.25 / (0.2) |
| TipRound | 0.8 | 0.8 | 0.8 | 0.8 | 0 (平らな扇) |
| DownAngle ± / Shape | 4 (= 幹の傾き) | 70 ± 25 / 40 | 45 ± 25 / 10 | 40 ± 20 / 10 | 0 (鉛直) ・ 60 ± 12 / 50 ± 15 / (45 ± 20)、Shape 0 |
| Rotate ± / Alternate | - | 137.5 ± 45 / 螺旋 | 137.5 ± 30 / 螺旋 | 60 ± 45 / 交互 | 137.5 ± 25 / 30 / (40)、螺旋 |
| TipBias | 0 | 0 | 0 | 0.25 | 0 |
| Curve / CurveBack / Variance | 0 / 0 / 40 | 25 / -25 / 45 | 10 / -15 / 30 | 12 / -25 / 55 | 0 / 0 / 12 ・ 20 / 0 / 35 ・ 10 / 0 / 50 ・ (0 / 0 / 60) |
| 屈曲 (2.2、CurveVariance から) | 分岐より上 ±12 | ±13.5 | ±9 | - | - / ±10.5 / ±15 |
| Tropism [deg] | 5 | 10 | 4 | -15 | 11.46 (= v1 の 0.2 を角度に) |
| BaseOffset | 0.34 | 0.10 | 0.30 (主枝の外側 70 %) | 0.10 | 0.35 / 0.15 / 0.1 / (0.1) |
| Segments / Radial | 12 / 16 | 8 / 9 | 6 / 6 | 4 / 5 | 10 / 14 ・ 7 / 8 ・ 4 / 6 ・ (3 / 5) |
| SegSplits / SplitAngle ± | 0.10 / 40 ± 15 | 0.10 / 35 ± 10 | 0 / 35 ± 10 | 0 / 30 ± 10 | 0 / 30 ± 8 |
| BarkUVScale | 1 | 1.5 | 3 | 4 | 1 |

全体 (Oak): `Seed 1`、`Levels 4`、`TropismScale 1`、`BarkUVTilesPerMeter 2`、`BaseSplits 1`、樹冠 `Hemispherical` / `PruneRatio 0.5` / `PruneWidth 0.8` / `PruneWidthPeak 0.62` / `PrunePowerLow 0.7` / `PrunePowerHigh 0.3`、幹 `TrunkFlare 0.6` / `TrunkFlareHeight 0.10` / `GroundOffset 0` / `PipeExponent 2` / `JunctionSink 0.6` / `JunctionCollar 0.35` / `TwigRadiusThreshold 0.022` / `BarkAOInner 0.85` / `BarkMoss 0.55`、葉 `LeavesPerBranch 6` / `LeavesOnParentLevel 4` / `LeafSize 0.10` ± 0.25 / `LeafAspect 0.65` / `LeafAngle 40` ± 20 / `LeafTipBias 1.0` / `LeafTerminalCount 1` / `LeafBareTwigFraction 0.08` / `LeafPetioleLength 0.015` / `LeafDroop 20` / `LeafCup 12`、房 `ClustersPerTwig 5` / `TerminalClusters 1` / `ClusterSize 0.30` / `ClusterAspect 1.25`、陰影 `LeafNormalBlend 0.7` / `LeafNormalUpBias 0.15` / `LeafAOInner 0.65` / `LeafAOBottom 0.8` / `LeafAOFalloff 0.6` / `LeafHueVariance 0.4` / `LeafBranchVariance 0.3` / `LeafDeadFraction 0.02` / `LeafTintVariance 0.15` / `LeafErosion 0.25` / `LeafTint (1, 1, 1)`。
Sapling の全体値: `Levels 3`、`BarkUVTilesPerMeter 1`、`BaseSplits 0`、`Cylindrical` / `PruneRatio 0`、`TrunkFlare 0` / `PipeExponent 0` / `JunctionSink 0` / `JunctionCollar 0` / `TwigRadiusThreshold 0` / `BarkAOInner 1` / `BarkMoss 0`、葉 12 枚 / `LeafSize 0.22` ± 0.3 / `LeafAspect 0.6` / `LeafAngle 50` ± 20、`LeavesOnParentLevel 0` / `LeafTipBias 0` / `LeafTerminalCount 0` / `LeafBareTwigFraction 0` / `LeafPetioleLength 0` / `LeafDroop 0` / `LeafCup 0`、`ClustersPerTwig 0` / `TerminalClusters 0`、`LeafNormalBlend 0` / `LeafNormalUpBias 0` / `LeafAOInner 1` / `LeafAOBottom 1` / `LeafHueVariance 0` / `LeafBranchVariance 0` / `LeafDeadFraction 0` / `LeafTintVariance 0.25` / `LeafErosion 0`。

**既定値での出力 (ハーネス計測、Seed 1、polish3):** Oak = 片 532 本 (小枝セクション 501、クローン 20。階層ごと 7 / 21 / 84 / 420)、単葉 2306 枚、房 2352 枚 (カード 4658、`EstimateCards` 5600)、頂点 45842、三角形 47498 (樹皮 4522 / 単葉 4612 / 房 4704 / 小枝 33660)、SDF 三角形 4522、`EstimateStems` 532、`GenerateMs` 最小 8.7〜9.9 ms (平均は負荷で約 11 ms)。幹: 根元半径 0.17 m (張り出し込み 0.27 m)、二又は y 1.96 m (幹の先端 0.132 m、主幹 0.099 / 0.086 m、接線から 8.4 / 33.6 deg)、主枝は幹の片に 1 本 + 主幹に 6 本。カードの頂点は y 1.12〜5.05 m、幹から最も遠い頂点は 3.20 m。Sapling = 113 本、葉 928 枚、三角形 9836、全て樹皮セクション。

**`Sanitize()` の範囲:** `Levels 1..4`、`TropismScale -5..5`、`BarkUVTilesPerMeter 0.01..100`、`BaseSplits 0..3`、`PruneRatio 0..1`、`PruneWidth 0.1..1.5`、`PruneWidthPeak 0.05..0.95`、`PrunePower* 0.1..3`、`TrunkFlare 0..2`、`TrunkFlareHeight 0.02..0.6`、`GroundOffset 0..0.5`、`PipeExponent 0..3` (0 < n < 1 は 1 に)、`JunctionSink 0..0.9`、`JunctionCollar 0..1`、`TwigRadiusThreshold 0..0.1`、`BarkAOInner` / `BarkMoss 0..1`、`LeavesPerBranch 0..64`、`LeavesOnParentLevel 0..32`、`LeafSize 0.01..5`、`LeafSizeVariance 0..0.95`、`LeafAspect 0.1..3`、`LeafAngle 0..180`、`LeafAngleVariance 0..90`、`LeafTipBias 0..4`、`LeafTerminalCount 0..min(4, LeavesPerBranch)`、`LeafBareTwigFraction 0..0.5`、`LeafPetioleLength 0..0.1`、`LeafDroop 0..55`、`LeafCup 0..40`、`ClustersPerTwig 0..8`、`TerminalClusters 0..3`、`ClusterSize 0.05..2`、`ClusterAspect 0.3..3`、`LeafNormalBlend 0..0.85`、`LeafNormalUpBias 0..1`、`LeafAOInner` / `LeafAOBottom 0.1..1`、`LeafAOFalloff 0..3`、`LeafHueVariance` / `LeafBranchVariance` / `LeafTintVariance 0..1`、`LeafDeadFraction 0..0.3`、`LeafErosion 0..0.3`、`LeafTint 0..2` / 成分。階層: `Branches 0..64`、`Length 0.01..200 (L0) / 3`、`LengthVariance 0..0.95`、`LengthShape 0.05..2`、`Radius 0.0015..20 m (L0) / 0.05..1`、`Taper 0..1`、`DownAngle 0..180`、`DownAngleVariance 0..90`、`Curve` / `CurveBack` / `Rotate ±360`、`RotateVariance 0..180`、`CurveVariance 0..360`、`BaseOffset 0..0.95`、`Segments 1..32`、`RadialSegments 3..32`、`Tropism ±90`、`DownAngleShape ±60`、`TipBias 0..3`、`RatioPower 0.5..1.5`、`TipRound 0..1`、`SegSplits 0..1`、`SplitAngle 8..90`、`SplitAngleVariance 0..45`、`BarkUVScale 0.1..10`。その後 0.10 の予算削減 (枝数 → カード → 三角形)。

---

## 3. テクスチャ (`FProceduralTreeTextures`、v2)

| ファイル (`Saved/ProceduralTexture/`) | 内容 |
|---|---|
| `TreeBark_diff_v2.dds` (1024², 11 mips, sRGB, 両方向タイル) | 樹皮 v6 = 溝の網 (`FFurrowNetwork`、格子なし。v4 の千鳥格子の Worley は蛇皮 / パイナップルの網、v5 の細い溝は遠目で髪の毛の線になった)。タイルあたり `BarkFurrowsU 8` 本の縦の溝 (畝の平均幅 128 テクセル。Oak の幹は周 2 タイルで 16 本、主枝は 1 タイルで 8 本)。溝 k の中心線 = k + 0.5 + ジッタ ±0.15 + 4 オクターブの 1D FBM の蛇行 (±0.3、512 行の表)、幅の揺らぎ ±0.05。35 % の溝は V の一区間で長い先細りで途切れる (2 本の畝が合流)。畝 (溝の間) ごとに 0〜3 本 (10 / 30 / 40 / 20 %) の斜めの連絡溝 (V の長さ 0.25〜0.42 タイル ≈ 20〜35 deg、両端は溝に接線方向で合流 = 浅い Y、楔の高さは揃わない。途切れた溝に当たる連絡溝は使わない)。畝の個体値 (高さ 0.55..1.0 / 明るさ / 薄片 / 地衣 / 暖色 / 傾き / 横の割れ目の位相) は「溝の間 x 連絡溝で区切られた何番目か」で引き、消えかけの溝の両側では隣の畝と混ぜる (段差を出さない)。断面: 平らな底 (`BarkFurrowFloor 0.18`) + V 字の壁 (`BarkFurrowWall 0.35`、溝の幅 ≈ 間隔の 0.53) + 丸い天辺 (`BarkCrest 0.25`)、縁はぎざぎざ、2 スケールのドメインワープ。天辺は V 字の横の割れ目 (タイルあたり平均 8 行、畝ごとの位相、4 割は無い) で段々のブロックに割り、欠け / 細かな穴 / 縦長の薄片 (54 x 15) / 縦の繊維。色: 天辺は風化した灰茶 (≈ 0.41 / 0.35 / 0.27)、壁は赤みのある茶、底は焦げ茶 (≈ 0.25 / 0.20 / 0.13、底の中心の細い割れ目はさらに暗い)。焦げ茶は `1 - SmoothStep(0.05, 0.95, ridge)` で壁全体にわたってなだらかに入る。天辺 : 底 ≈ 3 : 1 (線形輝度、日の当たらない幹でも溝が読める)。地衣 (灰緑の痂、溝の壁と一部の畝の天辺) と苔 (くすんだオリーブ、溝の底) は控えめ (G - R <= 12/255) |
| `TreeBark_nor_dx_v2.dds` | 法線用の高さでは溝の断面だけ `BarkFurrowRelief 4` 倍 (細部は等倍)、半径 1 でぼかしてからラップ差分 `normalize(-dh/du, -dh/dv, 1)`、強さ 7。各畝に光の当たる壁と影の壁ができ、ミップ 3 でも残る。既存アセットの `nor_dx` と同じ規約 (B = cross(N, T) = +V) |
| `TreeBark_arm_v2.dds` | R = AO = `lerp(BarkAOFloor 0.80, 1, SmoothStep(0, 0.9, ridge))` x キャビティ (半径 6 / 48 テクセルでぼかした高さとの差、0.85..1) (最小 ≈ 0.68) / G = ラフネス 溝 0.95 → 畝 0.80、地衣 0.88 / B = 0 |
| `TreeLeaf_diff_v2.dds` (512², 10 mips, sRGB) | `EvalLeaf` (`FLeafVariant`): 樫 (Quercus robur 型) の葉。倒卵形 (最大幅は葉先寄り、付け根へ細る。葉身 v 0..0.90) の半幅に `LobeCount 4.5` 周期の丸い裂片 (`lobe = 1 - (0.5 - 0.5 cos(...))^1.4`: 裂片は広く丸く、湾入は狭いが底は丸い。山は周期の 0.38 = 外かつ葉先向き。湾入の深さ `LobeDepth 0.42` x 裂片ごとに 0.75..1.15、葉先と付け根で浅い。左右で約 1/4 周期ずらす) = 片側 3〜4 個 + 葉柄 (v 0.88..1、半幅 0.016)。主脈 + 裂片ごとに 1 本の側脈 (片側最大 6、各裂片の先へ向けて葉先側へ曲がる `VeinCurve 0.20`)。色: 葉肉 (0.27, 0.44, 0.13)〜(0.36, 0.52, 0.17) をドーム x fBm で、黄色いまだら、脈は明るく、縁は黄緑の帯 + 茶の縁、葉柄は茶。α = 輪郭への正確な 2D 符号付き距離からのランプ (8 テクセル)、α 0 の下も葉の色 (縁の灰色のフリンジを出さない) |
| `TreeLeaf_nor_dx_v2.dds` | ハイトフィールド (ドーム 0.30 - 主脈 0.55 - 側脈 0.30 + 脈間の盛り上がり 0.12 + fBm 0.05 - 縁の落ち 0.35) の差分、近傍はクランプ (ラップしない)、強さ 3 (`LeafNormalStrength`。強いとカードの照り返しがぎらつく)。α < 0.05 のテクセルは (0.5, 0.5, 1) |
| `TreeLeaf_arm_v2.dds` | R = AO (脈 / 葉柄で暗い、>= 0.05) / G = ラフネス 葉身 0.55 ・ 脈 0.65 ・ 縁 3 テクセルと葉柄 0.70 (カードが銀色に光らない) / B = 0 |
| `TreeCluster_diff_v2.dds` (1024², 11 mips, 2x2 セル) | 房アトラス。セル c (512²、原点 `((c & 1), (c >> 1)) x 512`) に下中央から上へ伸びる仮想の小枝 (長さ 0.5 セル、±6 deg 傾く、3 テクセル幅の茶) と `5 + (c & 1)` 枚の樫の葉を左右交互に、根元 55 deg → 先端 18 deg (±8) の回転、長さ 0.42〜0.52 セル、縦横比 0.65、色相 0.92〜1.08 と下の層ほど暗い明度で `EvalLeaf` を逆写像して合成 (画家のアルゴリズム: α > 0.5 で上書き、他は α で lerp、α は max)。縁 24 テクセルは透明なガター (葉はランプ込みでその内側)。種は `7000 + c` で `Tree.Seed` と無関係 |
| `TreeCluster_nor_dx_v2.dds` | 所有インスタンスの高さ場 (同じインスタンスのテクセルだけで差分、重なりの境目に段差を出さない) x 強さ 3 + インスタンスごとの傾き (接線空間で外向き 0.15〜0.35 / 上下 -0.20〜0.35 = 房カードを立体に見せる偽の傾き)。ガター / α < 0.05 は平坦 |
| `TreeCluster_arm_v2.dds` | R = 葉の AO x 付け根 0.85 → 先端 1 x 層 0.88..1 (>= 0.10) / G = 葉身 0.55 ・ 脈 0.65 ・ 縁と葉柄 0.70 ・ 小枝 0.65 / B = 0。ガター (1, 0.55, 0) |
| `TreeTwig_diff_v2.dds` (256², 9 mips, 両方向タイル) | 若い小枝の樹皮: 灰茶〜くすんだオリーブ茶 (0.27, 0.23, 0.17)〜(0.33, 0.30, 0.22) の低周波むら (幹の天辺よりやや暗い: 明るい棒に見えない) + 縦の筋と繊維 (リッジ 24 x 4、色と高さの両方)。皮目 (6 x 8 格子の Worley F1 を異方距離 (0.4, 1.0) で、半分のセルは無し、約 36 x 11 テクセルの横長) を (0.60, 0.54, 0.43) へ明るく盛り上げ、周りを 10 % 暗く |
| `TreeTwig_nor_dx_v2.dds` | 高さ場 (0.5 + 0.15 皮目 - 0.04 縁 + 0.12 繊維 + 微細) のラップ差分、強さ 5 |
| `TreeTwig_arm_v2.dds` | R = AO 0.9..1 / G = ラフネス 0.55 (皮目)..0.65 / B = 0 |
| `TreeFlat_nor_dx_v2.dds` (4², 3 mips) | 平坦法線 (生成に失敗したときの代替) |

- **DDS:** DX10 ヘッダ (`DXGI_FORMAT_R8G8B8A8_UNORM`) + 2x2 ボックスフィルタのミップチェーン。BaseColor は既存の `DDS_LOADER_FORCE_SRGB` 経路で sRGB になる。ファイル名の `_v2` はヘッダの `Version` (固定) から来るので、絵柄を変えたら `Saved/ProceduralTexture/Tree*_v2.dds` を消して作り直させる。
- **α のランプ (葉 / 房):** `α = saturate(LeafCoverageClip - dEdge / (RampTexels x テクセル))` (`dEdge` = 輪郭への符号付き距離、内側が負。`RampTexels` = 512² 以上で 8、未満で 4)。`α = 0.3333` の等値線がちょうど輪郭に乗るので、シルエットは作った形のまま。
- **カバレッジ保存ミップ (`WriteDDS(..., PreserveCoverageClip, CellsX, CellsY)`、葉 / 房の diff だけ):** ボックスフィルタだけだと遠いミップほど `α >= clip` のテクセルが減って葉がやせる (遠景で樹冠が透け、遠い CSM カスケードで葉の影が消える)。セル単位 (アトラスはセルごと) に mip 0 のカバレッジ `c0` (α >= 85 のテクセル比) を取り、各ミップの α に倍率 s ([0.25, 8] を 12 回二分探索) を掛けてカバレッジを `c0` に最も近づける。セルが 4x4 以下になったら直前の倍率を使い回す。RGB は触らず、ミップ連鎖は倍率を掛ける前の α から作る。マスクは `テクスチャ α x 頂点 α >= OpacityMaskClipValue (0.3333)` なので、頂点 α (`LeafErosion`、>= 0.7) と Details の clip 値を動かすと遠景の葉の太さが変わる。
- **注意:** TwoSided の裏面では N だけ反転し T は反転しないので、V 方向の凹凸 (脈、房の傾き) は裏面で逆に見える (強さ 3 / 傾き <= 0.35 に抑えて許容)。ARM の G が 0 でないので、葉 / 樹皮スロットの Roughness スライダーはフォールバックとしてしか効かない。`Material.Specular` は照明で使われない。

---

## 4. Details / INI / マテリアル

### 4.1 Details (`DrawProceduralTreeSection`、アクター選択時に表示)

- 先頭に統計 `Stems %u (twig %u)  Leaves %u  Clusters %u  Tris %u (DF %u)  Gen %.1f ms  Rebuild %.1f ms` (+ `(rebuilding...)`)。`Seed` (+ `Random`)、`Preset` コンボ (`Oak 5 m` / `Sapling (v1)`: Seed 以外を `MakePreset` に置き換えて再生成。現在値とハッシュが一致するプリセット名を表示し、どれとも違えば `Custom`)、`Rebuild` (強制再生成)、`Reset Tree Materials` (`ApplyDefaultMaterials`)。
- グループ: **Crown** (Levels、Crown Shape コンボ、Prune Ratio / Width / Width Peak / Power Low / Power High) → **Trunk & Bark** (Trunk Flare / Height、Ground Offset、Pipe Exponent、Junction Sink / Collar、Twig Radius Threshold、Bark UV Tiles / m、Bark AO Inner、Bark Moss、Tropism Scale、Base Splits) → **Leaves** (Generate Leaves、Leaves / Branch、Leaves On Parent Level、Leaf Size / Variance / Aspect / Angle / Angle Variance、Leaf Tip Bias、Leaf Terminal Count、Bare Twig Fraction、Petiole Length、Leaf Droop、Leaf Cup) → **Clusters** (Clusters / Twig、Terminal Clusters、Cluster Size / Aspect) → **Leaf Shading** (Normal Blend / Up Bias、AO Inner / Bottom / Falloff、Hue / Branch / Dead / Tint Variance、Erosion、Leaf Tint の ColorEdit3 (HDR、0..2)) → **Level 0..Levels-1** (Length / Radius、Level 1 以降は Branches / Length (x parent) / Length Shape / Radius (x parent) / Ratio Power、Length Variance、Taper、Tip Round、Down Angle ± / Shape、Alternate、Rotate ±、Tip Bias、Curve / Curve Back / Curve Variance、Tropism、Seg Splits、Split Angle ±、Child Base Offset、Segments、Radial Segments、Bark UV Scale)。各項目にツールチップ。
- スライダーは離した時 (`IsItemDeactivatedAfterEdit`)、チェックボックス / コンボ / ボタンは押した時に `RequestRebuild()`。Leaf Tint はピッカー内の編集で Deactivated が立たないので、触ったアクターを覚えて全項目から手が離れたときに 1 回再生成する。範囲は `Sanitize` に合わせてあるが、CTRL+クリックの直接入力は `AlwaysClamp` を付けた Levels / Segments / Radial Segments 以外では範囲外も通り、次の Tick の `Sanitize` で丸まる (配列の添字になる Levels だけは表示側でも丸める)。Pipe Exponent は 0 < n < 1 を UI 側でも 0 / 1 に寄せる。

### 4.2 INI (`[Actor.N]`)

- `WriteTreeParams` が全フィールドを `Tree.<Field>` / `Tree.L<i>.<Field>` (i = 0..3、未使用階層も保存) で書く: `Seed`, `Levels`, `TropismScale`, `BarkUVTilesPerMeter`, `BaseSplits`, `CrownShape` (int), `Prune*`, `TrunkFlare*`, `GroundOffset`, `PipeExponent`, `Junction*`, `TwigRadiusThreshold`, `BarkAOInner`, `BarkMoss`, `GenerateLeaves` (bool), `LeavesPerBranch`, `LeavesOnParentLevel`, `Leaf*`, `ClustersPerTwig`, `TerminalClusters`, `Cluster*`, `LeafTint` (`r,g,b` の 1 キー)、階層ごとに v1 の 15 キー + `Tropism`, `CurveBack`, `DownAngleShape`, `TipBias`, `Alternate` (bool), `RatioPower`, `TipRound`, `SegSplits`, `SplitAngle`, `SplitAngleVariance`, `BarkUVScale`。合計 153 キー + `Tree.MaterialPreset` = 154 キー。読み込みは在るキーだけ上書きし、最後に `Sanitize()`。`Reset Actor` はコード既定値に戻して再生成する。
- **既定値の変更と保存済み INI:** 読み込みは保存済みの `Tree.*` をそのまま使うので、古いビルドが保存した `EngineSettings.ini` は自分の `Tree.*` 値 (その時点の Oak の既定値) を持ち続け、コンストラクタの新しい既定値 (polish 1〜3) は現れない (Details の Preset 表示は `Custom` になる)。新しい既定値を採るには Details の `Preset` コンボで `Oak 5 m` を選ぶ (Seed は保たれる) か、`Reset Actor` を押す。パラメータの版キーは無い (`Tree.MaterialPreset` はマテリアルだけ)。
- **旧 `Tree.Tropism` (v1、単位なし、既定 0.2) の読み替え:** `Tree.TropismScale` が無く `Tree.Tropism` が在る旧 INI では `TropismScale = 1` にし、`Tree.L<i>.Tropism` が無い階層の `Tropism` を `旧値 x 57.3` [deg] にする (v1 は全長で接線に 0.2 を足していた ≒ 11.5 deg)。旧 INI に `Tree.L<i>.Tropism` が在ることは無いので曖昧さは無い。`Tree.Tropism` は書かない。負の `Rotate` は「左右交互」を意味しない (明示的な `Alternate`)。
- **`Tree.MaterialPreset` (マテリアル既定値の版):** `CaptureActor` が `ATree::MaterialPresetVersion` (= 3) を書き、`ReadActor` はキーが無ければ 0 (旧 INI) と読む。`ApplyActor` はコンポーネントの `C0.M*.*` を適用した後、`TreeMaterialPreset < MaterialPresetVersion` なら `ApplyDefaultMaterials()` で 4 スロットを既定値に戻す (旧 INI の Masked / 非 Substrate の葉や、版 2 の艶の強い葉が新しい見た目を隠さない)。同じ版で保存されたユーザーのマテリアル編集はそのまま残る。コンストラクタのマテリアル既定値を変えたら版を上げる (2 = Substrate TwoSidedWrap の葉、3 = 葉の艶 (F0 0.02 / F90 0.35 / Roughness 0.65) と透過色 (0.45, 0.70, 0.18))。
- `ApplyComponent` はスロット数を `min(メッシュ, 保存)` で回すので、`NumMaterials=2` の旧 INI でも 4 スロットは縮まない。

### 4.3 マテリアルの既定値 (`ATree::ApplyDefaultMaterials`、版 3)

- **樹皮 (0) / 小枝 (3):** Lit / Opaque、BaseColor 白 (色はテクスチャ)、Metallic 0、Specular 0.3、Roughness 0.85 (ARM G が 0 のときのフォールバック)、NormalWeight 1、片面、非 Substrate。
- **単葉 (1) / 房 (2) (同じ設定):** BaseColor 白、Masked (`OpacityMaskClipValue = LeafCoverageClip 0.3333`)、TwoSided、Specular 0.15、Roughness 0.65 (フォールバック、葉のクチクラは粗め)、Substrate `TwoSidedWrap`: `SubstrateDiffuseAlbedo (1, 1, 1)` (テクスチャ x 頂点カラーに乗算される。既定 0.18 だと 5 倍暗い)、`F0 0.02` (裏面の銀色の光沢を抑える)、`F90 0.35` (粗い蝋質の葉はグレージングでも鏡面にならない: 斜めのカードが空を映して灰色に光らない)、`TransmittanceColor (0.45, 0.70, 0.18)` 線形 (透過項はテクスチャで色付かないので、これが逆光の葉の色)、`Thickness 1 cm` (`exp(-tau) == T`)、`PhaseAnisotropy 0.35` (前方散乱)、`IsThin`、`FuzzColor (0.35, 0.40, 0.22)` / `FuzzAmount 0`、`FuzzRoughness 0.6`。曲げた葉の法線は CSM の受け側オフセット / DF のレイ開始 / Lumen の収集にも使われるので `LeafNormalBlend <= 0.85`。

### 4.4 葉の照明 (TWO_SIDED_WRAP のピクセルだけの補正)

polish2 までは樹冠の逆光側がほぼ黒く潰れていた (頂点カラーの暗化を全て切っても同じ = 照明側の問題)。DeferredPS に一時的なデバッグ出力 (葉のピクセルを照明の項ごとに 0..1 で塗り分け、hero / close の 2 視点) を入れて調べ、補正後に取り除いた:

- **主因は Lumen のスカイ可視率:** 葉のピクセルの 45 % / 55 % が 0.1 未満、43 % / 33 % が 0.1〜0.3。DeferredPS は Substrate の IBL 全体に skyOcclusion を掛けるので、樹冠の中の空の光は 0〜20 % になる。このシーンは太陽が弱く (強度 0.5)、樹冠の光の大半は空の光。
- **CSM の影:** 77 % / 63 % が 0.1 未満。`SubstrateDeferredLighting` では葉の透過が Diffuse の中にあり `if (Shadow.SurfaceShadow > 0)` の内側だったので、他の葉の影にある葉は太陽の透過を全て失っていた。
- **原因でないもの:** 法線を光の側へ反転して影を取り直しても 76 % / 63 % (法線オフセットによる自己影ではなく、外側の葉の影の中にある)。GBuffer AO は 0.6〜1.0、N·L は見えている面が太陽の反対側を向いていることを確認しただけ。
- **効果 (葉の平均 sRGB、R / G / B):** 補正前 11.9 / 17.7 / 10.6、透過の影だけ 13.9 / 21.7 / 11.6、スカイの下限だけ 20.5 / 35.1 / 19.6、両方 22.6 / 38.6 / 20.8。

補正 (`SSSType == SUBSTRATE_SSS_TYPE_TWO_SIDED_WRAP` だけ。他のマテリアルの出力は変わらない):

1. **`SubstrateEvaluation.hlsl`:** 両面ラップの裏面透過を Diffuse から `Lighting.Transmission` (`SlabTransmittance x TransNoL x Phase4Pi / π x kD x falloff`) に分けた。`SubstrateDeferredLighting(…, LightAttenuation, TransmissionAttenuation, Dither)` は `(Diffuse + Specular) x SurfaceShadow + Transmission x TransmissionShadow` を返し、葉では `Shadow.TransmissionShadow = max(TransmissionShadow, TransmissionAttenuation)`、ライトの評価条件は `SurfaceShadow > 0 || TransmissionShadow > 0`。定数は冒頭の `FOLIAGE_TRANSMISSION_SHADOW_MAX 0.6` (遮蔽物直後の透過の残り = 1 枚を透過した光) / `FOLIAGE_TRANSMISSION_SHADOW_DEPTH 0.9 m` (樹冠を通る光の減衰長) / `FOLIAGE_SKY_OCCLUSION_FLOOR 0.45`。
2. **`ShadowFilteringCommon.hlsl` `GetDirectionalFoliageTransmissionShadow`:** CSM の影の中なら、受光点から遮蔽物まで (光の方向) の距離 d を 3x3 テクセルのうち遮蔽しているテクセルで平均し、`lerp(0.6 exp(-d / 0.9), 1, SurfaceShadow)` を返す (遮蔽物を一様な葉の媒質とみなす。UE の Subsurface Shadow と同じ近似)。影の外 / CSM の範囲外 (DF / フェード帯) は SurfaceShadow のまま、距離フェード帯では元の値へ戻す。
3. **`DeferredPS.hlsl`:** 葉のピクセルではディレクショナルライトの透過の影を (2) で求め、`skyOcclusion = lerp(0.45, 1, skyOcclusion)` (アルファテストのカードが重なる樹冠の中では、スクリーン空間のスカイ可視率は信用できない)。
4. **`ForwardLightingCommon.hlsl`:** `GetForwardDirectLightingSubstrate` に `DirectionalTransmissionShadow` 引数を追加 (選択ディレクショナルライトだけが使い、ローカルライトは LightAttenuation と同じ値)。`TranslucentPS.hlsl` は directionalShadow を両方に渡すので、前方 / 半透明の経路の結果は変わらない。

---

## 5. 検証

### 5.1 ジオメトリのハーネス (`TreeTest.cpp`、エンジン非依存)

`ProceduralTree.cpp` + `ProceduralMeshGenerator.cpp` + `TreeTest.cpp` をリポジトリ外の小さな vcxproj (v145、C++17、/utf-8、/W4、警告ゼロ) で MSBuild し `TreeTest.exe` を走らせる (最後に `ALL PASSED`)。ヘッダはフルパスで include する (リポジトリの `Time.h` が `<time.h>` を隠すのでインクルードパスには足さない)。生成側の形の定数 (`LimbKinkFactor`、`LimbTiers` / `LimbTierPull`、`ChildSlotJitter`、`ClusterTMin`、`LeafCullMargin` など) はハーネスに参照値として写してあるので、変えたら両方を直す。

- **全ビルド共通 (`ValidateMesh`):** 4 セクション固定 / `MaterialIndex == 添字` / SDF 寄与は樹皮だけ / インデックス範囲 / NaN なし / 法線 単位長 / 接線 単位長で法線に直交 / `CountInvertedTriangles == 0` (全セクション) / 樹皮と小枝は閉じた面 (位置で溶接して全ての辺がちょうど 2 回) / 統計 (セクション別三角形、DF 三角形、小枝セクションの有無) / 単葉 1 枚 = 4 頂点 2 三角形。
- **T1 (2 パス / サブストリーム):** 決定性 (同じパラメータで同じジオメトリハッシュ、Seed+1 で変わる)、未使用階層で変わらない、最深階層の float 変更で樹皮 / 小枝がビット単位で同じ、枝ごとのブロック比較 (木の経路で対応付け) によるサブストリームの分離、Sapling 113 本。
- **T3 (葉の陰影):** 最悪値 (Blend 0.85 / Droop 55 / Cup 40 / UpBias 1 / Dead 0.3 / Erosion 0.3) で巻き順 0、色 [0.1, 1]、α [0.7, 1]、面法線との内積 > 0.1、AO が暗くする向き、暖色 / 寒色の両方、灰色テスト (HueVariance 0 で r == g == b)、枯れ葉 0.7 倍、T3 の float は頂点を動かさない、苔は上向きの面と根元の帯だけ、小枝の法線ブレンド <= 20 deg。
- **T4 (葉の配置):** 独立した再集計 (枝ごとの枚数、t の範囲 / 単調、葉柄 = 樹皮半径 + スタンドオフ ±1 mm) との一致、`Level[3].Branches 1` / `BaseOffset 0.95` で正確な枚数 (84 x 6)、裸枝は bare 0 のビルドの部分集合、親階層は裸枝にしない (t >= 0.35)、上位階層の枝先の葉、TipBias はセクション 1 / 2 だけ動かす。
- **T9 (半径モデル):** 半径が根元から先端へ単調非増加 (襟の 2.5 rc を除く)、根元リングの全頂点が親の中、パイプモデルの段階的な細り (幹 0.17 → 0.027 m)、0.85 x 親の上限、沈み込みの振り分け非依存、UV の正方テクセル (クローンの継承込み)、半球 / 襟 / 張り出しリング込みの閉面。
- **T10 (包絡):** `PruneRatio 1` / `PruneWidth 0.3` で全ての枝先 / 葉の中心が `1.05 R(h) + 0.3 m` 以内、枝数は `PruneRatio 0` と同じ、8 種の ShapeRatio の独立実装との一致、下向き角の形と 5..175 のクランプ、頂芽優勢の帯 (段への寄せ込み)、左右交互の方位 / 交互性 / 分散、間引き後の枚数 <= 規則。
- **T11 (曲がり):** `Curve 90 / CurveBack -90` の S 字が 1 deg 以内で閉じる、`CurveVariance 180` でも隣接接線の差 <= 40 deg、屈光性の鉛直面内回転の厳密な再現、鉛直での飽和、Oak 既定値で L1 の鎖 (14 本) が分岐を跨いで予定どおりに前半で立ち上がり後半で下がる、L2 の後半が単調、L3 の小枝 420 本が全て単調に垂れる (真下で終わるのは 24 本)、Seed 2..4 でも L3 が跳ね返らない。
- **T12 (分岐):** 片の数 / 枝数 = `EstimateStems` (50 組のランダムなパラメータで一致)、子の数の保存、クローンの根元リングが親の中 (根元は先端から 0.1..0.8 r)、閉面、分岐を跨いだ曲がり / 屈光性 / 屈曲の予定の継続、揺れ 60 の分岐で片の中の曲がり <= 7.5 + 屈曲 deg、根元の分岐が常に最初、`BaseSplits 0 / 1 / 3` の片の数、幹のランプの連続、Oak の二又 (y 1.96 m、主幹の半径と逸れの角、分岐点のまわりの主枝)。
- **T8 (房):** セクション 2 の UV がセルの inset の内側、`ClustersPerTwig 0` でセクション 2 が空かつ 0 / 3 がビット単位で同じ、枝ごとの再集計 (取り分 / 枝先 6 割 / 裸枝 / 切られた片 / 側枝の胴の 2 枚 / 上位階層の枝先)、混合、枝先の房の 90 deg、包絡の間引き。
- **T14 (集約):** Oak 既定値の数値 (2.9) の出力と帯の判定: 片 500..570、単葉 2100..2550、房 2100..2600、三角形 43k..52k、DF < 10k かつ == セクション 0、`NumStems <= MaxTotalStems`、`NumLeaves + NumClusters <= MaxLeafCards`、`GenerateMs` 20 回の最小 < 15 ms (平均 / 最大も表示。Sapling 参照ビルドが 3 ms を超える = マシンが重いときは WARN)、セクションごとの衛生 (| |N| - 1 |, | |T| - 1 |, |dot(N, T)| <= 1e-3、色 [0, 1]、α >= 0.7)、UV から求めた接線の利き手 (`ComputeTangents` の累積を再現、UV が定義される頂点で負 0。極 / 円盤の中心は UV が退化)、極端な設定 (Branches 64 x 3、カード上限、葉なし、32 x 32 分割 + SegSplits 1 + BaseSplits 3) で予算と構造的な三角形上界 (Sanitize の 500k の上界で 0.28〜0.47 M、報告帯 600k の内)、Sapling の継続性 (113 本 / 928 枚)。帯 (`Band()`) は見た目に依存する目安で、外れても報告だけ (FAIL にしない)。

### 5.2 テクスチャのハーネス (`TreeTextureTest.cpp`)

`ProceduralTreeTextures.cpp` + `TreeTextureTest.cpp` を同様に単独ビルドし、ハーネス側の `Saved/ProceduralTexture/*.dds` を消してから走らせる (`EnsureGenerated` は無いファイルだけ作る)。検査: 13 ファイルの DDS ヘッダ / サイズ / ミップ数 (樹皮 1024² x 3 / 11、葉 512² x 3 / 10、房 1024² x 3 / 11、小枝 256² x 3 / 9、平坦 4²)、`DXGI_FORMAT_R8G8B8A8_UNORM`、葉 / 房の α カバレッジが 8x8 のミップまで (房はセルごと) mip 0 の ±0.03 以内、ランプ幅 ~8 テクセルと clip 値が各行のランプの中、葉の輪郭が非対称で片側 3..6 個の丸い裂片 (湾入は裂片の 20 % 以上)、法線の単位長 (±2 %) / α < 0.05 で平坦 / 脈の凹凸、ARM の R / G が 0 にならず B = 0、ラフネスの範囲 (葉 / 房 0.55..0.70、小枝 0.55..0.65、樹皮 0.80..0.95)、樹皮 / 小枝の 6 枚のタイル性 (端の列 / 行の差 < 3/255 または隣接列と同程度、無関係な列の半分未満)、樹皮: ミップ 3 で 1 行あたり 6..10 本の溝の帯 (平均幅 >= タイルの 0.038 = 髪の毛の線でない)、AO 0.5..1、中間の茶、焦げ茶の溝 3..40 % / 黒 (< 30/255) < 0.5 %、地衣 / 苔 > 0.5 % かつ控えめ (G - R <= 12/255)、天辺 ≈ (0.42, 0.36, 0.28) / 底 ≈ (0.25, 0.19, 0.13) / 線形輝度比 2..5、法線に光の当たる壁と影の壁 (mip 0 / 3 で各 10 % 以上)、`mean |n.x| > 2 x mean |n.y|` (縦に長い溝で菱形の網でない)、小枝のオリーブ灰 (幹の天辺より明るくない) / 皮目 3..40 %、房のセル境界とガターが透明 / 小枝の線 / 緑が主 / 4 セルが同一でない / 灰色のフリンジなし / 左右両方の傾き、再ベイクの決定性 (バイト一致) と時間 (葉 < 300 ms、房 < 1 s、樹皮 + 小枝 < 1 s、初回起動の全体 < 2 s。壁時計なのでマシン負荷で揺れる)。

### 5.3 実機

- **ヘッドレス描画:** Release の `-taatest=static -taaframes=30 -taacapture=30 -taaout=<dir>` (終了コード 0)。カメラは `[Actor.5]` の `C0.Location` / `C0.Rotation` を一時的に木へ向けた INI で (INI はバックアップして復元、md5 確認)。コード既定値で見るときは ATree の `[Actor.N]` から `Tree.*` / `C0.M*.*` / `C0.NumMaterials` を除き、フォグを切る。木 (7, 0, 6) の見え: ヒーロー `7.0,2.6,13.5` / `0,3.1416,0` (逆光)、順光のヒーロー `7.0,2.6,-1.5` / `0,0,0`、ワイド `12,2.4,1.5` / `0,-0.838,0`、クローズ `9.5,2.8,3.5` / `-0.11,-0.785,0`。木を `7,-200,6` へ隠した描画は木以外のピクセルの回帰の基準 (照明の補正が他のマテリアルを変えないことの確認)。目視項目: 平たい丸屋根の樹冠 (尖塔にならない)、太い主枝が樹冠の中に見えて空が抜ける、主枝 / 主幹がうねる、下の枝は水平寄りで上は立つ、非対称な二又と壺形の主幹、逆光の葉が緑で黒くない、枝先に針状の輝きや裸の鞭が無い、幹が大枝の上で段階的に細る、張り出した根元と根元の苔、溝が分岐を通って続く、L1 の付け根に明度の継ぎ目が無い、遠景で樹冠が透けない。
- **実行中の再生成 (`-taatest=tree -taaframes=60`):** カメラ静止、2 フレームごとに Seed を 1..5 で巡回させて `RequestRebuild` (60 フレームで 30 回)。再生成後のメッシュが SDF スロットを持たないフレームがあれば終了コード **4**、Release で `RebuildMs` の最大が 250 ms を超えたら **5** (Debug は記録のみ)。ログ: `tree scenario: rebuilds= invalid_df_frames= max_atlas_slots=<n>/16 max_rebuild_ms= (ceiling 250, checked) last: stems= twigs= leaves= clusters= tris= df_tris=`。polish3 の結果: 終了コード 0、`rebuilds=30 invalid_df_frames=0 max_atlas_slots=7/16 max_rebuild_ms=110.7`、`last: stems=532 twigs=501 leaves=2306 clusters=2352 tris=47498 df_tris=4522`。同じハッシュは `Saved/ProceduralMesh` の SDF キャッシュに当たり `RebuildMs` がベイク抜きになるので、上限の計測は `Saved/ProceduralMesh` を消してから 1 回走らせる。
- **INI 往復:** 通常起動 → 終了で `[Actor.N]` に `Tree.*` 154 キーが書かれ、`Tree.Seed` / `Tree.LeavesPerBranch` を書き換えた INI で描画が変わる。旧 INI (`Tree.Tropism` 在り、`Tree.MaterialPreset` 無し) で葉が Substrate になり屈光性が残ること、`Tree.MaterialPreset=3` + 編集済み `C0.M1.*` が残ること、`Tree.MaterialPreset=2` の INI では葉が版 3 の既定値に戻ることを確認する。
- 絵柄を変えた後の実機確認では `Saved/ProceduralTexture/Tree*_v2.dds` を先に消す (ファイル名は v2 のまま)。

### 5.4 未確認 / 判断待ち (polish3 時点)

- 何もコミットしていない。ハーネスは両方 `ALL PASSED`、メインのビルドは警告なし、`-taatest=tree` は終了コード 0。
- 批評で挙がって未対応のもの: 縁から見たカードが黒い刃 / 破片に見える (GeometryPS でグレージング角のカードを clip で消す案)、葉の裏面を明るくする色、房アトラスの AO の下限 0.10 (0.5 案)、露出度の低い房カードの間引き (樹冠の内側の空の穴と重複描画)、中距離の葉のざらつき (alpha-to-coverage 等)、分岐の襟の膨らみ、V 字に折ったカード / 3x3 の房アトラス、遠景の撮影位置 (テーブルの天板に隠れて樹冠の遠目を判定できない)。
- 単葉の輪郭は行ごとの半幅なので、裂片は前へ反り返れない (2D の SDF にすれば直る)。
- `ProceduralTreeTextures.cpp` 冒頭のファイルコメントは樹皮を「v5」と書いている (実装は v6 の調整込み)。
- 三角形の大半 (33660 / 47498) は小枝セクション。`GenerateMs` の平均はマシン負荷で揺れる (最小が予算の値)。テクスチャハーネスの壁時計の検査 (1 s / 300 ms / 1 s / 2 s) は重い負荷で落ちることがある。
- Sapling は v1 と完全には同じでない (2.9: 屈曲 / 枠の揺れ / 段 / 主枝の半径の下限はパラメータでない規則なので効く)。

---

## 6. 対象外 (意図して含めない)

- **根 / 板根 (buttress):** 既存シーンは木を板の上に置くので `GroundOffset` で埋める (根元の張り出しは `TrunkFlare`)。
- **樹皮のディスプレースメント / 幹の捻り、空間コロナイゼーション (space colonisation) による小枝生成、樹冠の SDF オクルーダ (Masked の `BaseColor.a` は照明経路で読まれず、BLAS は不透明、重なる殻は偶奇判定で中空になる):** 技術的な理由で却下。再検討なしに足さない。
- **葉以外のシェーダ変更、ARM.b を透過マスクに使う変更:** 照明の変更は TWO_SIDED_WRAP のピクセルだけ (4.4)。葉の透過の色は Substrate TwoSidedWrap の `TransmittanceColor`、葉ごとの陰影は頂点の法線 / 色で出す。
- **ヘッダの型の変更:** `FProceduralTreeParams` / `FProceduralTreeLevelParams` のフィールドと `GeneratorVersion` / テクスチャの `Version` は固定。生成規則の改訂は `GeneratorRevision`、パラメータにしない形の定数は `ProceduralTree.cpp` 冒頭の `constexpr` で持つ。
- **風アニメーション、LOD / インスタンシング** (複数の木は現状 1 本ずつ `ATree` をスポーンする)。
- **SDF 内の枝の重なり:** 親に埋め込んだ子枝 / クローンの根元は偶奇判定で「外」になる。64³ の解像度では影響は小さい。
- **`branch.fbx` のような既存メッシュを枝として並べる方式。**
- Lumen の HWRT は葉 / 房カードを不透明な矩形として扱う (`FORCE_OPAQUE`): 大きな房カードで反射 / GI の遮蔽が塊状に見えるなら `ClusterSize` を下げるか `ClustersPerTwig 0`。

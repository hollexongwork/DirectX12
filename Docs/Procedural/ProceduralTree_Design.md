# プロシージャルツリー生成 設計書

**対象ブランチ:** `Procedural` (基点 `7575fb7`)。
**スコープ:** パラメータから木 (幹 / 枝 / 葉) のメッシュを実行時に生成し、既存のスタティックメッシュ描画経路 (ベースパス / シャドウ / Distance Field / Lumen / Velocity) にそのまま載せる。Details での編集と INI への保存を含む。
**前提:** 単位はメートル、Y アップ、左手系。表面は時計回り、接線 = +U、`dP/dV = cross(N, dP/dU)` (ProceduralMeshGenerator.h の規約)。

---

## 0. 決定事項

1. **メッシュは FBX と同じ `FBXModel` に載せる。** `FBXModel::BuildFromProceduralMesh` (CPU 頂点列 → GPU バッファ + ローカル境界 + SDF ベイク + DXR BLAS) を通すので、描画系は木を特別扱いしない。この経路と `ProceduralMeshGenerator.h/.cpp` (基本形状 / ノイズ / 接線計算 / FNV ハッシュ) は、破棄済みコミット `2b051fd` (reflog) の基盤部分をそのまま再利用した。デモ 6 体のスポーンと INI の変更は取り込んでいない。
2. **木は `ATree` (AStaticMeshActor 派生) 1 体 = `FProceduralTreeParams` 1 組。** メッシュは `FAssetManager::CreateProceduralMesh("Tree_<パラメータハッシュ>")` で共有する。同じパラメータの木は GPU バッファ / SDF スロット / BLAS を共有する。
3. **プロシージャルメッシュのキャッシュは弱参照。** パラメータ編集のたびに別名のメッシュが生まれるので、`FAssetManager` は `weak_ptr` で持ち、使うコンポーネント / プロキシが居なくなった時点で解放する。`FBXModel` のデストラクタが `FDistanceFieldAtlas::ReleaseMesh` でスロットを返却し、`AddMesh` は空きスロットを優先して再利用する (スロットは 16 個しかない)。
4. **再生成は描画の外 (Tick) で行う。** メッシュ構築は SDF ベイク / BLAS 構築でコマンドリストをフラッシュする (`FlushAndResetCommandList`) ため、ImGui のコールバック (フレーム内) では行わない。Details / INI からの変更は `ATree::RequestRebuild()` で予約し、次の `Tick` が `RebuildTree()` を呼ぶ。初回の生成も最初の Tick で行う。
5. **葉は Mesh Distance Field に含めない。** 葉カードは開いた面で、レイ交差の偶奇で符号を決める SDF ベイクを乱す。`FProceduralMeshSection::bAffectDistanceField = false` を追加し、`FBXModel::CreateSubset` が三角形の蓄積を飛ばす。描画 / シャドウ / BLAS には参加する。各枝は先端 / 根元にキャップを持つ閉じたチューブにして、SDF の内外判定を安定させる。
6. **テクスチャも生成する。** 既存アセットに樹皮 / 葉のテクスチャが無く、テクスチャ未設定のスロットは直前のバインドを引きずる (`FStaticMeshSceneProxy::BindSlot` は存在するものだけバインド)。`FProceduralTreeTextures` が樹皮 (BaseColor / Normal / ARM)、葉 (BaseColor α マスク / ARM)、平坦法線を RGBA8 + ミップの DDS として `Saved/ProceduralTexture/` に書き出し、既存の DDS ローダで読む。ファイルが揃っていれば生成しない (絵柄を変えたら `Version` を上げる)。
7. **同じパラメータからは常に同じメッシュ。** 乱数はシード付き `mt19937` を自前の一様変換で使う (標準の分布クラスは処理系依存)。`FProceduralTreeParams::ComputeHash` (FNV-1a、`GeneratorVersion` 込み、使っている階層分のみ) がキャッシュ名 / SDF キャッシュ (`Saved/ProceduralMesh/Tree_<hash>_<ジオメトリハッシュ>.mdf`) のキーになる。
8. **INI は後方互換。** `[Actor.N]` に `Tree.*` キーを足すだけ。既存キーは変えない。木のアクターはフォグの後にスポーンし、既存の `[Actor.N]` の添字を変えない。

---

## 1. 構成

| ファイル | 役割 |
|---|---|
| `ProceduralTree.h/.cpp` | `FProceduralTreeParams` (全体 + 階層ごと) / `FProceduralTreeBuilder::Build` (骨格 → 樹皮チューブ + 葉カード) / `FProceduralTreeTextures` (DDS 生成) |
| `Tree.h/.cpp` | `ATree`: パラメータ保持、ハッシュ比較、`FAssetManager` 経由のメッシュ差し替え、マテリアル 2 スロット (樹皮 Opaque / 葉 Masked Two Sided) |
| `ProceduralMeshGenerator.h/.cpp` | `FProceduralMeshSection` / `FProceduralMeshData` (2b051fd から再利用) + `bAffectDistanceField` |
| `FBXModel.h/.cpp` | `BuildFromProceduralMesh` / `CreateSubset` / `FinalizeLoad` (2b051fd から再利用) + DF 除外フラグ + デストラクタでのスロット返却 |
| `AssetManager.h/.cpp` | `CreateProceduralMesh` / `FindProceduralMesh` (弱参照キャッシュ) |
| `DistanceFieldAtlas.h/.cpp` | `GetIfExists` / `ReleaseMesh` / 空きスロットの再利用、非キャッシュベイク (2b051fd) |
| `ImGuiManager.cpp` | Details の "Procedural Tree" セクション (`DrawProceduralTreeSection`) |
| `SettingsManager.h/.cpp` | `ActorSnapshot::Tree` + `Tree.*` キーの読み書き、`ApplyActor` → `SetTreeParams` |
| `GameManager.cpp` | `ATree` を (7, 0, 6) にスポーン |

---

## 2. 生成アルゴリズム (Weber & Penn の簡略版)

### 2.1 骨格

`GenerateStem(Level, Origin, Direction, Normal, Length, BaseRadius)` を幹 (Level 0) から再帰する。

- 枝は `Segments` 本のセグメント。各セグメントで接線を
  1. 系統的な曲がり `Curve / Segments` [deg] (鉛直面内、+ で上へ。回転の符号は「上向きになる方」を実測で選ぶ)、
  2. ランダムな曲がり `±CurveVariance / Segments` [deg] (接線に直交するランダム軸まわり)、
  3. 屈光性 `Tropism / Segments` (上方向 +Y を足して正規化)
  の順に更新し、フレームの法線は回転最小フレーム (`TransportNormal`) で運ぶ。
- 半径は根元 `BaseRadius` から先端 `BaseRadius * Taper` へ線形。
- 骨格点の接線は隣接セグメントの平均にして、リングの折れを抑える。
- 子枝: `BaseOffset..1` の範囲に `Branches` 本を等間隔 (±0.25 区間のジッタ) に置き、親の軸まわりに `Rotate ± RotateVariance` (既定 137.5° = 黄金角) ずつ回し、親の接線から `DownAngle ± DownAngleVariance` 倒す。長さは `親の長さ × Length × lerp(1, LengthShape, 付け根からの位置) × (1 ± LengthVariance)`、半径は `付け根での親の半径 × Radius` (親の 0.9 倍を上限、最小 3 mm)。
- 乱数の消費順は「骨格 → 子枝の仕様 → 葉 → 子枝の再帰」で固定。

### 2.2 樹皮チューブ

- 骨格点ごとに `RadialSegments + 1` 頂点のリング (継ぎ目の頂点を複製)。位置 = 点 + (cosθ N + sinθ B) r、法線は先細りの傾き分だけ接線側へ倒す。接線 (+U) は周方向。
- UV: U = 周方向 (根元の円周 × `BarkUVTilesPerMeter` を整数に丸めたタイル数)、V = 枝に沿った距離 × `BarkUVTilesPerMeter`。
- 先端 / 根元に扇形のキャップ。巻き順は頂点法線で向きを正す (`AddTriangle`)。
- 幹の根元は頂点カラーで 0.72 → 1.0 に暗くする。

### 2.3 葉カード

- 最終階層 (`Levels - 1`) の枝に `LeavesPerBranch` 枚。枝の 15 %〜100 % に分布、黄金角で回し、枝の接線から `LeafAngle ± LeafAngleVariance`。
- 1 枚 = 四角形 2 三角形。長さ `LeafSize × (1 ± LeafSizeVariance)`、幅 = 長さ × `LeafAspect`。葉面はおおむね上向き (法線の Y ≥ 0) にしてから葉軸まわりに ±35° 捻る。
- UV は V = 0 が葉先、V = 1 が付け根 (テクスチャの葉の向きと一致)。頂点カラーで `LeafTintVariance` 分の明度ばらつき。
- セクション 1 (MaterialIndex 1)、`bAffectDistanceField = false`。

### 2.4 パラメータの既定値 (落葉樹風 3 階層)

| | Level 0 (幹) | Level 1 | Level 2 | Level 3 |
|---|---|---|---|---|
| Branches | - | 14 | 7 | 4 |
| Length | 4.0 m | 0.55 | 0.5 | 0.5 |
| LengthShape | - | 0.45 | 0.7 | 0.8 |
| Radius | 0.16 m | 0.6 | 0.55 | 0.55 |
| Taper | 0.55 | 0.3 | 0.25 | 0.2 |
| DownAngle | - | 60° ± 12 | 50° ± 15 | 45° ± 20 |
| Curve / Variance | 0 / 12 | 20 / 35 | 10 / 50 | 0 / 60 |
| BaseOffset | 0.35 | 0.15 | 0.1 | 0.1 |
| Segments / Radial | 10 / 14 | 7 / 8 | 4 / 6 | 3 / 5 |

全体: `Seed 1`、`Levels 3`、`Tropism 0.2`、葉 12 枚 / 枝、`LeafSize 0.22 m`。既定値で枝 99 本、葉 1176 枚、約 1.4 万三角形。

`Sanitize()` が各値を生成可能な範囲に丸め、枝の総数 (1 + b1 + b1 b2 + …) が 20000 を超えるときは最深階層から `Branches` を減らす。

---

## 3. テクスチャ (`FProceduralTreeTextures`)

| ファイル (`Saved/ProceduralTexture/`) | 内容 |
|---|---|
| `TreeBark_diff_v1.dds` (512², sRGB) | タイル可能な値ノイズ fBm (U 方向 12 周期 / V 方向 3 周期 = 縦の畝) + 細部。溝 = 焦げ茶、畝 = 明るい茶、低周波で苔の緑 |
| `TreeBark_nor_dx_v1.dds` | 同じハイトフィールドの差分から `normalize(-dh/du, -dh/dv, 1)`。既存アセットの `nor_dx` と同じ規約 (B = cross(N, T) = +V) |
| `TreeBark_arm_v1.dds` | R = AO (溝で暗い) / G = ラフネス 0.75〜0.9 / B = 0 |
| `TreeLeaf_diff_v1.dds` (256², sRGB) | α = 葉の形 (`sin(πv)^0.6` の半幅プロファイル + 縁のゆらぎ + 葉柄)、RGB = 緑のグラデーション + 主脈 / 側脈 + まだら |
| `TreeLeaf_arm_v1.dds` (4²) | AO 1 / ラフネス 0.6 / メタリック 0 |
| `TreeFlat_nor_dx_v1.dds` (4²) | 平坦法線 (葉スロットに前のスロットの法線マップを引きずらせないため) |

DDS は DX10 ヘッダ (`DXGI_FORMAT_R8G8B8A8_UNORM`) + 2x2 ボックスフィルタのミップチェーン。BaseColor は既存の `DDS_LOADER_FORCE_SRGB` 経路で sRGB になる。

---

## 4. Details / INI

- Details (アクター選択時、コンポーネントに依らず表示): `Procedural Tree` セクション。統計 (Stems / Leaves / Verts / Tris)、Seed (+ Random)、Levels、Tropism、Bark UV Tiles / m、Leaves グループ、Level 0..Levels-1 のグループ、Rebuild ボタン。スライダーは離した時に `RequestRebuild()`、チェックボックス / ボタンは押した時。
- INI: `[Actor.N]` に `Tree.Seed` / `Tree.Levels` / `Tree.Tropism` / `Tree.BarkUVTilesPerMeter` / `Tree.GenerateLeaves` / `Tree.Leaf*` / `Tree.L<i>.<Field>` (i = 0..3、未使用階層も保存)。読み込み後に `Sanitize()`。`Reset Actor` はコード既定値に戻して再生成する。

---

## 5. 検証

- 単体テスト (エンジン非依存、`ProceduralTree.cpp` + `ProceduralMeshGenerator.cpp` を MSBuild で単独ビルド): インデックス範囲 / NaN なし / 法線が単位長 / 接線が法線に直交 / 巻き順が法線と一致 (`CountInvertedTriangles == 0`) / 決定性 (同じパラメータで同じジオメトリハッシュ) / シードで変わる / 未使用階層の変更でハッシュが変わらない / `Sanitize` の枝数上限 / 葉の数 = 最終階層の枝数 × LeavesPerBranch / DDS のヘッダとサイズ。
- 実機: Release ビルドで `-taatest=static` のヘッドレス描画 (カメラを一時的に木へ向けた INI で) のキャプチャを目視確認。
- 実行中の再生成: テストドライバに `tree` シナリオを追加 (`-taatest=tree -taaframes=60`)。2 フレームごとに Seed を 1..5 で巡回させて `RequestRebuild` し (60 フレームで 30 回)、再生成後のメッシュが SDF スロットを持たないフレームがあれば終了コード 4。Release / Debug (D3D12 デバッグレイヤー有効) とも終了コード 0、`max_atlas_slots=7/16` (既存メッシュ 5 + 木 2 枠を使い回し)。
- INI 往復: 通常起動 → 終了で `[Actor.N]` に `Tree.*` 72 キーが書かれ、`Tree.Seed` / `Tree.LeavesPerBranch` を書き換えた INI での描画が変わることを確認。

---

## 6. 対象外 (今後)

- 葉の Substrate TwoSidedWrap (透過光) / 風アニメーション。
- LOD / インスタンシング (複数の木は現状 1 本ずつ `ATree` をスポーンする)。
- SDF 内の枝の重なり (親に埋め込んだ子枝の根元は偶奇判定で「外」になる。64³ の解像度では影響は小さい)。
- `branch.fbx` のような既存メッシュを枝として並べる方式。

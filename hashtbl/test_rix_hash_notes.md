# test_rix_hash.c テストシナリオ

## 共通設定

| 項目 | 値 |
|------|-----|
| ノード型 | `struct mynode { uint64_t key[2]; }` (128bit key) |
| キー割り当て | `key[0] = 1-origin index, key[1] = 0xDEADC0DE00000000` (basic) / `0` (fuzz) |
| ハッシュ関数 | Murmur3-finalizer 系 mix (両 64bit 半を独立 mix 後 XOR) |
| basic 用バケット | `NB_BK_BASIC=4` (4×16=64スロット、ノード20個) |

---

## test_init_empty

**目的**: 初期化直後のテーブルが空であることを確認する。

1. `RIX_HASH_INIT` 後、`rhh_nb == 0` かつ `rhh_mask == NB_BK_BASIC - 1` を検証。
2. 空テーブルに対して `find` → `NULL` を返すことを確認。
3. 空テーブルに対して `remove` → `NULL` を返すことを確認。

---

## test_insert_find_remove

**目的**: insert / find / remove の基本動作を確認する。

1. 20ノードを全て insert → 各戻り値が `NULL`（成功）。
2. insert 後 `rhh_nb == 20`。
3. 全ノードを `find` → 各自の正しいポインタが返る。
4. 存在しないキーを `find` → `NULL`。
5. 偶数インデックスのノードを `remove` → 削除ノード自身のポインタが返る。
6. remove 後 `rhh_nb == 10`。
7. 削除済みノードを `find` → `NULL`。残存ノードを `find` → 正しいポインタ。
8. 存在しないキーを `remove` → `NULL`。

---

## test_duplicate

**目的**: 同一キーの重複 insert を正しく検出することを確認する。

1. `g_basic[0]` を insert → `NULL`（成功）。
2. 同じ `g_basic[0]` を再 insert → 既存ノードポインタ (`&g_basic[0]`) が返る。`rhh_nb` は増えない。
3. プール外の別オブジェクト（同じキー値を持つ `dup`）を insert →
   - 重複チェックはキー比較で行われ、インデックス計算より先に発生するため、`dup` のプール外アドレスは問題なし。
   - 戻り値は既存の `&g_basic[0]`。`rhh_nb` は増えない。

---

## test_staged_find

**目的**: 3ステージ分解 API (hash_key / scan_bk / cmp_key) の x1/x2/x4 バリアントが `find` と同じ結果を返すことを確認する。

### x1 staged
- 全 20 ノードに対して `hash_key` → `scan_bk` → `cmp_key` を順に実行し、正しいポインタが返ることを確認。
- 存在しないキーに対して `cmp_key` が `NULL` を返すことを確認。

### x2 bulk
- `keys2 = { g_basic[0].key, bad_key }` で `hash_key2` → `scan_bk2` → `cmp_key2` を実行。
- `res2[0] == &g_basic[0]`, `res2[1] == NULL` を検証。

### x4 bulk
- `keys4 = { g_basic[3].key, bad_key, g_basic[7].key, g_basic[11].key }` で `hash_key4` → `scan_bk4` → `cmp_key4` を実行。
- `res4[0..3]` が期待ポインタ (`&g_basic[3]`, `NULL`, `&g_basic[7]`, `&g_basic[11]`) と一致することを検証。

---

## test_walk

**目的**: walk コールバックの巡回と早期終了を確認する。

1. 全 20 ノード insert 後、`walk_count_cb` でカウント → `20`。
2. 5ノード remove 後、再度 walk → `15`。
3. 早期終了テスト: コールバックが 3 回呼ばれたら `99` を返す。
   - `walk` の戻り値が `99` であること。
   - コールバックがちょうど 3 回呼ばれていること。

---

## test_fuzz

**目的**: ランダム操作をモデル (`present[]` 配列) と照合し、整合性を確認する。

### パラメータ（デフォルト）

| 引数 | デフォルト | 説明 |
|------|-----------|------|
| seed | `0xC0FFEE11` | 乱数シード |
| N | 512 | ノードプールサイズ |
| nb_bk | 64 | バケット数 (64×16=1024スロット、充填率 ~50%) |
| ops | 200000 | 操作回数 |

コマンドライン: `./hash_test [seed [N [nb_bk [ops]]]]`

### 操作の比率

| 操作 | 確率 | 検証内容 |
|------|------|---------|
| insert | 60% | `present[idx]` 済みなら既存ポインタ返却、未登録なら `NULL`（成功）または `elm`（テーブル満杯、スキップ可） |
| remove | 20% | `present[idx]` 済みなら削除ノード返却、未登録なら `NULL` |
| find   | 20% | `present[idx]` 済みなら正しいポインタ、未登録なら `NULL` |

### 定期チェック
- 1024 ステップごとに `head.rhh_nb == in_table`（モデルのカウント）を検証。

### 終了時チェック
- `head.rhh_nb == in_table` の最終確認。
- `walk` で実際に巡回できるノード数が `in_table` と一致することを確認。

---

## ビルド方法

```sh
# hashtbl/ ディレクトリで
make hash_test          # ビルド
make hash_test_run      # ビルド + 実行
./hash_test             # デフォルトパラメータで実行
./hash_test 42 1024 128 500000  # seed=42, N=1024, nb_bk=128, ops=500000
```

ビルドフラグ: `-std=gnu11 -g -O2 -mavx2 -Wall -Wextra`
(AVX-512 は skip; AVX2 のみ有効)

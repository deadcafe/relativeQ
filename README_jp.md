# librix -- Relative Index Library

共有メモリ向けインデックスベースデータ構造ライブラリ。

librix は、BSD 標準データ構造 (SLIST, LIST, STAILQ, TAILQ, CIRCLEQ,
Red-Black ツリー) と高性能カッコーハッシュテーブル 3 種を、生ポインタを
一切埋め込まない**相対インデックス**で実装します。共有メモリや mmap
領域にそのまま格納できます。

## 目次

- なぜインデックスか?
- 設計
- リポジトリ構成
- クイックスタート
- 共通ヘルパー
- キュー構造体
  - RIX_SLIST
  - RIX_LIST
  - RIX_STAILQ
  - RIX_TAILQ
  - RIX_CIRCLEQ
- Red-Black ツリー -- RIX_RB
- カッコーハッシュテーブル
  - バリアント比較
  - RIX_HASH (フィンガープリント、可変長キー)
  - RIX_HASH32 (uint32_t キー)
  - RIX_HASH64 (uint64_t キー)
- フローキャッシュサンプル
- ビルド
- 並行性
- テスト
- ライセンス

---

## なぜインデックスか?

生ポインタはプロセスローカルであり再配置不可能です。代わりに**符号なし
インデックス**を格納することで:

- プロセスをまたいで、またリマップ後も構造体を再配置できる。
- 共有メモリやファイルバック領域に自然に収まる。
- ゼロ初期化でヘッド/ノードが自動的に空状態になる。
- 同一領域をマップする 32/64 ビット混在プロセス間でポインタサイズの
  不一致を回避できる。

---

## 設計

| 概念 | 詳細 |
|------|------|
| NIL 番兵 | `RIX_NIL = 0` -- ゼロ値が「要素なし」を意味する |
| 1-origin マッピング | 有効インデックスは `1 ... UINT_MAX-1`; `pool[i]` <-> インデックス `i+1` |
| ポインタを格納しない | ヘッドとリンクフィールドはインデックスのみ |
| 一時ポインタ | 変換はマクロ呼び出し時に `base` (要素配列) を渡すことで行う |
| 規格 | C11; 外部依存なし; サブシステムごとに単一ヘッダ |

インデックス <-> ポインタ変換マクロ:

```c
RIX_IDX_FROM_PTR(base, p)   /* (p - base) + 1  (NULL -> RIX_NIL) */
RIX_PTR_FROM_IDX(base, i)   /* base + (i-1)    (0 -> NULL)        */
```

---

## リポジトリ構成

```
include/
  librix.h          傘ヘッダ (全サブシステムをインクルード)
  rix/
    rix_defs_private.h  共通マクロ、インデックスヘルパー  (内部用; 自動インクルード)
    rix_hash_arch.h     アーキテクチャ依存ディスパッチ・SIMD ヘルパー (内部用; 自動インクルード)
    rix_queue.h     SLIST / LIST / STAILQ / TAILQ / CIRCLEQ
    rix_tree.h      Red-Black ツリー
    rix_hash.h      カッコーハッシュ -- フィンガープリント版 (可変長キー)
    rix_hash32.h    カッコーハッシュ -- uint32_t キー版
    rix_hash64.h    カッコーハッシュ -- uint64_t キー版
    rix_hash_key.h  カッコーハッシュ -- uint32_t / uint64_t キー版 (統合)
samples/
  DESIGN.md         設計ドキュメント
  DESIGN_JP.md      設計ドキュメント (日本語)
  fcache/           ライブラリ
    include/
      flow_cache.h            傘ヘッダ: 3 バリアント全体をインクルード
      flow_cache_decl.h       バリアント header が使う共通宣言
      flow4_cache.h           IPv4 5 タプルフローキャッシュ (20 バイトキー)
      flow6_cache.h           IPv6 5 タプルフローキャッシュ (44 バイトキー)
      flow_unified_cache.h    IPv4+IPv6 統合テーブル (44 バイトキー、family フィールド)
    src/
      flow4.c                 IPv4 公開 wrapper + backend 選択
      flow4_backend.c         IPv4 backend テンプレート、gen/sse/avx2/avx512 として多重コンパイル
      flow6.c                 IPv6 公開 wrapper + backend 選択
      flow6_backend.c         IPv6 backend テンプレート、gen/sse/avx2/avx512 として多重コンパイル
      flowu.c                 Unified 公開 wrapper + backend 選択
      flowu_backend.c         Unified backend テンプレート、gen/sse/avx2/avx512 として多重コンパイル
      backend.h               backend ops テーブル (内部用)
      body.h                  実装テンプレート (内部用)
      hash_direct.h           direct-find 用 hash 生成 helper (内部用)
    lib/                      生成物 (libfcache.a / libfcache.so)
  fcache2/          experimental action-cache 再設計 (flow4 のみ)
    include/
      flow_cache2.h          fcache2 公開ヘッダの傘
      flow4_cache2.h         flow4 action-cache API
    src/
      flow4.c                flow4 専用実装
    lib/                     生成物 (libfcache2.a / libfcache2.so)
  test/
    fcache_test.c           正確性テスト + ベンチマーク (全 3 バリアント)
    fcache_test_body.h      テンプレート: テスト・ベンチマーク関数 (内部用)
    ht4_backend.c           test 専用 raw-hash テンプレート、gen/sse/avx2/avx512 として多重コンパイル
    ht4.h                   test 専用 raw flow4 hash ベンチ宣言
    perf.sh                 単一 workload 向け perf stat wrapper
```

---

## クイックスタート

```c
#include "librix.h"      /* キュー + ツリー + 全ハッシュバリアント */
```

必要なものだけインクルードすることも可能:

```c
#include "rix/rix_queue.h"   /* キュー構造体のみ                      */
#include "rix/rix_tree.h"    /* Red-Black ツリーのみ                  */
#include "rix/rix_hash.h"    /* カッコーハッシュ (fp 版)              */
#include "rix/rix_hash32.h"  /* カッコーハッシュ (u32 キー版)         */
#include "rix/rix_hash64.h"  /* カッコーハッシュ (u64 キー版)         */
#include "rix/rix_hash_key.h"/* カッコーハッシュ (u32 + u64 統合版)   */
```

### キューの最小サンプル

```c
struct node {
    int value;
    RIX_TAILQ_ENTRY(node) link;
};

RIX_TAILQ_HEAD(qhead);
struct qhead h;
struct node *base;   /* 共有メモリ上の要素配列 */

RIX_TAILQ_INIT(&h);

RIX_TAILQ_INSERT_TAIL(&h, base, &base[0], link);
RIX_TAILQ_INSERT_TAIL(&h, base, &base[1], link);

struct node *it;
RIX_TAILQ_FOREACH(it, &h, base, link) {
    /* it->value を使用 */
}

RIX_TAILQ_REMOVE(&h, base, &base[0], link);
```

---

## 共通ヘルパー

任意の rix 公開ヘッダ経由で利用可能 (内部では `rix_defs_private.h` で定義):

```c
RIX_NIL                          /* 0 -- ヌルインデックス */
RIX_IDX_FROM_PTR(base, p)        /* ポインタ -> インデックス */
RIX_PTR_FROM_IDX(base, i)        /* インデックス -> ポインタ (i==0 なら NULL) */
RIX_IDX_IS_NIL(i)                /* i == RIX_NIL */
RIX_IDX_IS_VALID(i, cap)         /* 1 <= i <= cap */

RIX_MIN(a, b)
RIX_MAX(a, b)
RIX_COUNT_OF(arr)
RIX_OFFSET_OF(type, field)
RIX_CONTAINER_OF(ptr, type, field)
RIX_SWAP(a, b)
RIX_CLAMP(v, lo, hi)
RIX_ASSERT(expr)
RIX_STATIC_ASSERT(expr, msg)
```

---

## キュー構造体

以下の全マクロで共通の引数:
- `type` -- 要素の構造体名 (`struct` キーワードなしの裸の識別子)
- `field` -- `type` 内に埋め込んだリンクフィールド名
- `base` -- 要素配列への `type *` ポインタ
- `head` -- コンテナヘッドへのポインタ

### RIX_SLIST

単方向リスト。先頭挿入 O(1)、削除 O(n)。

```c
/* 宣言 */
RIX_SLIST_ENTRY(type)                   /* 構造体内リンクフィールド */
RIX_SLIST_HEAD(name)                    /* ヘッド型の宣言 */
RIX_SLIST_HEAD_INITIALIZER(var)         /* 静的初期化子 */

/* 初期化・参照 */
RIX_SLIST_INIT(head)
RIX_SLIST_EMPTY(head)                   /* 空なら 1 */
RIX_SLIST_FIRST(head, base)             /* 先頭要素または NULL */
RIX_SLIST_NEXT(elm, base, field)        /* 次要素または NULL */

/* 変更 */
RIX_SLIST_INSERT_HEAD(head, base, elm, field)
RIX_SLIST_INSERT_AFTER(base, slistelm, elm, field)
RIX_SLIST_REMOVE_HEAD(head, base, field)
RIX_SLIST_REMOVE_AFTER(base, elm, field)
RIX_SLIST_REMOVE(head, base, elm, type, field)   /* O(n) 線形探索 */

/* 反復 */
RIX_SLIST_FOREACH(var, head, base, field)
RIX_SLIST_FOREACH_SAFE(var, head, base, field, tvar)
RIX_SLIST_FOREACH_PREVINDEX(var, varidxp, head, base, field)
```

### RIX_LIST

双方向リスト。任意位置での挿入・削除が O(1)。

```c
/* 宣言 */
RIX_LIST_ENTRY(type)
RIX_LIST_HEAD(name)
RIX_LIST_HEAD_INITIALIZER(var)

/* 初期化・参照 */
RIX_LIST_INIT(head)
RIX_LIST_EMPTY(head)
RIX_LIST_FIRST(head, base)
RIX_LIST_NEXT(elm, base, field)

/* 変更 */
RIX_LIST_INSERT_HEAD(head, base, elm, field)
RIX_LIST_INSERT_AFTER(head, base, listelm, elm, field)
RIX_LIST_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_LIST_REMOVE(head, base, elm, field)
RIX_LIST_SWAP(head1, head2, base, type, field)

/* 反復 */
RIX_LIST_FOREACH(var, head, base, field)
RIX_LIST_FOREACH_SAFE(var, head, base, field, tvar)
```

### RIX_STAILQ

単方向テールキュー。先頭・末尾挿入がともに O(1)。

```c
/* 宣言 */
RIX_STAILQ_ENTRY(type)
RIX_STAILQ_HEAD(name)
RIX_STAILQ_HEAD_INITIALIZER(var)

/* 初期化・参照 */
RIX_STAILQ_INIT(head)
RIX_STAILQ_EMPTY(head)
RIX_STAILQ_FIRST(head, base)
RIX_STAILQ_LAST(head, base)
RIX_STAILQ_NEXT(head, base, elm, field)

/* 変更 */
RIX_STAILQ_INSERT_HEAD(head, base, elm, field)
RIX_STAILQ_INSERT_TAIL(head, base, elm, field)
RIX_STAILQ_INSERT_AFTER(head, base, tqelm, elm, field)
RIX_STAILQ_REMOVE_HEAD(head, base, field)
RIX_STAILQ_REMOVE_AFTER(head, base, elm, field)
RIX_STAILQ_REMOVE(head, base, elm, type, field)   /* O(n) 線形探索 */
RIX_STAILQ_REMOVE_HEAD_UNTIL(head, base, elm, field)
RIX_STAILQ_CONCAT(head1, head2, base, field)
RIX_STAILQ_SWAP(head1, head2, base)

/* 反復 */
RIX_STAILQ_FOREACH(var, head, base, field)
RIX_STAILQ_FOREACH_SAFE(var, head, base, field, tvar)
```

### RIX_TAILQ

双方向テールキュー。先頭・末尾・任意位置での挿入・削除がすべて O(1)。

```c
/* 宣言 */
RIX_TAILQ_ENTRY(type)
RIX_TAILQ_HEAD(name)
RIX_TAILQ_HEAD_INITIALIZER(var)

/* 初期化・参照 */
RIX_TAILQ_INIT(head)
RIX_TAILQ_RESET(head)           /* INIT の別名 */
RIX_TAILQ_EMPTY(head)
RIX_TAILQ_FIRST(head, base)
RIX_TAILQ_LAST(head, base)
RIX_TAILQ_NEXT(head, base, elm, field)
RIX_TAILQ_PREV(head, base, elm, field)

/* 変更 */
RIX_TAILQ_INSERT_HEAD(head, base, elm, field)
RIX_TAILQ_INSERT_TAIL(head, base, elm, field)
RIX_TAILQ_INSERT_AFTER(head, base, listelm, elm, field)
RIX_TAILQ_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_TAILQ_REMOVE(head, base, elm, field)
RIX_TAILQ_CONCAT(head1, head2, base, field)
RIX_TAILQ_SWAP(head1, head2, base)

/* 反復 */
RIX_TAILQ_FOREACH(var, head, base, field)
RIX_TAILQ_FOREACH_SAFE(var, head, base, field, tvar)
RIX_TAILQ_FOREACH_REVERSE(var, head, base, field)
```

### RIX_CIRCLEQ

循環双方向リスト。FIRST が LAST に戻る循環走査が可能。

```c
/* 宣言 */
RIX_CIRCLEQ_ENTRY(type)
RIX_CIRCLEQ_HEAD(name)
RIX_CIRCLEQ_HEAD_INITIALIZER(var)

/* 初期化・参照 */
RIX_CIRCLEQ_INIT(head)
RIX_CIRCLEQ_EMPTY(head)
RIX_CIRCLEQ_FIRST(head, base)
RIX_CIRCLEQ_LAST(head, base, field)
RIX_CIRCLEQ_NEXT(elm, base, field)
RIX_CIRCLEQ_PREV(elm, base, field)

/* 変更 */
RIX_CIRCLEQ_INSERT_HEAD(head, base, elm, field)
RIX_CIRCLEQ_INSERT_TAIL(head, base, elm, field)
RIX_CIRCLEQ_INSERT_AFTER(head, base, listelm, elm, field)
RIX_CIRCLEQ_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_CIRCLEQ_REMOVE(head, base, elm, field)

/* 反復 (1 周分) */
RIX_CIRCLEQ_FOREACH(var, head, base, field)
RIX_CIRCLEQ_FOREACH_REVERSE(var, head, base, field)
RIX_CIRCLEQ_FOREACH_SAFE(var, head, base, field, tvar)
RIX_CIRCLEQ_FOREACH_REVERSE_SAFE(var, head, base, field, tvar)
```

---

## Red-Black ツリー -- RIX_RB

自己平衡 BST。挿入・削除・検索がすべて O(log n)。

### 最小サンプル

```c
struct rbnode {
    int key;
    RIX_RB_ENTRY(rbnode) rb;
};

static int rb_cmp(const struct rbnode *a, const struct rbnode *b) {
    return (a->key > b->key) - (a->key < b->key);
}

RIX_RB_HEAD(rbtree);
RIX_RB_PROTOTYPE(rbt, rbnode, rb, rb_cmp)
RIX_RB_GENERATE (rbt, rbnode, rb, rb_cmp)

void demo(struct rbtree *rh, struct rbnode *base) {
    RIX_RB_INIT(rh);

    base[0].key = 42;
    RIX_RB_INSERT(rbt, rh, base, &base[0]);

    struct rbnode probe = { .key = 42 };
    struct rbnode *hit = RIX_RB_FIND(rbt, rh, base, &probe);

    struct rbnode *it;
    RIX_RB_FOREACH(it, rbt, rh, base) { /* 昇順走査 */ }
}
```

### API リファレンス

```c
/* 宣言・コード生成 */
RIX_RB_ENTRY(type)
RIX_RB_HEAD(name)
RIX_RB_HEAD_INITIALIZER(var)
RIX_RB_INIT(head)
RIX_RB_PROTOTYPE(name, type, field, cmp)     /* extern 宣言 */
RIX_RB_GENERATE (name, type, field, cmp)     /* 実装生成 */

/* 操作 */
RIX_RB_INSERT(name, head, base, elm)   /* NULL -> 挿入成功; 非 NULL -> 重複 */
RIX_RB_REMOVE(name, head, base, elm)   /* 削除した elm を返す */
RIX_RB_FIND  (name, head, base, key)   /* 完全一致または NULL */
RIX_RB_NFIND (name, head, base, key)   /* 下界 (key 以上の最小ノード) */
RIX_RB_MIN   (name, head, base)
RIX_RB_MAX   (name, head, base)
RIX_RB_NEXT  (name, base, elm)
RIX_RB_PREV  (name, base, elm)

/* 反復 */
RIX_RB_FOREACH        (var, name, head, base)   /* 昇順 */
RIX_RB_FOREACH_REVERSE(var, name, head, base)   /* 降順 */
```

比較関数シグネチャ: `int cmp(const type *a, const type *b)` -- 狭義弱順序。

---

## カッコーハッシュテーブル

インデックスベースのカッコーハッシュ 3 バリアント。共通特性:

- **バケットあたり 16 スロット** (SIMD 並列スキャン)
- **実行時 SIMD ディスパッチ** -- `rix_hash_arch_init(enable)` でソースファイルごとに Generic / SSE / AVX2 / AVX-512 を選択
- **XOR 対称性による 2 候補バケット** -- 削除 O(1)、リハッシュ不要
- **N 段先行パイプラインルックアップ** -- DRAM レイテンシを複数リクエスト間で隠蔽
- **1-origin インデックス格納** -- `RIX_NIL = 0` が空スロットを示す; 生ポインタなし

### バリアント比較

| ヘッダ | キー格納場所 | バケットサイズ | 適したキー |
|--------|------------|--------------|-----------|
| `rix_hash.h`   | フィンガープリントをバケットに、フルキーをノードに格納 | 128 B (2 CL) | 可変長・大きなキー |
| `rix_hash32.h` | `uint32_t` キーをバケットに直接格納 | 128 B (2 CL) | 32 ビット整数キー |
| `rix_hash64.h` | `uint64_t` キーをバケットに直接格納 | 192 B (3 CL) | 64 ビット整数キー |

性能 (DRAM コールド、1000 万エントリ、パイプライン x8):

| バリアント | サイクル/op |
|-----------|------------|
| `rix_hash32` | ~58-60 |
| `rix_hash64` | ~62-66 |
| `rix_hash` (fp) | ~84-88 |

#### バケットスキャン性能 (`find_u32x16`、16 スロット/バケット、L2 ウォーム)

最内ホットパスはバケット内 16 スロットのフィンガープリント / キースキャンです。
128 バケットが L2 キャッシュに収まった状態でシングルコア測定:

| ビルドフラグ | 実行時レベル | cy/バケット | 備考 |
|------------|------------|-----------|------|
| `-msse4.2` のみ       | GEN (enable=0) | 36.0 | 純スカラーループ |
| `-msse4.2` のみ       | **SSE**        |  6.3 | XMM 128-bit — GEN 比 ×5.7 |
| `-mavx2 -msse4.2`    | SSE            |  6.0 | XMM 128-bit |
| `-mavx2 -msse4.2`    | **AVX2**       |  3.3 | YMM 256-bit — SSE 比 ×1.8 |
| `-mavx2 -msse4.2`    | GEN (enable=0) |  3.8 | コンパイラが AVX2 で自動ベクトル化 * |

\* `-mavx2` ビルドではコンパイラが GEN スカラーループを AVX2 で自動ベクトル化するため、
`enable=0` (GEN 強制) でも実際には AVX2 命令が実行されます。

**まとめ:** SSE レベルが最も有効なのは SSE4.2 はあるが AVX2 がない CPU
(Sandy Bridge / Ivy Bridge、2011〜2012 年世代) です。
AVX2 以降の CPU では AVX2 パスが最適です。

---

### RIX_HASH (フィンガープリント、可変長キー)

ノード構造体には、現在所属している bucket に対応する hash を保持する
`hash_field` 整数 field が必要です。`SLOT` variant では、さらに現在の
slot を保持する呼び出し側定義の整数 `slot_field` を持ちます。

```c
#include "rix/rix_hash.h"

/* 1. typedef 必須 -- マクロは裸の識別子を使用する */
typedef struct mynode mynode;
struct mynode {
    uint8_t  key[16];    /* キーフィールド (可変長) */
    uint32_t cur_hash;   /* current-bucket hash (名前は任意) */
    uint32_t value;
};

/* 2. ヘッドの宣言と API 生成 */
RIX_HASH_HEAD(myht);
RIX_HASH_GENERATE(myht, mynode, key, cur_hash, my_cmp_fn)
/* 型付き hash hook を渡す場合:
 * RIX_HASH_GENERATE_EX(myht, mynode, key, cur_hash,
 *                      my_cmp_fn, my_hash_fn)
 */

typedef struct mynode_slot mynode_slot;
struct mynode_slot {
    uint8_t  key[16];
    uint32_t cur_hash;
    uint16_t slot;
    uint16_t value;
};

RIX_HASH_HEAD(myht_slot);
RIX_HASH_GENERATE_SLOT(myht_slot, mynode_slot, key, cur_hash, slot, my_cmp_fn)

/* 3. 任意: このソースファイルで SIMD を有効化 */
rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

/* 4. 64 バイトアライメントのバケット配列を確保 */
struct rix_hash_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));
memset(buckets, 0, NB_BK * sizeof(*buckets));   /* 0 = 全スロット空 */
mynode *pool = calloc(N, sizeof(*pool));         /* 1-origin: pool[0] = インデックス 1 */

struct myht head;
RIX_HASH_INIT(&head, NB_BK);   /* NB_BK は 2 の冪乗であること */
```

#### 単発操作

```c
/* 挿入: NULL -> 成功; 別ポインタ -> 重複; elm 自身 -> テーブル満杯 */
mynode *dup = myht_insert(&head, buckets, pool, &pool[i]);

/* キーポインタで検索 */
mynode *hit = myht_find(&head, buckets, pool, key_ptr);

/* ノードポインタで削除 */
mynode *rem = myht_remove(&head, buckets, pool, &pool[i]);

/* 全エントリ巡回: cb が 0 を返す間継続、非 0 で停止 */
myht_walk(&head, buckets, pool, cb, arg);
```

#### パイプライン (ステージ型) 検索

複数のルックアップを同時進行させて DRAM レイテンシを隠蔽する:

```c
struct rix_hash_find_ctx_s ctx[4];
const void *keys[4] = { k0, k1, k2, k3 };
mynode *results[4];

/* ステージ 1: ハッシュ計算 + バケットプリフェッチ */
myht_hash_key4(ctx, &head, buckets, keys);
/* ステージ 2: フィンガープリントスキャン */
myht_scan_bk4 (ctx, &head, buckets);
/* ステージ 3: フルキー比較 */
myht_cmp_key4 (ctx, pool, results);
```

バルクバリアント: `_key1` / `_key2` / `_key4` / `_key8` (サフィックスが件数)。

#### `RIX_HASH_GENERATE` オプション

| バリアント | マクロ |
|-----------|-------|
| 外部リンケージ | `RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)` |
| 外部リンケージ + custom hash | `RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)` |
| 外部リンケージ + slot-aware remove | `RIX_HASH_GENERATE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn)` |
| 外部リンケージ + slot-aware remove + custom hash | `RIX_HASH_GENERATE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn)` |
| `static` リンケージ | `RIX_HASH_GENERATE_STATIC(name, type, key_field, hash_field, cmp_fn)` |
| `static` リンケージ + custom hash | `RIX_HASH_GENERATE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)` |
| `static` リンケージ + slot-aware remove | `RIX_HASH_GENERATE_STATIC_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn)` |
| `static` リンケージ + slot-aware remove + custom hash | `RIX_HASH_GENERATE_STATIC_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn)` |

`cmp_fn` シグネチャ: `int cmp_fn(const type *a, const type *b)` -- 等しければ 0 を返す。
`hash_fn` シグネチャ: `union rix_hash_hash_u hash_fn(const key_type *key, uint32_t mask)`。
`slot_field` は `[0, RIX_HASH_BUCKET_ENTRY_SZ - 1]` を表現できる
呼び出し側定義の整数型であれば十分です。

`SLOT` variant は次を維持します。

- `node->hash_field & mask == current_bucket`
- `buckets[current_bucket].idx[node->slot_field] == node_idx`

これにより `remove()` は direct-slot になり、非 `SLOT` variant のような
`idx[16]` scan を行いません。

---

### RIX_HASH32 (uint32_t キー)

ノード構造体に `hash_field` 不要。キー自体をバケットに直接格納するため、
`scan_bk` が 32 ビット完全一致比較を行う。

```c
#include "rix/rix_hash32.h"

typedef struct entry32 entry32;
struct entry32 {
    uint32_t key;    /* キーフィールド (名前は任意) */
    uint32_t value;
};

RIX_HASH32_HEAD(ht32);
RIX_HASH32_GENERATE(ht32, entry32, key)

rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

struct rix_hash32_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));
memset(buckets, 0, NB_BK * sizeof(*buckets));
entry32 *pool = calloc(N, sizeof(*pool));

struct ht32 head;
RIX_HASH32_INIT(&head, NB_BK);
```

#### API

```c
entry32 *ht32_insert(&head, buckets, pool, &pool[i]);
entry32 *ht32_find  (&head, buckets, pool, key_value);   /* 値渡しでキー指定 */
entry32 *ht32_remove(&head, buckets, pool, key_value);
int      ht32_walk  (&head, buckets, pool, cb, arg);

/* パイプライン検索 (rix_hash と同じステージ構成) */
struct rix_hash32_find_ctx_s ctx[4];
uint32_t keys[4] = { k0, k1, k2, k3 };
entry32 *results[4];

ht32_hash_key4(ctx, &head, buckets, keys);
ht32_scan_bk4 (ctx, &head, buckets);
ht32_cmp_key4 (ctx, pool, results);
```

---

### RIX_HASH64 (uint64_t キー)

RIX_HASH32 と同じインターフェースで `uint64_t` キーに対応。
バケットは 128 B ではなく 192 B (3 キャッシュライン)。

```c
#include "rix/rix_hash64.h"

typedef struct entry64 entry64;
struct entry64 {
    uint64_t key;
    uint32_t value;
};

RIX_HASH64_HEAD(ht64);
RIX_HASH64_GENERATE(ht64, entry64, key)

struct rix_hash64_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));

struct ht64 head;
RIX_HASH64_INIT(&head, NB_BK);

entry64 *ht64_insert(&head, buckets, pool, &pool[i]);
entry64 *ht64_find  (&head, buckets, pool, key_value);
entry64 *ht64_remove(&head, buckets, pool, key_value);
```

パイプラインステージは同じパターン: `ht64_hash_key4`, `ht64_scan_bk4`,
`ht64_cmp_key4`。

#### 全ハッシュバリアント共通の注意事項

- `rix_hash_arch_init(enable)` は任意。呼ばない場合、各ソースファイルは
  デフォルトで Generic パスのまま動作する。
- SIMD を有効にする場合は、ハッシュ操作を行う各ソースファイルで
  `rix_hash_arch_init(enable)` を呼ぶこと。
  `RIX_HASH_ARCH_AUTO` を渡すと利用可能な最良の SIMD を自動選択（推奨）。
  `RIX_HASH_ARCH_SSE` を渡すと SSE XMM 止まり（SSE4.2、AVX2 不使用）。
  `RIX_HASH_ARCH_AVX2` を渡すと AVX-512 が存在しても AVX2 に制限。
  `0` を渡すと Generic（スカラー）強制 — ベンチマーク比較用。
- バケット配列は**64 バイトアライメント**必須 (`aligned_alloc(64, ...)` または `posix_memalign`)。
- `NB_BK` は**2 の冪乗**かつ 2 以上であること。
- `insert` の戻り値:
  - `NULL` -- 挿入成功
  - 別のポインタ -- 重複 (同じキーがすでに存在)
  - `elm` 自身 -- テーブル満杯 (キックアウト深度上限に達した)
- 安全な充填率: 全スロット (`NB_BK * 16`) の **80% 以下**。

---

## フローキャッシュサンプル

`samples/` は `rix_hash` を基盤とした本番品質のフローキャッシュを提供します。
IPv4 専用・IPv6 専用・IPv4+IPv6 統合の 3 バリアントがあります。

3つのバリアントはすべて小さな fat binary
（`GEN` + `SSE` + `AVX2` + `AVX512` backend）としてビルドされます。
`*_cache_init()` は backend を明示指定でき、`AUTO` を指定すれば
実行時に利用可能な最適 backend を自動選択します。

### インクルード

```c
#include "flow_cache.h"   /* 傘ヘッダ: 3 バリアント全てをインクルード */
```

### バリアント

| バリアント | ヘッダ | キー | エントリサイズ |
|-----------|--------|------|--------------|
| `flow4` | `flow4_cache.h` | IPv4 5 タプル + vrfid (20 B) | 128 B (2 CL) |
| `flow6` | `flow6_cache.h` | IPv6 5 タプル + vrfid (44 B) | 128 B (2 CL) |
| `flowu` | `flow_unified_cache.h` | IPv4 or IPv6 + family フィールド (44 B) | 128 B (2 CL) |

### ライフサイクル

```c
/* リソース確保 */
unsigned nb_bk = flow_cache_nb_bk_hint(max_entries); /* ~50% 充填率でのサイジング */
struct rix_hash_bucket_s *buckets =
    aligned_alloc(64, nb_bk * sizeof(*buckets));
struct flow4_entry *pool =
    aligned_alloc(64, flow_cache_pool_size(max_entries, sizeof(*pool)));
unsigned pool_count = flow_cache_pool_count(max_entries); /* 2^n、最小 64 */

/* 初期化 */
struct flow4_cache fc;
flow4_cache_init(&fc, buckets, nb_bk, pool, pool_count,
                 FLOW_CACHE_BACKEND_AUTO, /* または GEN / SSE / AVX2 / AVX512 */
                 30000,           /* timeout_ms: 30 秒 */
                 NULL, NULL, NULL); /* init_cb, fini_cb, cb_arg (NULL = no-op fast path) */

printf("backend = %s\n",
       flow_cache_backend_name(flow4_cache_backend(&fc)));

/* 一括クリア (VRF 削除、インターフェースダウンなど) */
flow4_cache_flush(&fc);
```

サンプルのベンチマーク / デバッグ用途では、
`samples/test/flow_cache_test --backend auto|gen|sse|avx2|avx512`
でも backend を固定でき、各バリアントで実際に選ばれた backend を表示します。
さらに perf 向けの単一 workload 実行として `--bench-case`,
`--list-bench-cases`, `--json`, `--measure-child`,
`--pause-before-measure` も使えます。
packet loop 系の case には `pkt_hit_only`, `pkt_miss_only`,
`pkt_std`（90% hit / 10% miss）, `pkt_tight` があります。
組み込みの flow cache バリアントでは、hash stage に 20 バイト / 44 バイト
key 向けの固定長 CRC32 fast path も入り、ホットパスで汎用 `hash_bytes()`
ループを避けます。
insert の fast path では、duplicate / empty-slot scan の前に
2本の candidate bucket line も先に温めます。
miss が多い batch では、`flow*_cache_insert_batch()` が miss 群の hash を
先にまとめて作り、その plan から candidate bucket を温めて、各 key を
再 hash せずに insert 段へ渡します。
packet loop の perf case では、hit 確定後に後段更新用の CL1 も温め、
`cache_expire()` の呼び出し間隔を miss 率に応じて 1 / 2 / 4 / 8 batch に
後退させ、少数 miss で即 every-batch に戻りすぎないようにしています。

### `fcache2` (experimental redesign)

`samples/fcache2/` は、`fcache` の lookup pipeline を維持したまま、
entry layout と aging policy だけを切り替える flow4 専用プロトタイプです。

- 現在の flow4 entry は single-cache-line の 64B
- `RIX_HASH_GENERATE_SLOT_EX` による slot-aware remove
- `lookup_batch()` は `entry_idx` だけを返し、direct pointer
  や AP payload は返さない
- `fill_miss_batch()` は miss key を登録し、結果として得られた
  `entry_idx` を返す
- `fc2_flow4_cache_maintain()` が、caller が指定した bucket 範囲だけを
  idle/background 用に舐める reclaim API を提供する
- 現在の flow4 実装は `fcache` と同じ 4-stage batch lookup
  (`hash_key -> scan_bk -> prefetch_node -> cmp_key`) を使い、
  staged fingerprint scan を AVX2 helper に直接結び付けている
- `fill_miss_batch()` も `fcache` と同じ prehash + bucket prefetch 型の
  insert plan を使い、その上に local pressure relief を載せている
- insert 前 local pressure relief は candidate bucket のみを対象にし、
  bucket 内の occupied entry を staged prefetch して full-bucket 評価を行い、
  最古の expired victim を最大 1 件だけ選ぶ。bucket の密度閾値は
  global fill の上昇に合わせて `15/16 -> 14/16 -> 13/16` と前倒しされる
- idle/background maintenance は grouped bucket walk を使い、
  bucket prefetch と staged entry prefetch の上で各 bucket の expired entry
  を全件 `remove_at()` で回収する
- `fc2_flow4_cache_stats()` で lookup/fill と local relief / maintenance の
  call/check/eviction を確認できる
- bucket / entry の prefetch は `rix_hash` の共通 helper に寄せてあり、
  lookup / insert / maintenance で同じ語彙で追える

`fcache2` には built-in の hit-path governor も global expire walk も
ありません。pure search は `fcache` と同等に保ち、比較対象は aging
policy のみです。

- insert-bucket relief (fill 連動の `15/16 -> 14/16 -> 13/16` +
  sampled `8-of-16`)
- 明示的な idle/background maintenance (`16x1`)

現時点のスコープは意図的に絞ってあり、flow4 のみ、idx 指向の result API、
そして既存 `fcache` との横並び比較用です。

現在の `fc2` bench は、`tests/fcache2/fc2_bench` の引数付き mode を使うのが
前提です。

```sh
./tests/fcache2/fc2_bench rate_compare <desired_entries> <start_fill_pct> <hit_pct> <pps>
./tests/fcache2/fc2_bench rate_compare_timeout <desired_entries> <start_fill_pct> <hit_pct> <pps> <timeout_ms>
./tests/fcache2/fc2_bench rate_fc2_only <desired_entries> <start_fill_pct> <hit_pct> <pps>
./tests/fcache2/fc2_bench rate_trace_custom <desired_entries> <start_fill_pct> <hit_pct> <pps> <timeout_ms> <soak_mul> <report_ms> <fill0> <fill1> <fill2> <fill3> <k0> <k1> <k2> <k3> [kick_scale]
```

現時点の採用 trace profile は次です。

```text
thresholds: 70 / 73 / 75 / 77
kicks:      0 / 0 / 1 / 2
```

既知の検証組み合わせは次で一括再実行できます。

```sh
./samples/test/run_fc2_bench_matrix.sh flow4
make -C samples/test matrix VARIANT=flow4
make -C samples/fcache2 matrix
make -C tests/fcache2 matrix
```

### パケット処理ループ

```c
uint64_t now = flow_cache_rdtsc();

/* 1. パイプライン一括ルックアップ */
flow4_cache_lookup_batch(&fc, keys, nb_pkts, results);

/* 2. パケットごとの後処理 */
unsigned misses = 0;
for (unsigned i = 0; i < nb_pkts; i++) {
    if (results[i]) {
        flow4_cache_touch(results[i], now);   /* ヒット: タイムスタンプを更新 */
        /* 直接 userdata を更新: MY_PAYLOAD(results[i])->packets++ 等 */
    } else {
        flow4_cache_insert(&fc, &keys[i], now); /* ミス: init_cb が userdata を初期化 */
        misses++;
    }
}

/* 3. 適応的タイムアウト調整 */
flow4_cache_adjust_timeout(&fc, misses);

/* 4. エージングによるエビクション */
flow4_cache_expire(&fc, now);
/* flow4_cache_expire() は高圧時に内部で 2stage へ自動切替する。
   flow4_cache_expire_2stage() で明示強制も可能。 */
```

### API 一覧

```c
/* ライフサイクル */
void     flow4_cache_init (fc, buckets, nb_bk, pool, pool_count, backend,
                           timeout_ms, init_cb, fini_cb, cb_arg);
void     flow4_cache_flush(fc);

/* backend 選択 / 確認 */
typedef enum {
    FLOW_CACHE_BACKEND_AUTO,
    FLOW_CACHE_BACKEND_GEN,
    FLOW_CACHE_BACKEND_SSE,
    FLOW_CACHE_BACKEND_AVX2,
    FLOW_CACHE_BACKEND_AVX512,
} flow_cache_backend_t;
flow_cache_backend_t flow4_cache_backend(fc);    /* 初期化後に実際に選ばれた backend */
const char *flow_cache_backend_name(flow_cache_backend_t backend);

/* サイジングヘルパー */
unsigned flow_cache_pool_count(max_entries);              /* cache_init 用エントリ数 (2^n、最小 64) */
size_t   flow_cache_pool_size (max_entries, entry_size);  /* aligned_alloc 用バイト数 */
unsigned flow_cache_nb_bk_hint(max_entries);              /* ~50% 充填率向けバケット数 (2^n) */

/* cache_init の入力前提:
 *   pool_count は 2^n かつ 64 以上
 *   要求した backend が未対応なら GEN にフォールバック */

/* ルックアップ */
void                flow4_cache_lookup_batch(fc, keys, nb_pkts, results);
unsigned            flow4_cache_lookup_touch_batch(fc, keys, nb_pkts, now, results);
struct flow4_entry *flow4_cache_find        (fc, key);   /* 単発、パイプラインなし */

/* 変更 */
struct flow4_entry *flow4_cache_insert(fc, key, now);
void                flow4_cache_remove(fc, entry);

/* ヒット処理 */
void flow4_cache_touch(entry, now);   /* タイムスタンプを更新; 後で userdata を更新 */

/* エージング */
void flow4_cache_expire         (fc, now);
void flow4_cache_expire_2stage  (fc, now);
void flow4_cache_adjust_timeout (fc, misses);

/* 統計 */
void     flow4_cache_stats     (fc, out);   /* struct flow_cache_stats に書き込む */
unsigned flow4_cache_nb_entries(fc);

/* タイムスタンプ (バリアント共通) */
uint64_t flow_cache_rdtsc(void);
uint64_t flow_cache_calibrate_tsc_hz(void);
uint64_t flow_cache_ms_to_tsc(tsc_hz, ms);
```

`flow4` を `flow6` または `flowu` に置き換えると他のバリアントになります。
3 バリアントは同一の API を持ちます。

### FC_CALL -- バリアント汎用呼び出しマクロ

テンプレート生成 API はシンボルが追いにくいため、`FC_CALL` マクロを使うと
バリアントを直接指定して呼び出せます。

```c
#include "flow_cache.h"

/* FC_CALL(prefix, suffix) は prefix##_##suffix に展開される */
FC_CALL(flow4, cache_init)(&fc, buckets, nb_bk, pool, pool_count,
                           FLOW_CACHE_BACKEND_AUTO, timeout_ms,
                           init_cb, fini_cb, cb_arg);
FC_CALL(flow4, cache_lookup_batch)(&fc, keys, nb_pkts, results);
FC_CALL(flow4, cache_insert)(&fc, &keys[i], now);
FC_CALL(flow4, cache_touch)(results[i], now);
FC_CALL(flow4, cache_adjust_timeout)(&fc, misses);
FC_CALL(flow4, cache_expire)(&fc, now);
FC_CALL(flow4, cache_backend)(&fc);

/* prefix はマクロトークンでも可 -- 先に展開されてから結合される */
#define MY_FC flow6
FC_CALL(MY_FC, cache_init)(&fc, ...);   /* -> flow6_cache_init(...) */
```

### 統計構造体

```c
struct flow_cache_stats {
    uint64_t lookups;     /* lookup_batch 呼び出し総数 */
    uint64_t hits;        /* キャッシュヒット数 */
    uint64_t misses;      /* キャッシュミス数 */
    uint64_t inserts;     /* 挿入エントリ数 */
    uint64_t evictions;   /* タイムアウトによるエビクション数 */
    uint64_t removes;     /* 明示的な削除数 (remove/flush) */
    uint32_t nb_entries;  /* 現在のエントリ数 */
    uint32_t max_entries; /* プール容量 */
};
```

### 性能 (実測、DRAM コールド)

| 操作 | サイクル数 |
|------|-----------|
| パイプライン一括ルックアップ | 150-220 cy/pkt |
| エクスパイア (分散後) | ~10-20 cy/pkt |

---

## ビルド

C11 必須。推奨フラグ:

```sh
# AVX2（推奨デフォルト）
cc -std=gnu11 -O3 -mavx2 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror  \
   -I/path/to/librix/include       \
   your_sources.c

# AVX-512
cc -std=gnu11 -O3 -mavx512f -mavx2 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror             \
   -I/path/to/librix/include                  \
   your_sources.c

# Generic スカラーのみ（SIMD サーチなし、CRC32C ハッシュは維持）
cc -std=gnu11 -O3 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror \
   -I/path/to/librix/include      \
   your_sources.c
```

付属のテスト・ベンチマークスイートでは `SIMD=` 変数で一括切り替え可能:

```sh
make SIMD=avx2    # デフォルト
make SIMD=avx512
make SIMD=gen
```

コンパイラと最適化レベルも上書き可能:

```sh
make CC=gcc   OPTLEVEL=3
make CC=clang OPTLEVEL=3
```

現状ツリーはこの条件で GCC / Clang の両方でビルドできる前提です。

`samples/fcache` の lookup pipeline 定数の既定値は
`FLOW_CACHE_LOOKUP_STEP_KEYS=16`,
`FLOW_CACHE_LOOKUP_AHEAD_STEPS=8`,
`FLOW_CACHE_LOOKUP_AHEAD_KEYS=128` です。
`AHEAD_KEYS` は hardware prefetch 回数ではなく、
software pipeline の段間距離です。
tuning 比較では `EXTRA_CFLAGS` で上書きできます:

```sh
make -C samples/fcache static CC=gcc OPTLEVEL=3 \
     EXTRA_CFLAGS='-DFLOW_CACHE_LOOKUP_STEP_KEYS=8 -DFLOW_CACHE_LOOKUP_AHEAD_KEYS=64'
make -C samples/test all CC=gcc OPTLEVEL=3 \
     EXTRA_CFLAGS='-DFLOW_CACHE_LOOKUP_STEP_KEYS=8 -DFLOW_CACHE_LOOKUP_AHEAD_KEYS=64'
```

開発時のサニタイザ:

```sh
-fsanitize=address,undefined -fno-omit-frame-pointer
```

---

## 並行性

librix は内部同期を持ちません。全構造体はシングルスレッド、または独自ロック
(futex, pthread mutex, プロセス共有プリミティブ等) 下での
マルチリーダ/シングルライタに適しています。

ロックフリー / RCU 動作はスコープ外であり、追加設計が必要です。

---

## テスト

```sh
# キュー / ツリー / ハッシュ ユニットテスト
make -C tests

# フローキャッシュ 正確性テスト + ベンチマーク
make -C samples
./samples/test/flow_cache_test -n 1000000

# perf stat / perf record 向けの単一 workload 実行
./samples/test/flow_cache_test --bench-case flow4:pkt_std --backend avx2 --json

# measured loop 直前で一度停止し、その後に perf attach
./samples/test/flow_cache_test --bench-case flow4:pkt_hit_only \
    --backend avx2 --pause-before-measure --json

# perf stat wrapper
make -C samples/test perf PERF_CASE=flow4:pkt_std PERF_BACKEND=avx2
```

テストカバレッジ:

- 空 / シングルトン / 複数要素の各操作の遷移
- 全挿入・削除バリアント
- 削除しながらの安全な反復
- Red-Black 不変条件 (ルートが黒、赤-赤なし、黒高さ一致)
- ハッシュテーブル: 重複検出、パイプラインステージ正確性、walk カウント
- ファジング: ランダム insert/find/remove をリファレンスモデルと照合
- フローキャッシュ: find, remove, flush, expire, batch lookup, insert 枯渇

---

## ライセンス

BSD 3-Clause。[LICENSE](LICENSE) を参照。

---

## 謝辞

キューおよびツリー API は BSD `sys/queue.h` / `sys/tree.h` インターフェースを
踏襲し、生ポインタをインデックスに置き換えることで堅牢な共有メモリ展開を
実現しています。

カッコーハッシュテーブルは XOR ベースの 2 バケット方式を採用し、
プリフェッチ駆動のステージ型ルックアップにより O(1) 償却挿入を達成しています。

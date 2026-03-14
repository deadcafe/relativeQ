# フローキャッシュ設計書

## 1. 概要

高性能パケット処理向けフローキャッシュ。
スローパス検索（ACL、QoS等）の結果を5-tuple + vrfidをキーとして
キャッシュする。librix のインデックスベースデータ構造
（`rix_hash.h`）のデモンストレーションとして設計。

### 利用シナリオ

- L3/L4 妥当性チェック直後に呼び出し
- パケットはベクタ処理（バッチあたり約256パケット）
- DRAMレイテンシを隠蔽するパイプライン方式
- スレッドごとのインスタンス（ロックフリー、同期不要）
- 対象: VPP オリジナルプラグイン、共有メモリアプリケーション

### 性能目標と実測結果

| 操作 | 条件 | 目標 (cy/op) | 実測 (cy/op) |
|---|---|---|---|
| find（パイプライン） | DRAM コールドバケット | ~100 | 40-143 |
| insert（ミス時） | バケット L2 ウォーム（find後） | ~60 | 69-92 |
| touch（ヒット時） | ノードウォーム（プリフェッチ後） | ~10 | ~10 |
| expire 償却/pkt | 適応的スキャン（64-1024/batch） | ~15-25 | 13-40 |
| pkt ループ（標準） | lookup+insert+expire | ~200-250 | 150-215 |

#### プールサイズ別 lookup 性能（IPv4, batch=256, DRAM-cold）

| Pool | メモリ | batch lookup | single find | パイプライン効果 |
|---|---|---|---|---|
| 1K | 144KB (L2) | 40 cy/key | 111 cy/key | 2.8x |
| 100K | 18MB (LLC境界) | 47-60 cy/key | 322 cy/key | 6.0x |
| 1M | 144MB (DRAM) | 100-132 cy/key | 953 cy/key | 7.5x |
| 4M | 576MB (DRAM) | 115-143 cy/key | 1066 cy/key | 8.1x |

プールが大きくなるほどパイプラインの効果が顕著。1K→4Mで4000倍の
サイズ差に対し、batch lookupは40→135 cy/key（3.4倍）に留まる。

#### プールサイズ別 pkt processing loop（IPv4, 標準サイジング）

| Pool | lookup+insert | expire/pkt | 合計 |
|---|---|---|---|
| 1K | 119 cy/pkt | 13 cy/pkt | 132 cy/pkt |
| 1M | 150 cy/pkt | 37 cy/pkt | 187 cy/pkt |
| 4M | 170 cy/pkt | 36 cy/pkt | 206 cy/pkt |

### Xeon スループット試算

2 GHz Xeonで200 cy/pkt = コアあたり10 Mpps。
2 Mppsで1コアの約20%を使用 — 大半のデプロイメントに十分。

## 2. 3つの動作モード

フローキャッシュはデプロイメントの要件に合わせて3つのバリアントを提供する。
すべてのバリアントはマクロテンプレートにより実装を共有する
（§12 テンプレートアーキテクチャ参照）。

### 2.1 分離テーブル（IPv4専用またはIPv6専用）

| | `flow4_cache` | `flow6_cache` |
|---|---|---|
| ヘッダ | `flow4_cache.h` | `flow6_cache.h` |
| キー構造体 | `flow4_key` (20B) | `flow6_key` (44B) |
| エントリサイズ | 128B (2 CL) | 128B (2 CL) |
| キー比較 | 6フィールド等値比較 | memcmp×2 + 4フィールド |
| 用途 | IPv4専用環境 | IPv6専用環境 |

分離テーブルの根拠:
- バリアントごとに固定キーサイズなので、コンパイラが cmp_fn を最適化可能
- ホットパスにv4/v6分岐なし
- 均一要素プール（エントリごとのサイズ区別不要）
- IPv4専用: キーは20Bで、ルックアップごとに24Bのハッシュ入力を節約

### 2.2 統合テーブル（デュアルスタック）

| | `flowu_cache` |
|---|---|
| ヘッダ | `flow_unified_cache.h` |
| キー構造体 | `flowu_key` (44B) |
| エントリサイズ | 128B (2 CL) |
| キー比較 | `memcmp(a, b, 44)` |
| 用途 | デュアルスタック（IPv4 + IPv6を単一テーブルで） |

統合キーはオフセット0に `family` フィールド（1バイト）を持つ:

```c
struct flowu_key {
    uint8_t  family;       /*  1B: FLOW_FAMILY_IPV4(4) / IPV6(6) */
    uint8_t  proto;        /*  1B */
    uint16_t src_port;     /*  2B */
    uint16_t dst_port;     /*  2B */
    uint16_t pad;          /*  2B */
    uint32_t vrfid;        /*  4B */
    union {                /* 32B */
        struct { uint32_t src, dst; uint8_t _pad[24]; } v4;
        struct { uint8_t  src[16], dst[16]; }           v6;
    } addr;
};  /* 44B total */
```

特性:
- IPv4エントリは未使用の24バイト（`_pad`）をゼロ埋め
- `family` はキーの一部であるため、同一ポート/プロトコルのv4/v6は衝突しない
- `memcmp(a, b, 44)` で両ファミリを単一比較
- ヘルパー関数 `flowu_key_v4()` / `flowu_key_v6()` でキー構築
- プール容量 = v4 + v6 合計 — 個別のサイジング不要
- 単一ハッシュテーブル、単一バケット配列、単一フリーリスト

### 2.3 性能比較

1Kエントリ（タイトサイジング、pktループ）で計測:

| 指標 | IPv4 | IPv6 | 統合 |
|---|---|---|---|
| lookup（100%ヒット） | 44 cy/key | 54 cy/key | 52 cy/key |
| pkt ループ | 134 cy/pkt | 143 cy/pkt | 146 cy/pkt |
| expire 償却 | 9.0 cy/pkt | 7.8 cy/pkt | 8.6 cy/pkt |

統合はIPv6専用の約10%以内の性能。IPv4キーの24Bパディングによる
ハッシュ/比較オーバーヘッドは無視できる程度で、パイプラインが
DRAMレイテンシを隠蔽するため — ボトルネックは計算ではなく
メモリアクセスにある。

### 2.4 バリアントの選択

- **IPv4専用** (`flow4_cache`): レガシーまたはIPv4限定のネットワークセグメント。
  最小キー（20B）、キーあたりのハッシュ計算が最速。
- **IPv6専用** (`flow6_cache`): IPv6限定セグメント。
- **統合** (`flowu_cache`): デュアルスタック。単一プールによりメモリ管理が
  簡素化され、v4/v6個別のキャパシティプランニングが不要。
  新規デプロイメントに推奨。

## 3. キー構造体

### 3.1 IPv4 フローキー（20バイト）

```c
struct flow4_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
};
```

### 3.2 IPv6 フローキー（44バイト）

```c
struct flow6_key {
    uint8_t  src_ip[16];
    uint8_t  dst_ip[16];
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
};
```

### 3.3 統合フローキー（44バイト）

上記 §2.2 を参照。

## 4. ノードレイアウト

3つのバリアントすべてが同じ128バイト（2 CL）のエントリレイアウトを使用する。

### 4.1 設計原則

- CL0（バイト0-63）: パイプラインルックアップ + エクスパイアスキャンでアクセスされるフィールド（ホット）
- CL1（バイト64-127）: ヒット時 / スローパスでアクセスされるフィールド（ウォーム）
- ノードは64バイト境界にアライン（`__attribute__((aligned(64)))`）
- `last_ts` と `free_link` はCL0に配置 — エクスパイアスキャンとフリーリスト操作が1CL内で完結

### 4.2 エントリレイアウト（全バリアント共通）

```
CL0 (64 bytes) — lookup + expire ホットパス:
  key           20B (v4) / 44B (v6, unified)
  cur_hash       4B   O(1)除去用 hash_field
  last_ts        8B   最終アクセスTSC（0 = 無効）
  free_link      4B   SLISTエントリ（フリーリストインデックス）
  reserved      28B (v4) / 4B (v6, unified)   64Bへのパディング

CL1 (64 bytes) — ヒット時 / スローパス:
  action         4B   キャッシュされたACL結果
  qos_class      4B   キャッシュされたQoSクラス
  packets        8B   パケットカウンタ
  bytes          8B   バイトカウンタ
  reserved      40B   64Bへのパディング
```

合計: 128バイト = 2キャッシュライン（全3バリアント共通）。

#### CL0/CL1 配置の根拠

- **`last_ts` をCL0に配置**: expire スキャンは `last_ts` のみ読み取って
  期限切れ判定を行う。CL0にあることで、期限切れでないエントリに対して
  CL1へのアクセスが不要。SW プリフェッチとの相性も良い。
- **`free_link` をCL0に配置**: エントリはハッシュテーブルかフリーリストの
  いずれかに存在する。`free_link` はフリーリスト操作時のみ使用され、
  `last_ts` と同じCLにあることで操作が1CL内で完結する。
- **`action`/`qos_class` をCL1に配置**: ヒット時のアクション適用とスロー
  パスでの更新はCL1アクセスとなるが、これらはシーケンシャル処理であり
  パイプラインのボトルネックにはならない。

IPv6キー（44B）はCL0に4Bのリザーブ付きで収まる。
IPv4キー（20B）はCL0に28Bのリザーブ付きで収まる — パディングは
多いがエントリサイズは同じなので、プールメモリは同一。

## 5. ハッシュテーブル設定

- ハッシュバリアント: `rix_hash.h` フィンガープリント（任意キーサイズ、cmp_fn）
- `RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)`
- `hash_field`（`cur_hash`）によりO(1)除去が可能（再ハッシュ不要）
- キックアウトはXORトリック使用: `alt_bk = (fp ^ hash_field) & mask`
- バケット: 128バイト（2 CL）、16スロット/バケット
- ランタイムSIMDディスパッチ: Generic / AVX2 / AVX-512

### 5.1 テーブルサイジング

自動サイジングは約50%充填を目標: `nb_bk = next_pow2(max_entries * 2 / 16)`

| max_entries | nb_bk | 総スロット数 | バケットメモリ | ノードメモリ (128B) | 合計 |
|---|---|---|---|---|---|
| 100K | 16K | 256K | 2 MB | 12.5 MB | 14.5 MB |
| 1M | 128K | 2M | 16 MB | 128 MB | 144 MB |
| 4M | 512K | 8M | 64 MB | 512 MB | 576 MB |
| 10M | 2M | 32M | 256 MB | 1.25 GB | 1.5 GB |

スレッドごとに割り当て。

### 5.2 bk[0] 配置率 vs 充填率

充填率75%以下では、98%以上のエントリがプライマリバケット（bk[0]）に
配置される。`scan_bk` はこれらのエントリに対して1 CLのみアクセスし、
DRAMアクセスを最小化する。

| 充填率 | bk[0] % | bk[1] % |
|---|---|---|
| 10-50 | 99.5-100 | 0-0.5 |
| 60 | 99.7 | 0.3 |
| 70 | 99.2 | 0.8 |
| 75 | 98+ | ~2 |
| 80 | 96 | 4 |
| 90 | 91 | 9 |

これが75%閾値が重要な理由である（§8参照）。

## 6. パイプライン設計

### 6.1 ルックアップパイプライン

`rix_hash.h` のステージドfindは4段構成:

```
Stage 0: hash_key       ハッシュ計算、bucket[0] CL0をプリフェッチ
Stage 1: scan_bk        バケット[0]内でSIMDフィンガープリントスキャン
Stage 2: prefetch_node  候補ノードの flow_entry CL0をプリフェッチ
Stage 3: cmp_key        完全キー比較、ヒット/NULLを返す
```

### 6.2 N-aheadソフトウェアパイプライン

nb_pktsパケットをBATCH幅のステップで、KPDバッチ先行して処理:

```
BATCH = 8          (ステップあたりの処理キー数)
KPD   = 8          (バッチ単位のパイプライン深度)
DIST  = BATCH*KPD  (= 64キー先行)
```

```c
for (i = 0; i < nb_pkts + 3 * DIST; i += BATCH) {
    if (i                  < nb_pkts) hash_key_n      (i,        BATCH);
    if (i >= DIST       && ...)       scan_bk_n       (i-DIST,   BATCH);
    if (i >= 2*DIST     && ...)       prefetch_node_n (i-2*DIST, BATCH);
    if (i >= 3*DIST     && ...)       cmp_key_n       (i-3*DIST, BATCH);
}
```

### 6.3 タイムスタンプ

- TSC（`rdtsc`）はルックアップループ**前に1回だけ**読み取り
- ベクタ内の全パケットに同じ `now` 値を使用
- パケットごとのTSC読み取りなし
- 初期化時に50 msスリープで校正

## 7. ヒット / ミス処理

### 7.1 find時のミス: 空アクションでの即時挿入

キャッシュミス時、新規エントリを**即座に**挿入する
（スローパスへの遅延なし）。エントリは `action = FLOW_ACTION_NONE` で作成される。

根拠:
- ルックアップ直後で、バケットのキャッシュラインはまだL2にある
  （約5 cyアクセス vs 約200 cy DRAM）。挿入コストが大幅に低下。
- フローはキャッシュ内に存在するようになる。同一フローの後続
  パケットは即座にヒットする（ACLがアクションを設定する前であっても）。
- スローパスのACL/QoSルックアップは後で実行され、既存エントリの
  action/qos_classフィールドを**インプレースで更新**する。

### 7.2 処理フロー

```
process_vector(pkts[256]):

  now = rdtsc()                             // ベクタあたり1回のTSC読み取り

  Phase 1: パイプラインルックアップ          // ~55-73 cy/pkt
    cache_lookup_batch(keys, results)

  Phase 2: 結果処理                          // シーケンシャル
    misses = 0
    for each pkt:
      if hit:
        cache_touch(entry, now, len)        // ~10 cy (CL1 ウォーム)
        if entry->action != NONE:
          apply cached action/qos           // ファストパス
        else:
          enqueue to slow-path              // アクション未設定
      if miss:
        cache_insert(fc, key, now)          // ~60 cy (バケット L2 ウォーム)
        misses++                            // 挿入は常に成功（3段フォールバック）
        enqueue to slow-path                // ACL/QoSルックアップ必要

  Phase 3: タイムアウト調整 + エクスパイア   // ~13-40 cy/pkt 償却
    cache_adjust_timeout(fc, misses)        // ミスレート駆動タイムアウト調整
    cache_expire(fc, now)                   // 適応的スキャン（常時実行）

  (スローパス: ACL/QoSルックアップ、その後 entry->action/qos_class を更新)
```

Phase 3 は無条件で毎バッチ実行する。スキャン量は充填率に応じて
自動調整される（§8.2参照）。充填率が低い場合はスキャン量も少なく
（64エントリ/バッチ）、コストは最小限に抑えられる。

### 7.3 スローパス更新

ACL/QoSルックアップ完了後、既存のキャッシュエントリを
インプレースで更新:

```c
cache_update_action(entry, action, qos_class);
```

これはCL0への書き込み。ハッシュテーブルの操作は不要。

## 8. エビクション戦略

### 8.1 LRU用TAILQを使わない理由

TAILQベースのLRUは毎ヒット時にprev/nextノードへの書き込みが必要。
これらの書き込みはランダムなコールドキャッシュライン（prev/nextの
隣接ノード）にアクセスし、パイプライン効率を破壊する。

代わりに: **タイムスタンプのみのアプローチ**を採用。

- ヒット時: ヒットノードのCL0に `last_ts = now` を書き込む（既にウォーム）
- ヒットパスでのリンクリスト操作なし
- エビクションはスキャンにより犠牲者を探索

### 8.2 適応的エクスパイア（常時実行）

毎バッチ後にエクスパイアを無条件実行する。スキャン量と実効タイムアウトは
ワークロードに応じて自動調整される。

#### 充填率駆動スキャン量（16段階レベル）

プールの充填率に基づきスキャン量を自動調整する:

```c
static inline unsigned
cache_expire_level(const struct cache *fc)
{
    unsigned nb   = fc->ht_head.rhh_nb;
    unsigned half = fc->max_entries >> 1;
    if (nb <= half) return 0;
    unsigned excess = nb - half;
    unsigned shift  = __builtin_ctz(fc->max_entries) - 4;
    unsigned level  = excess >> shift;
    return (level > 15) ? 15 : level;
}

static inline unsigned
cache_expire_scan(const struct cache *fc)
{
    unsigned level = cache_expire_level(fc);
    if (level >= 4) return FLOW_CACHE_EXPIRE_SCAN_MAX;  /* 1024 */
    return FLOW_CACHE_EXPIRE_SCAN_MIN << level;         /* 64..512 */
}
```

| 充填率 | レベル | スキャン量/バッチ |
|---|---|---|
| ≤ 50% | 0 | 64 |
| 50-56% | 1 | 128 |
| 56-62% | 2 | 256 |
| 62-69% | 3 | 512 |
| ≥ 69% | 4+ | 1024 |

充填率50%以下ではスキャン64/バッチ — コストは最小限。充填率が上がると
スキャン量が指数的に増加し、積極的にエントリを回収する。

#### ミスレート駆動タイムアウト調整

新規フロー（ミス）の発生率に応じて実効タイムアウトを自動調整する。
高ミスレート → タイムアウト短縮 → エビクション促進。
低ミスレート → タイムアウト回復 → キャッシュ効果向上。

```c
static inline void
cache_adjust_timeout(struct cache *fc, unsigned misses)
{
    uint64_t eff  = fc->eff_timeout_tsc;
    uint64_t base = fc->timeout_tsc;
    uint64_t min_to = fc->min_timeout_tsc;

    /* 減衰: ミス数に比例 */
    if (misses > 0) {
        uint64_t decay = (eff * misses) >> DECAY_SHIFT;  /* 1/4096 per miss */
        if (decay == 0) decay = 1;
        if (eff > min_to + decay)
            eff -= decay;
        else
            eff = min_to;
    }

    /* 回復: ベースタイムアウトへ緩やかに復帰 */
    eff += (base - eff) >> RECOVER_SHIFT;  /* 1/256 per batch */

    fc->eff_timeout_tsc = eff;
}
```

定常状態では減衰と回復が平衡し、安定した実効タイムアウトに収束する。

パラメータ:
- `FLOW_CACHE_TIMEOUT_DECAY_SHIFT = 12`: 減衰感度（batch=256 → log2(256)+4）
- `FLOW_CACHE_TIMEOUT_RECOVER_SHIFT = 8`: 回復速度（1/256 per batch）
- `FLOW_CACHE_TIMEOUT_MIN_MS = 1000`: 最小タイムアウト下限（1.0秒）

#### エクスパイア本体（単段 / 2段パイプライン）

```c
void cache_expire(struct cache *fc, uint64_t now)
{
    unsigned max_scan = cache_expire_scan(fc);
    /* プールをカーソル位置からスキャン、期限切れをフリーリストに回収 */
    for (unsigned i = 0; i < max_scan; i++) {
        /* SW prefetch: CL0 pf_dist先をプリフェッチ */
        prefetch(&pool[(cursor + i + pf_dist) & mask]);
        entry = &pool[(cursor + i) & mask];
        if (entry->last_ts == 0) continue;        /* 空きスロット */
        if (now - entry->last_ts <= timeout) continue;  /* 存命 */
        hash_remove(entry);
        entry->last_ts = 0;
        free_list_push(entry);
    }
    cursor += max_scan;
}
```

2段パイプラインバリアント（`cache_expire_2stage`）は中距離（`pf_dist/2`）で
期限切れ候補のバケットをプリフェッチし、`hash_remove` 時のDRAMアクセスを
削減する。プールがLLCに収まらず、かつエビクション率が高い場合に有効。

#### スイープカバレッジ

- カーソルは1回の呼び出しで `max_scan` 分進む（エビクション数に関係なく）
- プール全体の走査周期 = max_entries / (scan × batches_per_sec)
- 2 Mpps、batch=256の場合:
  - レベル0（scan=64）: 1Mプール → 2.0秒で全走査
  - レベル4（scan=1024）: 1Mプール → 0.13秒で全走査
- タイムアウトN秒の場合、スイープ周期 < Nが必要。
  最小タイムアウト1.0秒に対し、レベル0でも十分なカバレッジ。

### 8.3 挿入保証（3段フォールバック）

キャッシュ `insert` は常に成功する（NULLを返さない）。
3段のフォールバックにより空きエントリを確保する:

```
1. フリーリスト    → SLIST_FIRST で O(1) 取得
2. evict_one      → カーソルスキャン（1/8プール上限）で期限切れを探索
3. evict_bucket_oldest → 当該キーのバケット内最古エントリを強制エビクト
```

#### evict_one（期限切れスキャン、1/8バウンド）

```c
static struct entry *
cache_evict_one(struct cache *fc, uint64_t now)
{
    const unsigned max_scan = fc->max_entries >> 3;  /* 1/8 of pool */
    for (unsigned i = 0; i < max_scan; i++) {
        prefetch(&pool[(cursor + pf_dist) & mask]);
        entry = &pool[cursor];
        cursor = (cursor + 1) & mask;
        if (entry->last_ts == 0) continue;
        if (now - entry->last_ts <= timeout) continue;
        hash_remove(entry);
        return entry;
    }
    return NULL;  /* 1/8スキャン内に期限切れなし */
}
```

スキャンを1/8に制限することで最悪ケースのコストをバウンドする:

| Pool | フルスキャン | 1/8スキャン |
|---|---|---|
| 100K | 33,207 cy/pkt | 3,988 cy/pkt |
| 1M | - | 適正 |
| 4M | - | 適正 |

フルスキャンでは100Kプールで33K cy/pktの異常値が発生していた。
全エントリが存命の場合、131K×128B = 16MBのメモリをスキャンするため。
1/8バウンドにより16Kエントリ = 2MBに削減される。

#### evict_bucket_oldest（強制エビクト、最終手段）

evict_oneでも空きが見つからない場合、挿入予定キーのハッシュから
対象バケット（bk0, bk1）を特定し、両バケット内で最も古いエントリを
強制エビクトする:

```c
static struct entry *
cache_evict_bucket_oldest(struct cache *fc, const struct key *key)
{
    /* キーをハッシュしてbk0, bk1を特定 */
    hash = crc32(key);
    find bk0, bk1 from hash;

    /* 全32スロット（16×2バケット）をスキャンし最古を発見 */
    oldest = NULL;
    for each slot in bk0, bk1:
        if entry->last_ts < oldest_ts:
            oldest = entry;

    hash_remove(oldest);
    return oldest;
}
```

この関数はプールエントリとバケットスロットの**両方を同時に解放**する。
そのため後続の `ht_insert` はファストパス（カッコウ不要）で完了する。

実行頻度: 極めてまれ。evict_oneが1/8スキャンで期限切れを1件も
見つけられなかった場合のみ発生。

### 8.4 ミスレートのシミュレーション結果

`expire_sim.py` によるシミュレーション（2 Mpps、base_timeout=5.0s、
min_timeout=1.0s）:

#### プールサイズ別定常状態

| miss% | new/s | 100K fill% | 100K TO | 1M fill% | 1M TO | 4M fill% | 4M TO |
|---|---|---|---|---|---|---|---|
| 5% | 39K | 19.5% | 5.0s | 19.5% | 5.0s | 4.9% | 5.0s |
| 10% | 78K | 39.1% | 5.0s | 39.1% | 5.0s | 9.8% | 5.0s |
| 20% | 156K | 78.1% | 5.0s | 78.1% | 5.0s | 19.5% | 5.0s |
| 30% | 234K | 100.0% | 2.2s | 100.0% | 4.5s | 29.3% | 5.0s |
| 50% | 390K | 100.0% | 1.3s | 100.0% | 2.6s | 48.8% | 5.0s |

実効タイムアウトはプールサイズに依存しない（ミスレートのみで決定）。
ただしプールが飽和すると充填率駆動スキャンが加わり、タイムアウトは
さらに短縮される。

#### 主な知見

- **5-10% ミス**: 通常運用。全プールサイズで充填率低く、タイムアウト最大。
- **20% ミス**: 100Kプールは78%充填（まだ安定）。
- **30%+ ミス**: 100K/1Mプールは飽和、タイムアウトが自動短縮。
  4Mプールは余裕あり。
- **自然なフィードバック**: 高ミスレート → ACLスローパスコスト増 →
  実効pps低下 → 新規フロー/秒低下 → 充填率低下。

#### プールサイジングガイドライン

```
pool_size ≥ max_new_flow_rate × base_timeout × 2
```

| 条件 | 推奨プールサイズ |
|---|---|
| 2Mpps, 10%ミス, 5s TO | ≥ 780K → 1M |
| 2Mpps, 20%ミス, 5s TO | ≥ 1.56M → 2M |
| 2Mpps, 30%ミス, 5s TO | ≥ 2.34M → 4M |

一般的なミスレート参考値:
- 通常運用: 5-10%
- ピーク時: 15-20%
- 攻撃時: 30%+

### 8.5 タイムアウトパラメータ

```c
void cache_init(..., uint64_t timeout_ms);
```

タイムアウト値はランタイムパラメータ。
適切な値はワークロードに依存:
- 短寿命フロー（Web）: 1-5秒
- 長寿命フロー（ストリーミング）: 30-60秒
- 0 = エクスパイアなし（`timeout_tsc = UINT64_MAX`）

最小タイムアウト下限は `FLOW_CACHE_TIMEOUT_MIN_MS = 1000`（1.0秒）。
これ以下ではキャッシュ効果が期待できないため、ミスレートが極端に
高くても1.0秒を下回らない。

## 9. フリーリスト管理

- entry[max_entries] の事前割り当てプール（max_entries は2のべき乗）
- 初期化時: 全エントリをSLISTフリーリストにプッシュ
- insert: フリーリストからポップ → 空なら3段フォールバック（§8.3参照）
- expire/evict: フリーリストにプッシュバック
- `free_link` フィールドはCL0に配置 — `last_ts`（0 = 無効）と同じCL内で、
  フリーリスト操作が1CL内で完結する
- エントリはハッシュテーブルかフリーリストのいずれかに存在し、
  両方に同時に存在することはない

## 10. API

3つのバリアントすべてがマクロテンプレートにより同じAPI形式を公開する。
関数名にはバリアントプレフィックスを使用: `flow4_`、`flow6_`、または `flowu_`。

```c
/* 初期化 — 呼び出し元が事前割り当てメモリを提供 */
void PREFIX_cache_init(struct PREFIX_cache *fc,
                       struct rix_hash_bucket_s *buckets,
                       unsigned nb_bk,
                       struct PREFIX_entry *pool,
                       unsigned max_entries,
                       uint64_t timeout_ms);

/* バッチルックアップ — パイプラインホットパス */
void PREFIX_cache_lookup_batch(struct PREFIX_cache *fc,
                               const struct PREFIX_key *keys,
                               unsigned nb_pkts,
                               struct PREFIX_entry **results);

/* 挿入 — ミス時に呼び出し、常に成功（3段フォールバック） */
struct PREFIX_entry *PREFIX_cache_insert(struct PREFIX_cache *fc,
                                         const struct PREFIX_key *key,
                                         uint64_t now);

/* アクション更新 — スローパスACL/QoSルックアップ後に呼び出し */
static inline void
PREFIX_cache_update_action(struct PREFIX_entry *entry,
                           uint32_t action, uint32_t qos_class);

/* タッチ — タイムスタンプ+カウンタ更新（インライン、ヒットごと） */
static inline void
PREFIX_cache_touch(struct PREFIX_entry *entry,
                   uint64_t now, uint32_t pkt_len);

/* ミスレート駆動タイムアウト調整（インライン、バッチごと） */
static inline void
PREFIX_cache_adjust_timeout(struct PREFIX_cache *fc,
                             unsigned misses);

/* 適応的エクスパイア — スキャン量は充填率で自動調整 */
void PREFIX_cache_expire(struct PREFIX_cache *fc,
                         uint64_t now);

/* 2段パイプラインエクスパイア — バケットプリフェッチあり */
void PREFIX_cache_expire_2stage(struct PREFIX_cache *fc,
                                 uint64_t now);

/* 充填率レベル（0-15）— デバッグ/統計用 */
static inline unsigned
PREFIX_cache_expire_level(const struct PREFIX_cache *fc);

/* 統計スナップショット */
void PREFIX_cache_stats(const struct PREFIX_cache *fc,
                        struct flow_cache_stats *out);
```

注: `cache_over_threshold()` と `max_expire` パラメータは廃止。
エクスパイアは常時実行され、スキャン量は充填率から自動決定される。

### 統合バリアント専用ヘルパー

```c
struct flowu_key flowu_key_v4(uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               uint8_t proto, uint32_t vrfid);

struct flowu_key flowu_key_v6(const uint8_t *src_ip, const uint8_t *dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               uint8_t proto, uint32_t vrfid);
```

## 11. テンプレートアーキテクチャ

全フローキャッシュバリアントはCマクロテンプレートにより実装を共有する。
プロトコル固有のコード（キー構造体、エントリ構造体、比較関数）は
各バリアントのヘッダで定義する。それ以外はすべて自動生成される。

### 11.1 テンプレートパラメータ

| マクロ | 例（IPv4） | 用途 |
|---|---|---|
| `FC_PREFIX` | `flow4` | 関数名プレフィックス |
| `FC_ENTRY` | `flow4_entry` | エントリ構造体タグ |
| `FC_KEY` | `flow4_key` | キー構造体タグ |
| `FC_CACHE` | `flow4_cache` | キャッシュ構造体タグ |
| `FC_FLAG_VALID` | `FLOW4_FLAG_VALID` | 有効フラグ定数 |
| `FC_HT` / `FC_HT_PREFIX` | `flow4_ht` | ハッシュテーブル名 |
| `FC_FREE_HEAD` | `flow4_free_head` | フリーリストヘッドタグ |

### 11.2 テンプレートファイル

| ファイル | 生成内容 | 使用箇所 |
|---|---|---|
| `flow_cache_decl_private.h` | キャッシュ構造体、API宣言、インラインヘルパー | `.h` ファイル |
| `flow_cache_body_private.h` | init、insert、lookup_batch、expire、stats | `.c` ファイル |
| `flow_cache_test_body.h` | テスト+ベンチマーク関数 | テスト `.c` |

### 11.3 新バリアントの追加

新しいフローキャッシュバリアント（例: MPLS）を追加するには:

1. キー構造体、エントリ構造体、比較関数を定義
2. `RIX_HASH_HEAD` / `RIX_HASH_GENERATE`
3. `FC_*` マクロを設定せずに `#include "flow_cache_decl_private.h"` を早期インクルード（Section 1のみ）し、構造体定義の後に `FC_*` マクロを設定して再度 `#include "flow_cache_decl_private.h"`（Section 2）
4. `FC_*` マクロを設定し、ソースで `#include "flow_cache_body_private.h"`
5. `FCT_*` マクロを設定し、テストで `#include "flow_cache_test_body.h"`

他のコード変更は不要 — すべてのロジックはテンプレート内にある。

## 12. ファイル構成

```
samples/
  DESIGN.md                本文書（英語版）
  DESIGN_JP.md             本文書（日本語版）

  fcache/                  ライブラリ（libfcache.a / libfcache.so）
    Makefile

    # 公開ヘッダ（ユーザーが直接 include する）
    flow_cache.h             共通定義（TSC、stats、パイプラインパラメータ）
    flow4_cache.h            IPv4: キー、エントリ、cmp、ハッシュ生成、テンプレート展開
    flow4_cache.c            IPv4: テンプレート展開
    flow6_cache.h            IPv6: キー、エントリ、cmp、ハッシュ生成、テンプレート展開
    flow6_cache.c            IPv6: テンプレート展開
    flow_unified_cache.h     統合: キー（family+union）、エントリ、cmp、テンプレート展開
    flow_unified_cache.c     統合: テンプレート展開

    # 内部専用ヘッダ（ライブラリ内部のみ; _private サフィックス）
    flow_cache_decl_private.h  テンプレート: キャッシュ構造体 + API宣言（Section 1: 一度のみ / Section 2: FC_PREFIX 毎）
    flow_cache_body_private.h  テンプレート: 実装（init/insert/lookup/expire）

  test/                    テストバイナリ
    Makefile
    flow_cache_test.c        正当性テスト + ベンチマーク（全3バリアント）
    flow_cache_test_body.h   テンプレート: テスト + ベンチマーク関数
```

## 13. ビルド依存関係

```
rix/rix_defs_private.h  インデックスマクロ、ユーティリティ（内部専用）
rix/rix_queue.h         SLIST（フリーリスト）
rix/rix_hash.h          フィンガープリントハッシュテーブル（SIMDディスパッチ）
```

TAILQ依存なし（LRUをタイムスタンプのみのアプローチに置換）。
SIMD高速化ハッシュ操作には `-mavx2` または `-mavx512f` が必要。

## 14. スレッド安全性

フローキャッシュは **per-thread（スレッド専有）、lock-free** モデルで設計されている。

### モデル

| 項目 | 詳細 |
|---|---|
| インスタンス所有 | 1スレッドにつき1つの `flow4_cache` / `flow6_cache` |
| 同期 | なし — ロック・アトミック・RCU 不使用 |
| スレッド間共有状態 | なし |
| age_cursor | キャッシュごと; 所有スレッドのみが更新 |

### 設計根拠

対象環境（VPPプラグイン、DPDKポールモード）では:

- 各ワーカースレッドは専用のパケットキューを持ち、排他的に処理する。
- フローキャッシュはスレッドローカル状態（フローごとのカウンタ、アクションキャッシュ）を保持する。
- スレッド間の共有がないため、フォルス・シェアリング・コンテンション・キャッシュライン競合が発生しない。
- VPPの「グローバル可変状態なし」アーキテクチャに適合する。

### ユーザーへの指針

- 1つのキャッシュインスタンスを複数スレッドで **共有しない**。
- 同一インスタンスに対して2スレッドから同時に **APIを呼び出さない**。
- 複数コアでトラフィックを処理する場合は、ワーカースレッドごとに1つのキャッシュを確保する。
- スレッドをまたいだフロー統計アクセスが必要な場合は、静止点（例: コントロールプレーンスレッドがquiescent期間後に読み取る）か、適切なメモリ順序付きのコピーで対応する。

### 共有メモリでの利用

librixデータ構造（ハッシュテーブル、フリーリスト）は **インデックス（ポインタなし）** を格納する。
これにより、異なる仮想アドレスに同じ共有メモリ領域をマップする複数プロセス間で
データを再配置できる。ただし、同一の `flow_cache` インスタンスを複数プロセスが同時に変更する場合は、
外部での調停（シャードごとのロック・RCUなど）が必要になる。
前述のシングルライタモデルではこの要件を完全に排除できる。

## 15. 競合分析と総合評価

### 14.1 機能比較

| 特性 | **librix** | **DPDK rte_hash** | **OVS EMC** | **VPP bihash** | **nf_conntrack** |
|---|---|---|---|---|---|
| データ構造 | 16-way cuckoo, FP | 8-way cuckoo | direct-mapped | 4/8-way cuckoo | chained hash |
| キー格納 | ノード（FPバケット） | バケット内 | ノード内 | バケット内 | ノード内 |
| ルックアップ | 4段パイプライン+SIMD | パイプラインなし | 1CL直接参照 | パイプラインなし | チェーン走査 |
| 除去 | O(1) cur_hash | O(1) position | O(1) | O(n) 再ハッシュ | O(1) hlist |
| 共有メモリ | **ネイティブ対応** | 不可（ポインタ） | 不可 | 不可 | 不可 |
| エビクション | 適応的スキャン+強制 | なし（手動） | タイムスタンプ | なし | conntrack GC |
| SIMD | AVX2/512 FPスキャン | CRC32のみ | なし | なし | なし |
| バッチ処理 | ネイティブ対応 | bulk lookup あり | なし | なし | なし |
| スレッドモデル | per-thread, lock-free | RCU or lock | per-thread | per-thread | RCU + spinlock |

### 14.2 個別比較

#### vs DPDK rte_hash

rte_hash はデータプレーン向けハッシュテーブルの事実上の標準。8-way バケットで
高い充填率を達成するが、パイプライン化されたバッチルックアップは提供しない。
`rte_hash_lookup_bulk` はキーごとに独立してDRAMアクセスするため、コールド
キャッシュではレイテンシが積み上がる。librix の4段パイプラインは DRAMアクセスを
重畳するため、大規模プールで5-8倍の優位性がある。

ただし rte_hash はDPDKエコシステムとの統合（mempool, ring, EAL）が完成しており、
単体のハッシュ性能以外の運用面では成熟度が高い。

#### vs OVS EMC（Exact Match Cache）

EMC は direct-mapped（1-way）で、ヒット時は 1CL アクセスのみ。極めて高速だが
ミス率が高い（衝突時に必ずミス）。EMC は SMC（Signature Match Cache = cuckoo）の
前段キャッシュとして機能する2層構造。

librix は 16-way cuckoo 単層で、EMC の高速ヒットパスは持たないが、ミス率は
桁違いに低い。パケット処理全体のスループットでは librix が有利な場面が多い。

#### vs VPP bihash

bihash は VPP の標準ハッシュテーブル。除去に O(n) 再ハッシュが必要で、
エビクション不向き。フローキャッシュ用途では除去頻度が高いため、cur_hash
による O(1) 除去が決定的な優位性。bihash はルックアップもパイプライン化されて
おらず、大規模テーブルでの性能差は大きい。

#### vs nf_conntrack

Linuxカーネルのコネクショントラッカー。chained hash でスケーラビリティに
限界があり、GC は RCU + タイマーベース。データプレーン高速化を目的とした設計
ではなく、機能の豊富さ（NAT, helper, expectation）が強み。比較対象としては
異なる設計目標。

### 14.3 アーキテクチャ上の強み

**インデックスベース設計（最大の差別化要因）**

ポインタを一切格納しないため、共有メモリ・mmap・プロセス間共有がそのまま動作する。
これは他の高性能フローテーブル実装にはない特性。VPPプラグインや複数プロセスからの
参照が必要な環境では決定的な優位性がある。

**メモリアクセスパターンの最適化**

CL0/CL1の分離が一貫している。`last_ts` をCL0に配置することで、expire スキャンが
CL0のみで完結し、存命エントリに対するCL1アクセスが発生しない。パケット処理に
おいて最も頻繁な操作（lookup → hit の判定）が最小限のキャッシュラインアクセスで
完了する。

**4段パイプラインの効果**

実測で 2.8x（L2）→ 8.1x（DRAM-cold）のスピードアップ。プールが大きくなるほど
効果が顕著で、DRAMレイテンシ隠蔽という本来の目的を達成している。40-143 cy/key は
DRAM-cold条件として優秀。

### 14.4 エビクション戦略の評価

**適応的タイムアウトの設計は堅実**

- 減衰/回復の平衡による定常状態収束は制御理論的に安定
- シフト演算のみで除算なし — データプレーンに適している
- 最小タイムアウト1.0秒の下限はキャッシュ効果を保証する妥当な選択

**3段フォールバックによる挿入保証**

キャッシュとして insert が失敗しないのは正しい設計判断。evict_bucket_oldest が
プールエントリとバケットスロットを同時解放するため、後続の ht_insert が必ず
ファストパスで完了する点は巧妙。

**evict_one の 1/8 バウンドに関する考察**

1/8 バウンドは最悪ケースを抑制するが、全エントリが存命で期限切れが見つからない
場合、毎回 1/8 スキャン → evict_bucket_oldest という高コストパスに入る。
攻撃トラフィック下でこのパスが連続すると、insert あたり数千サイクルのコストが
発生しうる。ただし、適応的タイムアウトが先に効いてタイムアウトを短縮するため、
実運用では evict_bucket_oldest に到達する頻度は極めて低いと考えられる。

### 14.5 改善の余地

**性能面**

- `expire_2stage` の効果が定量的に未検証。単段との比較ベンチマークがあると
  判断材料になる
- バッチ insert（ミスが複数ある場合のパイプライン化）は未実装。
  ミス率が高い場合の改善余地がある

**機能面**

- フローごとのコールバック（expiry notification）がない。エントリ除去時に
  カウンタの収集や統計の更新が必要な場合、現状では対応できない
- per-flow タイムアウト（例: TCP established vs SYN）は未対応。
  全エントリが同一タイムアウト

**運用面**

- ランタイムでのタイムアウト変更 API がない（init 時のみ）
- プールの動的リサイズは不可（事前割り当て固定）

### 14.6 総合判定

データプレーンのフローキャッシュとしては非常に完成度が高い設計。

1. **パイプライン + SIMD + CL配置最適化**の3つが一貫して設計されている
2. **適応的エビクション**により手動チューニング不要で安定動作する
3. **挿入保証**によりキャッシュとしての信頼性が確保されている
4. **インデックスベース**により共有メモリ展開が可能

150-215 cy/pkt（lookup + insert + expire 込み）は、2 GHz Xeon で 10 Mpps/core
に相当し、実用上十分な性能。テンプレートアーキテクチャによる3バリアント対応も、
コード重複なしで実現しており保守性が高い。

## 付録 A. librix の使い方

librixは共有メモリおよびmmapされた領域向けのインデックスベース
データ構造を提供するヘッダオンリーのC11ライブラリである。
本付録ではコアコンセプトとAPIパターンを解説する。フローキャッシュ
（§1-13）はこれらのプリミティブ上に構築された実アプリケーションである。

### A.1 コアコンセプト: ポインタの代わりにインデックス

従来のBSD `queue.h` / `tree.h` はリスト/ツリーノードに生ポインタを
格納する。以下の場合にこれが問題となる:
- 構造体が共有メモリに配置される場合（プロセスごとにベースアドレスが異なる）
- メモリが再マッピングされる場合（`mremap`、ファイルバックmmap）
- 構造体が32/64ビットプロセス境界を越える場合

librixはすべてのポインタを**符号なしインデックス**（1オリジン）に置換する:

```
Index 0 = RIX_NIL（番兵、NULLに相当）
Index 1 = base[0]
Index 2 = base[1]
  ...
Index n = base[n-1]
```

各API呼び出しは `base` ポインタ（要素配列）を受け取り、呼び出し時に
インデックスをポインタに変換する。格納されたインデックスは位置独立
— `base` がどこにマッピングされていても有効。

```c
#include <rix/rix_defs.h>

struct item pool[1024];

/* ポインタ → インデックス */
unsigned idx = RIX_IDX_FROM_PTR(pool, &pool[42]);  /* → 43 */

/* インデックス → ポインタ */
struct item *p = RIX_PTR_FROM_IDX(pool, 43);       /* → &pool[42] */

/* NIL ハンドリング */
unsigned nil = RIX_IDX_FROM_PTR(pool, NULL);        /* → 0 (RIX_NIL) */
struct item *np = RIX_PTR_FROM_IDX(pool, RIX_NIL);  /* → NULL */
```

ゼロ初期化ですべてのデータ構造が有効な空状態になる
（すべてのインデックスフィールドが0 = RIX_NIL）。

### A.2 キュー（`rix/rix_queue.h`）

BSD `sys/queue.h` に対応する5種類のキューバリアント:

| マクロプレフィックス | 型 | 操作 |
|---|---|---|
| `RIX_SLIST` | 片方向リンクリスト | INSERT_HEAD, REMOVE_HEAD, FOREACH |
| `RIX_LIST` | 双方向リンクリスト | INSERT_HEAD/AFTER/BEFORE, REMOVE |
| `RIX_STAILQ` | 片方向テールキュー | INSERT_HEAD/TAIL, REMOVE_HEAD |
| `RIX_TAILQ` | 双方向テールキュー | INSERT_HEAD/TAIL/AFTER/BEFORE, REMOVE |
| `RIX_CIRCLEQ` | 循環キュー | INSERT_HEAD/TAIL/AFTER/BEFORE, REMOVE |

すべてのマクロはインデックス-ポインタ変換のために追加引数 `base` を取る。

#### 例: SLIST（片方向リンクリスト）

```c
#include <rix/rix_queue.h>

struct node {
    int value;
    RIX_SLIST_ENTRY(struct node) link;
};

RIX_SLIST_HEAD(node_head, node);

/* プールとヘッド */
struct node pool[100];
struct node_head head;

/* 初期化 */
RIX_SLIST_INIT(&head);

/* 挿入 — 注: base は elm の前に指定 */
RIX_SLIST_INSERT_HEAD(&head, pool, &pool[0], link);
RIX_SLIST_INSERT_HEAD(&head, pool, &pool[1], link);

/* イテレーション */
struct node *n;
RIX_SLIST_FOREACH(n, &head, pool, link) {
    printf("value = %d\n", n->value);
}

/* ヘッドの除去 */
RIX_SLIST_REMOVE_HEAD(&head, pool, link);
```

#### 例: TAILQ（双方向テールキュー）

```c
#include <rix/rix_queue.h>

struct task {
    int priority;
    RIX_TAILQ_ENTRY(struct task) tlink;
};

RIX_TAILQ_HEAD(task_head, task);

struct task pool[256];
struct task_head head;

RIX_TAILQ_INIT(&head);

/* テールに挿入 */
RIX_TAILQ_INSERT_TAIL(&head, pool, &pool[0], tlink);
RIX_TAILQ_INSERT_TAIL(&head, pool, &pool[1], tlink);

/* 特定要素の除去 */
RIX_TAILQ_REMOVE(&head, pool, &pool[0], task, tlink);

/* イテレーション（前方および逆方向） */
struct task *t;
RIX_TAILQ_FOREACH(t, &head, pool, tlink) {
    printf("priority = %d\n", t->priority);
}
```

**重要**: `RIX_SLIST_HEAD`、`RIX_TAILQ_REMOVE` 等の `type` 引数には
`struct` を含めてはならない — 例: `struct node` ではなく `node` を使用。
マクロが内部で `struct` を付加する。

### A.3 赤黒木（`rix/rix_tree.h`）

O(log n) の挿入/検索/除去を持つ平衡二分探索木。

```c
#include <rix/rix_tree.h>

struct record {
    uint32_t key;
    uint32_t data;
    RIX_RB_ENTRY(struct record) rb_entry;
};

/* 比較関数: <0, 0, >0 を返す */
static int
record_cmp(const struct record *a, const struct record *b)
{
    return (a->key < b->key) ? -1 : (a->key > b->key);
}

/* ヘッド宣言と関数生成 */
RIX_RB_HEAD(record_tree);
RIX_RB_GENERATE(record_tree, record, rb_entry, record_cmp)

struct record pool[1000];
struct record_tree tree;

RIX_RB_INIT(&tree);

/* 挿入 */
pool[0].key = 42;
RIX_RB_INSERT(record_tree, &tree, pool, &pool[0]);

/* 検索 */
struct record query = { .key = 42 };
struct record *found = RIX_RB_FIND(record_tree, &tree, pool, &query);

/* 順序付きイテレーション */
struct record *r;
RIX_RB_FOREACH(r, record_tree, &tree, pool) {
    printf("key=%u data=%u\n", r->key, r->data);
}

/* 除去 */
RIX_RB_REMOVE(record_tree, &tree, pool, &pool[0]);
```

### A.4 ハッシュテーブル（`rix/rix_hash.h`）

16-wayバケット、SIMD高速化フィンガープリントマッチング、
O(1)除去を備えたカッコウハッシュテーブル。

3つのバリアントが同じバケット構造を共有:

| ヘッダ | キー格納方式 | キー型 | バケットサイズ |
|---|---|---|---|
| `rix_hash.h` | バケットにフィンガープリント、ノードにキー | 任意構造体 | 128B (2CL) |
| `rix_hash32.h` | バケットに `uint32_t` キー | `uint32_t` | 128B (2CL) |
| `rix_hash64.h` | バケットに `uint64_t` キー | `uint64_t` | 192B (3CL) |

#### A.4.1 セットアップ: フィンガープリントバリアント（`rix_hash.h`）

```c
#include <rix/rix_hash.h>

struct my_key {
    uint32_t src;
    uint32_t dst;
    uint16_t port;
    uint16_t pad;
};

struct my_node {
    struct my_key key;
    uint32_t      cur_hash;   /* hash_field: O(1)除去に必要 */
    int           data;
};

/* キー比較: 等しければ非ゼロを返す */
static int
my_cmp(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(struct my_key)) == 0;
}

/* ヘッド構造体宣言と全関数生成 */
RIX_HASH_HEAD(my_ht);
RIX_HASH_GENERATE(my_ht, my_node, key, cur_hash, my_cmp)
```

`RIX_HASH_GENERATE` は以下の関数を生成する:

| 関数 | 説明 |
|---|---|
| `my_ht_init(head, nb_bk)` | `nb_bk` バケットで初期化（2のべき乗） |
| `my_ht_find(head, bk, base, key)` | 単発ルックアップ |
| `my_ht_insert(head, bk, base, elm)` | ノード挿入（成功時NULLを返す） |
| `my_ht_remove(head, bk, base, elm)` | `cur_hash` フィールド経由のO(1)除去 |
| `my_ht_walk(head, bk, base, cb, arg)` | 全エントリのイテレーション |

ステージドfind関数（N-aheadパイプライン用）:

| 関数 | ステージ | 説明 |
|---|---|---|
| `my_ht_hash_key(ctx, head, bk, key)` | 0 | キーハッシュ、バケットプリフェッチ |
| `my_ht_scan_bk(ctx, head, bk)` | 1 | SIMDフィンガープリントスキャン |
| `my_ht_prefetch_node(ctx, base)` | 2 | 候補ノードのプリフェッチ |
| `my_ht_cmp_key(ctx, base)` | 3 | 完全キー比較 |

#### A.4.2 基本的な使い方

```c
/* 重要: 起動時にTUごとに1回呼び出すこと */
rix_hash_arch_init();

/* 割り当て */
unsigned nb_bk = 1024;   /* 2のべき乗であること */
struct rix_hash_bucket_s *buckets = calloc(nb_bk, sizeof(*buckets));
struct my_node *pool = calloc(10000, sizeof(*pool));

/* 初期化 */
struct my_ht ht;
my_ht_init(&ht, nb_bk);

/* 挿入 */
pool[0].key = (struct my_key){ .src = 1, .dst = 2, .port = 80 };
struct my_node *dup = my_ht_insert(&ht, buckets, pool, &pool[0]);
/* dup == NULL: 成功
 * dup == &pool[0]: テーブル満杯（キックアウト限界）
 * dup == other: 重複キー発見（既存エントリを返す） */

/* 検索 */
struct my_key query = { .src = 1, .dst = 2, .port = 80 };
struct my_node *found = my_ht_find(&ht, buckets, pool, &query);

/* 除去 — cur_hash経由のO(1)、再ハッシュ不要 */
my_ht_remove(&ht, buckets, pool, &pool[0]);

/* 全エントリのウォーク */
int print_cb(struct my_node *node, void *arg) {
    printf("src=%u dst=%u\n", node->key.src, node->key.dst);
    return 0;  /* 0 = 継続、非ゼロ = 停止 */
}
my_ht_walk(&ht, buckets, pool, print_cb, NULL);
```

#### A.4.3 N-aheadパイプラインルックアップ

高スループットの一括ルックアップ（例: パケット処理）では、
DRAMレイテンシを隠蔽する4段パイプラインを使用:

```c
#define BATCH  8
#define KPD    8
#define DIST   (BATCH * KPD)   /* 64キー先行 */

void
batch_find(struct my_ht *ht,
           struct rix_hash_bucket_s *bk,
           struct my_node *base,
           const struct my_key *keys,
           unsigned nb_keys,
           struct my_node **results)
{
    struct rix_hash_find_ctx_s ctx[nb_keys];
    unsigned total = nb_keys + 3 * DIST;

    for (unsigned i = 0; i < total; i += BATCH) {
        /* Stage 0: ハッシュ + バケットプリフェッチ */
        if (i < nb_keys) {
            unsigned n = (i + BATCH <= nb_keys) ? BATCH : nb_keys - i;
            for (unsigned j = 0; j < n; j++)
                my_ht_hash_key(&ctx[i+j], ht, bk, &keys[i+j]);
        }
        /* Stage 1: SIMDフィンガープリントスキャン */
        if (i >= DIST && i - DIST < nb_keys) {
            unsigned b = i - DIST;
            unsigned n = (b + BATCH <= nb_keys) ? BATCH : nb_keys - b;
            for (unsigned j = 0; j < n; j++)
                my_ht_scan_bk(&ctx[b+j], ht, bk);
        }
        /* Stage 2: 候補ノードのプリフェッチ */
        if (i >= 2*DIST && i - 2*DIST < nb_keys) {
            unsigned b = i - 2*DIST;
            unsigned n = (b + BATCH <= nb_keys) ? BATCH : nb_keys - b;
            for (unsigned j = 0; j < n; j++)
                my_ht_prefetch_node(&ctx[b+j], base);
        }
        /* Stage 3: 完全キー比較 */
        if (i >= 3*DIST && i - 3*DIST < nb_keys) {
            unsigned b = i - 3*DIST;
            unsigned n = (b + BATCH <= nb_keys) ? BATCH : nb_keys - b;
            for (unsigned j = 0; j < n; j++)
                results[b+j] = my_ht_cmp_key(&ctx[b+j], base);
        }
    }
}
```

DRAMコールド時に約55-73 cy/key を達成。単発 `my_ht_find()` の
約388 cy/key に対し5-7倍の改善。

#### A.4.4 `rix_hash_arch_init()` — SIMDディスパッチ

`rix_hash_arch_init()` はCPU機能（AVX2、AVX-512）を検出し、
利用可能な最速のSIMDフィンガープリントスキャンの関数ポインタを設定する。

**ハッシュテーブルを使用する翻訳単位ごとに、ハッシュ操作の前に
1回呼び出す必要がある。** ディスパッチ変数 `rix_hash_arch` は
TUごとに `static` であるため、各 `.c` ファイルで個別に呼び出しが必要。

```c
/* main() またはモジュール初期化の先頭で: */
rix_hash_arch_init();
```

SIMD高速化を有効にするには `-mavx2` または `-mavx512f` でビルドする。
これらのフラグがない場合、汎用スカラーフォールバックが使用される。

#### A.4.5 固定キーバリアント

`uint32_t` または `uint64_t` キーには、キーをバケットに直接格納する
特殊バリアントを使用（フィンガープリントなし、ノード側キー比較なし）:

```c
#include <rix/rix_hash32.h>

struct my_entry {
    uint32_t data;
};

RIX_HASH32_HEAD(idx_ht);
RIX_HASH32_GENERATE(idx_ht, my_entry)

/* 使い方は同様だが、キーは構造体ポインタではなく uint32_t 値 */
idx_ht_insert(&ht, buckets, pool, &pool[0], key_value);
struct my_entry *found = idx_ht_find(&ht, buckets, pool, key_value);
```

### A.5 共有メモリデプロイメント

すべてのデータ構造がインデックス（ポインタではなく）を格納するため、
共有メモリに直接配置できる:

```c
/* プロセスA: 作成と投入 */
void *shm = mmap(NULL, size, PROT_READ|PROT_WRITE,
                 MAP_SHARED, shm_fd, 0);
struct my_node *base = (struct my_node *)shm;
struct my_ht *ht = (struct my_ht *)(shm + node_area_size);
/* ... 初期化と挿入 ... */

/* プロセスB: アタッチとクエリ（異なる仮想アドレス） */
void *shm2 = mmap(NULL, size, PROT_READ|PROT_WRITE,
                  MAP_SHARED, shm_fd, 0);
struct my_node *base2 = (struct my_node *)shm2;
struct my_ht *ht2 = (struct my_ht *)(shm2 + node_area_size);
/* base2でfindが動作 — インデックスは位置独立 */
struct my_node *found = my_ht_find(ht2, buckets2, base2, &query);
```

ポインタの修正は不要。同じインデックス値が両プロセスで有効である。
インデックスは絶対アドレスではなく `base` からのオフセットだからである。

### A.6 まとめ: BSD queue.h vs librix

| 観点 | BSD `queue.h` | librix |
|---|---|---|
| ノードリンク | 生ポインタ | 符号なしインデックス（1オリジン） |
| 番兵 | `NULL` | `RIX_NIL` (0) |
| API追加引数 | — | `base`（要素配列ポインタ） |
| 共有メモリ | ポインタ修正が必要 | そのまま動作 |
| mmap/remap | 全ポインタが無効化 | インデックスは有効のまま |
| 32/64ビット相互運用 | ポインタサイズ不一致 | インデックスは `unsigned`（32ビット） |
| ゼロ初期化 | 無効な状態 | 有効な空状態 |
| データ構造 | SLIST, LIST, STAILQ, TAILQ, CIRCLEQ | 同上 + 赤黒木 + 3種ハッシュバリアント |
| 性能 | ポインタ間接参照 | インデックス演算 + 間接参照 |

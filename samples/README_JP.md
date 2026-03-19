# フローキャッシュサンプル

librix のインデックスベースデータ構造（`rix_hash.h`）を用いた
本番品質のフローキャッシュ実装。高性能パケット処理向けに
パイプライン方式のバッチ検索で DRAM レイテンシを隠蔽する。

## 2つのバージョン

| | fcache (v1) | fcache (v2) |
|---|---|---|
| エントリサイズ | 128B（2 CL） | 64B（1 CL） |
| ユーザペイロード | CL1 に 64B | なし（呼び出し側が entry_idx で参照） |
| バリアント | flow4, flow6, flowu | flow4, flow6, flowu |
| SIMD ディスパッチ | 実行時選択（gen/sse/avx2/avx512） | AVX2 直接バインド |
| エビクション | グローバル走査 + 閾値 | ローカル relief + バケット予算制 maintenance |

v2 が現在の開発対象。v1 は比較用に維持。

## クイックスタート

```sh
# 全体ビルド
make -C samples/fcache static
make -C samples/fcache static

# v2 テスト実行
make -C tests/fcache fc_test && tests/fcache/fc_test

# v2 バリアント比較ベンチ（flow4 vs flow6 vs flowu）
make -C tests/fcache fc_vbench && taskset -c 0 tests/fcache/fc_vbench

# v1-vs-v2 比較ベンチ
make -C tests/fcache fc_bench && taskset -c 0 tests/fcache/fc_bench
```

## ファイル構成

```
samples/
  fcache/          v1 ライブラリ（128B エントリ）
  fcache/         v2 ライブラリ（64B エントリ）
  test/            v1 テスト + ベンチマーク

tests/fcache/     v2 テスト + ベンチマーク
  test_flow_cache2.c       正当性テスト（全3バリアント）
  bench_flow_cache2.c      v1-vs-v2 比較ベンチ
  bench_fc_variants.c     v2 バリアント比較ベンチ
```

## v2 バリアント

- **flow4**: IPv4 5-tuple + vrfid（20B キー）
- **flow6**: IPv6 5-tuple + vrfid（44B キー）
- **flowu**: 統合 IPv4/IPv6、family フィールド付き（44B キー）

全エントリは 64 バイト（single cache line）。エントリレイアウトと
ベンチマークデータは `DESIGN_JP.md` を参照。

## API (v2)

各バリアントは同じ関数セットを公開（`PREFIX` = `flow4` / `flow6` / `flowu`）：

```c
fc_PREFIX_cache_init()             // 初期化
fc_PREFIX_cache_flush()            // 全エントリをフリーリストに返却
fc_PREFIX_cache_lookup_batch()     // パイプラインバッチ検索
fc_PREFIX_cache_fill_miss_batch()  // ミスキーを挿入
fc_PREFIX_cache_maintain()         // idle/background 回収
fc_PREFIX_cache_remove_idx()       // インデックス指定で削除
fc_PREFIX_cache_nb_entries()       // 現在のエントリ数
fc_PREFIX_cache_stats()            // カウンタのスナップショット
```

## 依存関係

- librix ヘッダ: `rix/rix_hash.h`, `rix/rix_queue.h`, `rix/rix_defs.h`
- コンパイラ: GCC または Clang（`-mavx2 -msse4.2` 要）
- 外部ライブラリ不要

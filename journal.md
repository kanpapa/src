# AXPvme 230 ポーティング開発ジャーナル

# journal.md

## 2026-06-25〜26: AXPvme 230 ポーティング — swpctx double error halt 調査

### 背景

DEC AXPvme 230（21066A / LCA チップセット、system type 10 = `ST_DEC_APXVME_64`）に NetBSD/alpha を Diskless で動かすポーティング。
`call_pal PAL_OSF1_swpctx` 実行時に halt code=6（double error halt）が発生し、PC=0x19e90 で停止していた。

---

### 調査: BCE=0 による Bcache 無効化の試み（全て失敗）

swpctx が Bcache probe/invalidation を発行し、外部 Bcache コントローラが応答しないためハングしていると仮定し、BCE=0 を書いてから swpctx を呼ぶ方針を試みた。

#### 試みた場所と結果

| 場所 | 結果 |
|------|------|
| `dec_axpvme_64_init()`（早期ブート） | D4 出力後にハング（非同期フラッシュに追い付かれる） |
| `axpvme_predisable_bcache()`（`alpha_init()` 末尾、`alpha_mb()` あり） | `alpha_init() end` で即ハング（`mb()` が deadlock） |
| 同上（`alpha_mb()` なし） | 同じ結果（L1 miss → LCA ブロック → ハング） |

#### 根本原因

BCE=0 を MEMC_CAR に書いた瞬間、LCA が外部 Bcache の dirty line flush を開始する。
外部 Bcache コントローラが flush に応答しないため、LCA は全メモリアクセスをブロックする。
CPU は L1 キャッシュヒットで数命令動けるが、L1 miss が発生した時点でフリーズ。
`alpha_mb()` は outstanding memory operation の完了を待つため、BCE=0 直後の呼び出しで deadlock する。

---

### 調査: PALcode ダンプによる probe 命令特定（進行中）

BCE=0 アプローチを断念し、PALcode 内の Bcache probe 命令を NOP パッチする方針に転換。

PALcode は DRAM 上（MDDT クラスタ）に存在し、カーネルから書き込み可能。
halt PC=0x19e90 の周辺命令を読み出してデコードする計画。

#### 修正したファイル

**`sys/arch/alpha/alpha/machdep.c`**
- `#include "opt_dec_axpvme_64.h"` を追加（64行目付近）
  - **バグ**: `#ifdef DEC_AXPVME_64` を使っていたが、opt ファイルをインクルードしていなかったため常に偽となりダンプコードが実行されなかった
  - **解決策**: 既存の `opt_dec_3000_300.h` 等と並べて `opt_dec_axpvme_64.h` をインクルード
- PALcode ダンプコード（`#ifdef DEC_AXPVME_64` ブロック）を K0SEG 書き直しブロックの**前**に移動
  - 以前は K0SEG ブロックの後ろに置いていたため、`mb()` がハングした場合にダンプが実行されなかった
- K0SEG ブロックの前後に `pre-K0SEG` / `post-K0SEG` ハングマーカーを追加
- ループ変数 `i` → `pi` にリネーム（外側スコープの `i` と衝突し `-Werror=shadow` でビルドエラー）

**`sys/arch/alpha/alpha/dec_axpvme_64.c`**
- `axpvme_predisable_bcache()` 関数（BCE=0 書き込み）は保持しているが、現在は呼び出していない

---

### 現在の起動ログ（ビルド後・実機未確認）

```
[   1.0000000] axpvme: rpb_pcs_off=0x180 rpb_pcs_size=0x280 pcs_flags=0x1ed
[   1.0000000] axpvme: alpha_init() end
[   1.0000000] axpvme: pre-K0SEG
[   1.0000000] axpvme: post-K0SEG
（PALcode ダンプ未表示 ← opt ファイルインクルード漏れが原因、修正済み）
```

---

### 次のアクション

1. 修正済みカーネルを実機で実行し、PALcode ダンプ（`axpvme: pal_memaddr=...` 〜 `0x19exx: xxxxxxxx ...`）を取得する
2. 0x19e90 付近の Alpha 命令をデコードして Bcache probe 命令列を特定する
3. `alpha_init()` 内で該当命令を NOP（`0x47ff041f`: `bis r31,r31,r31`）でパッチする
4. swpctx 通過確認（`mark3` 以降の出力を確認）
5. swpctx 通過後: PCI 割込みルーティング実装（`pci_axpvme_64.c`）

---

### ハードウェアメモ

- `MEMC_CAR = 0x217f9c75`: BCE=1（512KB 外部 Bcache 有効）、BCS=3
- K1SEG 書き込み・`PAL_cflush` → Bcache probe/invalidation → **ハング**
- K0SEG 書き込み → L1 write-back → 正常動作
- SRM Console V17.0-0、OSF PALcode V1.45-12
- PALcode 物理ベースアドレス: `pcs_pal_memaddr`（PCS フィールド）で確認予定

## 2026-06-27: 8259 PIC 再初期化による割り込み修正

### 問題
`tlp0: filter setup and transmit timeout` — 21040 Ethernet が filter setup フレームを 5 秒で watchdog タイムアウト。DHCP パケットが一切ワイヤに流れない。

### 診断の鍵
ユーザーより「SRM の netboot ではネットワーク利用・NFS マウントとも問題なし」との情報。
これにより以下が確定した：
- 21040 ハードウェアは正常
- LCA の DMA 自体は機能している（SRM レベルでは）
- SRM はポーリング方式でネットワークを使用
- NetBSD の割り込み駆動ドライバのみが失敗する

### 根本原因
SRM ファームウェアが 8259 (i82378 SIO) の ICW2（割り込みベクタベース）を
PC 互換値（マスタ=0x08、スレーブ=0x70）等で設定したまま OS に引き渡す。

Alpha OSF/1 PALcode は IACK で IRQ 番号（0–15 直値）が返ることを期待するため、
ICW2 は master=0x00、slave=0x08 でなければならない。

NetBSD の `sio_intr_setup()` は従来 ICW を再初期化せず OCW1（マスク）と ELCR のみ
変更していたため、SCB ベクタの計算が狂い割り込みハンドラ（`sio_iointr` → `tlp_intr`）
が呼ばれなかった。

### 修正
`sys/arch/alpha/pci/sio_pic.c` の `sio_intr_setup()` に 8259 の完全な再初期化を追加：

- ICW1: 0x11（カスケード、ICW4 必要）
- ICW2: 0x00 / 0x08（マスタ/スレーブの IACK ベクタベース）
- ICW3: 0x04 / 0x02（スレーブは IR2 に接続、スレーブ ID=2）
- ICW4: 0x01（8086 モード）
- OCW1: 0xff（全割り込みマスク。後続の sio_setirqstat() で個別に有効化）

全 SIO ベースの Alpha プラットフォームに適用（他プラットフォームでは SRM が
正しい値を設定済みなので実質ノーオペレーション）。

### 関連ファイル
- `sys/arch/alpha/pci/sio_pic.c`（変更）
- `sys/arch/alpha/pci/lca_dma.c`（前セッション: PREWRITE/POSTREAD Bcache eviction）

### 結果（ビルド確認済み）
- filter setup タイムアウトが消滅 ✓
- RPCC timecounter 逆行も解消（IRQ 0 も同じ ICW2 誤りで狂っていたため、副作用として修正）✓
- DHCP フェーズに到達するが、パケットがワイヤに出ていない（次セクション参照）

---

## 2026-06-27: Bcache eviction アドレスのバグ修正（DHCP TX 不具合）

### 問題
8259 ICW2 修正後、filter setup は成功して DHCP フェーズに到達するが、サーバー側
tcpdump でパケットが一切確認されない。

### 診断
- filter setup TX は `sc->sc_cddmamap`（低 PA、early allocation）を使用 → 問題なし
- 通常 TX（DHCP）は `txs->txs_dmamap`（mbuf、ヒープから任意 PA）を使用
- 64MB RAM で PA ≥ 63.5MB（= 64MB − 512KB）の mbuf に対して eviction アドレスが
  `a + 512KB ≥ 64MB` → DRAM 範囲外アクセス
- ハードウェアが PA を aliasing（mod 64MB）して**別の** Bcache スロット**を evict
- DMA バッファの dirty ラインが Bcache に残留
- 21040 DMA read → dirty probe → AXPvme 230 の Bcache コントローラが probe に無応答
  → TX stall（サイレント失敗、クラッシュなし、タイムアウトメッセージなし）

### 修正
`sys/arch/alpha/pci/lca_dma.c` の `lca_axpvme_dmamap_sync` 内、eviction アドレスを
`a + LCA_AXPVME_BCACHE_SIZE` から `a ^ LCA_AXPVME_BCACHE_SIZE` に変更。

XOR（ビット 19 toggle）の性質：
- PA と PA ^ 512KB は同じ Bcache スロット（インデックス bits [18:5] が同一）
- 64MB 以内の任意の PA に対して結果も必ず 64MB 以内（ビット 25 = 64MB ビットは変化しない）
- 正逆どちら方向にも使えるため PA の上限問題が発生しない

### 残課題
- DHCP パケットがワイヤに出るかを実機で確認
- DHCP 成功後: dnsmasq に `dhcp-option=17,<root-path>` を設定
- NFS root マウント成功の確認
- NFS マウント後: PCI 割り込みルーティングの実装（pci_axpvme_64.c）

---

## 2026-06-27: AUI メディア強制と Bcache eviction 削除

### 発見: 前提が誤りだった

ハードウェアの Technical Description を精査した結果、以下が判明：

**LCA（21066A）は Bcache DMA コヒーレンシをチップ内部で自動管理する。**
- DMA Read が Dirty Bcache にヒット → LCA が Bcache から PCI デバイスへ直接データを供給
- DMA Write が Bcache にヒット → Bcache 更新 + DRAM 書き込み
- 外部プローブ/インバリデーションプロトコルは不要

また AXPvme 230 のオンボード 21040 は**フロントパネルの AUI コネクタ**に接続されており、
10BASE-T インターフェースは存在しない。

### 根本原因: tulip ドライバのメディア設定誤り

`TULIP_CHIP_21040` のデフォルト mediasw は `tlp_21040_mediasw`（`tlp_21040_tmsw_init`）で、
`ifmedia_set(IFM_ETHER | IFM_10_T)` が最終メディアとして選択される。
AUI 接続のボードで 10BASE-T SIA が設定されるため、TX フレームがワイヤに出ない
（エラーなし・ハングなし、単にキャリアなしでサイレント失敗）。

### 修正 1: Bcache eviction コードを削除（`sys/arch/alpha/pci/lca_dma.c`）

`lca_axpvme_dmamap_sync()` 関数全体と `lca_dma_init()` 内の
`#ifdef DEC_AXPVME_64` インストールブロックを削除。
LCA がハードウェアで DMA コヒーレンシを管理するため不要だったことが判明。
（`opt_dec_axpvme_64.h` / `machine/rpb.h` のインクルードも削除）

### 修正 2: AUI メディア強制（`sys/dev/pci/if_tlp_pci.c`）

インクルードに追加：
```c
#ifdef __alpha__
#include "opt_dec_axpvme_64.h"
#ifdef DEC_AXPVME_64
#include <machine/rpb.h>
#endif
#endif
```

`TULIP_CHIP_21040` ケースに追加：
```c
#if defined(__alpha__) && defined(DEC_AXPVME_64)
    if (cputype == ST_DEC_APXVME_64)
        sc->sc_mediasw = &tlp_21040_auibnc_mediasw;
#endif
```

`tlp_21040_auibnc_mediasw` は `tlp_21040_auibnc_tmsw_init` を使用し、
`ifmedia_set(IFM_ETHER | IFM_10_5)` で AUI をデフォルト選択する。

### 結果（ビルド確認済み）
- ビルド成功 ✓
- 次回実機テスト: DHCP パケットがワイヤに出るかを確認

---

## 2026-06-27: 21040 TX 割り込みロスト修正（setup frame スタック）

### 問題

前セッションのビルド（AUI メディア強制 + SIA 値パッチ）後、実機でテストすると
DHCP パケットが依然ワイヤに出ない。診断用 print を追加して調査。

追加した print の結果:
```
tlp0: filter_setup deferred: dirty=1 doing_setup=1  ← 繰り返し
tlp0: tlp_start: WANT_SETUP set, snd.len=0          ← 繰り返し
```

### 根本原因: `tlp_init` 内の TX 割り込みロスト

`tlp_init` の処理順：

1. (line 1943) `tlp_filter_setup` → setup frame を TX リングに投入 → `TXPOLL_TPD` 書き込み → 21040 が TX 処理を開始
2. (line 1948) `tmsw_set` → `tlp_sia_media` → **`tlp_idle(ST|SR)` で TX を一時停止** → SIA レジスタ再設定 → `OPMODE` 書き戻して TX 再開
3. TX 再開後、21040 が setup frame を処理 → `CSR_STATUS.TI` アサート（TX 割り込み発生）
4. CPU が IRQ 6（エッジトリガー）を受信 → `tlp_intr` 呼び出し
5. **`tlp_intr`: `IFF_RUNNING` がまだ 0 → `return 0`（早期リターン）**
6. エッジトリガー IRQ の場合、割り込みエッジは消費済み → CSR_STATUS.TI はクリアされないが次の割り込みエッジは来ない
7. (line 1963) `IFF_RUNNING` 設定
8. `tlp_init` 返却。以降 `tlp_txintr` は呼ばれない → `DOING_SETUP` が永久にセットされたまま

その後: DHCP ソケット作成がマルチキャスト参加 → `ENETRESET` → `tlp_filter_setup` → `dirty=1 doing_setup=1` → `WANT_SETUP` セット → `tlp_start` が即 return し続ける。

### 修正: `tlp_init` の末尾で `tlp_txintr` をポーリング

`sys/dev/ic/tulip.c`、`IFF_RUNNING` 設定直後に追加：

```c
ifp->if_flags |= IFF_RUNNING;
sc->sc_if_flags = ifp->if_flags;

/* The TX interrupt for the filter setup frame may have fired
 * before IFF_RUNNING was set, causing tlp_intr to ignore it
 * (edge-triggered IRQ).  Poll the TX ring now to catch it. */
tlp_txintr(sc);
```

この呼び出しにより：
- setup frame が処理済み（OWN=0）なら `DOING_SETUP` をクリア、`WANT_SETUP` があれば `filter_setup` を再発行
- setup frame がまだ処理中（OWN=1）なら何もしない（次の TX 割り込みは IFF_RUNNING 設定後に来る → 正常処理）

全チップ共通修正。interrupt コンテキスト不要（ioctl コンテキスト＝KERNEL_LOCK 保持下から安全に呼べる）。

### 関連ファイル
- `sys/dev/ic/tulip.c`（変更: `tlp_txintr` ポーリング追加、診断 print も追加中）
- `sys/dev/pci/if_tlp_pci.c`（OPMODE/SIA 診断 print + SIA 値パッチ）

### 結果（ビルド確認済み）
- ビルド成功 ✓
- 次回実機テスト: `filter_setup deferred` / `WANT_SETUP` メッセージが消え DHCP パケットがワイヤに出ることを確認

### 残課題（前セッション時点）
- DHCP パケットがワイヤに出るかを実機で確認
- DHCP 成功後: dnsmasq に `dhcp-option=17,<root-path>` を設定
- NFS root マウント成功の確認
- NFS マウント後: PCI 割り込みルーティングの実装（pci_axpvme_64.c）

---

## 2026-06-27: DMA キャッシュコヒーレンシー修正（lca_dma.c）

### 症状の変化

前セッションの `tlp_txintr` ポーリング削除後: `txfree=1023 flags=0x1802`（最初から DOING_SETUP=1 のまま）。
CSR_TXLIST 修正をしても1枚目の setup frame すら処理されない。

### 根本原因の確定: 外部 write-back Bcache による DMA 非コヒーレンシー

AXPvme 230 の 21066A には 512KB 外部 write-back Bcache（BCE=1, BCS=3）。
`_bus_dmamap_sync` = `alpha_mb()` のみで Bcache ダーティラインを DRAM にフラッシュしない。

**PREWRITE フロー（失敗）**:
1. CPU: `desc0.td_status = OWN=1` → Bcache ダーティ、DRAM は OWN=0
2. OPMODE で TX 開始 → NIC が DRAM を読む → OWN=0 → TX SUSPENDED
3. STATUS_TI 割り込みなし → DOING_SETUP=1 永続

AXPpci 33 は外部 Bcache なし（L1 8KB のみ）→ 自然エビクションで問題が隠れる。

**PAL_cflush / K1SEG write は使用不可**: どちらも AXPvme 230 でハング。

### 修正: 競合エビクション（conflict-eviction）

直接マッピング 512KB Bcache で `phys + 512KB` を K0SEG 経由で読むと、`phys` の Bcache ラインが内部的にエビクトされ DRAM に書き戻される。外部プロトコルなし → AXPvme 230 で安全。

#### `sys/arch/alpha/pci/lca_dma.c` の変更

- `lca_bcache_dmamap_sync()` を追加:
  - `BUS_DMASYNC_PREWRITE | BUS_DMASYNC_POSTREAD` 時に競合エビクション実行
  - `phys = ds_addr + offset - _wbase`（_wbase = LCA_DIRECT_MAPPED_BASE = 1GB）
  - `conflict_va = ALPHA_PHYS_TO_K0SEG(phys) + lc_bcache_size`
  - 32 バイト stride でキャッシュライン単位にループ（21066A の cacheline size = 32B）
- `lca_dma_init()`: `lc_bcache_size != 0` なら direct-mapped タグの `_dmamap_sync` を差し替え

### ビルド結果

ビルド成功 ✓（`~/obj/sys/arch/alpha/compile/AXPVME/netbsd`）

### 残課題

- 実機テスト: DHCP パケットがワイヤに出ることを確認
- DHCP 成功後: dnsmasq に `dhcp-option=17,<root-path>` 設定 → NFS root マウント確認
- NFS マウント後: `pci_axpvme_64.c` で PCI 割り込みルーティング実装

---

## 2026-06-28: NIC ソフトウェアポーリング実装（割り込みハング根本対策）

### 問題: ハードウェア割り込みによる Bcache ハング

AXPvme 230 の SRM PAL は、SIO 8259 が CPU IRQ&lt;1&gt; を通知したとき LCA PCI IACK レジスタ
（物理 `0x1A0000000`）を読まない。DECaxppci 33 の PAL はこれを自動実行するが、
AXPvme 230 は VMEbus 用に設計された PAL のため省略されている。

IACK なしで PAL がベクタを返す → `interrupt.c` の SCB ディスパッチが誤ったスロットを
参照 → `scb_stray` → `printf` → PROM コールバック → Bcache invalidation → **ハング**。

### 設計方針: ソフトウェアポーリング

ハードウェア割り込みを完全に抑制し、1 tick ごとのコールアウトで `tlp_intr` を直接呼ぶ。

- `sio_pci_intr_establish` でハンドラを SCB に登録した直後、8259 OCW1 でその IRQ を
  再マスクする（ハードウェア割り込みがCPUに届かないようにする）。
- `callout_reset(&axpvme_poll_callout, 1, axpvme_poll, NULL)` で 1 tick 後から polling 開始。
- `axpvme_poll` は登録済みハンドラを順に呼び出し、`callout_schedule` で次 tick をスケジュール。

### 修正: `sys/arch/alpha/pci/pci_axpvme_64.c`（新規実装）

以前は最小スタブ（`sio_pci_intr_establish` をそのまま使用）だったものを本実装に置換：

- `axpvme_poll_callout`（`struct callout`）: polling callout
- `axpvme_poll_ih[]`（`struct alpha_shared_intrhand *[4]`）: 登録ハンドラ配列
- `axpvme_64_intr_establish()`:
  1. `sio_pci_intr_establish()` でハンドラ登録（SCB 設定、8259 アンマスク）
  2. `alpha_pci_intr_handle_get_irq(&ih)` で IRQ 番号を取得
  3. `IO_ICU1` (0x020) の OCW1 で該当 IRQ を再マスク
  4. `axpvme_poll_ih[]` に登録し、初回なら `callout_reset` でポーリング開始
- `pci_axpvme_64_pickintr()`:
  - `axpvme_iot = iot` を保存（8259 アクセス用）
  - `callout_init(&axpvme_poll_callout, 0)` 初期化
  - `pc->pc_intr_establish = axpvme_64_intr_establish`

### `pci_intr_handle_t` のキャスト修正

`pci_intr_handle_t` は Alpha では `struct { u_long value; }` 型のため、`(int)ih` は
コンパイルエラー（`aggregate value used where integer was expected`）。
`alpha_pci_intr_handle_get_irq(&ih)` (`u_int` 返し) を使用することで解決。

### 修正: `sys/arch/alpha/conf/AXPVME`

IRQ6 コンフリクトを除去：
```
- fdc0    at isa? port 0x3f0 irq 6 drq 2
+ #fdc0   at isa? port 0x3f0 irq 6 drq 2  # (IRQ6 used by NIC on AXPvme 230)
```

### ビルド結果

ビルド成功 ✓（`~/obj/sys/arch/alpha/compile/AXPVME/netbsd`）  
コンパイル警告なし。

### 次のアクション

1. 実機テスト: `tlp0` の attach ログ、DHCP 送受信を確認
2. `tlp_intr` がポーリングで呼ばれているか（IFF_RUNNING 設定後、RX/TX 処理が進むか）確認
3. DHCP 成功 → NFS diskless boot 達成を確認

---

## 2026-06-28: conflict-eviction アドレス XOR 修正（lca_dma.c）

### 症状

`nfs_boot: trying DHCP/BOOTP` でシステム停止。

### 根本原因

`lca_bcache_dmamap_sync()` の conflict-eviction アドレスが加算方式（`va + bcache_size`）だった。

- Bcache サイズ = 512KB = 0x80000
- RAM = 64MB = 0x4000000
- RX/TX バッファが PA ≥ 0x3F80000 (63.5MB) にある場合:
  `conflict_va = K0SEG(PA + 512KB) = K0SEG(64MB+)` → DRAM 範囲外
- LCA は PA ≥ 64MB へのアクセスにマシンチェックを返す → ハング

NIC の TX mbuf・RX バッファは汎用アロケータから任意 PA に割り当てられるため、
DHCP Discover 送信時または受信時に 63.5MB 境界を踏む可能性があった。

### 修正（`sys/arch/alpha/pci/lca_dma.c`）

```c
- vaddr_t conflict_va = va + (vaddr_t)lcp->lc_bcache_size;
+ vaddr_t conflict_va = va ^ (vaddr_t)lcp->lc_bcache_size;
```

XOR（bit19 トグル）の性質：
- 512KB 直接マッピング Bcache のインデックスは bits [18:5]（14 ビット）
- PA と PA ^ 0x80000 は bit19 が異なるが bits [18:5] は同一 → 同一 Bcache スロット
- PA に対する読み出しで PA の dirty ライン eviction が発生（write-back to DRAM）
- PA < 64MB (bit26=0) → PA ^ 0x80000 も bit26=0 → 必ず 64MB 以内

### ビルド結果

ビルド成功 ✓

---

## 2026-06-29: ポーリングレート修正（KERNEL_LOCK 競合除去）

### 問題

前セッションで NFS diskless boot に成功したが、以下の症状が残った：

- `axpvme_poll: tick 1` → `tick 1024` 間隔が ~700ms（理想: ~977µs、1024 Hz）
- DHCP 取得に ~35 分、NFS root マウントに ~1.7 時間かかる
- 実効ポーリングレート ~1.4 Hz（1024 Hz の 1/730）

### 根本原因: alpha_shared_intr_wrapper の KERNEL_LOCK 競合

`axpvme_poll` が `ih->ih_fn(ih->ih_arg)` を呼び出していたが、
tlp ドライバは `PCI_INTR_MPSAFE` フラグなしで登録されるため、
`ih->ih_fn = alpha_shared_intr_wrapper` となる。

`alpha_shared_intr_wrapper`:
```c
KERNEL_LOCK(1, NULL);          /* ← 最大 700ms ブロック */
rv = (*ih->ih_real_fn)(ih->ih_real_arg);  /* = tlp_intr */
KERNEL_UNLOCK_ONE(NULL);
```

NFS ブート中は NFS スタック（sunrpc、sosend、sorecv など）が
`KERNEL_LOCK` を長時間保持するため、callout スレッドが 700ms 近くブロックする。

タイムライン（1 サイクル = 700ms）：
1. callout 発火
2. `axpvme_poll` → `alpha_shared_intr_wrapper` → `KERNEL_LOCK` で 700ms ブロック
3. NFS スレッドがロック解放後、`tlp_intr` 実行（< 1µs）
4. `callout_schedule(co, 1)` → 次 tick を予約
5. 硬時計が 717 tick 進んでいるため即座に発火
6. 1 に戻る

### 修正: `ih_real_fn` 直接呼び出し + CALLOUT_MPSAFE

`sys/arch/alpha/pci/pci_axpvme_64.c` を 2 点変更：

#### 1. callout_init を CALLOUT_MPSAFE に変更

```c
/* 変更前 */
callout_init(&axpvme_poll_callout, 0);

/* 変更後 */
callout_init(&axpvme_poll_callout, CALLOUT_MPSAFE);
```

`CALLOUT_MPSAFE` を指定すると、callout インフラストラクチャが
`axpvme_poll` を呼び出す前に `kernel_lock` を取得しない。

#### 2. ih_fn → ih_real_fn に変更

```c
/* 変更前 */
(*ih->ih_fn)(ih->ih_arg);      /* = alpha_shared_intr_wrapper(ih) */

/* 変更後 */
(*ih->ih_real_fn)(ih->ih_real_arg);  /* = tlp_intr(sc) 直接 */
```

UP（単一 CPU）マシンのため、`KERNEL_LOCK` が防ぐ "複数 CPU による同時アクセス"
という問題が存在しない。`tlp_intr` は内部的に `sc->sc_lock`（NIC ごとのミューテックス）
で自己保護しているため、wrapper を bypass しても安全。

#### 3. 診断プリントの頻度調整

```c
/* 変更前: 最初の 5 tick + 1024 tick ごと（~1s ごとに出力） */
if (axpvme_poll_ticks <= 5 || (axpvme_poll_ticks & 0x3ff) == 0)

/* 変更後: 最初の 5 tick + 10240 tick ごと（~10s ごとに出力） */
if (axpvme_poll_ticks <= 5 || (axpvme_poll_ticks & 0x27ff) == 0)
```

1024 Hz で動作すると毎秒出力になるため、間隔を約 10 秒に延ばした。

### 期待される効果

- ポーリングレート: 1.4 Hz → 1024 Hz
- DHCP 取得: 35 分 → 数秒
- NFS root マウント: 1.7 時間 → 数分

### ビルド結果

ビルド成功 ✓（`~/obj/sys/arch/alpha/compile/AXPVME/netbsd`）

### 次のアクション

1. 実機テスト: `axpvme_poll: tick 1` → `tick 2` の間隔が ~1ms 以下になることを確認
2. DHCP 取得が数秒で完了することを確認
3. NFS root マウントが正常に完了することを確認

---

## 2026-06-29: mcclock ハング修正 + DS1386 TOY クロック実装

### 問題: mcclock_set_pcc_freq 無限ループでハング

AXPvme 230 の SIO (82378IB) 内蔵 MC146818 互換 RTC（ポート 0x70/0x71）は、
32.768 kHz 水晶振動子が接続されていないため発振しない。

`mcclock_attach()` 内の `mcclock_set_pcc_freq()` は MC_REGC の PF ビット（16 Hz
周期フラグ）が立つのを無限ループで待つ。PF が永遠に立たないため、autoconf 中に
システムがハング → 画面停止・tcpdump 停止。

### 修正 1: `mcclock_set_pcc_freq` にタイムアウトを追加

`sys/arch/alpha/alpha/mcclock.c` を修正：

- PCC カウンタ（`cpu_counter32()`）ベースの 125ms タイムアウトを追加
- タイムアウト時はエラーメッセージを出力して早期リターン
  → `ci_pcc_freq` は `cpu_attach()` が `hwrpb->rpb_cc_freq` から設定した値のまま
- `clockattach(mcclock_init, sc)` は引き続き呼ばれるため他プラットフォームへの影響なし

```c
tstart = cpu_counter32();
while (((*sc->sc_mcread)(sc, MC_REGC) & MC_REGC_PF) == 0) {
    if ((uint32_t)(cpu_counter32() - tstart) > timeout)
        goto fail;
}
```

### 修正 2: `mcclock_isa_match` で AXPvme 230 をスキップ

`sys/arch/alpha/isa/mcclock_isa.c` を修正：

AXPvme 230 では MC146818 の発振子が未接続なため、
ISA mcclock は attach しないよう `cputype == ST_DEC_AXPVME_64` のチェックを追加。
代わりに DS1386 ドライバが `clockattach` と `todr_attach` を担当する。

```c
if (cputype == ST_DEC_AXPVME_64)
    return 0;
```

### 修正 3: DS1386 TOY クロックドライバ新規実装

`sys/arch/alpha/isa/ds1386.c` を新規作成：

**DS1386 レジスタ配置（0x8000 + offset、直接アクセス）**

| オフセット | 内容 |
|-----------|------|
| 0x00 | 1/100 秒 (読み取り専用) |
| 0x01 | 秒 BCD (00-59) |
| 0x03 | 分 BCD (00-59) |
| 0x05 | 時 BCD (00-23) |
| 0x07 | 曜日 BCD (01-07) |
| 0x08 | 日 BCD (01-31) |
| 0x09 | 月 BCD (01-12); bit7=OSC_DIS, bit6=SQW_DIS |
| 0x0A | 年 BCD (00-99) |
| 0x0B | コマンドレジスタ A |

**MC146818 との違い**: インデックスポート不要。各レジスタを
`bus_space_read/write_1(iot, ioh, DS1386_XXX)` で直接読み書き。

**ドライバの役割**:
1. `todr_attach(&sc->sc_todr)` — 時刻取得・設定（gettime_ymdhms / settime_ymdhms）
2. `clockattach(ds1386_clock_init, sc)` — cpu_initclocks() が必要とする clock_init 登録

**ds1386_clock_init**: DS1386 のオシレータと SQW 出力が有効（bit7=0, bit6=0）で
あることを確認・設定するのみ。SRM が既に 1024 Hz で SQW を設定しているため、
基本的にはノーオペレーション。

**年処理**: 2桁 BCD の年 < 70 → 2000 + 年、>= 70 → 1900 + 年

### 修正 4: files.alpha / AXPVME 設定

`sys/arch/alpha/conf/files.alpha`:
```
device  axprtc
attach  axprtc at isa
file    arch/alpha/isa/ds1386.c  axprtc
```

（注: `ds1386` は末尾が数字のため config ツールがユニット番号と誤解するため `axprtc` と命名）

`sys/arch/alpha/conf/AXPVME`:
```
mcclock* at  isa? port 0x70    # AXPvme 230 ではスキップ (mcclock_isa.c 参照)
axprtc*  at  isa? port 0x8000  # DS1386 TOY clock on AXPvme 230
```

### 期待される起動ログ

```
mcclock0 at isa0 port 0x70-0x71: (not attaching on AXPvme 230)
axprtc0 at isa0 port 0x8000: DS1386 real-time clock
```

### ビルド結果

ビルド成功 ✓（`~/obj/sys/arch/alpha/compile/AXPVME/netbsd`）

### 次のアクション

1. 実機テスト:
   - `axprtc0 at isa0` 等のメッセージが出力されることを確認
   - mcclock ハングが解消されることを確認
   - axpvme_poll が 1024 Hz 近くで動作することを確認
   - DHCP・NFS が高速に完了することを確認

---

## 2026-06-29: `/sbin/init` 起動ハング — 根本原因診断と修正

### 問題の診断

tcpdump で、システムが以下の順にファイルを NFS 経由でロードしていることを確認:

```
"dev" / "console" / "sbin" / "init" / "libexec" / "ld.elf_so" /
"etc" / "ld.so.conf" / "lib" / "libutil.so.7" / "libutil.so.7.24" /
"libcrypt.so.1" / "libcrypt.so.1.0" / "libc.so.12" / "libc.so.12.220.1" /
"malloc.conf" / "console"  ← 2回目のここでハング
```

2回目の `"console"` lookup は、動的リンカが全共有ライブラリをロード完了した後、
init が `/dev/console` を実際に open して write しようとしたタイミング。
最後の NFS アクティビティから 7.5 分後にサーバー側から FIN が来てカーネルが
ARP にも応答していなかったため、カーネルが完全にフリーズしていることを確認。

### 根本原因

**lwp0 PCB (`物理 0x10a000`) + SRM PROM scratch + Bcache dirty = 永久ハング**

1. `start_init()` の fork 以降、コンテキストスイッチが始まる
2. lwp0 の PCB (`0x10a000`) へ K0SEG で書き込み → L1 dirty
3. 活発な cache activity（共有ライブラリ read 等）により L1 eviction が発生
4. dirty L1 line → Bcache fill → **Bcache の `0x10a000` が dirty**
5. init が `/dev/console` への write → `promcnputc` → SRM PROM callback
6. PROM callback が `0x10a000` を K1SEG write → Bcache probe → dirty line 発見
7. AXPvme 230 の Bcache controller が probe/invalidation に応答しない → **永久ハング**

早期 boot 中（`entropy: best effort` まで）は共有ライブラリ大量 read 前のため
L1 eviction が不十分で Bcache は `0x10a000` について clean → PROM call が成功していた。

### 試みて失敗したアプローチ: MEMC_CAR で BCE=0（Bcache 無効化）

`dec_axpvme_64_init()` 内で `REGVAL64(LCA_MEMC_CAR) = car & ~MEMC_CAR_BCE` を試みた。

**失敗理由**: LCA の MEMC_CAR に BCE=0 を書いた瞬間、LCA が Bcache の全 dirty line
flush を開始する。AXPvme 230 の外部 Bcache controller はこの flush プロトコルにも
応答しないため、書き込み直後から全メモリアクセスがブロックされる。
実機では `"Entering netbsd at ..."` の直後（最初の printf より前）にハング。

→ **MEMC_CAR による BCE 無効化は根本的に実行不可能。完全に revert。**

### 正しい修正: `promcnputc` / `promcngetc` に `start_init_exec` ガードを追加

**設計の鍵**: Bcache を無効化するのではなく、PROM callback 自体を条件付きで
スキップする。

`sys/kern/init_main.c` の `start_init_exec` 変数:
- 初期値 0
- `start_init_exec = 1` のセット = `/sbin/init` exec の直前（`vfs_mountroot()` 完了後）
- この時点で全 NFS boot メッセージ・`entropy: best effort` は出力済み

#### `sys/arch/alpha/alpha/prom.c` の変更

`promcnputc` と `promcngetc` に以下を追加:

```c
extern int start_init_exec;   /* sys/kern/init_main.c; set=1 before init exec */

/* In promcnputc / promcngetc: */
if (cputype == ST_DEC_AXPVME_64 && start_init_exec)
    return;  /* (return 0 for promcngetc) */
```

#### タイミング解析

| フェーズ | `start_init_exec` | PROM callback | 説明 |
|---------|-------------------|---------------|------|
| autoconf（device attach） | 0 | 動作 | Bcache clean、問題なし |
| NFS boot（vfs_mountroot） | 0 | 動作 | Bcache clean、問題なし |
| `entropy: best effort` | 0 | 動作 | 同上 |
| → `start_init_exec = 1` セット | **1** | **スキップ** | ここ以降は PROM 無音 |
| init exec + 共有ライブラリ load | 1 | スキップ | L1 eviction → Bcache dirty になるが問題なし |
| init が `/dev/console` open/write | 1 | スキップ | ハング回避 ✓ |

#### 注意点・制限

- `start_init_exec = 1` 以降のカーネルパニックはコンソール出力なしとなる。
  デバッグ能力が制限されるが、diskless boot 初回確認には十分。
- 長期的には Z8530 SCC コンソールドライバを実装して PROM console を完全に置換する。
- `cold` 変数は `configure2()` で 0 になるが、その後に NFS boot メッセージが
  出力されるため `!cold` チェックでは不可。`start_init_exec` が正確なゲート。

### ビルド結果

ビルド成功 ✓（`~/obj/sys/arch/alpha/compile/AXPVME/netbsd`）

### 実機テスト結果 ✓

**2026-06-30: NetBSD/alpha on DEC AXPvme 230 diskless NFS boot 完全成功。**

```
NetBSD client 11.99.6 NetBSD 11.99.6 (GENERIC-$Revision: 1.421 $) #110: \
    Tue Jun 30 06:42:32 JST 2026  ocha@ocha-ubuntu:/home/ocha/obj/sys/arch/alpha/compile/AXPVME alpha
```

- `entropy: best effort` は start_init_exec = 1 以降のため出ないが問題なし
- ping (ICMP) に応答 → カーネルは生存確認済み
- NFS lookup シーケンスにより以下が確認できた:

```
"dev","console"     → init が /dev/console を open
"sbin","init"       → /sbin/init ロード
"libexec","ld.elf_so" → 動的リンカ
"lib","libutil.so.7.24","libcrypt.so.1.0","libc.so.12.220.1" → ライブラリロード
"bin","sh"          → /bin/sh 起動
"rc","rc.subr","rc.conf","defaults/rc.conf" → /etc/rc スクリプト実行
"null","sysctl","constty","ttys" → デバイス・設定参照
"nsswitch.conf","nss_*.so.0" → NSS モジュードロード
"spwd.db"           → パスワードDB参照
```

rc スクリプトが最後まで実行されている。マルチユーザー起動に成功。

---

## 2026-06-30: NFS root 設定（サーバー側）

diskless boot を動かすために NFS root 側（`/export/client/root/` 以下）で
必要だった設定をまとめる。

### 1. `/etc/rc.conf` の修正

```
rc_configured=NO  →  rc_configured=YES
```

`rc_configured=NO` のままだと `/etc/rc` が single-user shell（`/bin/sh`）を起動して
そこで止まる。multi-user boot に進まないため sshd 等のデーモンが一切起動しない。

`sshd=YES`・`ftpd=YES` は最初から設定されていた。

### 2. マウントポイントの作成

```bash
mkdir /export/client/root/usr
mkdir /export/client/root/home
```

`/etc/fstab` に以下のエントリがあるが、root filesystem 内にディレクトリが
存在しなかったため NFS mount が失敗していた。

```
nfsserver:/export/client/usr   /usr   nfs  rw 0 0
nfsserver:/export/client/home  /home  nfs  rw 0 0
```

`/usr` が mount されないと `/usr/sbin/sshd`・`/usr/bin/grep` 等が全て
"not found" になり、rc スクリプトが多数エラーを出しながらも完走するが
肝心のデーモンが起動しない。

### 3. SSH ホスト鍵の生成

```bash
ssh-keygen -t rsa   -b 4096 -f /export/client/root/etc/ssh/ssh_host_rsa_key   -N ""
ssh-keygen -t ecdsa         -f /export/client/root/etc/ssh/ssh_host_ecdsa_key  -N ""
ssh-keygen -t ed25519       -f /export/client/root/etc/ssh/ssh_host_ed25519_key -N ""
chmod 600 /export/client/root/etc/ssh/ssh_host_*_key
```

### 4. `sshd_config` への `PermitRootLogin yes` 追加

デフォルトでは root ログインが禁止されているため追加が必要。

```bash
echo "PermitRootLogin yes" >> /export/client/root/etc/ssh/sshd_config
```

### 5. `authorized_keys` の設置

```bash
mkdir -p /export/client/root/root/.ssh
chmod 700 /export/client/root/root/.ssh
cat ~/.ssh/id_*.pub > /export/client/root/root/.ssh/authorized_keys
chmod 600 /export/client/root/root/.ssh/authorized_keys
```

### 6. `/etc/fstab` への `ptyfs` 追加

```
ptyfs  /dev/pts  ptyfs  rw
```

これがないと PTY allocation が失敗し `ssh` でインタラクティブシェルが
開けない（`ssh -T` のみ使用可能な状態になる）。

### 結果

```
$ ssh root@192.168.99.10
# uname -a
NetBSD client 11.99.6 NetBSD 11.99.6 (GENERIC-$Revision: 1.421 $) \
    #110: Tue Jun 30 06:42:32 JST 2026 \
    ocha@ocha-ubuntu:/home/ocha/obj/sys/arch/alpha/compile/AXPVME alpha
```

```
$ ftp 192.168.99.10
Connected to 192.168.99.10.
220 client.kanpapa.com FTP server (NetBSD-ftpd 20230930) ready.
```

DEC AXPvme 230 で NetBSD/alpha diskless NFS boot → SSH・FTP ログイン成功。

起動後のプロセス状態（`ps ax`）:

```
 PID TTY   STAT    TIME COMMAND
   0 ?     DKl  0:17.71 [system]
   1 ?     Is   0:01.02 init
 446 ?     Ss   0:00.79 /usr/sbin/syslogd -s
 815 ?     Is   0:00.31 sshd: /usr/sbin/sshd [listener] 0 of 10-100 startups
 868 ?     I    0:01.74 pickup -l -t unix -u
 893 ?     Ss   0:06.09 sshd: root@pts/0 (sshd)
 982 ?     Is   0:00.15 /usr/sbin/inetd -l
1303 ?     I    0:01.76 qmgr -l -t unix -u
1365 ?     Is   0:00.28 /usr/sbin/cron
1427 ?     Is   0:01.24 /usr/libexec/postfix/master -w
1432 ?     I    0:01.92 ftpd: nfsserver.kanpapa.com: connected: USER root
1571 ?     Is   0:00.25 /usr/libexec/ftpd -ll -D
 916 pts/0 Ss   0:01.45 -sh
1582 ?     Is+  0:00.32 /usr/libexec/getty std.9600 constty
```

init, syslogd, sshd, inetd, cron, postfix, ftpd が全て正常稼働。
PID 1582 の getty は constty（シリアルコンソール）で待機しているが、
PROM console 抑制中のためキー入力は届かない。
Z8530 SCC ドライバを実装すれば物理コンソールも復活する。

### 次のアクション

1. 長期: Z8530 SCC コンソールドライバの実装（PROM console からの脱却）
2. 長期: PCI interrupt routing の整理（`pci_axpvme_64.c`）

---

## 2026-07-01: Z8530 SCC カーネルコンソール実装

### 背景

NFS ブート・SSH アクセスは達成済みだったが、PROM console の `start_init_exec` ガードにより
init 起動後はカーネルメッセージが出力されなくなっていた。Z8530 SCC（ISA I/O 0x6000）を
直接駆動するポールドコンソールドライバを実装してこれを解消した。

### 調査: ハードウェアレイアウト（Technical Description 参照）

`AXPvme Single-Board Computer Technical Description.pdf` の Figure 1-43（SCC Memory Map）より：

```
UART_BASE_ADDR = ctb_csr = 0x6000

ISA I/O offset   Read (Rd)    Write (Wr)
+00              Ch B RR0     Ch B WR0   (control) — uncommitted
+04              Ch B Rx      Ch B Tx    (data)
+08              Ch A RR0     Ch A WR0   (control) — user console
+0C              Ch A Rx      Ch A Tx    (data)
```

- Channel A が user console（TD 1.12.4節）
- 各レジスタは ISA I/O 空間内で 4 バイト境界（ロングワード）に配置
- `ctb_csr` は SCC 全体のベースアドレス（Channel B のアドレス）

CTB（Console Terminal Block）形式は `struct ctb_tt`：
- `ctb_type = CTB_PRINTERPORT (2)`
- `ctb_csr  = 0x6000`（UART_BASE_ADDR）
- `ctb_baud = 9600`

### 実装ファイル

**新規作成: `sys/arch/alpha/isa/zs_isa.c`**

ポールド Z8530 コンソール。重要な設定値：
- `ZSISA_CTRL = 0x08`（Channel A WR0/RR0）
- `ZSISA_DATA = 0x0C`（Channel A TX/RX data）
- `ZSISA_MAPSIZE = 0x10`（0x6000〜0x600F をマップ）
- TX_READY = `ZSRR0_TX_READY` (bit 2)、RX_READY = `ZSRR0_RX_READY` (bit 0)
- SRM がすでに Channel A を 9600 baud 8N1 に設定済み → ドライバ側で再設定不要
- `zsisa_cnattach()` で先代コンソール（promcons）の `cn_dev` を引き継ぐ
  （`cn_dev = NODEV` のまま切り替えると `cnopen: no console device` でパニックするため）

**新規作成: `sys/arch/alpha/isa/zs_isa.h`**

`zsisa_cnattach()` のプロトタイプ。

**変更: `sys/arch/alpha/alpha/dec_axpvme_64.c`**

`dec_axpvme_64_cons_init()` に実装：
- CTB から `ctb_csr`（= 0x6000）を読み取り
- Channel A RR0（offset 0x08）をプローブして TX_RDY を確認
- `zsisa_cnattach()` を呼び出して Z8530 を kernel console に設定

**変更: `sys/arch/alpha/conf/files.alpha`**

- `file arch/alpha/isa/zs_isa.c  dec_axpvme_64` を追加
- `DEC_AXPVME_64` の defflag から `alpha_pci_consinit` を削除（未使用）

### デバッグで遭遇したバグと解決

**バグ 1: Channel B を誤って使用、存在しない ISA ポートへの書き込み**

最初の実装では offset 0（Ch B ctrl）と offset 1（存在しない ISA 0x6001）を使用していた。
ISA 0x6001 は SCC のどのレジスタにも対応せず、バスタイムアウト → hang。

解決: Technical Description を確認し、Channel A が offset 0x08/0x0C にあることを把握して修正。
**教訓: ハードウェアドライバ実装前に必ず Technical Description を確認すること。**

**バグ 2: `cn_dev = NODEV` による `cnopen` パニック**

init が `/dev/console` を open しようとすると `cnopen()` が `cn_dev == NODEV` を検出して
`panic: cnopen: no console device` を起こした。

解決: `zsisa_cnattach()` で `zsisa_consdev.cn_dev = cn_tab->cn_dev`（promcons の device 番号）を引き継ぐ。
カーネル `printf` は Z8530 経由で出力、`/dev/console` の open は promcons device として処理される。

**バグ 3: `axpvme_poll` デバッグ出力が 2 秒ごとに連続出力**

`pci_axpvme_64.c` の debug print 条件 `(axpvme_poll_ticks & 0x27ff) == 0` が
意図した 10 秒間隔ではなく 2 秒間隔で発火していた（ビットマスクの誤り）。

解決: ポーリングが正常動作（NFS・SSH 稼働確認済み）のためデバッグ print を削除。

### 達成された状態

- Z8530 Channel A（9600 baud 8N1）がカーネルコンソールとして機能
- カーネルのブートメッセージが実機シリアル端末に表示される
- NFS ルートマウント、SSH ログイン、ps 等の動作確認済み
- `start_init_exec` ガード後もシリアル端末にカーネルメッセージが出力される

### 残作業

- Z8530 の本格的な `zstty` TTY ドライバ実装（ユーザ空間から `/dev/console` への R/W）
- シリアル端末でのログインセッション（getty → login）

---

## 2026-07-03: Z8530 SCC 割り込み調査 と zsisa TTY ドライバ実装

### 割り込み構成の確認（AXPvme TD Section 1.17 / Figure 1-64）

Technical Description を確認し、Z8530 UART の割り込み配線を明らかにした：

- UART → **SIO IRQ4**（標準 ISA COM1 と同じ）
- SIO は LCA CPU IRQ\<1\> に接続
- VIC64 も UART を LICR5 (rank 16) で処理可能（IRQ\<2\> 経由）

**IRQ4 が使えない理由：**  
AXPvme 230 の SRM PAL は VIC64 (CPU IRQ\<2\>) 向けに設計されており、SIO 割り込み (CPU IRQ\<1\>) の処理時に **PCI IACK サイクルを実行しない**。IACK なしで 8259 割り込みが発生すると `scb_stray → printf → PROM コールバック → Bcache 無効化プロトコル → HANG` となる。このため IRQ4 を含む全 ISA IRQ（IRQ1/IRQ2 以外）を 8259 でマスクしている（`pci_axpvme_64.c` 参照）。

### zsisa TTY ドライバ設計・実装

**方針：**
- 割り込みを使わずポーリング TTY を実装
- `z8530sc`/`zstty` フレームワークは使用しない（ハードウェア割り込みが前提のため）
- RX: 1024 Hz callout で `ZSRR0_RX_READY` をポーリング → `l_rint` に流す
- TX: `t_oproc = zsisa_tty_start` でブロッキングポーリング TX
- ISA autoconf `CFATTACH_DECL_NEW(zsisa, ...)` で autoconf に組み込む
- `zsisa_attach` 後に `cn_tab->cn_dev` を zsisa0 のデバイス番号に更新

**修正ファイル：**

| ファイル | 変更内容 |
|--------|--------|
| `sys/arch/alpha/isa/zs_isa.c` | ISA attachment + polled TTY (cdev ops, callout RX, polled TX) を追加 |
| `sys/arch/alpha/isa/zs_isa.h` | `zsisa_cdevsw` extern 追加 |
| `sys/arch/alpha/conf/files.alpha` | `device zsisa: tty` / `attach zsisa at isa` 追加 |
| `sys/arch/alpha/conf/AXPVME` | `zsisa0 at isa? port 0x6000` 追加 |

**ビルド時に発生したバグと修正：**

1. `dev_type_stop` のシグネチャ誤り  
   - 誤: `void zsisa_stop(dev_t dev, int flag)` → 正: `void zsisa_stop(struct tty *tp, int flag)`
   - `dev_stop_t = void(struct tty *, int)` が正しいシグネチャ

2. `zsisa_match` probe で `already_mapped` 時に `ia->ia_iot` を使っていた問題  
   - `zsisa_cnattach` が使う `lcp->lc_iot` と `ia->ia_iot` は別ポインタ
   - `already_mapped = true` のときは `zsisa_iot` と `zsisa_ioh` を使うよう修正

**コミット:** `e04c4bbfb75`

### 残作業

- AXPvme 230 実機でテスト：zsisa0 attach → cn_dev 更新 → getty/login 動作確認
- NFS root の `/etc/ttys` または `inittab` で `/dev/console` に getty を設定
- 必要なら `/dev/zsisa0` の dev ファイル作成（major 番号は動的割り当て）

---

## 2026-07-03: `panic: sn->sn_opencnt` 修正 — `device-major zsisa` の欠落

### 症状

zsisa0 が attach された後（t=1.0s）、init (pid 104) が起動し
t≈13.9s で以下のパニックが発生した：

```
panic: kernel diagnostic assertion "sn->sn_opencnt" failed: file
    "sys/miscfs/specfs/spec_vnops.c", line 1722
Stopped in pid 104.104 (init) at netbsd:cpu_Debugger+0x4
```

### 根本原因: `device-major zsisa` が `majors.alpha` に未登録

`zsisa_attach()` 内：

```c
maj = cdevsw_lookup_major(&zsisa_cdevsw);  // → NODEVMAJOR = -1
dev = makedev(maj, device_unit(self));      // → makedev(-1, 0) = ゴミ値
cn_tab->cn_dev = dev;                       // → cn_dev にゴミ値
```

`cdevsw_lookup_major(&zsisa_cdevsw)` は cdevsw テーブルを線形検索するが、
`device-major zsisa char N zsisa` エントリが `majors.alpha` に存在しないため
`NODEVMAJOR` (-1) を返す。

`makedev(-1, 0)` は存在しない major に対応するゴミ dev_t になる。
その結果 `cn_tab->cn_dev` がゴミ値に設定される。

#### パニックに至るシーケンス

1. init が `/dev/console` を **第一回 open** → `cnopen()` 呼び出し
2. `cndev = cn_tab->cn_dev = ゴミ値`  
   `cdevvp(ゴミ値, &cn_devvp[0])` → `vnode_A`（sn_opencnt=0）を作成して `cn_devvp[0]` に格納
3. `VOP_OPEN(vnode_A)` → `spec_open(vnode_A)` → `sn_opencnt=1` →  
   `cdev_open(ゴミ値, ...)` → `cdevsw_lookup(ゴミ値)` → NULL → **ENXIO**
4. `spec_open` はロールバック：`sn_opencnt-- = 0`
5. `cnopen()` は ENXIO を返す。`cn_devvp[0] = vnode_A`（sn_opencnt=0）のまま

6. init が `/dev/console` を **第二回 open** → `cnopen()` 呼び出し
7. `cn_devvp[0] != NULLVP` → **VOP_OPEN を呼ばずに return 0（成功）**
   `vnode_A->sn_opencnt` は依然 0

8. init が fd を **close** → `spec_close(CN_VN)` → `cnclose()` →  
   `VOP_CLOSE(cn_devvp[0] = vnode_A)` → `spec_close(vnode_A)` →  
   `KASSERT(sn->sn_opencnt)` ← sn_opencnt=0 → **PANIC**

### 修正: `majors.alpha` に `device-major zsisa char 82 zsisa` を追加

`sys/arch/alpha/conf/majors.alpha`:
```
device-major	sysmon		char 80			sysmon
device-major	zsisa		char 82			zsisa    ← 追加
```

major 82 は既存エントリとの重複なし（コメント「Majors up to 143 are reserved for MD」）。

**効果:**  
- `cdevsw[82] = &zsisa_cdevsw` が generated `devsw.c` に登録される  
- `cdevsw_lookup_major(&zsisa_cdevsw) = 82` が返る  
- `cn_tab->cn_dev = makedev(82, 0)` = 正しい zsisa0 デバイス番号  
- `cnopen()` → `VOP_OPEN(zsisa_vn)` → `spec_open` → `sn_opencnt=1` → `zsisa_open()` → 成功  
- `cnclose()` → `VOP_CLOSE(zsisa_vn)` → `KASSERT(sn_opencnt=1)` ✓ パニックなし

### NFS root への /dev/zsisa0 作成（ユーザーが手動で実施）

```bash
# NFS サーバーの root export ディレクトリで：
mknod /export/client/root/dev/zsisa0 c 82 0
chmod 600 /export/client/root/dev/zsisa0
```

`/dev/console` は major=0（console pseudo-device）を通じて zsisa0 に
ルーティングされるため、getty は `/dev/console` を使えば /dev/zsisa0 なしでも動く。
直接 `/dev/zsisa0` を使う場合（`/etc/ttys` 設定等）に必要。

### ビルド結果

ビルド成功 ✓（`~/obj/sys/arch/alpha/compile/AXPVME/netbsd`）

生成確認:
```
devsw.c:298:  extern const struct cdevsw zsisa_cdevsw;
devsw.c:401:  &zsisa_cdevsw,	//  82
```

### 変更ファイル

| ファイル | 変更内容 |
|--------|--------|
| `sys/arch/alpha/conf/majors.alpha` | `device-major zsisa char 82 zsisa` を追加 |

### 実機テスト結果 ✓（2026-07-03）

**2026-07-03: Z8530 SCC 物理コンソールからの root ログイン成功。**

```
Starting cron.
Thu Jul  3 19:23:07 UTC 2064

NetBSD/alpha (client) (constty)

login: root
Jul  3 19:25:02 client login: ROOT LOGIN (root) on tty constty
Last login: Tue Jul  1 19:24:47 2064 from 192.168.99.1 on pts/0
...
NetBSD 11.99.6 (GENERIC-$Revision: 1.421 $) #121: Fri Jul  3 07:33:26 JST 2026
client# pwd
/root
client#
```

- `constty`（= zsisa0）上で getty が動作 ✓
- root ログイン成功 ✓
- PROM console ガードを撤廃せずとも物理コンソールが完全動作 ✓
- Z8530 SCC ポールド TTY ドライバが完成

---

## 2026-07-03: PROM console `start_init_exec` ガードの撤廃

### 背景

Z8530 SCC ドライバ実装完了により、`zsisa_cnattach()` の時点で
`cn_tab = &zsisa_consdev`（cn_putc = `zsisa_cnputc`）に切り替わる。
以後 `printf` は `zsisa_cnputc` 経由となり、`promcnputc` は呼ばれない。

`start_init_exec` ガードは「init exec 後に PROM callback が
物理 `0x10a000`（lwp0 PCB）に K1SEG write → Bcache probe → ハング」を
防ぐために追加したものだが、zsisa が console を引き継いだ後は
dead code となっていた。

### 変更内容（`sys/arch/alpha/alpha/prom.c`）

- `extern int start_init_exec;` 宣言を削除
- ガードコメントブロック（背景説明）を削除
- `promcnputc` / `promcngetc` / `promcnlookc` の各ガード判定を削除:
  ```c
  // 削除:
  if (cputype == ST_DEC_AXPVME_64 && start_init_exec)
      return;   // (または return 0)
  ```

### ビルド結果

ビルド成功 ✓（`~/obj/sys/arch/alpha/compile/AXPVME/netbsd`）

### 実機テスト結果 ✓

boot → login → shutdown まで Z8530 シリアルコンソールで問題なく動作確認済み。
カーネルメッセージが init 起動後も物理コンソールに出力されるようになった。

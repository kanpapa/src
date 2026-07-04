# DEC AXPvme 230 NetBSD/alpha ポーティング記録

DEC AXPvme 230（VMEbus Alpha SBC、21066A/LCA チップセット、system type 10）に
NetBSD/alpha を diskless NFS ブートで動かすまでのポーティング記録。
Alpha アーキテクチャに不慣れな方向けに、課題の背景から解決策まで解説する。

- **ターゲット**: DEC AXPvme 230 (ST_DEC_AXPVME_64 = 10)
- **ブート方式**: Diskless NFS boot
- **最終確認**: Kernel #123 — boot → Z8530 シリアルコンソール login → shutdown 正常動作
- **期間**: 2026-06-25 〜 2026-07-04

---

## Alpha アーキテクチャの予備知識

課題を理解するために必要な概念を先に説明する。

**SRM PROM（ファームウェア）**
PC の BIOS に相当する。Alpha では OS ブート後も SRM がシリアルポートへの出力などを担当し、OS からの「コールバック」で呼び出される。

**PALcode（Privileged Architecture Library）**
Alpha 固有のマイクロコード層。コンテキストスイッチ（プロセスの切り替え）などは `call_pal` 命令で PALcode を呼び出す。

**K0SEG / K1SEG（メモリセグメント）**
- `K0SEG`（`0xfffffc…|PA`）: L1 キャッシュ経由のアクセス（通常の書き込み）
- `K1SEG`（`0xfffffe…|PA`）: キャッシュを経由しない直接アクセス（uncached）
  同じ物理アドレスへのアクセスでも動作が全く異なる。

**外部 Bcache（ライトバック外部キャッシュ）**
AXPvme 230 の 21066A は 512 KB 外部ライトバック Bcache を持つ（`MEMC_CAR: BCE=1, BCS=3`）。
L1 キャッシュからあふれたデータが Bcache に書かれ、DRAM にはすぐ届かない。
**AXPvme 230 固有の問題**: Bcache の「probe/invalidation プロトコル」が機能しない。
K1SEG 書き込みや `PAL_cflush` はこのプロトコルを発行するため、AXPvme 230 では永久ハングする。

**PCB（Process Control Block）**
プロセスの「保存ファイル」。コンテキストスイッチ時にレジスタ・スタックポインタをここに保存・復元する。
lwp0（最初のスレッド）の PCB は物理アドレス `0x10a000` に配置された。

---

## 課題一覧（時系列順）

| # | 症状 | 根本原因 | 解決策 | 変更ファイル |
|---|------|----------|--------|-------------|
| 01 | `cpu_notsupp` で即終了 | system type 10 未登録 | `dec_axpvme_64_init` 登録 | `cpuconf.c`, `dec_axpvme_64.c` |
| 02 | `swpctx` で halt code 6 | PCB が PROM scratch で破壊 + Bcache dirty + locore debug print | K0SEG PCB 書き直し + debug print 除去 | `machdep.c`, `locore.s` |
| 03 | 割り込みが届かない | 8259 ICW2 が PC 互換値のまま | ICW1-4 完全再初期化 | `sio_pic.c` |
| 04 | DMA TX/RX が失敗 | Bcache dirty が DRAM 未到達 + XOR バグ | 競合エビクション sync（`va ^ bcache_size`） | `lca_dma.c` |
| 05 | NIC TX サイレント失敗 | 21040 が 10BASE-T 設定、AUI コネクタ未使用 | `tlp_21040_auibnc_mediasw` 強制 | `if_tlp_pci.c` |
| 06 | DHCP パケット未送信 | TX 割り込みが `IFF_RUNNING=0` でスキップ | `IFF_RUNNING` 設定後に `tlp_txintr()` ポーリング | `tulip.c` |
| 07 | 割り込みでシステムハング | SRM PAL が IACK 不実行 → `scb_stray` → printf → Bcache hang | 全 ISA IRQ マスク、1024 Hz ソフトウェアポーリング | `pci_axpvme_64.c` |
| 08 | NIC ポーリング 1.4 Hz | `KERNEL_LOCK` 競合で callout が 700ms ブロック | `CALLOUT_MPSAFE` + `ih_real_fn` 直接呼び出し | `pci_axpvme_64.c` |
| 09 | autoconf で無限ループ停止 | MC146818 水晶未接続 → PF フラグが永遠に立たない | PCC タイムアウト + mcclock スキップ + DS1386 新規ドライバ | `mcclock.c`, `mcclock_isa.c`, `ds1386.c` |
| 10 | init 起動後にシステムフリーズ | PROM callback → K1SEG write → Bcache probe → ハング | `start_init_exec` ガード（後に撤廃） | `prom.c` |
| 11 | 物理コンソール未実装 | Z8530 SCC TTY ドライバなし | Z8530 ポールドコンソール + TTY ドライバ新規実装 | `zs_isa.c` |
| 12 | `zsisa0` が attach されない | autoconf 中に TX ビジー → `TX_RDY=0` → probe 失敗 | already_mapped 時は probe スキップ | `zs_isa.c` |
| 13 | `panic: sn->sn_opencnt` | `majors.alpha` に zsisa 未登録 → ゴミ dev_t | `device-major zsisa char 82` 追加 | `majors.alpha` |
| 14 | PROM ガードが dead code | zsisa が `cn_tab` を引き継いだため promcnputc 不使用 | `start_init_exec` ガード削除 | `prom.c` |

---

## 詳細解説

### 課題 01 — cpu_notsupp で即終了

NetBSD/alpha は起動時に SRM PROM からシステムタイプ番号を読み取り、
どの初期化コードを呼ぶか決定する。AXPvme 230 のシステムタイプ `10`（`ST_DEC_AXPVME_64`）が
`cpuconf.c` のテーブルに未登録のため、カーネルは「サポートされていない CPU」と表示して即終了していた。

```c
/* cpuconf.c への追加 */
cpu_init(ST_DEC_AXPVME_64, dec_axpvme_64_init, "AXPvme 64"),
```

新規ファイル `dec_axpvme_64.c` でプラットフォーム初期化関数を実装し、
カーネルコンフィグ `AXPVME` も新規作成した。

---

### 課題 02 — swpctx で halt code 6（最難関）

`swpctx`（PALcode のコンテキストスイッチ命令）実行時に halt code 6（double error halt）が発生。
PC=`0x19e90` で停止した。

#### 2 つの要因が重なっていた

**要因 1 — SRM PROM が PCB の物理アドレスに上書きする**

lwp0 の PCB はたまたま物理 `0x10a000` に置かれた。
SRM はコンソール `printf` コールバックのスクラッチ領域としてまさに `0x10a000` を使っており、
`printf` のたびに古いブートデータ（`0xde0a`）を書き込んで `apcb_ksp`（スタックポインタ）を破壊していた。

**要因 2 — locore.s に debug print が残っていた**

`CALL(alpha_init)` と `call_pal PAL_OSF1_swpctx` の間にデバッグ出力（`mark1`/`mark2`）があった。
この printf → SRM コールバック → `0x10a000` 上書きが `swpctx` 直前に発生していた。

#### 失敗したアプローチ: Bcache 無効化（BCE=0）

`MEMC_CAR` の `BCE=0` を書き込もうとしたが、LCA が Bcache の全 dirty ライン flush を開始し、
外部コントローラが flush に応答しないため全メモリアクセスがブロックされた。
L1 キャッシュヒットで数命令は動くが、L1 miss 発生時点でフリーズ。
この方向は根本的に実行不可能と判断し完全に断念した。

#### 解決策

```c
/* machdep.c: alpha_init() 末尾 — 最後の printf より後 */
#ifdef DEC_AXPVME_64
if (cputype == ST_DEC_AXPVME_64) {
    /* K0SEG（キャッシュ経由）で PCB を正しい値に書き直す。
     * K1SEG 書き込みは Bcache probe → ハングするため不可。 */
    struct alpha_pcb *apcb = (struct alpha_pcb *)ALPHA_PHYS_TO_K0SEG(...);
    apcb->apcb_ksp = ...;
    alpha_mb();
}
#endif
```

- `machdep.c`: `alpha_init()` 末尾で K0SEG（キャッシュ経由）を使って PCB を正しい値に書き直す
- `locore.s`: `mark1`/`mark2` debug print を完全に削除

**重要な不変条件**: `alpha_init()` の K0SEG ブロック以降、`swpctx` 実行まで printf を一切呼んではならない。

---

### 課題 03 — 割り込みが間違ったハンドラへ飛ぶ（8259 ICW2 誤設定）

SRM ファームウェアは 8259 PIC の ICW2（割り込みベクタベース）を
PC 互換値（マスタ=`0x08`、スレーブ=`0x70`）のまま残していた。
Alpha PALcode は IACK で IRQ 0〜15 の直値が返ることを期待するため、
ICW2 はマスタ=`0x00`、スレーブ=`0x08` でなければならない。
ベクタがずれて `scb_stray` や無関係なハンドラに飛び、NIC ドライバが一切呼ばれなかった。

```c
/* sio_pic.c: 8259 完全再初期化 */
bus_space_write_1(..., 0x11);  /* ICW1: カスケード、ICW4 必要 */
bus_space_write_1(..., 0x00);  /* ICW2: マスタ ベクタベース=0 */
bus_space_write_1(..., 0x08);  /* ICW2: スレーブ ベクタベース=8 */
/* ICW3/ICW4... */
```

他の Alpha プラットフォームでは SRM が正しく設定済みのため、全機種共通修正として安全。

---

### 課題 04 — DMA TX/RX が失敗（Bcache コヒーレンシ問題）

CPU が DMA ディスクリプタを Bcache に書いても DRAM にはまだ届いておらず、
NIC が DRAM を読むと古いデータ（`OWN=0`）を見てしまう。
通常のキャッシュフラッシュ手段（K1SEG 書き込み、`PAL_cflush`）は AXPvme 230 でハングするため使えない。

**競合エビクション（conflict-eviction）を使う**:
直接マッピング 512 KB Bcache で PA ^ 512KB を K0SEG 経由で読むと、
PA の dirty ラインが DRAM に追い出される（外部プロトコルなし）。

```c
/* 誤: 加算（PA > 63.5MB で PA+512KB > 64MB → マシンチェック） */
conflict_va = K0SEG(phys) + bcache_size;

/* 正: XOR（bit19 をトグル = 同一スロット、常に 64MB 以内） */
conflict_va = K0SEG(phys) ^ bcache_size;
```

最初に `+` で実装したため、RAM 63.5MB 以上の mbuf で PA > 64MB となりマシンチェックが発生した。
`^` に修正して解決。

---

### 課題 05 — NIC TX サイレント失敗（AUI メディア未選択）

AXPvme 230 のオンボード 21040（Tulip）NIC はフロントパネルの AUI コネクタのみに接続されており、
10BASE-T や BNC インターフェースは存在しない。
tulip ドライバのデフォルトメディアは 10BASE-T のため、TX フレームがキャリアなしでサイレントに失敗していた。

```c
/* if_tlp_pci.c: AXPvme 230 の場合のみ AUI 強制 */
#if defined(__alpha__) && defined(DEC_AXPVME_64)
if (cputype == ST_DEC_APXVME_64)
    sc->sc_mediasw = &tlp_21040_auibnc_mediasw;
#endif
```

SIA（Serial Interface Adapter）レジスタも SRM が設定した正しい AUI 値にパッチする。

---

### 課題 06 — DHCP パケット未送信（tlp_init 内の競合状態）

`tlp_init` の処理順序に問題があった:

1. フィルタセットアップフレームを TX リングに投入 → NIC が TX 開始
2. SIA メディア設定（TX 一時停止・SIA 再設定・TX 再開）
3. TX 完了 → CPU に割り込み発生
4. `tlp_intr` が呼ばれるが **`IFF_RUNNING` がまだ 0** → 早期リターン
5. エッジトリガー割り込みのため次のエッジは来ない → `DOING_SETUP` が永久にセット

```c
/* tulip.c: IFF_RUNNING 設定直後に追加 */
ifp->if_flags |= IFF_RUNNING;
tlp_txintr(sc);  /* フィルタセットアップ完了を処理 */
```

全チップ共通修正として安全。

---

### 課題 07 — ハードウェア割り込みでシステムハング（IACK なし問題）

AXPvme 230 の SRM PAL は、SIO 8259 の CPU IRQ\<1\> 受信時に
**PCI IACK（割り込み確認）サイクルを実行しない**。
IACK なしでは PALcode がベクタを正しく取得できず `scb_stray` が呼ばれる。
`scb_stray` には `printf` があり:

```
scb_stray → printf → SRM コールバック → 0x10a000 K1SEG write
          → Bcache probe → AXPvme 230 外部コントローラ無応答 → 永久ハング
```

IRQ1/IRQ2 以外の全 ISA IRQ が「届いた瞬間にシステムフリーズ」する状態だった。

**解決策: ソフトウェアポーリング**

ハードウェア割り込みを全て諦め、1024 Hz コールアウトで NIC ハンドラを直接呼び出す。

```c
/* pci_axpvme_64.c */
static void axpvme_poll(void *arg) {
    for (int i = 0; i < n_handlers; i++)
        (*ih[i]->ih_real_fn)(ih[i]->ih_real_arg);  /* tlp_intr 直接 */
    callout_schedule(&axpvme_poll_callout, 1);
}
```

8259 の IRQ はハンドラ登録直後に再マスクして CPU に届かないようにする。

---

### 課題 08 — NIC ポーリングが 1.4 Hz（KERNEL_LOCK 競合）

1024 Hz のはずが実効レート約 1.4 Hz。DHCP 取得に 35 分かかっていた。

NFS ブート中は NFS スタック（sunrpc、ソケット処理）が `KERNEL_LOCK` を最大 700ms 連続保持する。
`axpvme_poll` が `alpha_shared_intr_wrapper` 経由で `KERNEL_LOCK` を要求すると、
ロック取得まで 700ms ブロックされる。1 サイクルが 700ms = 実効 1.4 Hz。

```c
/* 変更前 */
callout_init(&axpvme_poll_callout, 0);
(*ih->ih_fn)(ih->ih_arg);        /* = alpha_shared_intr_wrapper → KERNEL_LOCK */

/* 変更後 */
callout_init(&axpvme_poll_callout, CALLOUT_MPSAFE);  /* callout 側の KERNEL_LOCK も不要 */
(*ih->ih_real_fn)(ih->ih_real_arg);                  /* = tlp_intr 直接 */
```

AXPvme 230 は UP（単一 CPU）のため `KERNEL_LOCK` の SMP 保護は不要。

---

### 課題 09 — autoconf で無限ループ停止（mcclock + DS1386）

NetBSD は ISA バス `0x70/0x71` の MC146818 互換 RTC に対して
16 Hz 周期フラグ（`MC_REGC_PF`）が立つのを無限ループで待つ。
AXPvme 230 の MC146818 は 32.768 kHz 水晶振動子が未接続のため発振せず、
フラグが永遠に立たない → autoconf 中にシステムが完全停止。

**修正 1**: `mcclock_set_pcc_freq()` に 125ms PCC タイムアウトを追加。
**修正 2**: `mcclock_isa_match()` で AXPvme 230 の場合は attach をスキップ。

AXPvme 230 に実際に搭載されているのは **Maxim DS1386** TOY クロック（ISA I/O `0x8000`）。
MC146818 と異なりインデックスポートが不要で各レジスタに直接アクセスできる。
デバイス名は `axprtc` とした（`ds1386` は末尾が数字のため config ツールがユニット番号と誤解する）。

---

### 課題 10 — init 起動後にシステムフリーズ（PROM callback × Bcache）

NFS ブートで `entropy: best effort` まで出力した後、
init が `/dev/console` を open しようとした瞬間に永久ハングした。

```
NFS から共有ライブラリ大量読み込み
 → L1 キャッシュ圧迫で eviction
 → lwp0 PCB (0x10a000) のデータが Bcache に dirty で残留
init → /dev/console write → promcnputc → SRM コールバック
 → 0x10a000 に K1SEG write → Bcache probe → 外部コントローラ無応答 → 永久ハング
```

**暫定修正**: `promcnputc`/`promcngetc` に `start_init_exec`（`/sbin/init` exec 直前に 1 になる変数）
チェックを追加し、init 起動後は PROM コールバックをスキップ。

この修正により init 起動後のカーネルメッセージは出力されなくなるが、
NFS ブート初回確認には十分。Z8530 SCC ドライバ実装後に撤廃した（課題 14）。

---

### 課題 11 — 物理コンソール未実装（Z8530 SCC ドライバ）

PROM console ガードにより init 起動後のカーネルメッセージが出力されなかった。
AXPvme 230 のオンボード Z8530 SCC（ISA I/O `0x6000`）を直接駆動するドライバを実装した。

**Technical Description（PDF）で判明した SCC レイアウト**:
```
ISA I/O offset   Read      Write
+0x00            Ch B RR0  Ch B WR0   (未使用)
+0x04            Ch B Rx   Ch B Tx    (未使用)
+0x08            Ch A RR0  Ch A WR0   ← ユーザーコンソール（制御）
+0x0C            Ch A Rx   Ch A Tx    ← ユーザーコンソール（データ）
```

各レジスタが 4 バイト境界（ロングワード）にあることを TD を見て初めて確認した
（最初 Ch B を使用して ISA バスタイムアウト → ハングするバグを踏んだ）。

SRM がすでに Channel A を 9600 baud 8N1 に設定済みのため、ドライバ側での再設定は不要。

**ISA 割り込み（IRQ4）は使えない**（課題 07 参照）ため、完全ポーリング方式:
- TX: `ZSRR0_TX_READY`（bit2）確認しながら送信（100ms タイムアウト）
- RX: 1024 Hz callout で `ZSRR0_RX_READY`（bit0）をポーリング → `l_rint` に流す

`zsisa_cnattach()` で `cn_tab = &zsisa_consdev` に切り替えることで、
以後の `printf` は Z8530 経由になり PROM コールバックは呼ばれなくなる。

---

### 課題 12 — zsisa0 が autoconf でアタッチされない（probe レース）

ISA autoconf 時に `zsisa_match()` が `ZSRR0_TX_READY` を確認するが、
このとき Z8530 はカーネルブートメッセージを送信中で TX ビジー状態。
1 回読むと `TX_READY=0` → 「デバイスなし」と誤判定して attach がスキップされた。

```c
/* zsisa_match(): 早期 cnattach で確認済みなら probe スキップ */
if (zsisa_iot != NULL)
    goto found;   /* TX_RDY チェックを省略 */
```

---

### 課題 13 — panic: sn->sn_opencnt（majors.alpha 未登録）

zsisa0 attach 約 12 秒後、init（pid 104）が `spec_vnops.c:1722` でパニックした。

**根本原因**: `majors.alpha` に `zsisa` エントリがなく、
`cdevsw_lookup_major(&zsisa_cdevsw)` が `NODEVMAJOR = -1` を返す。

```
makedev(-1, 0) = ゴミの dev_t 値
cn_tab->cn_dev = ゴミ値

cnopen 1回目: ゴミ dev_t で vnode 作成 (sn_opencnt=0) → ENXIO
             → cn_devvp[0] = vnode (sn_opencnt=0 のまま)
cnopen 2回目: cn_devvp[0] != NULL → VOP_OPEN スキップ → "成功"
             → sn_opencnt は依然 0
close: VOP_CLOSE → KASSERT(sn_opencnt) → sn_opencnt=0 → PANIC
```

```
# majors.alpha への追加
device-major    zsisa       char 82     zsisa
```

これにより `cdevsw[82] = &zsisa_cdevsw` が生成 `devsw.c` に登録され、
`cn_tab->cn_dev = makedev(82, 0)` という正しいデバイス番号が設定される。

---

### 課題 14 — PROM ガードが dead code（撤廃）

`zsisa_cnattach()` 後は `cn_tab = &zsisa_consdev`（`cn_putc = zsisa_cnputc`）となり、
`printf` は Z8530 経由で出力される。`promcnputc` は一切呼ばれなくなるため、
課題 10 で追加した `start_init_exec` ガードは dead code となった。

```c
/* 削除した全ガード */
extern int start_init_exec;
if (cputype == ST_DEC_AXPVME_64 && start_init_exec)
    return;  /* promcnputc / promcngetc / promcnlookc の各冒頭から削除 */
```

---

## 最終状態

```
NetBSD 11.99.6 (GENERIC-$Revision: 1.421 $) #123: Fri Jul  3 08:17:15 JST 2026
ocha@ocha-ubuntu:/home/ocha/obj/sys/arch/alpha/compile/AXPVME alpha
```

| 機能 | 状態 |
|------|------|
| NFS diskless boot | 動作 |
| Z8530 シリアルコンソール（9600 baud、ポーリング） | 動作 |
| getty ログイン（constty） | 動作 |
| SSH / FTP アクセス | 動作 |
| syslogd / sshd / inetd / cron / postfix / ftpd | 動作 |
| PCI interrupt routing | ソフトウェアポーリング（1024 Hz）で代替 |

## 主要変更ファイル

| ファイル | 変更内容 |
|--------|--------|
| `sys/arch/alpha/alpha/cpuconf.c` | system type 10 登録 |
| `sys/arch/alpha/alpha/dec_axpvme_64.c` | 新規: プラットフォーム初期化、Z8530 コンソール init |
| `sys/arch/alpha/alpha/machdep.c` | K0SEG PCB 書き直しブロック |
| `sys/arch/alpha/alpha/prom.c` | start_init_exec ガード追加・撤廃 |
| `sys/arch/alpha/alpha/mcclock.c` | PCC タイムアウト追加 |
| `sys/arch/alpha/conf/AXPVME` | 新規: カーネルコンフィグ |
| `sys/arch/alpha/conf/majors.alpha` | device-major zsisa char 82 追加 |
| `sys/arch/alpha/isa/ds1386.c` | 新規: DS1386 TOY クロックドライバ（axprtc） |
| `sys/arch/alpha/isa/mcclock_isa.c` | AXPvme 230 での attach スキップ |
| `sys/arch/alpha/isa/zs_isa.c` | 新規: Z8530 ポールドコンソール + TTY ドライバ |
| `sys/arch/alpha/pci/lca_dma.c` | 競合エビクション DMA sync |
| `sys/arch/alpha/pci/pci_axpvme_64.c` | 新規: ソフトウェアポーリング割り込み |
| `sys/arch/alpha/pci/sio_pic.c` | 8259 ICW1-4 完全再初期化 |
| `sys/dev/ic/tulip.c` | IFF_RUNNING 設定後 tlp_txintr ポーリング |
| `sys/dev/pci/if_tlp_pci.c` | AUI メディア強制 + SIA レジスタパッチ |

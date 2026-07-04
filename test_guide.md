# DEC AXPvme 230 NetBSD/alpha テストガイド

DEC AXPvme 230 上で NetBSD/alpha を diskless NFS ブートで動かすための
テスト環境構成とテスト手順をまとめる。

---

## 1. テスト環境

### 1.1 ハードウェア構成

| 項目 | 内容 |
|------|------|
| **ターゲット機** | DEC AXPvme 230 |
| CPU | Alpha 21066A (LCA チップセット) |
| メモリ | 64 MB |
| Bcache | 512 KB 外部ライトバック (BCE=1, BCS=3) |
| ネットワーク | オンボード 21040 (Tulip) / AUI コネクタのみ |
| シリアル | Z85C30 SCC / Channel A (ISA I/O 0x6000) |
| ファームウェア | SRM Console V17.0-0 / OSF PALcode V1.45-12 |
| **ホスト機** | Ubuntu Linux (ocha-ubuntu) |
| ネットワーク IF | enx6084bd485c85 |

> **注意**: AXPvme 230 は発熱が多いため、電源投入中は必ずエアフローを当てること。

### 1.2 ネットワーク構成

```
[Ubuntu ホスト]                    [AXPvme 230]
192.168.99.1                       192.168.99.10
enx6084bd485c85 ─── (直結/HUB) ─── ewa0 (21040 AUI)
  │
  ├─ NFS サーバー  /export/client/root/
  └─ dnsmasq       DHCP/BOOTP + TFTP
```

- ホスト: `192.168.99.1`
- クライアント (AXPvme): `192.168.99.10`
- NFS root export: `/export/client/root/`

### 1.3 シリアルコンソール接続

| 項目 | 設定値 |
|------|--------|
| ボーレート | 9600 baud |
| データ長 | 8 bit |
| パリティ | なし |
| ストップビット | 1 |
| フロー制御 | なし |
| ホスト側デバイス | `/dev/ttyUSB0`（環境により変わる） |

**ターミナルソフト**: GTKTerm

GTKTerm の設定（Configuration → Port）:
- Port: `/dev/ttyUSB0`（環境により変わる）
- Baud Rate: `9600`
- Bits: `8`
- Stopbits: `1`
- Parity: `None`
- Flow control: `None`

### 1.4 ホスト側ソフトウェア

| ソフトウェア | 用途 |
|-------------|------|
| NetBSD-current ソースツリー | カーネルビルド |
| `build.sh` / clang/gcc (NetBSD クロスコンパイラ) | Alpha クロスビルド |
| NFS サーバー (nfs-kernel-server) | diskless root ファイルシステム提供 |
| dnsmasq | DHCP/BOOTP + TFTP サーバー |
| tcpdump | ネットワークデバッグ |
| GTKTerm | シリアルコンソール |

---

## 2. NFS diskless 環境構築

### 2.1 NFS サーバー設定 (`/etc/exports`)

```
/export/client/root  192.168.99.10(rw,sync,no_subtree_check,no_root_squash)
/export/client/usr   192.168.99.10(rw,sync,no_subtree_check,no_root_squash)
/export/client/home  192.168.99.10(rw,sync,no_subtree_check,no_root_squash)
```

設定反映:
```bash
sudo exportfs -ra
sudo systemctl restart nfs-kernel-server
```

### 2.2 dnsmasq 設定

```bash
sudo systemctl start dnsmasq
```

dnsmasq の DHCP/BOOTP 設定例（`/etc/dnsmasq.conf` または `/etc/dnsmasq.d/axpvme.conf`）:

```
# AXPvme 230 の MAC アドレスを固定 IP に割り当て
dhcp-host=<MACアドレス>,192.168.99.10,axpvme

# BOOTP/DHCP で NFS root パスを通知
dhcp-option=17,192.168.99.1:/export/client/root

# TFTP サーバー（必要な場合）
enable-tftp
tftp-root=/export/client/tftpboot
```

### 2.3 NFS root ファイルシステム構造

```
/export/client/
├── root/          ← NFS root (/) として AXPvme にマウントされる
│   ├── dev/
│   │   ├── console
│   │   └── zsisa0   (mknod c 82 0 で作成)
│   ├── etc/
│   │   ├── rc.conf      (rc_configured=YES, sshd=YES)
│   │   ├── fstab        (ptyfs /dev/pts ptyfs rw を追加)
│   │   └── ssh/
│   │       ├── sshd_config   (PermitRootLogin yes)
│   │       └── ssh_host_*_key
│   ├── root/
│   │   └── .ssh/
│   │       └── authorized_keys
│   ├── usr/   ← マウントポイント
│   └── home/  ← マウントポイント
├── usr/       ← /usr として別マウント
└── home/      ← /home として別マウント
```

**初回セットアップ時の追加作業**:

```bash
# /dev/zsisa0 デバイスファイルを作成（major=82, minor=0）
mknod /export/client/root/dev/zsisa0 c 82 0
chmod 600 /export/client/root/dev/zsisa0

# SSH ホスト鍵の生成
ssh-keygen -t rsa   -b 4096 -f /export/client/root/etc/ssh/ssh_host_rsa_key   -N ""
ssh-keygen -t ecdsa         -f /export/client/root/etc/ssh/ssh_host_ecdsa_key  -N ""
ssh-keygen -t ed25519       -f /export/client/root/etc/ssh/ssh_host_ed25519_key -N ""
chmod 600 /export/client/root/etc/ssh/ssh_host_*_key

# SSH 公開鍵の配置
mkdir -p /export/client/root/root/.ssh
chmod 700 /export/client/root/root/.ssh
cat ~/.ssh/id_*.pub > /export/client/root/root/.ssh/authorized_keys
chmod 600 /export/client/root/root/.ssh/authorized_keys

# マウントポイントの作成
mkdir -p /export/client/root/usr
mkdir -p /export/client/root/home
```

---

## 3. カーネルビルドとデプロイ

### 3.1 ビルド手順

```bash
# /home/ocha/NetBSD/src で実行
./build.sh -U -u -j2 -O ~/obj -m alpha -a alpha kernel=AXPVME
```

| オプション | 意味 |
|-----------|------|
| `-U` | root 権限不要 |
| `-u` | インクリメンタルビルド |
| `-j2` | 並列ジョブ数 2 |
| `-O ~/obj` | オブジェクトディレクトリ |
| `-m alpha -a alpha` | アーキテクチャ指定 |
| `kernel=AXPVME` | カーネルコンフィグ名 |

ビルド成果物: `~/obj/sys/arch/alpha/compile/AXPVME/netbsd`

### 3.2 デプロイ手順

```bash
# NFS root にカーネルをコピー
sudo cp ~/obj/sys/arch/alpha/compile/AXPVME/netbsd /export/client/root/.
```

---

## 4. テスト手順

### 4.1 ホスト側の準備

```bash
# 1. dnsmasq 起動
sudo systemctl start dnsmasq

# 2. ネットワーク監視（別ターミナルで実行）
sudo tcpdump -i enx6084bd485c85 -n host 192.168.99.10

# 3. シリアルコンソール接続（GTKTerm を起動して接続）
gtkterm &
```

### 4.2 AXPvme 230 側の起動

SRM コンソールで:

```
>>>boot ewa0
```

`ewa0` はオンボード 21040 Ethernet（AUI コネクタ）。

### 4.3 起動確認チェックリスト

#### ブートシーケンス（シリアルコンソールで確認）

```
Entering netbsd at 0x...
[ boot log ... ]
lca0: LCA-family PCI/memory support
axpvme_poll: tick 1
axpvme_poll: tick 2
[ ... ]
zsisa0 at isa0 port 0x6000-0x600f: Z8530 SCC Channel A, 9600 baud, polled
[ ... ]
entropy: best effort
[ ... ]
NetBSD/alpha (client) (constty)

login:
```

#### 確認項目

| 確認内容 | コマンド | 期待結果 |
|---------|---------|---------|
| カーネルバージョン | `uname -a` | `NetBSD client 11.99.6 ... alpha` |
| ネットワーク疎通 | `ping 192.168.99.1` | ICMP 応答あり |
| SSH ログイン | `ssh root@192.168.99.10` | ログイン成功 |
| NFS マウント確認 | `mount` または `df` | `/usr`・`/home` が NFS でマウント済み |
| プロセス確認 | `ps ax` | init, sshd, cron, inetd など稼働 |
| コンソールログイン | シリアル端末から | `login:` プロンプトでログイン可能 |
| シャットダウン | `shutdown -h now` | 正常終了 |

#### ネットワーク到達確認（ホスト側 tcpdump で確認）

```
# DHCP/BOOTP シーケンス
192.168.99.10 > 192.168.99.1: BOOTP/DHCP Request
192.168.99.1  > 192.168.99.10: BOOTP/DHCP Reply

# NFS マウント
192.168.99.10 > 192.168.99.1: NFS lookup "etc" ...
192.168.99.10 > 192.168.99.1: NFS lookup "rc.conf" ...
```

### 4.4 デバッグ手順

**ブートが途中で止まった場合**:

1. tcpdump で最後の NFS アクセスを確認 → どのファイルを読みに行って止まったか特定
2. シリアルコンソールで最後の出力を確認
3. `axpvme_poll: tick N` のカウントが止まっていれば → カーネルパニックかフリーズ

**SSH が繋がらない場合**:

```bash
# ホスト側から確認
ping 192.168.99.10          # ICMP 疎通確認
ssh -v root@192.168.99.10   # 詳細ログで原因確認
```

**NFS マウントが失敗する場合**:

```bash
# ホスト側で exports を確認
sudo exportfs -v
showmount -e 192.168.99.1
```

---

## 5. ハードウェア制約事項（AXPvme 230 固有）

| 制約 | 詳細 |
|------|------|
| **K1SEG 書き込みは使用不可** | K1SEG (`0xfffffe…`) 書き込みは Bcache probe/invalidation を発生させ永久ハング |
| **PAL_cflush 使用不可** | 同上 |
| **BCE=0（Bcache 無効化）不可** | MEMC_CAR に BCE=0 を書くと LCA が flush を開始し同様にハング |
| **ISA ハードウェア割り込み不使用** | SRM PAL が IACK 未実行 → scb_stray → printf → Bcache hang のため全 ISA IRQ マスク |
| **PROM printf の制限** | 旧: init 起動後は PROM callback 禁止（現在は zsisa がコンソールを引き継いだため不要） |
| **NIC は AUI のみ** | 21040 の 10BASE-T/BNC インターフェースは未接続。必ず AUI コネクタを使用 |

---

## 6. ファイル構成（参照先）

| ファイル | 内容 |
|--------|------|
| `CLAUDE.md` | ビルドコマンド・アーキテクチャ概要 |
| `bootup.md` | 起動時のコマンドチートシート |
| `porting_summary.md` | 全ポーティング課題と解決策の詳細 |
| `journal.md` | 開発ジャーナル（作業履歴） |
| `AXPvme230_NetBSD_porting_report.md` | ポーティング初期調査レポート |
| `sys/arch/alpha/conf/AXPVME` | カーネルコンフィグ |
| `sys/arch/alpha/alpha/dec_axpvme_64.c` | プラットフォーム初期化 |
| `sys/arch/alpha/isa/zs_isa.c` | Z8530 SCC コンソール/TTY ドライバ |
| `sys/arch/alpha/pci/pci_axpvme_64.c` | PCI 割り込み（ソフトウェアポーリング） |

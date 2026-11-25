# etherip6 — EtherIP (RFC 3378) over IPv6 カーネルドライバ

Ethernet フレームを IPv6 パケット（Next Header = 97）でカプセル化する Linux 用の
仮想 L2 ネットワークドライバです。RFC 3378 の EtherIP ヘッダ（version 3、
2 バイト）を使用し、終端アドレス（local / remote）は **IPv6 のみ** を扱います。

MTU 9000 のジャンボフレームを、外側の経路 MTU が 1280 しかないリンク越しでも
通過させるため、外側 IPv6 パケットを RFC 8200 の Fragment Header で
フラグメントします。受信側は Linux IPv6 スタックの再組み立てをそのまま利用します。

```
             inner Ethernet frame (最大 9014 バイト)
        +--------------------------------------------------+
        | IPv6 header (40) | EtherIP (2) | Ethernet frame   |
        +--------------------------------------------------+
          next header = 97  0x30 0x00
```

---

## 1. 特徴

| 項目 | 内容 |
| --- | --- |
| 準拠 | RFC 3378 EtherIP / IP protocol 97 / EtherIP version 3 |
| デバイス種別 | 仮想 Ethernet（`ARPHRD_ETHER`）。ブリッジポートとして利用可能 |
| 終端 | IPv6 の local / remote の 1 対 1（ポイントツーポイント） |
| デフォルト MTU | 9000（設定可能: 68〜65475） |
| フラグメンテーション | 外側 IPv6 を `ip6_fragment()` で自動分割。受信側はカーネルが再組み立て |
| 外側 MTU の上限 | `outer-mtu` 属性で指定可能（ICMPv6 PTB が使えない環境向け） |
| 設定インタフェース | rtnetlink（`IFLA_LINKINFO` / `IFLA_INFO_DATA`）+ `etherip6ctl` |
| 受信 | プロトコル 97 の `inet6_protocol` ハンドラ。`gro_cells` による GRO |
| 名前空間 | netns ごとの受信テーブル。デバイスは netns ローカル |
| 統計 | per-CPU の 64bit 統計（`ip -s link` / `etherip6ctl show`） |
| 性能設計 | RCU ロックレス受信、per-CPU dst キャッシュ、noqueue、LLTX、SG/HW csum パススルー |
| ライセンス | GPL-2.0 |

### できないこと（設計上の制約）

- 外側は IPv6 のみ（IPv4 終端は非対応）。
- EtherIP 自体には認証・暗号化・整合性保護はありません。必要な場合は外側 IPv6 に
  IPsec（XFRM）を適用してください（`ip6_dst_lookup_flow()` 経由で XFRM ポリシーが
  参照されます）。
- 1 デバイスにつき 1 ピアです。複数ピアはデバイスを分けてください。
- デバイスは `NETIF_F_NETNS_LOCAL` のため netns 間を移動できません。
  使用する netns の中で作成してください。
- 外側の ICMPv6 Packet Too Big はドライバでは処理しません（後述）。
- TSO/GSO はデバイスとして広告しません（スタック側でセグメント化されます）。

---

## 2. ビルドとインストール

### 必要なもの

- ビルド対象カーネルのヘッダ（`/lib/modules/$(uname -r)/build`）
- gcc / make
- ユーザ空間ツールは libc のみ（`etherip6ctl`）
- テスト（任意）: `qemu-system-x86_64`, `cpio`, `gzip`, `zstd`,
  `dpkg-deb`, `apt-get`, `ethtool`

```sh
# モジュール + ツール
make

# 警告を厳しめに確認（W=1）
make check

# インストール（カーネルモジュール + /usr/local/sbin/etherip6ctl）
sudo make install
sudo depmod -a
sudo modprobe etherip6
```

カーネルモジュールは `src/etherip6.ko`、ツールは `tools/etherip6ctl` にできます。
モジュールを直接ロードする場合は `sudo insmod src/etherip6.ko` でも構いません。

ロードすると `dmesg` に次が出ます。

```
etherip6: EtherIP over IPv6 driver loaded (RFC 3378, IP protocol 97)
```

---

## 3. 使い方

### 3.1 最短の手順

```sh
# 1) デバイス作成と端点設定（rtnetlink）
sudo etherip6ctl create eip0 \
    local  2001:db8::1 \
    remote 2001:db8::2 \
    mtu 9000

# 2) 有効化
sudo etherip6ctl set eip0 up

# 3) 確認
sudo etherip6ctl show eip0
```

反対側（別ホスト / 別 netns）では local と remote を入れ替えます。

```sh
sudo etherip6ctl create eip0 \
    local  2001:db8::2 \
    remote 2001:db8::1 \
    mtu 9000
sudo etherip6ctl set eip0 up
```

`local` と `remote` は IPv6 のユニキャストアドレスです。両方同時に指定する必要が
あります（片方だけの指定はエラー）。リンクローカルアドレスを使う場合は
`link eth0` のように出力デバイスも指定してください。

### 3.2 コマンド一覧

```sh
etherip6ctl create <name> [remote <ipv6>] [local <ipv6>]
                          [link <dev|ifindex>] [hoplimit <n>]
                          [tclass <n>] [flowlabel <n>]
                          [outer-mtu <n>] [mtu <n>] [up]

etherip6ctl set    <name> [remote <ipv6>] [local <ipv6>]
                          [link <dev|ifindex>] [hoplimit <n>]
                          [tclass <n>] [flowlabel <n>]
                          [outer-mtu <n>] [mtu <n>] [up|down]

etherip6ctl show   [<name>]
etherip6ctl del    <name>
```

例:

```sh
# 出力デバイスを固定し、外側の hop limit / traffic class / flow label を設定
etherip6ctl set eip0 link eth0 hoplimit 32 tclass 46 flowlabel 0x12345

# 外側経路の MTU が 1400 と分かっている場合（ICMPv6 PTB が届かない環境向け）
etherip6ctl set eip0 outer-mtu 1400

# 経路 MTU に戻す
etherip6ctl set eip0 outer-mtu 0

# 設定の表示
etherip6ctl show eip0
```

`etherip6ctl show` の出力例:

```
eip0 (ifindex 2): mtu 9000, state UP, mac 7a:b3:ac:1a:44:d7
  local    2001:db8::1
  remote   2001:db8::2
  link     0
  outer    hoplimit 64, tclass 0, flowlabel 0x00000, mtu 1400
  rx       570 packets, 5140140 bytes, 0 errors, 0 dropped
  tx       570 packets, 5140140 bytes, 0 errors, 0 dropped
```

### 3.3 iproute2 との併用

デバイスの作成自体は stock の iproute2 でも可能です（作成時点では未設定）。

```sh
sudo ip link add eip0 type etherip6 mtu 9000
sudo etherip6ctl set eip0 local 2001:db8::1 remote 2001:db8::2
sudo ip link set eip0 up
```

stock iproute2 は `etherip6` 固有の属性を解釈しないため、`remote` / `local` などの
設定は `etherip6ctl`（または生の rtnetlink）で行ってください。作成・削除・
up/down・MTU 変更・統計表示は `ip` でも可能です。

### 3.4 ブリッジ（L2 トンネル）として使う

```sh
sudo ip link add br0 type bridge
sudo ip link set br0 mtu 9000
sudo ip link set eip0 master br0
sudo ip link set br0 up
sudo ip link set eip0 up
```

EtherIP は Ethernet フレームをそのまま運ぶので、ブリッジのポートとして
802.1Q タグ付きフレームも透過します。両端でブリッジを組めば L2 延伸になります。

### 3.5 L3 インタフェースとして使う

```sh
sudo ip link set eip0 up
sudo ip -6 addr add 2001:db8:100::1/64 dev eip0
sudo ip addr add 10.0.0.1/30 dev eip0
```

EtherIP はブロードキャスト/マルチキャストフレームもカプセル化するため、
単一ピアのポイントツーポイント環境では ARP / IPv6 ND もそのまま通ります。
（デバイスに付けた MAC アドレスと、ピアが送ってくる MAC アドレスの関係に注意。
ブリッジ用途では promisc になるため通常は問題ありません。）

### 3.6 netns

デバイスは netns ローカルです。対象の netns 内で作成してください。

```sh
sudo ip netns add ns1
sudo ip netns exec ns1 etherip6ctl create eip0 \
    local 2001:db8::1 remote 2001:db8::2 mtu 9000
sudo ip netns exec ns1 etherip6ctl set eip0 up
```

---

## 4. MTU 9000 とフラグメンテーション

### 4.1 カプセル化オーバーヘッド

| 部分 | バイト数 |
| --- | --- |
| 外側 IPv6 ヘッダ | 40 |
| EtherIP ヘッダ | 2 |
| Fragment Header（分割時のみ） | 8 |

外側パケット長 = 40 + 2 + 内側フレーム長です。たとえば MTU 9000 の IP パケットは
Ethernet ヘッダ込みで 9014 バイト、外側は 9056 バイトになります。

### 4.2 外側 IPv6 の分割

送信時は `ip6_local_out()` に `skb->ignore_df = 1` を付けて渡します。外側パケットが
経路 MTU（`dst_mtu()`）を超えると、IPv6 スタックの `ip6_fragment()` が
RFC 8200 の Fragment Header を付けて分割します。EtherIP のペイロードは
「不透明な L2 フレーム」なので、外側を分割しても内側フレームの意味は変わりません。

受信側では、IPv6 の再組み立てがプロトコル 97 のハンドラより先に走るため、
ドライバは再組み立て済みの完全なフレームを受け取ります。

実測（同梱の VM テスト、外側経路 MTU 1280）:

```
[stats] Ip6FragCreates +80, Ip6ReasmOKs +10
```

9000 バイトと 9014 バイトのフレーム各 5 個（計 10 個）を送り、各 8 フラグメントに
分割され（80 個）、10 個すべてが再組み立てされて受信側でバイト単位一致しました。

### 4.3 外側 MTU の決め方

通常は経路 MTU が使われます（出力デバイスの MTU、または `ip -6 route` の `mtu`）。

```sh
# 経路 MTU を 1400 にする例
sudo ip -6 route add 2001:db8::2/128 dev eth0 mtu 1400
```

経路上の MTU が出力リンクより小さい場合、IPv6 ルータは分割せずに破棄します。
EtherIP（プロトコル 97）に対する ICMPv6 Packet Too Big は本ドライバでは処理しない
ため、次のいずれかで運用してください。

1. 経路に正しい MTU を設定する（推奨。カーネルの PMTU 例外もここに入ります）
2. `outer-mtu` で外側パケット長の上限を指定する

```sh
# 外側を 1400 バイト以下に分割する（実際は 1352 バイト単位）
sudo etherip6ctl set eip0 outer-mtu 1400
```

`outer-mtu` は外側 IPv6 パケット全体の上限です。実際のフラグメント ペイロードは
`(outer-mtu - 40 - 8)` を 8 の倍数に切り下げた値になります。`0` は「経路 MTU を
使う」の意味です。`outer-mtu` が経路 MTU より大きい場合は経路 MTU が優先されます。

> ICMPv6 PTB をドライバで処理しない理由: RFC 3378 には内側へのエラー通知方法が
> なく、外側 PTB を内側の L2 フレームに正しく対応付ける手段がありません。運用上は
> 経路 MTU か `outer-mtu` で明示的に決めるのが安全です。

### 4.4 フレーム長の制限

| 制限 | 値 | 備考 |
| --- | --- | --- |
| 最小内側フレーム | 14 バイト | Ethernet ヘッダ必須 |
| 最大内側フレーム | `mtu + 18` | VLAN タグ 1 個分の余裕 |
| 最大 MTU | 65475 | 外側が 65535 バイトを超えないため |
| デフォルト MTU | 9000 | |

AF_PACKET で生フレームを送る場合、カーネル側は VLAN タグのないフレームを
`mtu + 14` までに制限します（`packet_extra_vlan_len_allowed()`）。MTU 9000 の
IP パケット（フレーム長 9014）は通ります。

---

## 5. 性能設計

本ドライバは「仮想 L2 トンネルの終端」という前提で、次の設計にしています。

1. **受信は RCU + per-netns ハッシュ**
   - `(local, remote)` をキーにしたハッシュでデバイスを探します。受信経路に
     ロックはありません（書き込み側は RTNL）。
   - 設定は不変の `etherip6_binding` として RCU 公開し、TX/RX が中途半端な
     アドレスを見ないようにしています。差し替え時は `kfree_rcu()`。
2. **`gro_cells` による GRO**
   - 受信フレームを `netif_rx` で backlog に積むのではなく、`gro_cells` の
     NAPI に渡します。`NETIF_F_GRO` はカーネルが全デバイスに付与するため、
     受信フレームは通常の GRO 処理の対象になります。
3. **per-CPU の dst キャッシュ**
   - 経路検索は `dst_cache` に CPU ごとに保持します。ルート変更や PMTU 変更は
     `dst_check()` で無効化されます。設定変更時は `dst_cache_reset()`。
4. **noqueue + LLTX**
   - `tx_queue_len = 0` を指定するとカーネルが `IFF_NO_QUEUE` を付与し、
     qdisc は noqueue になります（`ip link` 表示の qlen は既定値 1000）。
     `NETIF_F_LLTX` により xmit ロックも回避します。
5. **ヘッドルームの事前確保**
   - `needed_headroom` を外側ヘッダ + リンクヘッダ分だけ確保し、ローカル生成
     フレームは再アロケーションなしでカプセル化します。転送フレームで不足する
     場合も `skb_cow_head()` で最小限のコピーに留めます。
6. **SG / HW checksum のパススルー**
   - `NETIF_F_SG | NETIF_F_HW_CSUM` を広告し、非線形 skb とチェックサム
     オフロードをそのまま外側に引き継ぎます。外側 IPv6 にはチェックサムが
     ありませんが、内側の L4 チェックサムはそのまま有効です。
   - 分割時は `ip6_fragment()` が `skb_checksum_help()` で内側チェックサムを
     確定してから分割します。
7. **GSO は広告しない**
   - スタック側でセグメント化してから xmit されます。内側フレームが MTU を
     超えた巨大な GSO セグメントがそのままカプセル化されるのを防ぎます。
8. **統計は per-CPU**
   - `dev->tstats`（`pcpu_sw_netstats`）に加算し、`dev_get_tstats64()` で
     64bit 統計を返します。
9. **フラグメンテーションはカーネル実装に委譲**
   - 自前のフラグメント処理を書かず、実績のある `ip6_fragment()` を使います。
     分割時はフラグメントごとに skb をアロケートしてコピーします（これは
     避けられないコスト）。分割が不要な経路では追加コピーはありません。

### 5.1 netns + veth + iperf3 による実測

同梱の VM テストは、2 つの netns を veth で接続し、それぞれに
`etherip6` デバイスを作成して、**iperf3 の TCP 通信**を内側で流します。

```
ns1                                      ns2
  eip0 (10.0.0.1/24)                       eip1 (10.0.0.2/24)
    |  EtherIP over IPv6 (proto 97)          |
  v1 (2001:db8:ff::1/64)  <-- veth -->  v2 (2001:db8:ff::2/64)
```

外側の経路 MTU と内側 MTU を変えて測定した結果（QEMU TCG、2 vCPU、
iperf3 3.16、5 秒、MSS 固定）:

| ケース | スループット | 公平な基準との比 |
| --- | --- | --- |
| veth 直接、GSO/TSO 有効（MSS 1400） | 1329 Mbits/s | 参考値（後述） |
| veth 直接、GSO/TSO/GRO 無効（MSS 1400） | 431 Mbits/s | 100%（公平な基準） |
| **トンネル、内側 1500、外側 9000（分割なし）** | **351 Mbits/s** | **81.4%** |
| トンネル、逆方向（`iperf3 -R`） | 363 Mbits/s | 84.2% |
| veth 直接、GSO 無効、ジャンボ（MSS 8960） | 925 Mbits/s | 100% |
| **トンネル、内側 9000、外側 9200（分割なし）** | **857 Mbits/s** | **92.6%** |
| トンネル、内側 9000、外側 1500（毎パケット分割） | 183 Mbits/s | 分割なしジャンボの 21.4% |

すべてのケースで TCP 再送は 0（最終実行）、TX/RX のエラー・ドロップも 0 でした。

**GSO/TSO 有効な veth との比較（約 26%）は公平ではありません。** veth は
GSO スーパーパケットをそのままピアに渡せますが、EtherIP は内側の
Ethernet フレームを実体化して外側に載せる必要があるため、内側 GSO を
フレーム単位にソフトウェア分割します。したがって、同じく GSO を無効に
した veth を基準にすると **非分割経路で 81〜93%** となり、残りの差は
カプセル化/デカプセル化と外側 IPv6 スタックの処理コストです。

外側で毎パケット分割するケース（内側 9000 / 外側 1500）は、フラグメント
ごとの skb アロケーションとコピーにより 21〜25% まで低下します。スループット
重視の経路では、内側 MTU を「外側経路 MTU - 42」程度に下げるか、外側を
ジャンボ対応にすることを推奨します（MTU 9000 自体は分割により通ります）。

#### フラグメント処理の検証（TCP）

外側 MTU・方向・並列度を変えて、フラグメントの生成数と再組み立て数を
`/proc/net/snmp6` の `Ip6FragCreates` / `Ip6ReasmOKs` /
`Ip6FragFails` / `Ip6ReasmFails` で検証しています。

| ケース（TCP） | 外側 MTU | FragCreates | ReasmOKs | 失敗 | スループット |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1500、順方向 | 1500 | 102381 | 14910 | 0 | 207 Mbits/s |
| 1500、逆方向（`-R`） | 1500 | 94926 | 13826 | 0 | 193 Mbits/s |
| 1400、双方向（`--bidir -P 2`） | 1400 | 128689 | 18720 | 0 | 各方向 約 134 Mbits/s |
| 1280、4 並列（`-P 4`） | 1280 | 143718 | 18385 | 0 | 合計 256 Mbits/s |
| 経路 9200 + `outer-mtu 1400` | 1400（cap） | 97747 | 14235 | 0 | 198 Mbits/s |

- `FragCreates / ReasmOKs` は約 **6.87**（外側 1500/1400）または
  **7.82**（外側 1280）で、9056 バイトの外側パケットがそれぞれ
  **7 / 8 フラグメント**になる計算と一致します。
- すべてのケースで `Ip6FragFails = 0`、`Ip6ReasmFails = 0`。
  TCP の再送は 0（1280 の 4 並列で合計 3、双方向で 1 のみ）で、
  データ欠損や再組み立て失敗はありませんでした。
- `outer-mtu 1400` は、経路 MTU が 9200 でも外側パケットを 1400 以下に
  分割できることを確認しています。

### 5.2 テストで見つかった問題と修正

netns + iperf3 テストにより、2 つの実際の不具合を発見して修正しました。

1. **受信時の CHECKSUM_PARTIAL を潰していた**
   - 当初は受信フレームを常に `CHECKSUM_NONE` にしていました。外側が
     veth のような仮想デバイスの場合、内側 TCP のチェックサムは
     `CHECKSUM_PARTIAL` のまま届くため、`CHECKSUM_NONE` にすると
     未計算のチェックサムを検証してしまい、TCP が全く通りませんでした。
   - 受信時に `CHECKSUM_PARTIAL` を保持し、内側スタックに完了させるよう
     修正しました。実回線では外側が `CHECKSUM_NONE` で届くため、従来通り
     内側スタックが検証します。
2. **per-CPU 統計の確保漏れ**
   - `dev->pcpu_stat_type` を設定していなかったため、カーネルが
     `dev->tstats` を確保せず、`dev_get_tstats64()` と
     `dev_sw_netstats_*_add()` が NULL の per-CPU ポインタを参照して
     いました。新しいデバイスの統計が既存デバイスの値に見えたり、
     無関係な per-CPU データを破壊する可能性がありました。
   - `dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS` を設定して修正し、
     新規デバイスの統計が 0 から始まることをテストで確認しています。

> 参考: 同梱 VM テストは QEMU の TCG（ソフトウェアエミュレーション）上で動くため、
> 上記の絶対値は実機の性能値ではありません。ただし同一 VM 内で GSO 無効の
> veth とトンネルを比較しているため、相対的な減衰の評価には使えます。

---

## 6. netlink インタフェース

デバイスは rtnetlink のリンク種別 `etherip6` として登録されます。設定は
`IFLA_LINKINFO` の中の `IFLA_INFO_DATA` に、次の属性で格納されます。

| 属性 | 型 | 意味 |
| --- | --- | --- |
| `IFLA_ETHERIP6_REMOTE` | `struct in6_addr` (16) | ピア（終端）アドレス |
| `IFLA_ETHERIP6_LOCAL` | `struct in6_addr` (16) | ローカル終端アドレス |
| `IFLA_ETHERIP6_LINK` | `__u32` | 出力デバイスの ifindex（0 = 経路任せ） |
| `IFLA_ETHERIP6_HOP_LIMIT` | `__u8` | 外側 hop limit（0 は既定 64） |
| `IFLA_ETHERIP6_TCLASS` | `__u8` | 外側 traffic class |
| `IFLA_ETHERIP6_FLOWLABEL` | `__u32` | 外側 flow label（20bit） |
| `IFLA_ETHERIP6_OUTER_MTU` | `__u32` | 外側パケット長の上限（0 = 経路 MTU） |

`RTM_NEWLINK` で作成・変更、`RTM_GETLINK` で取得、`RTM_DELLINK` で削除します。
`local` / `remote` は「両方指定」または「両方未指定（未設定デバイス）」のみ許可
されます。片方だけの指定は `-EINVAL` です。

> 注意: 上記の属性番号は本 out-of-tree ドライバの私有値です。upstream の
> UAPI には存在しません。iproute2 の `etherip6` プラグインを作る場合は同じ
> 番号を使ってください。

---

## 7. テスト

### 7.1 ビルド警告チェック

```sh
make check          # W=1 でカーネルモジュールをビルド
```

### 7.2 QEMU を使った機能テスト

`make vm-test` は、モジュールをビルドしたカーネルと **同じバージョン** の
Ubuntu カーネルイメージを取得し、最小の initramfs を作って QEMU（TCG）で
起動します。KVM は不要です。netns + iperf3 テスト用に `iperf3` /
`libiperf0` / `libsctp1` の .deb も取得し、`veth.ko` と `ethtool` を
initramfs に含めます（初回はネットワークアクセスが必要です）。

```sh
make vm-test
# または
KVER=6.8.0-147-generic VM_TIMEOUT=1200 ./tests/vm/run-vm-test.sh
```

テスト内容:

- モジュールのロード、`etherip6` デバイスの作成・設定・up/down
- `etherip6ctl` と rtnetlink の表示
- 64 / 1518 / 9000 / 9014 バイトのフレームを双方向に送受信し、バイト単位で照合
- **netns + veth + iperf3** による TCP スループット測定
  （GSO/TSO 有効・無効の veth 基準、トンネル順方向/逆方向、ジャンボ、
  外側フラグメントあり）
- 新規作成デバイスの統計が 0 から始まること
- iperf3 のスモークテスト（initramfs の libc/ライブラリ確認を含む）
- 外側経路 MTU 1280 で 9000 バイトフレームを送り、`Ip6FragCreates` /
  `Ip6ReasmOKs` が増えること、受信フレームが壊れないことを確認
- **TCP のフラグメント検証**: 外側 MTU 1500 / 1400 / 1280、順方向 /
  逆方向 / 双方向 / 4 並列、`outer-mtu 1400`（経路 9200）の各ケースで、
  `Ip6FragCreates` / `Ip6ReasmOKs` が増え、`Ip6FragFails` /
  `Ip6ReasmFails` が 0 であることを確認
- 外側経路 MTU 65536 でフラグメントが発生しないこと
- `outer-mtu 1400` 指定時に、経路 MTU が大きくても 1400 以下に分割されること
- `outer-mtu 0` に戻すと分割されないこと
- up 中の tclass / flowlabel / hoplimit / MTU 変更
- `ip link add ... type etherip6` による iproute2 からの作成
- 不正な設定（片方だけのアドレス、範囲外の `outer-mtu` / flowlabel、
  自分自身を `link` に指定）が拒否されること
- テスト後の `dmesg` に BUG / WARNING / Oops がないこと

実行結果の例（QEMU TCG）:

```
[stats] Ip6FragCreates +80, Ip6ReasmOKs +10
[ok] outer fragments were created and reassembled
...
[stats] Ip6FragCreates +0, Ip6ReasmOKs +0
[ok] no fragmentation on a jumbo path
...
OUTER-MTU OK
IPROUTE2_CREATE_OK
...
IPERF baseline_veth_gso rate=1329 Mbits/sec retr=0
IPERF baseline_veth_no_gso rate=431 Mbits/sec retr=0
IPERF tunnel_nofrag rate=351 Mbits/sec retr=0
IPERF tunnel_nofrag_reverse rate=363 Mbits/sec retr=0
IPERF baseline_veth_jumbo rate=925 Mbits/sec retr=0
IPERF tunnel_jumbo rate=857 Mbits/sec retr=0
IPERF tunnel_frag rate=183 Mbits/sec retr=0
RATIO tunnel_nofrag/baseline_gso=26.4% ... tunnel_nofrag/baseline_no_gso=81.4% ...
FRAG frag1500_forward creates=102381 fragfails=0 reasm=14910 reasmfails=0 rc=0
FRAG frag1500_reverse creates=94926 fragfails=0 reasm=13826 reasmfails=0 rc=0
FRAG frag1400_bidir creates=128689 fragfails=0 reasm=18720 reasmfails=0 rc=0
FRAG frag1280_p4 creates=143718 fragfails=0 reasm=18385 reasmfails=0 rc=0
FRAG outermtu1400_forward creates=97747 fragfails=0 reasm=14235 reasmfails=0 rc=0
FRAG_TCP_OK
NETNS_IPERF_OK
INIT OK
```

ハーネスは `SELFTEST PASS` / `FRAG_TCP_OK` / `NETNS_IPERF_OK` / `INIT OK`
を確認します。コンソールログは `.vmtest/console.log` に保存されます。過去の実行ログは
[docs/test-console-6.8.0-147.log](docs/test-console-6.8.0-147.log) にあります。

### 7.3 単体で使う

```sh
sudo insmod src/etherip6.ko
sudo ip link add lo0 type dummy   # 例: 外側の経路用（任意）
sudo etherip6ctl create eip0 local 2001:db8::1 remote 2001:db8::2 mtu 9000
sudo etherip6ctl set eip0 up
```

`tests/etherip6_selftest` は 2 つの `etherip6` デバイスを同一 netns 内で
ループバック経由で接続して送受信を検証します（VM テストで使用）。

---

## 8. 本番運用の注意

### 8.1 モジュール署名

out-of-tree モジュールのため、Secure Boot や `CONFIG_MODULE_SIG_FORCE=y` の
環境では、enrolled key でモジュールを署名する必要があります。

```sh
sudo /usr/src/linux-headers-$(uname -r)/scripts/sign-file \
    sha256 /path/to/signing_key.pem /path/to/signing_key.x509 src/etherip6.ko
```

同梱の VM テストは QEMU にカーネルを直接起動するため Secure Boot は無効で、
未署名モジュールがロードされます（カーネルは taint されます）。

### 8.2 ファイアウォール

外側は IPv6 の IP プロトコル番号 **97** です。ip6tables/nftables では
プロトコル 97 を明示的に許可してください。

```sh
sudo ip6tables -A INPUT  -p 97 -j ACCEPT
sudo ip6tables -A OUTPUT -p 97 -j ACCEPT
```

外側パケットは送信時に `NF_INET_LOCAL_OUT` / `NF_INET_POST_ROUTING` を、
受信時に `NF_INET_PREROUTING` / `NF_INET_LOCAL_IN` を通ります。既存の
ファイアウォールポリシーによっては、カプセル化パケットが落とされることが
あります。

### 8.3 機密性・完全性

EtherIP 自体には暗号化も認証もありません。インターネット越しに使う場合は、
外側 IPv6 に IPsec（XFRM）を適用してください。本ドライバの経路検索は
`ip6_dst_lookup_flow()` を使うため、外向きの XFRM ポリシーが適用されます。

### 8.4 MTU 設計

- 内側 MTU は「実際に通したいフレーム長 - 14」を目安に設定します。
- 経路 MTU が小さい場合は自動で外側分割されますが、分割にはコストがかかります。
  実測では内側 MTU 9000 / 外側 1500 の分割経路は、分割なしジャンボの
  約 21〜25% のスループットでした。可能なら経路 MTU に合わせて内側 MTU を
  下げてください（例: 外側 1500 なら内側 1458 程度）。
- 経路上の最小 MTU が分かっている場合は、経路 MTU か `outer-mtu` を明示して
  ください（ICMPv6 PTB は処理しません）。

### 8.5 監視

```sh
ip -s link show eip0
etherip6ctl show eip0
cat /proc/net/snmp6 | grep -E 'Ip6Frag|Ip6Reasm'
dmesg | grep etherip6
```

`tx_errors` / `tx_dropped` が増える場合は、経路、MTU、`outer-mtu`、ファイア
ウォールを確認してください。`rx_dropped` はデバイスが down のときや、
短いフレームを受けたときに増えます。

---

## 9. トラブルシューティング

| 症状 | 確認点 |
| --- | --- |
| `create` が `-EINVAL` | `local` と `remote` の両方を指定したか。マルチキャスト/リンクローカル + `link` の要否 |
| 送信しても届かない | `etherip6ctl show` の tx/rx、`ip -6 route get <remote> from <local>`、`dmesg` の "no route" |
| フラグメントされない | 外側 MTU が大きい（`ip -6 route get` / `outer-mtu`）。経路 MTU が 9000 以上なら分割不要 |
| 大きなフレームが落ちる | 内側 MTU、`mtu + 18` 制限、経路 MTU、`outer-mtu` |
| ループする | remote が自分自身の local になっていないか。`dmesg` の "routing loop" |
| 統計が見えない | `ip -s link show eip0` / `etherip6ctl show`（per-CPU 統計） |

---

## 10. ファイル構成

```
Makefile                     トップレベル（module/tool/tests/vm-test）
src/
  Makefile                   kbuild
  etherip6_uapi.h            共有 ABI（netlink 属性、定数）
  etherip6.h                 内部定義（priv / binding / per-net テーブル）
  etherip6_main.c            netdev、pernet、RX、モジュール初期化
  etherip6_xmit.c            TX（カプセル化、外側 MTU、フラグメント誘導）
  etherip6_netlink.c         rtnetlink 設定（newlink/changelink/fill_info）
tools/
  etherip6ctl.c              rtnetlink ベースの設定ツール
tests/
  etherip6_selftest.c        AF_PACKET による機能テスト
  vm/init                    VM の init スクリプト
  vm/run-vm-test.sh          QEMU テストハーネス
docs/
  design.md                  内部設計・並行性・データパス
```

---

## 11. 設計資料

より詳しいデータパス、RCU の使い方、設定変更のロールバック、netlink の
ライフサイクルは [docs/design.md](docs/design.md) を参照してください。

## 12. ライセンス

GPL-2.0。カーネルモジュールは `MODULE_LICENSE("GPL")` です。

# etherip6 テスト結果

## 環境

| 項目 | 値 |
| --- | --- |
| ホスト | Ubuntu 24.04.5 LTS, kernel `6.8.0-147-generic` |
| モジュール | `src/etherip6.ko`（同じ 6.8.0-147 のヘッダでビルド） |
| VM | QEMU 8.2.2, TCG（KVM なし）, 2 vCPU, 1 GiB RAM |
| ゲストカーネル | `linux-image-6.8.0-147-generic`（Ubuntu パッケージから取得） |
| iproute2 | 6.1.0 |
| ethtool | 6.7 |
| iperf3 | 3.16 |
| モジュール署名 | なし（Secure Boot 無効。カーネルは taint） |

## 実行方法

```sh
make check                     # カーネルモジュール（W=1）
make -C tests static           # テスト用の静的バイナリ
./tests/vm/run-vm-test.sh      # QEMU で起動して全テストを実行
```

ハーネスは次の順で実行します。

1. ホストのカーネルと同じリリースの Ubuntu カーネルイメージを取得
2. `busybox` + 実 `iproute2` + `ethtool` + `iperf3` + `veth.ko` +
   `etherip6.ko` + 静的 `etherip6ctl` / `etherip6_selftest` を含む
   initramfs を作成
3. QEMU（TCG）で起動し、`/init` でテストを実行
4. コンソールログの `SELFTEST PASS` / `FRAG_TCP_OK` / `NETNS_IPERF_OK` /
   `INIT OK` を確認

`make check` はカーネルモジュールを警告（W=1）なしでビルドできました
（コンパイラのバージョン差異に関する kbuild の注意のみ）。

---

## 1. カプセル化・フラグメンテーションのテスト

同一 netns 内に 2 つの `etherip6` デバイスを作り、IPv6 ループバック経由で
生の Ethernet フレームを送受信します。

### 構成

```
[   4.586563] etherip6: EtherIP over IPv6 driver loaded (RFC 3378, IP protocol 97)
MODULE LOADED
...
NL CONFIG OK
...
2: eip0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 9000 qdisc noqueue state UP ...
    etherip6 addrgenmode eui64 ...
```

`ip -d link show eip0` で `mtu 9000`、`minmtu 68`、`maxmtu 65475`、
`qdisc noqueue`、リンク種別 `etherip6` を確認。

### フラグメントあり（外側経路 MTU 1280、双方向）

```
== etherip6 selftest: eip0 -> eip1 ==
[ok] 64 byte frames
[ok] 1518 byte frames
[ok] 9000 byte frames
[ok] 9014 byte frames (mtu 9000 + Ethernet header)
[stats] Ip6FragCreates +80, Ip6ReasmOKs +10
[ok] outer fragments were created and reassembled
[burst] eip0: 500 x 1518 bytes ...
[ok] burst of 1518 byte frames
[burst] eip0: 50 x 9014 bytes ...
[ok] burst of 9014 byte frames
[stats] eip0 tx_packets +570, eip1 rx_packets +570
SELFTEST PASS
```

- 9000 バイトと 9014 バイトのフレーム各 5 個（計 10 個）が外側で
  80 フラグメントに分割され、10 個すべてが再組み立てされて受信側で
  バイト単位一致。
- 570 フレームすべてが TX/RX 統計に計上。
- 逆方向も同じ結果（`Ip6FragCreates +80, Ip6ReasmOKs +10`）。

### フラグメントなし（外側経路 MTU 65536）

```
[stats] Ip6FragCreates +0, Ip6ReasmOKs +0
[ok] no fragmentation on a jumbo path
SELFTEST PASS
JUMBO OK
```

### `outer-mtu` による上限（経路 MTU 65536、outer-mtu 1400）

```
[stats] Ip6FragCreates +70, Ip6ReasmOKs +10
[ok] outer fragments were created and reassembled
SELFTEST PASS
OUTER-MTU OK
```

`outer-mtu 0` に戻すと `+0 / +0` に戻ることも確認。

### ライブ再設定・設定検証

up 中の `tclass` / `flowlabel` / `hoplimit` / MTU 変更後に再度 570 フレームを
送受信して成功。以下が拒否されることも確認。

```
VALIDATION OK
```

- `local` のみ / `remote` のみ
- `outer-mtu 100`（1280 未満）
- `flowlabel 0x100000`（20bit 超過）
- `link eip0`（自分自身）

### iproute2 との相互運用

```
IPROUTE2_CREATE_OK
```

`ip link add eip2 type etherip6` で作成し、`ip link del` で削除できました。

---

## 2. netns + veth + iperf3 スループットテスト

### トポロジ

```
ns1                                      ns2
  eip0 (10.0.0.1/24)                       eip1 (10.0.0.2/24)
    |  EtherIP over IPv6 (proto 97)          |
  v1 (2001:db8:ff::1/64)  <-- veth -->  v2 (2001:db8:ff::2/64)
```

- 外側の終端アドレスは各 netns の `lo` に `/128` で付け、veth 経由の
  明示ルートで対向に到達させます。
- 内側は IPv4 の同一サブネット。ARP も EtherIP で透過します。
- トンネルデバイスは IPv6 を無効化し、内側 IPv4 のみのクリーンな測定に
  しています。
- iperf3 は 5 秒、MSS を `-M` で固定（1400 / 8960）。
- 基準は同じ veth ペア上の直接 iperf3。GSO/TSO/GRO を有効にした場合と、
  `ethtool -K ... off` で無効にした場合を測定。

### 結果（QEMU TCG、最終実行）

| ケース | スループット | 公平な基準との比 | TCP 再送 |
| --- | --- | --- | --- |
| veth 直接、GSO/TSO 有効、MSS 1400 | 1329 Mbits/s | 参考（不公平） | 0 |
| veth 直接、GSO/TSO/GRO 無効、MSS 1400 | 431 Mbits/s | 100% | 0 |
| **トンネル、内側 1500、外側 9000（分割なし）** | **351 Mbits/s** | **81.4%** | 0 |
| トンネル逆方向（`iperf3 -R`） | 363 Mbits/s | 84.2% | 0 |
| veth 直接、GSO 無効、MSS 8960 | 925 Mbits/s | 100% | 0 |
| **トンネル、内側 9000、外側 9200（分割なし）** | **857 Mbits/s** | **92.6%** | 0 |
| トンネル、内側 9000、外側 1500（分割あり） | 183 Mbits/s | 分割なしジャンボの 21.4% | 0 |

ログ上のマーカー:

```
IPERF baseline_veth_gso rate=1329 Mbits/sec retr=0
IPERF baseline_veth_no_gso rate=431 Mbits/sec retr=0
IPERF tunnel_nofrag rate=351 Mbits/sec retr=0
IPERF tunnel_nofrag_reverse rate=363 Mbits/sec retr=0
IPERF baseline_veth_jumbo rate=925 Mbits/sec retr=0
IPERF tunnel_jumbo rate=857 Mbits/sec retr=0
IPERF tunnel_frag rate=183 Mbits/sec retr=0
RATIO tunnel_nofrag/baseline_gso=26.4% ...
      tunnel_nofrag/baseline_no_gso=81.4% ...
NETNS_IPERF_OK
```

### フラグメント処理の検証（TCP）

外側 MTU・方向・並列度を変え、`/proc/net/snmp6` の
`Ip6FragCreates` / `Ip6ReasmOKs` / `Ip6FragFails` / `Ip6ReasmFails` で
フラグメント処理を検証しました。

| ケース（TCP） | 外側 MTU | FragCreates | ReasmOKs | FragFails | ReasmFails | スループット | 再送 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 順方向 | 1500 | 102381 | 14910 | 0 | 0 | 207 Mbits/s | 0 |
| 逆方向（`-R`） | 1500 | 94926 | 13826 | 0 | 0 | 193 Mbits/s | 0 |
| 双方向（`--bidir -P 2`） | 1400 | 128689 | 18720 | 0 | 0 | 各方向 約 134 Mbits/s | 1 |
| 4 並列（`-P 4`） | 1280 | 143718 | 18385 | 0 | 0 | 合計 256 Mbits/s | 3 |
| 経路 9200 + `outer-mtu 1400` | 1400（cap） | 97747 | 14235 | 0 | 0 | 198 Mbits/s | 0 |

- `FragCreates / ReasmOKs` は約 **6.87**（1500/1400）または **7.82**
  （1280）で、9056 バイトの外側パケットがそれぞれ **7 / 8 フラグメント**に
  なる計算と一致します。
- 全ケースで `Ip6FragFails = 0`、`Ip6ReasmFails = 0`。再送は最大 3 個
  （4 並列 1280 の合計）で、再組み立て失敗やデータ欠損はありません。
- `outer-mtu 1400` は、経路 MTU が 9200 でも外側パケットを 1400 以下に
  分割できることを確認しています。

### 考察

- **GSO/TSO 有効な veth との比較（27%）は公平ではありません。** veth は
  64 KiB の GSO スーパーパケットをそのままピアに渡せますが、EtherIP は
  内側の Ethernet フレームを実体化する必要があるため、内側 GSO を
  フレーム単位にソフトウェア分割します。GSO を無効にした同じ veth を
  基準にすると **非分割経路で 81〜93%** です。
- 非分割経路の残り約 7〜19% は、カプセル化/デカプセル化と外側 IPv6 スタックの
  処理コストです。TCP 再送・ドロップは 0 で、極端な減衰はありません。
- 外側フラグメントあり（内側 9000 / 外側 1500）は、フラグメントごとの
  skb アロケーションとコピーにより 21〜25% まで低下します。これは
  フラグメンテーションの本質的なコストです。スループット重視の経路では
  内側 MTU を「外側経路 MTU - 42」程度に下げるか、外側をジャンボ対応に
  してください。MTU 9000 自体は分割により通ります。

---

## 3. テストで見つかった不具合と修正

### 3.1 受信時に CHECKSUM_PARTIAL を潰していた（重大）

初回の netns + iperf3 テストでは、ping（ICMP）は通るのに **TCP が
まったく通らず、iperf3 がハング**しました。

原因は、受信時に `skb->ip_summed = CHECKSUM_NONE` を強制していたことです。
外側が veth のような仮想デバイスの場合、内側 TCP のチェックサムは
`CHECKSUM_PARTIAL` のままトンネル終端に届きます。`CHECKSUM_NONE` にすると
内側スタックが未計算のチェックサムを検証し、全 TCP セグメントを破棄します。

修正: `CHECKSUM_PARTIAL` は保持し、内側スタックに完了させる。
`CHECKSUM_UNNECESSARY` にはしないため、検証は省略されません。実回線では
外側が `CHECKSUM_NONE` で届くので、従来どおり内側スタックが検証します。

修正後、TCP が通り、上記のスループットが得られました。

### 3.2 per-CPU 統計の確保漏れ（重大）

netns テストで、**新規作成したデバイスの統計が既存デバイスの値（3420
パケット）から始まる**現象を発見しました。

原因は `dev->pcpu_stat_type` を設定していなかったことです。カーネルは
この値を見て `dev->tstats` を確保するため、未設定だと `dev->tstats` が
NULL のままになります。`dev_get_tstats64()` と `dev_sw_netstats_*_add()` は
NULL の per-CPU ポインタを参照し、per-CPU 領域の先頭（別デバイスや無関係な
per-CPU データ）を読み書きしていました。統計の混入だけでなく、per-CPU
データを破壊する可能性がある重大なバグです。

修正: `etherip6_setup()` で
`dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS` を設定。

検証: 新規デバイスを作成直後に `etherip6ctl show` し、rx/tx が 0 で
あることをテストに追加しました（`fresh device statistics are not zero` で
失敗するようにしています）。最終実行では 0 を確認。

---

## 4. まとめ

- 機能面: RFC 3378 のカプセル化、IPv6 終端、netlink 設定、MTU 9000 の
  外側フラグメント/再組み立て、双方向通信、ブリッジ向け L2 透過を確認。
- フラグメント: 外側 MTU 1500/1400/1280、順方向/逆方向/双方向/4 並列、
  `outer-mtu` 指定の全ケースで、生成数・再組み立て数が計算どおりで、
  `FragFails` / `ReasmFails` は 0。TCP 再送も最大 3 個で実質ゼロ。
- 性能面: GSO 無効の同一 veth 基準に対して、非分割経路で **81〜93%**。
  TCP 再送・ドロップは 0。極端な減衰はなく、残りはカプセル化の本質的
  コストです。外側フラグメント時は 21〜25% まで下がりますが、これは分割の
  コストとして説明できます。
- テストで見つかった 2 つの重大な不具合（CHECKSUM_PARTIAL の潰し、
  per-CPU 統計の NULL 参照）を修正しました。

完全なコンソールログ: [test-console-6.8.0-147.log](test-console-6.8.0-147.log)

## 注意

- QEMU TCG 上の絶対値はソフトウェアエミュレーションの結果であり、実機の
  性能値ではありません。実行ごとに ±10% 程度変動します。実機評価は
  2 ホスト間で `iperf3` を用い、経路 MTU とフラグメントの有無を変えて
  行ってください。
- 比較の基準は「GSO/TSO 無効の veth」です。GSO 有効の veth は L2 トンネルには
  不公平な基準です。

# etherip6 内部設計

本書は `etherip6` ドライバのデータパス、並行性、設定ライフサイクル、
フラグメンテーションの扱いをまとめたものです。利用方法は
[../README.md](../README.md) を参照してください。

---

## 1. 全体像

```
                 +-------------------------------+
  inner netdev   |  etherip6 (ARPHRD_ETHER)      |
  (eip0)         |  priv: cfg / binding / dst    |
                 +---------------+---------------+
                                 |
                      TX  etherip6_xmit()
                                 |
                 +---------------v---------------+
                 | IPv6 stack (ip6_local_out)    |
                 |  LOCAL_OUT -> POST_ROUTING    |
                 |  -> ip6_fragment if needed    |
                 +---------------+---------------+
                                 |
                      physical / route device
                                 |
                 +---------------v---------------+
                 | IPv6 stack (ip6_input)        |
                 |  reassembly -> proto 97       |
                 +---------------+---------------+
                                 |
                      RX  etherip6_rcv()
                                 |
                       per-netns hash lookup
                                 |
                 +---------------v---------------+
                 |  inner netdev (eip1) -> GRO   |
                 +-------------------------------+
```

- 1 つのカーネルモジュール（`etherip6.ko`）が 1 つの rtnetlink リンク種別
  `etherip6` を登録します。デバイス数に制限はありません。
- 外側は IPv6 のみです。内側は任意の Ethernet フレームです。
- 1 デバイス = 1 ピア（local / remote の組）です。

### ソースの役割

| ファイル | 役割 |
| --- | --- |
| `src/etherip6_main.c` | netdev_ops、pernet、受信ハンドラ、モジュール初期化 |
| `src/etherip6_xmit.c` | 送信（カプセル化、外側 MTU、フラグメント誘導） |
| `src/etherip6_netlink.c` | rtnetlink の newlink/changelink/fill_info |
| `src/etherip6.h` | 内部データ構造と関数宣言 |
| `src/etherip6_uapi.h` | カーネル / ユーザ空間共有の定数・属性番号 |
| `tools/etherip6ctl.c` | 生 rtnetlink を使う設定ツール |
| `tests/etherip6_selftest.c` | AF_PACKET による送受信検証 |
| `tests/vm/*` | QEMU を使った再現可能なテスト |

---

## 2. データ構造

```c
struct etherip6_priv {
    struct etherip6_cfg        cfg;      /* RTNL でのみ読み書き */
    struct etherip6_binding __rcu *binding; /* RCU 公開。TX/RX が参照 */
    struct dst_cache           dst_cache;   /* per-CPU 経路キャッシュ */
    struct gro_cells           gro_cells;   /* per-CPU NAPI / GRO */
};
```

### 2.1 `cfg` と `binding` を分ける理由

- `cfg` は netlink で変更される「現在の設定」です。書き込みは RTNL 保持下で
  行い、`fill_info()` も RTNL 保持下で読むため、netlink 側はロックなしで
  一貫した値を見られます。
- データパス（TX/RX）は RTNL を取りません。16 バイトの IPv6 アドレスを
  ロックなしで読むと、更新中に壊れた値を掴む可能性があります。
- そこで、設定変更のたびに **不変の `etherip6_binding` を新規確保**し、
  `rcu_assign_pointer()` で差し替えます。TX は `rcu_read_lock()` の下で
  必要な値をローカルにコピーし、RX は per-netns ハッシュを RCU で走査します。
- 旧 binding は `kfree_rcu()` で 1 グレースピリオド後に解放します。RCU
  リーダは常に完全な設定を見ます。

### 2.2 per-netns ハッシュ

```c
struct etherip6_net {
    struct hlist_head hash[256];
};
```

- `(local, remote)` を `jhash2()` でハッシュし、受信パケットの
  `(daddr, saddr)` からデバイスを探します。
- netns ごとに独立したテーブルです。受信ハンドラは
  `dev_net(skb->dev)` のテーブルだけを見るため、netns 間の漏れはありません。
- `link_ifindex` を設定しているデバイスは、その出力デバイスで受信した
  パケットだけにマッチします。同じ `(local, remote)` を共有するデバイスが
  ある場合は、完全一致（link 指定あり）を優先し、次にワイルドカード
  （link = 0）を使います。

---

## 3. ライフサイクル

### 3.1 モジュール初期化

```c
inet6_add_protocol(&etherip6_protocol, 97);   /* RX ハンドラ */
register_pernet_subsys(&etherip6_net_ops);    /* per-netns ハッシュ */
rtnl_link_register(&etherip6_link_ops);       /* "etherip6" 種別 */
```

終了時は逆順です。まずプロトコル 97 の受信を止め、次に
`rtnl_link_unregister()` で全 netns の `etherip6` デバイスを削除し、
最後に pernet を解除します。

### 3.2 作成（RTM_NEWLINK / newlink）

1. `rtnl_create_link()` が `setup()` を呼び、`ether_setup()` の後に
   MTU や `needed_headroom`、feature、`NETIF_F_NETNS_LOCAL` などを設定。
2. `etherip6_newlink()` が `IFLA_INFO_DATA` を検証し、`priv->cfg` に格納。
3. `register_netdevice()` を呼ぶ（newlink が登録責任を持つ）。
4. `ndo_init()` が `dst_cache_init()`、`gro_cells_init()`、
   `etherip6_bind()` を行う。`etherip6_bind()` は `cfg` から binding を
   確保して per-netns ハッシュに挿入し、`priv->binding` に RCU 公開。
5. 登録後に carrier を設定。

`ndo_init()` が失敗した場合は、その場で自分が確保した資源を解放します
（`register_netdevice()` は ndo_init 失敗時に ndo_uninit を呼ばないため）。

### 3.3 変更（RTM_NEWLINK / changelink）

- 現在の `cfg` をコピーし、メッセージに含まれる属性だけ上書きします。
  したがって「remote だけ変更」「local と remote を同時に変更」
  「local/remote を両方 `::` にして未設定に戻す」が可能です。
- 新しい binding を先に確保してから差し替えるため、メモリ不足時は
  旧設定・旧 binding がそのまま残ります（ロールバック不要）。
- 成功時は `dst_cache_reset()` と carrier の再評価を行います。

### 3.4 削除（RTM_DELLINK）

`unregister_netdevice_queue()` を呼びます。`ndo_uninit()` で binding を
ハッシュから外し、`gro_cells_destroy()`、`dst_cache_destroy()` を行います。

### 3.5 netns

デバイスは `NETIF_F_NETNS_LOCAL` です。netns 間の移動はできません。
pernet の受信テーブルは netns ごとに存在し、netns 破棄時には
デバイスの unregister が先に走るため、ハッシュは空になります。

---

## 4. 送信パス（`etherip6_xmit`）

```
ndo_start_xmit(skb, dev)
  |
  +-- フレーム長チェック: ETH_HLEN <= len <= min(mtu + 18, 65493)
  +-- xmit recursion チェック
  +-- rcu_read_lock(); binding の値をローカルへコピー; rcu_read_unlock()
  |
  +-- flowi6 構築
  |     proto = 97, oif = link_ifindex,
  |     daddr = remote, saddr = local, flowlabel
  |
  +-- dst = dst_cache_get(&priv->dst_cache)
  |     (なければ ip6_dst_lookup_flow() → dst_cache_set_ip6())
  |
  +-- dst->dev == dev ならループとして破棄
  +-- need = 40 + 2 + LL_RESERVED_SPACE(dst->dev)
  |     needed_headroom を必要なら引き上げ
  |     skb_cow_head(skb, need)
  |
  +-- skb_scrub_packet(skb, false)   ← dst/nfct/ext をリセット
  +-- skb_push(EtherIP 2 バイト); version=3, reserved=0
  +-- skb_push(IPv6 40 バイト); ip6_flow_hdr / payload_len / nexthdr / hop_limit
  +-- skb_dst_set(skb, dst); skb->ignore_df = 1
  +-- memset(skb->cb, 0, sizeof(struct inet6_skb_parm))
  +-- outer_mtu 指定時: IP6CB(skb)->frag_max_size = min(outer_mtu, dst_mtu)
  +-- stats 加算
  +-- dev_xmit_recursion_inc(); ip6_local_out(net, NULL, skb); dec()
```

### 4.1 経路と送信元アドレス

`ip6_dst_lookup_flow(net, NULL, &fl6, NULL)` を使います。これは

- 経路検索
- 送信元アドレスの選択（`local` が未指定の場合。本ドライバでは local 必須）
- XFRM ポリシー参照（外側 IPsec）

をまとめて行います。結果は per-CPU の `dst_cache` に保存し、ルート変更や
PMTU 変更は `dst_check()` で検出されます。設定変更時は明示的に
`dst_cache_reset()` します。

### 4.2 ヘッドルーム

外側 IPv6 と EtherIP ヘッダを push する前に、`skb_cow_head()` で

```
sizeof(struct ipv6hdr) + 2 + LL_RESERVED_SPACE(egress_dev)
```

を確保します。ローカル生成フレームは `needed_headroom` により最初から
十分なヘッドルームを持つため、通常は再アロケーションが発生しません。
転送フレームで不足する場合も、`pskb_expand_head()` は線形部分だけを
コピーします（frags は保持）。

### 4.3 `ignore_df` と `frag_max_size`

- `skb->ignore_df = 1` にすることで、`ip6_fragment()` が
  「ソケットが DF を要求している」と判断して破棄するのを防ぎます。
  EtherIP は内側フレームを不透明に運ぶため、外側の分割は常に透過です。
- `outer-mtu` が指定されている場合は `IP6CB(skb)->frag_max_size` に
  `min(outer_mtu, dst_mtu(dst))` を設定します。`ip6_fragment()` は
  この値以下に分割します。
- `frag_max_size` は送信 skb の cb を使うため、先に
  `memset(skb->cb, ...)` してから設定します。

### 4.4 内側ソケットとの分離

送信 skb はローカルソケット由来の場合 `skb->sk` を持ちます。しかし
`ip6_skb_dst_mtu()` と `ip6_fragment()` は

```c
struct ipv6_pinfo *np = skb->sk && !dev_recursion_level() ?
                        inet6_sk(skb->sk) : NULL;
```

という形で、xmit 再帰中は内側ソケットの PMTU ポリシーや `IPV6_MTU` を
参照しません。本ドライバは `ip6_local_out()` を
`dev_xmit_recursion_inc()` ～ `dev_xmit_recursion_dec()` の内側で呼ぶため、
外側パケットは常に経路 MTU（と `outer-mtu`）で分割されます。

`skb_orphan()` は呼びません。MSG_ZEROCOPY の skb はデストラクタが
ユーザページの完了処理を担うため、送信途中で orphan すると
ページの解放が早すぎる可能性があります。

### 4.5 チェックサム

- 外側 IPv6 にはチェックサムがありません。
- 内側 L4 のチェックサムは `CHECKSUM_PARTIAL` のまま引き継ぎます。
  分割時は `ip6_fragment()` が `skb_checksum_help()` で内側チェックサムを
  確定してから分割します。分割しない場合は egress デバイスの
  オフロード（またはソフトウェア）で計算されます。
- 受信側は、外側が実回線から来た場合は `CHECKSUM_NONE` にして内側スタックに
  検証を任せます（EtherIP には外側チェックサムがなく、内側チェックサムが
  正しいことを保証できないため）。
- 外側が veth などの仮想デバイスの場合、内側 TCP のチェックサムは
  `CHECKSUM_PARTIAL` のままトンネル終端に届きます。この状態を
  `CHECKSUM_NONE` に潰すと、内側スタックが未計算のチェックサムを検証して
  全パケットを落とします。そのため RX では `CHECKSUM_PARTIAL` を**保持**し、
  内側スタックに完了させます。`CHECKSUM_UNNECESSARY` にするわけではないので、
  検証自体は省略されません。
- 外側でフラグメントする場合は `ip6_fragment()` が送信側で
  `skb_checksum_help()` を呼ぶため、受信側には確定済みのチェックサムが
  届きます。

---

## 5. 受信パス（`etherip6_rcv`）

`inet6_add_protocol()` に登録したハンドラは、IPv6 の再組み立てと
拡張ヘッダ処理の後に呼ばれます。フラグは
`INET6_PROTO_NOPOLICY | INET6_PROTO_FINAL` です。

```
proto 97 handler(skb)
  |
  +-- pskb_may_pull(2); EtherIP ヘッダの version が 3 か確認
  +-- ip6h = ipv6_hdr(skb)
  +-- per-netns ハッシュを (daddr, saddr) で検索
  |     link 指定がある binding は skb->dev->ifindex と一致するものだけ
  |     完全一致を優先し、なければ link=0 のワイルドカード
  +-- netif_running(dev) でなければ rx_dropped
  +-- skb_pull(2)
  +-- pskb_may_pull(ETH_HLEN)
  +-- skb_scrub_packet(skb, false)
  +-- eth_type_trans(skb, dev)   ← pkt_type (BROADCAST/MULTICAST/OTHERHOST) を内側から判定
  +-- skb_postpull_rcsum(...); skb_reset_network_header()
  +-- skb->ip_summed = CHECKSUM_NONE
  +-- memset(skb->cb, 0, sizeof(struct inet6_skb_parm))
  +-- dev_sw_netstats_rx_add(dev, skb->len)
  +-- gro_cells_receive(&priv->gro_cells, skb)
```

### 5.1 scrub の位置

`skb_scrub_packet()` は `skb->pkt_type` を `PACKET_HOST` にリセットします。
内側フレームの L2 分類（ブロードキャスト/マルチキャスト/OTHERHOST）を
保つため、**eth_type_trans() の前に** scrub しています
（ip6_tunnel などは逆順ですが、L2 トンネルでは pkt_type が重要です）。

### 5.2 GRO

`gro_cells_receive()` は per-CPU の NAPI に skb を積み、
`napi_gro_receive()` を呼びます。デバイスに `NETIF_F_GRO` が付いている
（カーネルが `NETIF_F_SOFT_FEATURES` として全デバイスに付与）ため、
通常の GRO 処理が行われます。もし skb が clone されていたり、
バックログが上限に達した場合は `netif_rx()` にフォールバックします。

### 5.3 統計

受信は `dev_sw_netstats_rx_add()`（per-CPU）、送信は
`dev_sw_netstats_tx_add()` を使います。`ndo_get_stats64` は
`dev_get_tstats64()` で、64bit の rx/tx パケット・バイト数と、
`dev->stats` のエラー/ドロップ数を返します。

`etherip6_setup()` では `dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS` を
設定する必要があります。これを忘れるとカーネルは `dev->tstats` を確保せず、
`dev_sw_netstats_*` が **NULL の per-CPU ポインタ**を参照します。NULL の
`per_cpu_ptr()` は per-CPU 領域の先頭を指すため、統計が別デバイスの値に
見えたり、無関係な per-CPU データを破壊したりします。netns テストで
新規デバイスの統計が既存デバイスと同じ値になることで発見し、修正しました
（新規デバイスは 0 から開始することをテストで確認）。

---

## 6. 並行性

| 対象 | 保護 |
| --- | --- |
| `priv->cfg` | RTNL（newlink/changelink/fill_info/ndo_init/uninit） |
| `priv->binding` | RCU（TX/RX が参照）、RTNL（書き換え） |
| per-netns ハッシュ | RCU（RX 走査）、RTNL（挿入・削除） |
| `dst_cache` | per-CPU。BH 無効前提（xmit は BH 無効で呼ばれる） |
| `gro_cells` | per-CPU NAPI |
| 統計 | per-CPU（u64_stats） |
| `dev->needed_headroom` | `READ_ONCE` / `WRITE_ONCE` |

- binding は不変オブジェクトです。RX は `rcu_read_lock()` の中で
  ハッシュを走査し、見つけた binding の `dev` を使って処理します。
  binding の解放は `kfree_rcu()`、netdev の解放は unregister 側の
  グレースピリオドで守られるため、受信中の use-after-free はありません。
- 設定変更は「新 binding を挿入 → `priv->binding` を差し替え →
  旧 binding を unlink → `kfree_rcu()`」の順です。一時的に新旧両方が
  ハッシュに存在しますが、どちらも同じデバイスを指すため実害はありません。
- RX ハンドラ自体は IPv6 入力の RCU read-side から呼ばれます。本ドライバの
  `rcu_read_lock()` はネストです。
- netns の破棄時は、デバイスの unregister（ndo_uninit）が per-netns データの
  解放より先に走ります。

---

## 7. フラグメンテーションの詳細

### 7.1 オーバーヘッドと上限

```
outer_len = 40 (IPv6) + 2 (EtherIP) + inner_frame_len
```

- `ip6_fragment()` は 8 バイトの Fragment Header を追加します。
- 1 フラグメントのペイロードは `(mtu - 48) & ~7` バイトです。
- 最初のフラグメントには EtherIP ヘッダ（2 バイト）が含まれます。
  RFC 7112（最初のフラグメントに upper-layer header を含める）を満たします。
- 内側フレームの最大長は `mtu + 18`（VLAN 1 タグ分の余裕）です。
- `mtu` の最大は 65475 で、外側が 65535 バイトを超えないようにしています
  （IPv6 の jumbogram は生成しません）。

### 7.2 送信側

- `skb->ignore_df = 1` により `ip6_fragment()` が呼ばれます。
- `dst_mtu()` は経路 MTU（出力デバイスの MTU や `ip -6 route ... mtu`）です。
- `outer-mtu` 指定時は `frag_max_size` でさらに上限を下げます。
- フラグメントは `ip6_fragment()` の slow path で skb をアロケートして
  コピーします。9000 バイトフレームを 1280 バイト経路に流すと 8 個の
  フラグメントになります。
- 分割が不要な場合はコピーは発生しません（ヘッダの push のみ）。

### 7.3 受信側

- IPv6 の再組み立て（`ip6_frag`）がプロトコル 97 のハンドラより先に
  完了します。ドライバは完全なフレームを受け取ります。
- 再組み立ての上限はカーネルの `net.ipv6.ip6frag_high_thresh` などに
  従います。9000 バイト程度では通常問題になりません。
- 断片の欠落・タイムアウトはカーネルの `Ip6ReasmFails` に計上されます。

### 7.4 ICMPv6 PTB

本ドライバはプロトコル 97 の `err_handler` を登録していません。これは
EtherIP には内側へエラーを返す仕組みがなく、外側 PTB を内側フレームに
対応付ける標準的な方法がないためです。経路 MTU か `outer-mtu` で
明示的に制御してください（README 4.3 参照）。

将来 `err_handler` を実装する場合は、`skb->data` が引用された外側
IPv6 ヘッダを指すことを利用し、`ip6_update_pmtu()` で FIB に PMTU 例外を
入れたうえで `dst_cache_reset()` する方法が考えられます。

---

## 8. rtnetlink

### 8.1 属性

| 属性 | 型 | 検証 |
| --- | --- | --- |
| `IFLA_ETHERIP6_REMOTE` | `in6_addr` | 16 バイト固定、ユニキャスト |
| `IFLA_ETHERIP6_LOCAL` | `in6_addr` | 16 バイト固定、ユニキャスト |
| `IFLA_ETHERIP6_LINK` | u32 | 実在する ifindex、自分自身は不可 |
| `IFLA_ETHERIP6_HOP_LIMIT` | u8 | 0 は既定 64 |
| `IFLA_ETHERIP6_TCLASS` | u8 | 0〜255 |
| `IFLA_ETHERIP6_FLOWLABEL` | u32 | 20bit 以下 |
| `IFLA_ETHERIP6_OUTER_MTU` | u32 | 0 または 1280〜65535 |

- `local` / `remote` は「両方指定」または「両方未指定」のみ許可します。
  片方だけの指定は `-EINVAL` です。
- リンクローカルアドレスを使う場合は `LINK` が必須です（scope zone が
  決まらないため）。
- `LINK` は出力デバイスを固定します（`flowi6_oif`）。受信側でも
  そのデバイスで受信したパケットだけをマッチ対象にします。
- 属性番号は out-of-tree の私有値です。upstream 化する場合は UAPI の
  レビューが必要です。

### 8.2 ダンプ

`fill_info()` は `IFLA_INFO_DATA` の中に全属性を返します。
`get_size()` はそのためのバッファサイズを見積もります。統計は
カーネル共通の `IFLA_STATS64` として返されます。

---

## 9. 性能上のトレードオフ

| 設計 | 効果 | コスト/制約 |
| --- | --- | --- |
| `gro_cells` | RX の GRO、backlog 回避 | per-CPU NAPI をデバイスごとに保持 |
| per-CPU dst キャッシュ | 経路検索の削減 | 設定変更時に `dst_cache_reset()` が必要 |
| RCU binding | ロックレス TX/RX | 設定変更時に 1 binding 分のメモリ |
| noqueue + LLTX | qdisc キュー/ロックの回避（qlen 表示は既定値 1000） | ドライバ自身が再入可能である必要 |
| SG / HW_CSUM | 非線形 skb・チェックサムオフロードの維持 | フラグメント時はソフトウェア計算 |
| GSO 非広告 | 内側フレームが MTU を超えない | スタック側でセグメント化（CPU コスト） |
| 分割は `ip6_fragment()` | 実績のある実装、RFC 8200 準拠 | 分割時にフラグメントごとのコピー |
| per-CPU 統計 | 正確な 64bit、競合なし | - |

### 9.1 実測（netns + veth + iperf3）

QEMU TCG（2 vCPU）上で、2 netns を veth で接続し、内側で iperf3 を流した
結果です（iperf3 3.16、5 秒、MSS 固定）。

| ケース | スループット | 比 |
| --- | --- | --- |
| veth 直接、GSO/TSO 有効、MSS 1400 | 1329 Mbits/s | 参考（GSO は L2 トンネルに不公平） |
| veth 直接、GSO/TSO/GRO 無効、MSS 1400 | 431 Mbits/s | 100% |
| トンネル、内側 1500、外側 9000（分割なし） | 351 Mbits/s | 81.4% |
| トンネル逆方向（`-R`） | 363 Mbits/s | 84.2% |
| veth 直接、GSO 無効、MSS 8960 | 925 Mbits/s | 100% |
| トンネル、内側 9000、外側 9200（分割なし） | 857 Mbits/s | 92.6% |
| トンネル、内側 9000、外側 1500（分割あり） | 183 Mbits/s | 分割なしジャンボの 21.4% |

- すべてのケースで TCP 再送 0（最終実行）、ドロップ・エラー 0。
- veth を GSO 有効のまま基準にすると 27% になりますが、これは veth が
  GSO スーパーパケットをそのままピアに渡せるのに対し、EtherIP は内側
  Ethernet フレームを実体化する必要があるためです。公平な基準（GSO 無効）
  では非分割経路で 81〜93% です。
- 外側フラグメントはフラグメントごとの skb アロケーションとコピーのため
  コストが大きく、スループット重視なら内側 MTU を外側経路 MTU - 42 程度に
  下げるか、外側をジャンボ対応にしてください。

### 9.2 スループットの考え方

- 分割が不要な経路では、1 フレームあたり「ヘッダ 42 バイトの push」と
  「経路キャッシュ参照」程度のコストです。
- 分割が必要な場合、フラグメント数に比例した skb アロケーションと
  コピーが発生します。MTU 9000 を 1500 経路で使うと 6〜8 分割になります。
  スループットを最大化したい場合は、経路 MTU に合わせて内側 MTU を
  下げるか、ジャンボフレーム対応の経路を使ってください。
- 受信は GRO と NAPI を使いますが、EtherIP には外側チェックサムがないため
  `CHECKSUM_NONE` で渡します。TCP のチェックサム検証は受信側スタックが
  行います。

---

## 10. 既知の制約と拡張案

- **ICMPv6 PTB**: 未処理（前述）。`ip6_update_pmtu()` を使う `err_handler` を
  追加する余地があります。
- **iproute2 プラグイン**: stock iproute2 は `etherip6` 固有属性を解釈しません。
  現状は `etherip6ctl` を使います。プラグインを書く場合は
  `src/etherip6_uapi.h` の属性番号を共有してください。
- **netns 移動**: `NETIF_F_NETNS_LOCAL` のため不可。
- **マルチピア**: 1 デバイス 1 ピア。複数ピアはデバイスを分けます。
- **GSO / TSO**: 非対応（意図的）。将来 GSO を受ける場合は、内側フレームの
  セグメント化をドライバで行う必要があります。
- **XDP / AF_XDP**: 非対応。仮想デバイスとしては通常の `netif_receive_skb`
  系の処理のみです。
- **ハードウェアオフロード**: 外側にチェックサムがないため、外側の
  チェックサムオフロードはありません。内側のチェックサムはパススルーします。

---

## 11. テスト設計

`tests/vm/run-vm-test.sh` は次を行います。

1. カーネルモジュールをビルドしたカーネルと同じリリースの
   `linux-image-<KVER>` を取得（`apt-get download`）。
2. busybox + iproute2 + モジュール + 静的リンクした `etherip6ctl` /
   `etherip6_selftest` を含む initramfs を作成。
3. QEMU（TCG、KVM 不要）で起動し、`/init` を実行。
4. コンソールログの `SELFTEST PASS` / `INIT OK` / `INIT FAIL` を検査。

セルフテストは AF_PACKET で

- 送信側デバイスに Ethernet フレームを注入
- 受信側デバイスでフレームを受信
- 14 バイトの Ethernet ヘッダ、マジック、シーケンス番号、全ペイロードを照合

します。フラグメントの有無は `/proc/net/snmp6` の `Ip6FragCreates` と
`Ip6ReasmOKs` の差分で確認します。

さらに、2 netns を veth で接続し、それぞれに `etherip6` デバイスを
作成して iperf3 の TCP 通信を流すテストを行います。

```
ns1                                      ns2
  eip0 (10.0.0.1/24)                       eip1 (10.0.0.2/24)
  v1 (2001:db8:ff::1/64)  <-- veth -->  v2 (2001:db8:ff::2/64)
```

- 外側 veth の GSO/TSO/GRO を有効にした基準と、無効にした基準を測定
  （`ethtool -K` で切り替え）
- トンネル内側 MTU 1500（外側 9000、分割なし）と 9000（外側 9200、
  分割なし / 外側 1500、分割あり）を測定
- 順方向と `iperf3 -R` の逆方向を測定
- フラグメント処理の検証: 外側 MTU 1500 / 1400 / 1280、順方向 / 逆方向 /
  双方向 / 4 並列、`outer-mtu 1400`（経路 9200）の各ケースで
  `Ip6FragCreates` / `Ip6ReasmOKs` が増え、`Ip6FragFails` /
  `Ip6ReasmFails` が 0 であることを確認
- 新規デバイスの統計が 0 から始まることも確認

この構成は「実際にモジュールをロードしたカーネルで、TCP を含むデータパスを
通り、スループットが極端に減衰しないこと」を確認するためのものです。
絶対値は TCG のエミュレーション速度に依存します。

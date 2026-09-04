/*
 * Bridge/midi_coremidi.c — CoreMIDI 連携層(macOS)
 *
 * P633: 従来 `Core/px68k/x68k/midi_darwin.c` が担っていた「win32api の
 * midiOut* 互換 API を CoreMIDI で実装する」層を Bridge 側へ移設した実装。
 * `midi_darwin.c` は project.pbxproj の Compile Sources から除外され、
 * ファイル自体は Core 無改変のまま残っている(CLAUDE.md の Core File
 * Modification Policy に従い、Core ファイルは 1 バイトも編集しない)。
 * 上流 x11/SDL 層の責務を Bridge へ再実装するのと同じパターン。
 *
 * 移設元: Core/px68k/x68k/midi_darwin.c(全 359 行、kameya 2022/3/27)。
 * 外部から見える 10 個の公開関数 + 受信コールバックの振る舞いは踏襲し、
 * P633 Fix Plan が挙げた 3 点の欠陥のみ修正した:
 *
 *  (1) SysEx 187 バイト以上の無言破棄
 *      移設元は `MIDIPacketListAdd(packetList, sizeof(packetBuf), ...)` に
 *      `MIDBUF_SIZE`=200(win32api/mmsystem.h:49)固定のバッファを渡し、
 *      戻り値(NULL)を検査していなかった。ゲスト側の送信バッファは
 *      `MIDIBUFFERS`=1024(x68k/midi.c:29 `Tx_buff`/`MIDI_EXCVBUF`)まで
 *      使えるため、187 バイト以上の SysEx が空の PacketList のまま
 *      MIDISend() され、実機器へ何も届かなかった。
 *      → ゲスト最大長 1024 バイトを 1 パケットで収容できる容量を確保し、
 *        さらに `MIDIPacketListAdd()` の戻り値を必ず検査して、万一
 *        収まらない場合はチャンク分割して送り切る(無言破棄しない)。
 *
 *  (2) F1/F2/F3 のデータバイト切り捨て
 *      移設元は `(status & 0xf0) == 0xf0` を一律 1 バイト長として扱って
 *      いたため、F1(MTC quarter frame, 2B)・F2(song position pointer, 3B)・
 *      F3(song select, 2B)のデータバイトが落ちていた。送信元である
 *      `midi_data_out()`(x68k/midi.c:388-396)は既に正しい長さで
 *      組み立てているので、誤っていたのは送出側のみ。
 *
 *  (3) CFStringRef のリーク
 *      移設元はデバイス列挙ループ内で取得した CFStringRef を解放せず、
 *      ループ終了後に最後の 1 個だけ CFRelease していた。
 *      → 各周回で確実に解放する。
 *
 * ★P490 の回避コードについて: 移設元の `packetBuf` は `const uint8_t[]` で
 *   read-only セクションに置かれるため、`MIDIPacketListInit()` が書き込んだ
 *   瞬間に SIGBUS になる欠陥があり、P490 は Bridge から `packetList`
 *   ポインタを書込可能バッファへ付け替えて回避していた。本実装は最初から
 *   書込可能な static バッファを自前で所有するため、その回避策は不要
 *   (EmulatorBridge.c 側の extern 宣言・付け替えコードは P633 で削除済み)。
 */

/* CoreMIDI ヘッダは px68k 側ヘッダ(win32api 互換層)より前に置く
 * (移設元 midi_darwin.c:10-12 と同じ順序)。#import + CLANG_ENABLE_MODULES=YES
 * により CoreMIDI.framework が自動リンクされる。 */
#import <CoreMIDI/MIDIServices.h>
#import <CoreMIDI/CoreMIDI.h>
#import <mach/mach_time.h>

#include <stdint.h>
#include <string.h>

#include "../Core/px68k/common.h"          /* p6logd() */
#include "../Core/px68k/x68k/midi.h"       /* midiOut* 宣言 / Rx_buff / MIDI_R35 等 */
#include "../Core/px68k/x68k/irqh.h"       /* IRQH_Int() */
#include "px68k_compat.h"                  /* menu_items -> mx68k_menu_items */
#include "midi_shadow.h"                   /* P693: MIDI Viewer 用の送受信カウンタ */
#include "EmulatorBridge.h"                /* P693: mx68k_midi_get_* の宣言と照合するため */

/* ---- CoreMIDI 側の状態(移設元 midi_darwin.c:20-34 と同じ役割) ----
 * 移設元では非 static のグローバルだったが、外部から参照する利用者は
 * P490 の `packetList` 付け替えのみで、それは本サイクルで削除したため
 * すべてファイルスコープに閉じる。 */
static MIDIClientRef   mid_client   = 0;
static MIDIPortRef     mid_out_port = 0;
static MIDIEndpointRef mid_endpoint = 0;
static MIDIDeviceRef   mid_device   = 0;

static MIDIPortRef     mid_in_port  = 0;
static MIDIEndpointRef mid_source   = 0;

/* `*phmo` へ入れるダミーハンドル(移設元 midi_darwin.c:34,126,192)。
 * HANDLE は `void *`(win32api/windows.h:58)なので const は付けない。 */
static char mid_name[] = "px68k-MIDI";

/* menu_items[8]/[9] の列挙上限(移設元 midi_darwin.c:108,174 の i<8)。 */
#define P633_MIDI_DEV_MAX 8

/* ゲストが 1 メッセージで送りうる最大バイト数。
 * x68k/midi.c:29 `#define MIDIBUFFERS 1024` に由来し、SysEx は
 * `MIDI_Sendexclusive()`(midi.c:227-232)経由で最大この長さになる。 */
#define P633_MIDI_MAX_MSG_BYTES 1024

/* MIDIPacketList 用バッファ。P633_MIDI_MAX_MSG_BYTES のデータに
 * MIDIPacketList/MIDIPacket のヘッダとアライメント調整分を加えても
 * 十分収まる容量(実測上のオーバーヘッドは十数バイト程度)。
 * 容量が足りない場合でも下記 p633_midi_send_bytes() が
 * MIDIPacketListAdd() の戻り値を検査して分割送信へ落とすため、
 * このサイズの見積り誤りが無言破棄に化けることはない。 */
#define P633_MIDI_PKTLIST_BYTES 4096
static uint8_t g_midi_packet_buf[P633_MIDI_PKTLIST_BYTES] __attribute__((aligned(16)));

/* -----------------------------------------------------------------
 *  P693: MIDI Viewer 用カウンタの実体(宣言は Bridge/midi_shadow.h)。
 *  レイアウト隣接事故を避けるため、本ファイル(MIDI 送受信経路そのもの)に
 *  閉じて定義する —— opm_shadow / mercury_opn_shadow と同じ方針。
 * ----------------------------------------------------------------- */
_Atomic(unsigned long long) g_midi_tx_messages = 0;
_Atomic(unsigned long long) g_midi_tx_bytes    = 0;
_Atomic(uint32_t)           g_midi_tx_last     = 0;
_Atomic(unsigned long long) g_midi_rx_messages = 0;
_Atomic(unsigned long long) g_midi_rx_bytes    = 0;
_Atomic(uint32_t)           g_midi_rx_last     = 0;

/* 直近 1 メッセージを 1 ワードへ畳む(midi_shadow.h のパッキング契約の実装)。
 * 呼び出し側は data != NULL かつ len >= 1 を保証すること。
 *
 * ★len は 8bit しか無いので min(len, 0xff) へクランプする(SysEx は最大
 *   P633_MIDI_MAX_MSG_BYTES=1024 まで来るため、実長そのものは入らない
 *   —— あくまで表示用の目安値)。
 * ★data[1] / data[2] は実長がそこまで届いているときだけ読む。1 バイト
 *   メッセージ(例: 0xf8 クロックバイト)で呼び出し元バッファの外を
 *   読まないための必須ガード(P693 Code Review 指摘)。 */
static uint32_t
p693_pack_last_msg(const uint8_t *data, uint32_t len)
{
	uint32_t packed = (uint32_t)data[0];
	packed |= (uint32_t)((len > 0xffu) ? 0xffu : len) << 8;
	if (len >= 2) packed |= (uint32_t)data[1] << 16;
	if (len >= 3) packed |= (uint32_t)data[2] << 24;
	return packed;
}

/* -----------------------------------------------------------------
 *  共通送出ヘルパ(P633 追加)
 *  MIDIPacketListAdd() の戻り値を必ず検査し、1 回で収まらなければ
 *  チャンクを半分にしながら必ず全バイトを送り切る。
 * ----------------------------------------------------------------- */
static void
p633_midi_send_bytes(const uint8_t *data, uint32_t len)
{
	if (data == NULL || len == 0) {
		return;
	}

	/* P693: 「ゲストが送信しようとした回数」を、送出の成否より前に計上する。
	 * ★意図的に下のポートガードより前に置く —— Fix Plan の自己反証可能性節が
	 *   求める切り分け(tx_messages>0 かつ tx_bytes==0 なら送出層で落ちている、
	 *   tx_bytes>0 で無音なら外部機器側)を成立させるには、messages が
	 *   「試行回数」、bytes が「実際に CoreMIDI へ渡せたバイト数」という
	 *   別々の意味を持っていなければならない。両方を MIDISend() 直前で
	 *   一緒に加算すると必ず同時に増えるため、ゼロ表示の解釈が
	 *   1 つに絞れなくなる(デバイス未オープンと「そもそも送っていない」が
	 *   区別できない)。 */
	atomic_fetch_add_explicit(&g_midi_tx_messages, 1, memory_order_relaxed);
	atomic_store_explicit(&g_midi_tx_last, p693_pack_last_msg(data, len),
	                      memory_order_relaxed);

	if (mid_out_port == 0 || mid_endpoint == 0) {
		return;	/* デバイス未オープン(移設元は無効な endpoint へ送っていた) */
	}

	uint32_t sent = 0;
	while (sent < len) {
		uint32_t chunk = len - sent;
		if (chunk > P633_MIDI_MAX_MSG_BYTES) {
			chunk = P633_MIDI_MAX_MSG_BYTES;
		}

		MIDIPacketList *plist = (MIDIPacketList *)g_midi_packet_buf;
		MIDIPacket     *cur   = MIDIPacketListInit(plist);
		MIDIPacket     *added = MIDIPacketListAdd(plist,
		                                          (ByteCount)sizeof(g_midi_packet_buf),
		                                          cur, mach_absolute_time(),
		                                          (ByteCount)chunk, data + sent);

		/* 収まらなければ半分ずつ縮めて再試行(無言破棄しない)。 */
		while (added == NULL && chunk > 1) {
			chunk >>= 1;
			cur   = MIDIPacketListInit(plist);
			added = MIDIPacketListAdd(plist,
			                          (ByteCount)sizeof(g_midi_packet_buf),
			                          cur, mach_absolute_time(),
			                          (ByteCount)chunk, data + sent);
		}
		if (added == NULL) {
			/* 1 バイトすら入らない = 想定外。ここで諦めるが、移設元と違い
			 * 失敗した事実をログに残す。 */
			p6logd("MIDI:CoreMIDI: packet build failed (len=%u sent=%u)\n",
			       (unsigned)len, (unsigned)sent);
			return;
		}

		/* P693: 実際に CoreMIDI へ渡したバイト数(チャンク分割後の確定値)。
		 * Fix Plan §A / Code Review §3 が指定した「MIDISend() 直前」の位置。 */
		atomic_fetch_add_explicit(&g_midi_tx_bytes, (unsigned long long)chunk,
		                          memory_order_relaxed);

		MIDISend(mid_out_port, mid_endpoint, plist);
		sent += chunk;
	}
}

/* -----------------------------------------------------------------
 *  デバイス名の取得(P633: CFStringRef を各周回で確実に解放する)
 *  endpoint 名と device 名が異なる場合は "device:endpoint" 形式に
 *  組み立てる(移設元 midi_darwin.c:109-117,175-183 と同じ表示規則)。
 * ----------------------------------------------------------------- */
static void
p633_store_endpoint_name(MIDIEndpointRef endpoint, char *dst, size_t dst_size)
{
	CFStringRef epstrRef  = NULL;
	CFStringRef devstrRef = NULL;
	CFStringRef strRef    = NULL;
	MIDIEntityRef entity  = 0;

	dst[0] = '\0';

	if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &epstrRef) != noErr) {
		epstrRef = NULL;
	}
	/* endpoint -> entity -> device。仮想ポートは entity を持たないことが
	 * あるため、成功したときだけ device 名を引く(移設元は失敗時に
	 * 直前のデバイスの名前を使い回していた)。 */
	if (MIDIEndpointGetEntity(endpoint, &entity) == noErr) {
		if (MIDIEntityGetDevice(entity, &mid_device) == noErr) {
			if (MIDIObjectGetStringProperty(mid_device, kMIDIPropertyName,
			                                &devstrRef) != noErr) {
				devstrRef = NULL;
			}
		}
	}

	if (epstrRef != NULL && devstrRef != NULL
	    && CFStringCompare(epstrRef, devstrRef, 0) != kCFCompareEqualTo) {
		strRef = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@:%@"),
		                                  devstrRef, epstrRef);
	} else if (epstrRef != NULL) {
		strRef = (CFStringRef)CFRetain(epstrRef);
	}

	if (strRef != NULL) {
		/* kCFStringEncodingUTF8: UTF-8 でポート名を取り出す */
		if (!CFStringGetCString(strRef, dst, (CFIndex)dst_size, kCFStringEncodingUTF8)) {
			dst[0] = '\0';
		}
		CFRelease(strRef);
	}

	/* P633: 各周回で確実に解放する(移設元は解放漏れ)。 */
	if (epstrRef  != NULL) CFRelease(epstrRef);
	if (devstrRef != NULL) CFRelease(devstrRef);
}

/* -----------------------------------------------------------------
 *  OS からの受信コールバック(移設元 midi_darwin.c:39-63)
 * ----------------------------------------------------------------- */
static void
mid_In_callback(const MIDIPacketList *packetList,
                void *readProcRefCon,
                void *srcConnRefCon)
{
	(void)readProcRefCon;
	(void)srcConnRefCon;

	if ((MIDI_R35 & 0x01) == 0x00) return;	/* Rx-FIFO 受信禁止 */

	MIDIPacket *packet = (MIDIPacket *)packetList->packet;
	uint32_t count = packetList->numPackets;
	for (uint32_t j = 0; j < count; j++) {
		for (uint32_t i = 0; i < packet->length; i++) {
			Rx_buff[RxW_point] = packet->data[i];
			if (RxW_point < 250) { RxW_point++; }	/* buffer full */
		}

		/* P693: 受信の計上。★Rx_buff / RxW_point / RxR_point は一切読まない
		 * (既知の 2 スレッド無同期共有 = Docs/09 の D-73 に、新たな読み手を
		 * 足さないため)。ここで足すのは本サイクル新設の atomic だけ。
		 *
		 * ★計上の単位は CoreMIDI パケット。1 パケットに複数の MIDI
		 *   メッセージが載ることがあるので rx_messages は厳密な
		 *   「メッセージ数」ではなく「受信パケット数」だが、
		 *   rx_bytes(実バイト数)の分母として機能するという役割は同じ。
		 * ★上の early return(MIDI_R35 bit0 = Rx-FIFO 受信禁止)より後なので、
		 *   ゲストが受信を禁止している間に届いたバイトは計上されない。
		 *   MIDI_R35 はレジスタ節にそのまま表示するため、rx が 0 のときに
		 *   「機器が送っていない」のか「ゲストが受信を止めている」のかは
		 *   同一画面で区別できる。 */
		if (packet->length > 0) {
			atomic_fetch_add_explicit(&g_midi_rx_messages, 1,
			                          memory_order_relaxed);
			atomic_fetch_add_explicit(&g_midi_rx_bytes,
			                          (unsigned long long)packet->length,
			                          memory_order_relaxed);
			atomic_store_explicit(&g_midi_rx_last,
			                      p693_pack_last_msg(packet->data,
			                                         packet->length),
			                      memory_order_relaxed);
		}

		packet = MIDIPacketNext(packet);
	}

	if (MIDI_IntEnable & 0x20) {		/* 割り込み許可? */
		MIDI_IntFlag |= 0x20;		/* Rx int 発生 */
		MIDI_IntVect  = 0x0a;		/* set vector */
		IRQH_Int(4, &MIDI_Int);		/* int 4 */
	}
}

/* -----------------------------------------------------------------
 *  MIDI out port Open (CoreMIDI synth)  (移設元 midi_darwin.c:68-130)
 * ----------------------------------------------------------------- */
uint32_t
mid_outDevList(LPHMIDIOUT phmo)
{
	uint32_t Device_num = 0;
	OSStatus err_sts;

	/* Create Client for CoreMIDI */
	err_sts = MIDIClientCreate(CFSTR("px68k"), NULL, NULL, &mid_client);
	if (err_sts != noErr) {
		p6logd("MIDI:CoreMIDI: No out client created.\n");
		return Device_num;
	}

	/* Create OutPort for client */
	err_sts = MIDIOutputPortCreate(mid_client, CFSTR("px68k MIDI out_Port"), &mid_out_port);
	if (err_sts != noErr) {
		p6logd("MIDI:CoreMIDI: No out port created.\n");
		return Device_num;
	}

	/* Get the MIDIEndPoint */
	mid_endpoint = 0;
	uint32_t core_mid_num = (uint32_t)MIDIGetNumberOfDestinations();	/* 仮想ポート含む */
	if (core_mid_num == 0) {
		return Device_num;	/* No found */
	}

	/* Store MIDI out port LIST */
	for (uint32_t i = 0; i < core_mid_num; i++) {
		mid_endpoint = MIDIGetDestination(i);
		if ((mid_endpoint) && (i < P633_MIDI_DEV_MAX)) {	/* MAX item check(8 個まで) */
			p633_store_endpoint_name(mid_endpoint,
			                         menu_items[8][Device_num],
			                         sizeof(menu_items[8][Device_num]));
			p6logd("Find MIDI out:%s\n", menu_items[8][Device_num]);
			Device_num++;
		}
	}
	menu_items[8][Device_num][0] = '\0';	/* Menu END */

	if (core_mid_num != 0) {
		*phmo = (HANDLE)mid_name;	/* MIDI Active!(ダミーを代入しておく) */
	}

	return core_mid_num;
}

/* -----------------------------------------------------------------
 *  Search and Store MIDI in Device LIST  (移設元 midi_darwin.c:135-196)
 * ----------------------------------------------------------------- */
uint32_t
mid_inDevList(LPHMIDIOUT phmo)
{
	uint32_t Device_num = 0;
	OSStatus err_sts;

	/* Client は mid_outDevList() が作ったものを共用する
	 * (移設元 midi_darwin.c:143-149 でも同様にコメントアウト済み)。 */

	/* Create input port */
	err_sts = MIDIInputPortCreate(mid_client, CFSTR("px68k MIDI in_Port"),
	                              mid_In_callback, NULL, &mid_in_port);
	if (err_sts != noErr) {
		p6logd("MIDI:CoreMIDI: No port in created.\n");
		return Device_num;	/* No found */
	}

	uint32_t core_mid_num = (uint32_t)MIDIGetNumberOfSources();
	if (core_mid_num == 0) {
		return Device_num;	/* No found */
	}

	/* Store MIDI in port LIST */
	mid_source = 0;
	for (uint32_t i = 0; i < core_mid_num; i++) {
		mid_source = MIDIGetSource(i);
		if ((mid_source) && (i < P633_MIDI_DEV_MAX)) {	/* MAX item check(8 個まで) */
			p633_store_endpoint_name(mid_source,
			                         menu_items[9][Device_num],
			                         sizeof(menu_items[9][Device_num]));
			p6logd("Find MIDI in :%s\n", menu_items[9][Device_num]);
			Device_num++;
		}
	}
	menu_items[9][Device_num][0] = '\0';	/* Menu END */

	if (core_mid_num != 0) {
		*phmo = (HANDLE)mid_name;	/* MIDI Active!(ダミーを代入しておく) */
	}

	return core_mid_num;
}

/* -----------------------------------------------------------------
 *  set/change MIDI out Port  (移設元 midi_darwin.c:201-222)
 * ----------------------------------------------------------------- */
void
midOutChg(uint32_t port_no, uint32_t bank)
{
	(void)bank;	/* Bank select は移設元でもコメントアウト済み */

	/* All note off */
	for (uint32_t msg = 0x7bb0; msg < 0x7bc0; msg++) {
		midiOutShortMsg((HMIDIOUT)0, msg);
	}

	/* CoreMIDI endpoint change */
	mid_endpoint = MIDIGetDestination(port_no);
	if (mid_endpoint == 0) {
		p6logd("MIDI Change error.\n");
	}
}

/* -----------------------------------------------------------------
 *  set/change MIDI in Port  (移設元 midi_darwin.c:226-238)
 * ----------------------------------------------------------------- */
void
midInChg(uint32_t port_no)
{
	OSStatus err_sts;

	/* CoreMIDI endpoint change */
	mid_source = MIDIGetSource(port_no);
	err_sts = MIDIPortConnectSource(mid_in_port, mid_source, NULL);
	if (err_sts != noErr) {
		p6logd("MIDI in Change error.\n");
	}
}

/* -----------------------------------------------------------------
 *  MIDI Port close  (移設元 midi_darwin.c:243-271)
 * ----------------------------------------------------------------- */
uint32_t
midiOutClose(HMIDIOUT hmo)
{
	(void)hmo;

	OSStatus err_sts;

	/* Disconnect in Port */
	err_sts = MIDIPortDisconnectSource(mid_in_port, mid_source);
	if (err_sts != noErr) p6logd("Disconnect MIDI-Source err\n");

	/* Dispose Port */
	err_sts = MIDIPortDispose(mid_out_port);
	if (err_sts != noErr) p6logd("Dispose MIDI-out Port err\n");

	/* Dispose Client */
	err_sts = MIDIClientDispose(mid_client);
	if (err_sts != noErr) p6logd("Dispose MIDI-Client err\n");

	/* 移設元は破棄後もハンドルを保持したままだったが、再オープン時に
	 * 破棄済みハンドルへ送出しないよう 0 に戻す(p633_midi_send_bytes の
	 * ガードが効くようにするため)。 */
	mid_out_port = 0;
	mid_in_port  = 0;
	mid_endpoint = 0;
	mid_source   = 0;
	mid_client   = 0;

	return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------
 *  Send Short Message (演奏データ送信)  (移設元 midi_darwin.c:276-305)
 * ----------------------------------------------------------------- */
uint32_t
midiOutShortMsg(HMIDIOUT hmo, uint32_t msg)
{
	(void)hmo;
	uint8_t messg[4];

	/* (uint32)msg を byte に分解 */
	messg[0] = (uint8_t)(msg & 0xff);
	msg >>= 8;
	messg[1] = (uint8_t)(msg & 0xff);
	msg >>= 8;
	messg[2] = (uint8_t)(msg & 0xff);
	messg[3] = 0;

	/* length of msg
	 * P633 修正: 移設元は 0xf0 系を一律 1 バイトにしていたため
	 * F1/F2/F3 のデータバイトが落ちていた。 */
	uint32_t len;
	uint8_t  status = messg[0];
	if ((status & 0xf0) == 0xf0) {
		switch (status) {
		case 0xf1: len = 2; break;	/* MTC quarter frame        */
		case 0xf2: len = 3; break;	/* song position pointer    */
		case 0xf3: len = 2; break;	/* song select              */
		default:   len = 1; break;	/* F0/F4-FF: 1 バイト扱い   */
		}
	} else if (((status & 0xf0) == 0xc0) || ((status & 0xf0) == 0xd0)) {
		len = 2;			/* prog. chg / channel press */
	} else {
		len = 3;			/* note on/off, key press, cont.chg, pitch wheel */
	}

	p633_midi_send_bytes(messg, len);

	return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------
 *  Exclusive GO! (設定データ送信)  (移設元 midi_darwin.c:310-331)
 * ----------------------------------------------------------------- */
uint32_t
midiOutLongMsg(HMIDIOUT hmo, LPMIDIHDR pmh, uint32_t cbmh)
{
	(void)hmo;
	(void)cbmh;

	if (pmh == NULL || pmh->dwBufferLength == 0) {	/* length check */
		return MMSYSERR_NOERROR;
	}

	p633_midi_send_bytes((const uint8_t *)pmh->lpData, pmh->dwBufferLength);

	return MMSYSERR_NOERROR;
}

/*---Dummy--*/

uint32_t
midiOutUnprepareHeader(HMIDIOUT hmo, LPMIDIHDR pmh, uint32_t cbmh)
{
	(void)hmo;
	(void)pmh;
	(void)cbmh;
	return MMSYSERR_NOERROR;
}

uint32_t
midiOutPrepareHeader(HMIDIOUT hmo, LPMIDIHDR pmh, uint32_t cbmh)
{
	(void)hmo;
	(void)pmh;
	(void)cbmh;
	return MMSYSERR_NOERROR;
}

uint32_t
midiOutReset(HMIDIOUT hmo)
{
	(void)hmo;
	return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------
 *  P693: MIDI Viewer 向け getter(Swift から毎フレーム読まれる)
 *
 *  いずれも atomic スカラを 1 個読むだけで、生ポインタの逆参照を伴わない。
 *  よって呼び出し側(EmulatorEngine.fetchMonitorsAndPerfStats)で
 *  emulationLock を取る必要は無い —— P692 のストレージモニタが
 *  emulationLock を必要としたのは mx68k_scsi_live_* が SPC 内部の
 *  Disk* を逆参照するためであり、こちらはその条件に当たらない。
 *
 *  ★relaxed 読み: 6 個の値は互いに厳密な同一瞬間のスナップショットでは
 *    ない(MX68K_AudioBufferStatus と同じ割り切り)。モニタ表示専用。
 * ----------------------------------------------------------------- */

uint64_t mx68k_midi_get_tx_messages(void)
{
	return (uint64_t)atomic_load_explicit(&g_midi_tx_messages, memory_order_relaxed);
}

uint64_t mx68k_midi_get_tx_bytes(void)
{
	return (uint64_t)atomic_load_explicit(&g_midi_tx_bytes, memory_order_relaxed);
}

uint32_t mx68k_midi_get_tx_last(void)
{
	return atomic_load_explicit(&g_midi_tx_last, memory_order_relaxed);
}

uint64_t mx68k_midi_get_rx_messages(void)
{
	return (uint64_t)atomic_load_explicit(&g_midi_rx_messages, memory_order_relaxed);
}

uint64_t mx68k_midi_get_rx_bytes(void)
{
	return (uint64_t)atomic_load_explicit(&g_midi_rx_bytes, memory_order_relaxed);
}

uint32_t mx68k_midi_get_rx_last(void)
{
	return atomic_load_explicit(&g_midi_rx_last, memory_order_relaxed);
}

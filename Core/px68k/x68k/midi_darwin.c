/*
 macOS用CoreMIDI 対応の関数群
 Win.のMIDI関数とコンパチ(winmm.lib)で置き換えてます。
 CoreMIDIで検出された機器をリストします。
 MIMPIの音色変更FileはConfigで指定してください。
 
 2022/3/27  by kameya
*/

#import <CoreMIDI/MIDIServices.h>
#import <CoreMIDI/CoreMIDI.h>
#import <mach/mach_time.h>
#include "prop.h"
#include "winui.h"
#include "midi.h"
#include "irqh.h"

uint32_t midiOutShortMsg(HMIDIOUT , uint32_t );

	/* for CoreMIDI */
	MIDIClientRef mid_client;
	MIDIPortRef mid_out_port;
	MIDIEndpointRef mid_endpoint;
	MIDIDeviceRef mid_device;
	MIDIPacket* mid_Packet = 0;

	MIDIPortRef mid_in_port;
	MIDIEndpointRef mid_source;

	/* Create MIDIPacketList from packetbuff */
	const uint8_t packetBuf[MIDBUF_SIZE];
	MIDIPacketList *packetList = (MIDIPacketList *)packetBuf;

	const char mid_name[] = {"px68k-MIDI"};

/*
 call back from OS
*/
void
mid_In_callback(const MIDIPacketList* packetList,
				  void* readProcRefCon,
				  void* srcConnRefCon)
{
  if((MIDI_R35 & 0x01) == 0x00) return;//Rx-FIFO 受信禁止

  MIDIPacket *packet = (MIDIPacket *)packetList->packet;
  uint32_t count = packetList->numPackets;
  for(uint32_t j=0; j<count; j++){
   for(uint32_t i=0; i<packet->length; i++){
	 Rx_buff[RxW_point] = packet->data[i];
	 if(RxW_point < 250){ RxW_point++; }// buffer full
   }
   packet = MIDIPacketNext(packet);
  }

  if(MIDI_IntEnable & 0x20){// 割り込み許可？
    MIDI_IntFlag |= 0x20; // Rx int 発生
    MIDI_IntVect =  0x0a; // set vector
    IRQH_Int(4, &MIDI_Int);//int 4
  }

 return;
}

/*
   MIDI out port Open (CoreMIDI synth)
*/
uint32_t
mid_outDevList(LPHMIDIOUT phmo)
{
	uint32_t Device_num = 0;
	OSStatus err_sts;

	/* Create Client for coreMIDI */
	err_sts = MIDIClientCreate(CFSTR("px68k"), NULL, NULL, &mid_client);
	if (err_sts != noErr)
	{
		p6logd("MIDI:CoreMIDI: No out client created.\n");
		return Device_num;
	}

	// Create OutPort for client 
	err_sts = MIDIOutputPortCreate(mid_client, CFSTR("px68k MIDI out_Port"), &mid_out_port);

	if (err_sts != noErr)
	{
		p6logd("MIDI:CoreMIDI: No out port created.\n");
		return Device_num;
	}

	/* Get the MIDIEndPoint */
	mid_endpoint = 0;
	//uint32_t core_mid_num = MIDIGetNumberOfDevices();//仮想ポート含まない
	//mid_endpoint = MIDIGetDevice(core_mid_num);
	uint32_t core_mid_num = (uint32_t)MIDIGetNumberOfDestinations();//仮想ポート含む
	if (core_mid_num == 0)
	{
		  return Device_num; // No found
	}

	/* Store MIDI out port LIST */
	CFStringRef strRef,epstrRef,devstrRef;
	MIDIEntityRef mid_entity;
	 for(uint32_t i = 0; i<core_mid_num; i++){
		mid_endpoint = MIDIGetDestination(i);
		MIDIEndpointGetEntity(mid_endpoint, &mid_entity);
		MIDIEntityGetDevice(mid_entity, &mid_device);
		if((mid_endpoint) && (i<8)){// MAX item check(８個までLISTに制限)
			MIDIObjectGetStringProperty(mid_endpoint, kMIDIPropertyName, &epstrRef);
			MIDIObjectGetStringProperty(mid_device, kMIDIPropertyName, &devstrRef);
			if (devstrRef != NULL && CFStringCompare(epstrRef, devstrRef, 0) != kCFCompareEqualTo) {
			 strRef = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@:%@"), devstrRef, epstrRef);
			} else {
			 strRef = epstrRef;
			}
			//kCFStringEncodingUTF8：UTF8でポートの名前(string)を取り出す
			CFStringGetCString(strRef, menu_items[8][Device_num], sizeof(menu_items[8][Device_num]), kCFStringEncodingUTF8);
			p6logd("Find MIDI out:%s\n",menu_items[8][Device_num]);
			Device_num ++;
		}
	 }
	CFRelease(strRef);
	strcpy(menu_items[8][Device_num],"\0"); // Menu END 

	if(core_mid_num != 0){
	  *phmo = (HANDLE)mid_name; //MIDI Active!(ダミーを代入しておく)
	}

return core_mid_num;
}

/*
 Search and Store MIDI in Device LIST 
*/
uint32_t
mid_inDevList(LPHMIDIOUT phmo)
{

  uint32_t Device_num = 0;
  OSStatus err_sts;

  // Create Client for coreMIDI
  //err_sts = MIDIClientCreate(CFSTR("px68k"), NULL, NULL, &mid_client);

  //if (err_sts != noErr)
  //{
  //  p6logd("MIDI:CoreMIDI: No in client created.\n");
  //  return Device_num;
  //}

  // Create input port
  err_sts = MIDIInputPortCreate(mid_client, CFSTR("px68k MIDI in_Port"),
                            mid_In_callback, NULL, &mid_in_port);
  if (err_sts != noErr)
  {
    p6logd("MIDI:CoreMIDI: No port in created.\n");
    return Device_num; // No found
  }

  uint32_t core_mid_num = (uint32_t)MIDIGetNumberOfSources();
  if (core_mid_num == 0)
  {
    return Device_num; // No found
  }

	/* Store MIDI in port LIST */
	mid_source = 0;
	CFStringRef strRef,epstrRef,devstrRef;
	MIDIEntityRef mid_entity;
	 for(uint32_t i = 0; i<core_mid_num; i++){
	  mid_source = MIDIGetSource(i);
	  MIDIEndpointGetEntity(mid_source, &mid_entity);
	  MIDIEntityGetDevice(mid_entity, &mid_device);
	  if((mid_source) && (i<8)){// MAX item check(８個までLISTに制限)
	   MIDIObjectGetStringProperty(mid_source, kMIDIPropertyName, &epstrRef);
	   MIDIObjectGetStringProperty(mid_device, kMIDIPropertyName, &devstrRef);
	   if (devstrRef != NULL && CFStringCompare(epstrRef, devstrRef, 0) != kCFCompareEqualTo) {
	    strRef = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@:%@"), devstrRef, epstrRef);
	   } else {
	    strRef = epstrRef;
	   }
	   //kCFStringEncodingUTF8：UTF8でポートの名前(string)を取り出す
	   CFStringGetCString(strRef, menu_items[9][Device_num], sizeof(menu_items[9][Device_num]),kCFStringEncodingUTF8);
	   p6logd("Find MIDI in :%s\n",menu_items[9][Device_num]);
	   Device_num ++;
	  }
	 }
	CFRelease(strRef);
	strcpy(menu_items[9][Device_num],"\0"); // Menu END

	if(core_mid_num != 0){
	  *phmo = (HANDLE)mid_name; //MIDI Active!(ダミーを代入しておく)
	}

return core_mid_num;
}

/*-----------------------------------------
   set/change MIDI out Port 
------------------------------------------*/
void midOutChg(uint32_t port_no, uint32_t bank)
{
	OSStatus err_sts;
	HMIDIOUT hmo;

	/* All note off */
	for (uint32_t msg=0x7bb0; msg<0x7bc0; msg++) {
		midiOutShortMsg(hmo, msg);
	}

	/* CoreMIDI endpoint change */
	mid_endpoint = MIDIGetDestination(port_no);
	if (mid_endpoint == 0){
		p6logd("MIDI Change error.\n");
	}

	/* select BANK CC20 LSB */
	//uint32_t msg = ((bank << 16) | (0x20 << 8) | 0xb0);/*Bank select2*/
	//midiOutShortMsg(hmo, msg);

 return;
}
/*-----------------------------------------
   set/change MIDI in Port 
------------------------------------------*/
void midInChg(uint32_t port_no)
{
	OSStatus err_sts;

	/* CoreMIDI endpoint change */
	mid_source = MIDIGetSource(port_no);
	err_sts = MIDIPortConnectSource(mid_in_port, mid_source, NULL);
	if (err_sts != noErr){
		p6logd("MIDI in Change error.\n");
	}

 return;
}

/*-----------------------------------------
   MIDI Port close
------------------------------------------*/
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
	//err_sts = MIDIPortDispose(mid_in_port);
	//if (err_sts != noErr) p6logd("Dispose MIDI-in Port err\n");

	/* Dispose out endpoint */
	//err_sts = MIDIEndpointDispose(mid_endpoint);
	//if (err_sts != noErr) p6logd("Dispose MIDI-out Endpoint err\n");
	//err_sts = MIDIEndpointDispose(mid_source);
	//if (err_sts != noErr) p6logd("Dispose MIDI-in Endpoint err\n");

	/* Dispose Client */
	err_sts = MIDIClientDispose(mid_client);
	if (err_sts != noErr) p6logd("Dispose MIDI-Client err\n");

	return MMSYSERR_NOERROR;
}

/*-----------------------------------------
   Send Short Message (演奏データ送信)
------------------------------------------*/
uint32_t
midiOutShortMsg(HMIDIOUT hmo, uint32_t msg)
{
	(void)hmo;
	uint8_t messg[4];

	/* (uint32)msg を 4byte に分解 */
	messg[0] = (uint8_t)(msg & 0xff);
	msg>>=8;
	messg[1] = (uint8_t)(msg & 0xff);
	msg>>=8;
	messg[2] = (uint8_t)(msg & 0xff);

	/* initialize MIDIPacketList */
	mid_Packet = MIDIPacketListInit(packetList);

	/* length of msg */
	uint32_t len = 3;//note on/off/key press/cont.chg/pitch wheel chg
	if(((messg[0]&0xf0)==0xc0) || ((messg[0]&0xf0)==0xd0)){ len = 2; }//prog. chg / chnnel press ?
	if((messg[0]&0xf0)==0xf0){ len = 1; }

	/*(CoreMIDI) 電文とタイムスタンプ入れてPacketList更新 */
	MIDIPacketListAdd(packetList, (ByteCount)sizeof(packetBuf), mid_Packet,
				mach_absolute_time(), len, (uint8_t *)messg);

	/*(CoreMIDI) PacketList送信 */
	MIDISend(mid_out_port,mid_endpoint,packetList);

	return MMSYSERR_NOERROR;
}

/*-----------------------------------------
   Exclusive GO!  (設定データ送信)
------------------------------------------*/
uint32_t
midiOutLongMsg(HMIDIOUT hmo, LPMIDIHDR pmh, uint32_t cbmh)
{
	(void)hmo;
	(void)cbmh;

	if(pmh->dwBufferLength == 0){ //length check
	  return MMSYSERR_NOERROR;
	}

	/*(CoreMIDI) initialize MIDIPacketList */
	mid_Packet = MIDIPacketListInit(packetList);

	/*(CoreMIDI) 電文とタイムスタンプ入れてPacketList更新 */
	MIDIPacketListAdd(packetList, (ByteCount)sizeof(packetBuf), mid_Packet, 
				mach_absolute_time(), pmh->dwBufferLength, (uint8_t *)pmh->lpData);

	/*(CoreMIDI) PacketList送信 */
	MIDISend(mid_out_port,mid_endpoint,packetList);

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


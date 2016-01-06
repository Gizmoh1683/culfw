#include "board.h"
#ifdef HAS_W5500

#include "ethernet.h"
#include "wizchip_conf.h"
#include "DHCP/dhcp.h"
#include "socket.h"
#include "arch.h"
#include "display.h"
#include "registry.h"
#include "ttydata.h"
#include <avr/pgmspace.h>
#include <stdio.h>

#include <LUFA/Drivers/USB/USB.h>

#ifdef HAS_MQTT
#include "mqtt.h"
#endif

#ifdef HAS_PIGATOR
#include "pigator.h"
#include <LUFA/Drivers/Misc/RingBuffer.h>
RingBuffer_t PIMtoNET_Buffer;
uint8_t      PIMtoNET_Buffer_Data[USART_BUF_SIZE];

void toNETBuffer(uint8_t in) {
  if (RingBuffer_IsFull(&PIMtoNET_Buffer))
    return;
  
  RingBuffer_Insert(&PIMtoNET_Buffer, in);
}

#endif

//////////////////////////////////////////////////
// Socket & Port number definition for Examples //
//////////////////////////////////////////////////

#define SOCK_DHCP 6

////////////////////////////////////////////////
// Shared Buffer Definition for Loopback test //
////////////////////////////////////////////////
#define DATA_BUF_SIZE 2048
uint8_t gDATABUF[DATA_BUF_SIZE];

///////////////////////////
// Network Configuration //
///////////////////////////
wiz_NetInfo gWIZNETINFO = {
  .mac = {0xa4, 0x50, 0x55, 0xbb, 0xcd, 0x1f},
  .ip = {192, 168, 2, 168},
  .sn = {255, 255, 255, 0},
  .gw = {192, 168, 2, 1},
  .dns = {0, 0, 0, 0},
  .dhcp = NETINFO_DHCP
};

rb_t NET_Tx_Buffer;

uint8_t run_user_applications = 0;

void wizchip_select(void) {
  W5500_CS_PORT.OUTCLR = W5500_CS_PIN; 
}

void wizchip_deselect(void) {
  W5500_CS_PORT.OUTSET = W5500_CS_PIN; 
}

uint8_t wizchip_read() {
  return spi_send( 0xff );
}

void wizchip_write(uint8_t wb) {
  spi_send( wb );
}

void my_ip_assign(void) {
  getIPfromDHCP(gWIZNETINFO.ip);
  getGWfromDHCP(gWIZNETINFO.gw);
  getSNfromDHCP(gWIZNETINFO.sn);
  getDNSfromDHCP(gWIZNETINFO.dns);
  gWIZNETINFO.dhcp = NETINFO_DHCP;

  /* Network initialization */
  ctlnetwork(CN_SET_NETINFO, (void*) &gWIZNETINFO);

  printf("assigned IP from DHCP\r\n");
}

/************************************
 * @ brief Call back for ip Conflict
 ************************************/
void my_ip_conflict(void) {
  printf("CONFLICT IP from DHCP\r\n");
}

void ethernet_init(void) {
  uint8_t tmp;
  uint8_t memsize[2][8] = { { 2, 2, 2, 2, 2, 2, 2, 2 }, { 2, 2, 2, 2, 2, 2, 2, 2 } };
  
  wizchip_deselect();
  W5500_CS_PORT.DIRSET = W5500_CS_PIN; // CS

  W5500_RESET_PORT.DIRSET = W5500_RESET_PIN; // Reset chip
  W5500_RESET_PORT.OUTCLR = W5500_RESET_PIN; 
  my_delay_ms(10);
  W5500_RESET_PORT.OUTSET = W5500_RESET_PIN; 

  reg_wizchip_cs_cbfunc(wizchip_select, wizchip_deselect);
  reg_wizchip_spi_cbfunc(wizchip_read, wizchip_write);

  /* wizchip initialize*/
  if (ctlwizchip(CW_INIT_WIZCHIP, (void*) memsize) == -1) {
    DS_P(PSTR("WIZCHIP Initialized fail."));
  } else {
    DS_P(PSTR("WIZCHIP Initialized success."));
  }
  DNL();

  // prepare gWIZNETINFO from registry

  if (registry_get(REG_ETHERNET_MAC, &gWIZNETINFO.mac) != REG_STATUS_OK) {

    // read XMEGA serial and construct MAC address - if not stored in Registry already ...
    uint_reg_t CurrentGlobalInt = GetGlobalInterruptMask();
    GlobalInterruptDisable();
    
    uint8_t SigReadAddress = INTERNAL_SERIAL_START_ADDRESS;
    uint32_t SerialByte = 0;

    for (uint8_t SerialCharNum = 0; SerialCharNum < (INTERNAL_SERIAL_LENGTH_BITS / 8); SerialCharNum++)  {
      
      NVM.CMD     = NVM_CMD_READ_CALIB_ROW_gc;
      SerialByte += pgm_read_byte(SigReadAddress++);
      NVM.CMD     = 0;
      
    }
    
    SetGlobalInterruptMask(CurrentGlobalInt);

    // fixed to busware
    gWIZNETINFO.mac[0] = 0xa4;
    gWIZNETINFO.mac[1] = 0x50;
    gWIZNETINFO.mac[2] = 0x55;

    // variable
    gWIZNETINFO.mac[3] = (SerialByte>>16) & 0xff;
    gWIZNETINFO.mac[4] = (SerialByte>>8) & 0xff;
    gWIZNETINFO.mac[5] = SerialByte & 0xff;

    // store
    registry_set(REG_ETHERNET_MAC, &gWIZNETINFO.mac, 6);
  }
  
  registry_get(REG_ETHERNET_DHCP, &gWIZNETINFO.dhcp);

  if(gWIZNETINFO.dhcp != NETINFO_DHCP) {
    registry_get(REG_ETHERNET_IP4_ADDR, &gWIZNETINFO.ip);
    registry_get(REG_ETHERNET_IP4_GATEWAY, &gWIZNETINFO.gw);
    registry_get(REG_ETHERNET_IP4_NETMASK, &gWIZNETINFO.sn);
    registry_get(REG_ETHERNET_IP4_DNS, &gWIZNETINFO.dns);
  }

  // fix NetInfo and set
  ctlnetwork(CN_SET_NETINFO, (void*) &gWIZNETINFO);

  /* DHCP client Initialization */
  if(gWIZNETINFO.dhcp == NETINFO_DHCP) {
    DHCP_init(SOCK_DHCP, gDATABUF);
    // if you want different action instead default ip assign, update, conflict.
    // if cbfunc == 0, act as default.
    reg_dhcp_cbfunc(my_ip_assign, my_ip_assign, my_ip_conflict);
    run_user_applications = 0; // flag for running user's code
  } else {
    run_user_applications = 1; 
  }

#ifdef HAS_PIGATOR
  RingBuffer_InitBuffer(&PIMtoNET_Buffer, PIMtoNET_Buffer_Data, USART_BUF_SIZE);
#endif

}

void ethernet_func(char *in) {
  uint8_t x;
  uint8_t tmpstr[6] = {0,};
  
  if(in[1] == 'i') {
    ethernet_init();
    return;
  }

  if (in[1] == 'p') {
    if (ctlwizchip(CW_GET_PHYLINK, (void*) &x) == -1) {
      DS_P(PSTR("Unknown PHY Link stauts.\r\n"));
      return;
    }

    DS_P(PSTR("PHY Link status: "));

    if (x == PHY_LINK_OFF)
      DS_P(PSTR("OFF"));

    if (x == PHY_LINK_ON)
      DS_P(PSTR("ON"));
    
    DNL();
    return;
  }

  if (in[1] == 'n') {
    ctlnetwork(CN_GET_NETINFO, (void*) &gWIZNETINFO);
    // Display Network Information
    ctlwizchip(CW_GET_ID,(void*)tmpstr);
    if(gWIZNETINFO.dhcp == NETINFO_DHCP) printf("\r\n===== %s NET CONF : DHCP =====\r\n",(char*)tmpstr);
    else printf("\r\n===== %s NET CONF : Static =====\r\n",(char*)tmpstr);
    printf(" MAC : %02X:%02X:%02X:%02X:%02X:%02X\r\n", gWIZNETINFO.mac[0], gWIZNETINFO.mac[1], gWIZNETINFO.mac[2], gWIZNETINFO.mac[3], gWIZNETINFO.mac[4], gWIZNETINFO.mac[5]);
    printf(" IP : %d.%d.%d.%d\r\n", gWIZNETINFO.ip[0], gWIZNETINFO.ip[1], gWIZNETINFO.ip[2], gWIZNETINFO.ip[3]);
    printf(" GW : %d.%d.%d.%d\r\n", gWIZNETINFO.gw[0], gWIZNETINFO.gw[1], gWIZNETINFO.gw[2], gWIZNETINFO.gw[3]);
    printf(" SN : %d.%d.%d.%d\r\n", gWIZNETINFO.sn[0], gWIZNETINFO.sn[1], gWIZNETINFO.sn[2], gWIZNETINFO.sn[3]);

  }

#ifdef HAS_MQTT

  if (in[1] == 'q') {
    mqtt_init();
  }

  if (in[1] == 'Q') {
    mqtt_publish( "Moin", "Cool", 0, 0 );
  }

  if (in[1] == 'b')
    mqtt_subscribe("busware", 0);
  
#endif

}

int32_t rxtx_0() {
  int32_t ret;
  uint16_t size = 0, sentsize=0;

  if((size = getSn_RX_RSR(0)) > 0) {
    sentsize = TTY_BUFSIZE - TTY_Rx_Buffer.nbytes;
    if(size > sentsize) size = sentsize;
    ret = recv(0, gDATABUF, size);
    if(ret <= 0) return ret;

    sentsize = 0;
    while(size != sentsize)
      rb_put(&TTY_Rx_Buffer, gDATABUF[sentsize++]);

    input_handle_func(DISPLAY_TCP);
  }
    
  size = 0;
  while (NET_Tx_Buffer.nbytes) {
    gDATABUF[size++] = rb_get(&NET_Tx_Buffer);
  }

  sentsize = 0;
  while(size != sentsize) {
    ret = send(0, gDATABUF+sentsize,size-sentsize);
    if(ret < 0) {
      close(0);
      return ret;
    }
    sentsize += ret; // Don't care SOCKERR_BUSY, because it is zero.
  }

  return 1;
}

int32_t rxtx_1() {
#ifdef HAS_PIGATOR
  int32_t ret;
  uint16_t size = 0, sentsize=0;
  
  if((size = getSn_RX_RSR(1)) > 0) {

    sentsize = USART_BUF_SIZE;
    if(size > sentsize) size = sentsize;
    ret = recv(1, gDATABUF, size);
    if(ret <= 0) return ret;

    sentsize = 0;
    while(size != sentsize)
      toPIMBuffer(gDATABUF[sentsize++]);
    
  }

  sentsize = RingBuffer_GetCount(&PIMtoNET_Buffer);
  size = 0;
  while (sentsize--)
      gDATABUF[size++] = RingBuffer_Remove(&PIMtoNET_Buffer);
  
  sentsize = 0;
  while(size != sentsize) {
    ret = send(1, gDATABUF+sentsize,size-sentsize);
    if(ret < 0) {
      close(1);
      return ret;
    }
    sentsize += ret; // Don't care SOCKERR_BUSY, because it is zero.
  }

  return 1;
#else 
  return 1;
#endif
}

// TCP Server - does keep the sockets listening
//
int32_t tcp_server(uint8_t sn, uint16_t port) {
  int32_t ret;
  uint16_t size = 0, sentsize=0;
#ifdef _LOOPBACK_DEBUG_
  uint8_t destip[4];
  uint16_t destport;
#endif
  switch(getSn_SR(sn)) {
  case SOCK_ESTABLISHED :
    if(getSn_IR(sn) & Sn_IR_CON) {
#ifdef _LOOPBACK_DEBUG_
      getSn_DIPR(sn, destip);
      destport = getSn_DPORT(sn);
      printf("%d:Connected - %d.%d.%d.%d : %d\r\n",sn, destip[0], destip[1], destip[2], destip[3], destport);
#endif
      setSn_IR(sn,Sn_IR_CON);
    }

    if (sn == 0)
      return rxtx_0();

    if (sn == 1)
      return rxtx_1();
    
    break;
  case SOCK_CLOSE_WAIT :
#ifdef _LOOPBACK_DEBUG_
    printf("%d:CloseWait\r\n",sn);
#endif
    if((ret=disconnect(sn)) != SOCK_OK) return ret;
#ifdef _LOOPBACK_DEBUG_
    printf("%d:Socket closed\r\n",sn);
#endif
    break;
  case SOCK_INIT :
#ifdef _LOOPBACK_DEBUG_
    printf("%d:Listen, TCP server, port [%d]\r\n",sn, port);
#endif
    if( (ret = listen(sn)) != SOCK_OK) return ret;
    break;
  case SOCK_CLOSED:
#ifdef _LOOPBACK_DEBUG_
    printf("%d:TCP server start\r\n",sn);
#endif
    if((ret=socket(sn, Sn_MR_TCP, port, 0x00)) != sn)
      //if((ret=socket(sn, Sn_MR_TCP, port, Sn_MR_ND)) != sn)
      return ret;
#ifdef _LOOPBACK_DEBUG_
    printf("%d:Socket opened\r\n",sn);
#endif
    break;
  default:
    break;
  }

  return 1;
}

void ethernet_task(void) {

  if(gWIZNETINFO.dhcp == NETINFO_DHCP) {
    // make and keep DHCP happy ...
    switch(DHCP_run()) {
    case DHCP_IP_ASSIGN:
    case DHCP_IP_CHANGED:
      /* If this block empty, act with default_ip_assign & default_ip_update */
      //
      // This example calls my_ip_assign in the two case.
      //
      // Add to ...
      //
      break;
    case DHCP_IP_LEASED:
      //
      // TODO: insert user's code here
      run_user_applications = 1;
      //
      break;
    case DHCP_FAILED:
      /* ===== Example pseudo code ===== */
      // The below code can be replaced your code or omitted.
      // if omitted, retry to process DHCP
      /*
      my_dhcp_retry++;
      if(my_dhcp_retry > MY_MAX_DHCP_RETRY) {
	gWIZNETINFO.dhcp = NETINFO_STATIC;
	DHCP_stop(); // if restart, recall DHCP_init()
	printf(">> DHCP %d Failed\r\n", my_dhcp_retry);
	Net_Conf();
	Display_Net_Conf(); // print out static netinfo to serial
	my_dhcp_retry = 0;
      }
      */
      break;
      /*
    case DHCP_RUNNING:
      DC('R');
      break;
    case DHCP_STOPPED:
      DC('S');
      break;
      */
    default:
      break;
    }
  }

  if (!run_user_applications)
    return;

  tcp_server( 0, 2323 );
#ifdef HAS_PIGATOR
  tcp_server( 1, 2324 );
#endif
}
	

#endif


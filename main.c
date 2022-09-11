/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "dhserver.h"
#include "dnserver.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"

#include "bsp/board.h"
#include "tusb.h"
#include "usb_descriptors.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/apps/lwiperf.h"

#if TU_CHECK_MCU(ESP32S2) || TU_CHECK_MCU(ESP32S3)
// ESP-IDF need "freertos/" prefix in include path.
// CFG_TUSB_OS_INC_PATH should be defined accordingly.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#else
#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include "task.h"
#include "timers.h"
#endif

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+

/* Blink pattern
 * - 250 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum
{
  BLINK_NOT_MOUNTED = 250,
  BLINK_MOUNTED = 1000,
  BLINK_SUSPENDED = 2500,
};

// static timer
StaticTimer_t blinky_tmdef;
TimerHandle_t blinky_tm;

// static task for usbd
#if CFG_TUSB_DEBUG
#define USBD_STACK_SIZE (3 * configMINIMAL_STACK_SIZE)
#else
#define USBD_STACK_SIZE (3 * configMINIMAL_STACK_SIZE / 2)
#endif

StackType_t usb_device_stack[USBD_STACK_SIZE];
StaticTask_t usb_device_taskdef;

// static task for hid
#define HID_STACK_SZIE configMINIMAL_STACK_SIZE
StackType_t hid_stack[HID_STACK_SZIE];
StaticTask_t hid_taskdef;

void led_blinky_cb(TimerHandle_t xTimer);
void usb_device_task(void *param);
void hid_task(void *params);

/* lwip context */
static struct netif netif_data;

/* shared between tud_network_recv_cb() and service_traffic() */
static struct pbuf *received_frame;

/* this is used by this code, ./class/net/net_driver.c, and usb_descriptors.c */
/* ideally speaking, this should be generated from the hardware's unique ID (if available) */
/* it is suggested that the first byte is 0x02 to indicate a link-local address */
const uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};

/* network parameters of this MCU */
static const ip_addr_t ipaddr = IPADDR4_INIT_BYTES(192, 168, 7, 1);
static const ip_addr_t netmask = IPADDR4_INIT_BYTES(255, 255, 255, 0);
static const ip_addr_t gateway = IPADDR4_INIT_BYTES(0, 0, 0, 0);

/* database IP addresses that can be offered to the host; this must be in RAM to store assigned MAC addresses */
static dhcp_entry_t entries[] =
    {
        /* mac ip address                          lease time */
        {{0}, IPADDR4_INIT_BYTES(192, 168, 7, 2), 24 * 60 * 60},
        {{0}, IPADDR4_INIT_BYTES(192, 168, 7, 3), 24 * 60 * 60},
        {{0}, IPADDR4_INIT_BYTES(192, 168, 7, 4), 24 * 60 * 60},
};

static const dhcp_config_t dhcp_config =
    {
        .router = IPADDR4_INIT_BYTES(0, 0, 0, 0),  /* router address (if any) */
        .port = 67,                                /* listen port */
        .dns = IPADDR4_INIT_BYTES(192, 168, 7, 1), /* dns server (if any) */
        "usb",                                     /* dns suffix */
        TU_ARRAY_SIZE(entries),                    /* num entry */
        entries                                    /* entries */
};
static err_t linkoutput_fn(struct netif *netif, struct pbuf *p)
{
  (void)netif;

  for (;;)
  {
    /* if TinyUSB isn't ready, we must signal back to lwip that there is nothing we can do */
    if (!tud_ready())
      return ERR_USE;

    /* if the network driver can accept another packet, we make it happen */
    if (tud_network_can_xmit(p->tot_len))
    {
      tud_network_xmit(p, 0 /* unused for this example */);
      return ERR_OK;
    }

    /* transfer execution to TinyUSB in the hopes that it will finish transmitting the prior packet */
    tud_task();
  }
}

static err_t output_fn(struct netif *netif, struct pbuf *p, const ip_addr_t *addr)
{
  return etharp_output(netif, p, addr);
}

static err_t netif_init_cb(struct netif *netif)
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));
  netif->mtu = CFG_TUD_NET_MTU;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
  netif->state = NULL;
  netif->name[0] = 'E';
  netif->name[1] = 'X';
  netif->linkoutput = linkoutput_fn;
  netif->output = output_fn;
  return ERR_OK;
}

static void init_lwip(void)
{
  struct netif *netif = &netif_data;

  lwip_init();

  /* the lwip virtual MAC address must be different from the host's; to ensure this, we toggle the LSbit */
  netif->hwaddr_len = sizeof(tud_network_mac_address);
  memcpy(netif->hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
  netif->hwaddr[5] ^= 0x01;

  netif = netif_add(netif, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ip_input);
  netif_set_default(netif);
}

/* handle any DNS requests from dns-server */
bool dns_query_proc(const char *name, ip_addr_t *addr)
{
  if (0 == strcmp(name, "tiny.usb"))
  {
    *addr = ipaddr;
    return true;
  }
  return false;
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
  /* this shouldn't happen, but if we get another packet before
  parsing the previous, we must signal our inability to accept it */
  if (received_frame)
    return false;

  if (size)
  {
    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);

    if (p)
    {
      /* pbuf_alloc() has already initialized struct; all we need to do is copy the data */
      memcpy(p->payload, src, size);

      /* store away the pointer for service_traffic() to later handle */
      received_frame = p;
    }
  }

  return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
  struct pbuf *p = (struct pbuf *)ref;

  (void)arg; /* unused for this example */

  return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void service_traffic(void)
{
  /* handle any packet received by tud_network_recv_cb() */
  if (received_frame)
  {
    ethernet_input(received_frame, &netif_data);
    pbuf_free(received_frame);
    received_frame = NULL;
    tud_network_recv_renew();
  }

  sys_check_timeouts();
}

void tud_network_init_cb(void)
{
  /* if the network is re-initializing and we have a leftover packet, we must do a cleanup */
  if (received_frame)
  {
    pbuf_free(received_frame);
    received_frame = NULL;
  }
}
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/apps/lwiperf.h"
// Report IP results and exit
static void iperf_report(void *arg, enum lwiperf_report_type report_type,
                         const ip_addr_t *local_addr, u16_t local_port, const ip_addr_t *remote_addr, u16_t remote_port,
                         u32_t bytes_transferred, u32_t ms_duration, u32_t bandwidth_kbitpsec)
{
  static uint32_t total_iperf_megabytes = 0;
  uint32_t mbytes = bytes_transferred / 1024 / 1024;
  float mbits = bandwidth_kbitpsec / 1000.0;

  total_iperf_megabytes += mbytes;

  printf("Completed iperf transfer of %d MBytes @ %.1f Mbits/sec\n", mbytes, mbits);
  printf("Total iperf megabytes since start %d Mbytes\n", total_iperf_megabytes);
#if CYW43_USE_STATS
  printf("packets in %u packets out %u\n", CYW43_STAT_GET(PACKET_IN_COUNT), CYW43_STAT_GET(PACKET_OUT_COUNT));
#endif
}

//--------------------------------------------------------------------+
// Main
//--------------------------------------------------------------------+

int main(void)
{
  board_init();
  // soft timer for blinky
  blinky_tm = xTimerCreateStatic(NULL, pdMS_TO_TICKS(BLINK_NOT_MOUNTED), true, NULL, led_blinky_cb, &blinky_tmdef);
  xTimerStart(blinky_tm, 0);

  // Create a task for tinyusb device stack
  (void)xTaskCreateStatic(usb_device_task, "usbd", USBD_STACK_SIZE, NULL, configMAX_PRIORITIES - 1, usb_device_stack, &usb_device_taskdef);

  // Create HID task
  (void)xTaskCreateStatic(hid_task, "hid", HID_STACK_SZIE, NULL, configMAX_PRIORITIES - 2, hid_stack, &hid_taskdef);

  // skip starting scheduler (and return) for ESP32-S2 or ESP32-S3
#if !(TU_CHECK_MCU(ESP32S2) || TU_CHECK_MCU(ESP32S3))
  vTaskStartScheduler();
#endif

  return 0;
}

#if CFG_TUSB_MCU == OPT_MCU_ESP32S2 || CFG_TUSB_MCU == OPT_MCU_ESP32S3
void app_main(void)
{
  main();
}
#endif

// USB Device Driver task
// This top level thread process all usb events and invoke callbacks
void usb_device_task(void *param)
{
  (void)param;

  // This should be called after scheduler/kernel is started.
  // Otherwise it could cause kernel issue since USB IRQ handler does use RTOS queue API.
  tusb_init();
  init_lwip();
  while (!netif_is_up(&netif_data))
    ;
  while (dhserv_init(&dhcp_config) != ERR_OK)
    ;
  while (dnserv_init(&ipaddr, 53, dns_query_proc) != ERR_OK)
    ;
  // httpd_init();
  lwiperf_start_tcp_server_default(&iperf_report, NULL);
  // RTOS forever loop
  while (1)
  {
    // tinyusb device task
    tud_task();
    service_traffic();
  }
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void)
{
  xTimerChangePeriod(blinky_tm, pdMS_TO_TICKS(BLINK_MOUNTED), 0);
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
  xTimerChangePeriod(blinky_tm, pdMS_TO_TICKS(BLINK_NOT_MOUNTED), 0);
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
  (void)remote_wakeup_en;
  xTimerChangePeriod(blinky_tm, pdMS_TO_TICKS(BLINK_SUSPENDED), 0);
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
  xTimerChangePeriod(blinky_tm, pdMS_TO_TICKS(BLINK_MOUNTED), 0);
}

void hid_task(void *param)
{
  (void)param;

  while (1)
  {
    // Poll every 10ms
    vTaskDelay(pdMS_TO_TICKS(10));

    uint32_t const btn = board_button_read();

    // Remote wakeup
    if (tud_suspended() && btn)
    {
      // Wake up host if we are in suspend mode
      // and REMOTE_WAKEUP feature is enabled by host
      tud_remote_wakeup();
    }
    else
    {
      // Send the 1st of report chain, the rest will be sent by tud_hid_report_complete_cb()
      // send_hid_report(REPORT_ID_KEYBOARD, btn);
    }
  }
}

//--------------------------------------------------------------------+
// BLINKING TASK
//--------------------------------------------------------------------+
void led_blinky_cb(TimerHandle_t xTimer)
{
  (void)xTimer;
  static bool led_state = false;

  board_led_write(led_state);
  led_state = 1 - led_state; // toggle
}

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

#include "pio_xfer.h"
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
TaskHandle_t hid_stack[HID_STACK_SZIE];
TaskHandle_t hid_taskdef;
TaskHandle_t traffic_taskdef;

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
    {
      printf("%s,%d\n",__func__,__LINE__);
      return ERR_USE;
    }
      

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
  {
    printf("recv bug in usb recv_cb");
    return false;
  }

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

//--------------------------------------------------------------------+
// Main
//--------------------------------------------------------------------+
void traffic_task(void *param)
{
while (1)
{
  service_traffic();
}

}
// FreeRTOS includes
#include "FreeRTOS.h"
#include "timers.h"
#include "semphr.h"
int main(void)
{
  board_init();
  printf("app start\n");

  // soft timer for blinky
  blinky_tm = xTimerCreate(NULL, pdMS_TO_TICKS(BLINK_NOT_MOUNTED), true, NULL, led_blinky_cb);
  xTimerStart(blinky_tm, 0);
  TaskHandle_t rtos_task;
  TaskHandle_t rtos_task1;
  // Create a task for tinyusb device stack
  (void)xTaskCreate(usb_device_task, "usbd", USBD_STACK_SIZE, NULL, 0, &usb_device_taskdef);
  // xTaskCreate()
  //  Create HID task
  (void)xTaskCreate(hid_task, "hid", HID_STACK_SZIE, NULL, 5, &hid_taskdef);
  //(void)xTaskCreate(traffic_task, "traffic_task", HID_STACK_SZIE, NULL, 0, &traffic_taskdef);
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
#include <fcntl.h>
#include <sys/types.h>
#include "lwip/sockets.h"
int tcp_app()
{
  int i;
  int s;
  int c;
  int port = 2542;
  struct sockaddr_in address;
  s = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
  {
    perror("socket");
    return 1;
  }
  i = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &i, sizeof i);
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);
  address.sin_family = AF_INET;
  if (bind(s, (struct sockaddr *)&address, sizeof(address)) < 0)
  {
    perror("bind");
    return 1;
  }
  printf("%s,%d\n", __func__, __LINE__);
  if (listen(s, 1) < 0)
  {
    perror("listen");
    return 1;
  }

  fd_set conn;
  int maxfd = 0;
  FD_ZERO(&conn);
  FD_SET(s, &conn);
  maxfd = s;
  return 0;
}
int tcp_app_runloop()
{
  return 0;
}
static int sread(int fd, void *target, int len)
{
  unsigned char *t = target;
  while (len)
  {
    int r = read(fd, t, len);
    if (r <= 0)
    {
      return r;
    }
      
    t += r;
    len -= r;
  }
  return 1;
}
unsigned char buffer[2048 + 1024], result[1024 + 1024];
int handle_data(int fd, void *ptr)
{

  const char xvcInfo[] = "xvcServer_v1.0:2048\n";
  do
  {
    char cmd[16];
    
    memset(cmd, 0, 16);
    if (sread(fd, cmd, 2) != 1)
      return 1;
    if (memcmp(cmd, "ge", 2) == 0)
    {
      if (sread(fd, cmd, 6) != 1)
        return 1;
      memcpy(result, xvcInfo, strlen(xvcInfo));
      if (write(fd, result, strlen(xvcInfo)) != strlen(xvcInfo))
      {
        perror("write");
        return 1;
      }
      break;
    }
    else if (memcmp(cmd, "se", 2) == 0)
    {
      if (sread(fd, cmd, 9) != 1)
        return 1;
      memcpy(result, cmd + 5, 4);
      if (write(fd, result, 4) != 4)
      {
        perror("write");
        return 1;
      }
      break;
    }
    else if (memcmp(cmd, "sh", 2) == 0)
    {
      if (sread(fd, cmd, 4) != 1)
        return 1;
    }
    else
    {
      fprintf(stderr, "invalid cmd '%x %x'\n", cmd[0], cmd[1]);
      return 1;
    }
    // shift 4 word | len 4 word | nr_bytes * 2 tms and tdi
    int len;
    if (sread(fd, &len, 4) != 1)
    {
      fprintf(stderr, "reading length failed\n");
      return 1;
    }

    int nr_bytes = (len + 7) / 8;
    if (nr_bytes * 2 > sizeof(buffer))
    {
      fprintf(stderr, "buffer size exceeded\n");
      return 1;
    }
    if (sread(fd, buffer, nr_bytes * 2) != 1)
    {
      fprintf(stderr, "reading data failed\n");
      return 1;
    }
    //memset(result, 0, nr_bytes);
    int bytesLeft = nr_bytes;
    int bitsLeft = len;
    int byteIndex = 0;
    int tdi, tms, tdo;
    unsigned char *tms_ptr = malloc(nr_bytes + 8);
    memcpy(tms_ptr, buffer + nr_bytes, nr_bytes);
    unsigned char *tdi_ptr = malloc(nr_bytes + 8);
    memcpy(tdi_ptr, buffer, nr_bytes);
    pio_xfer_rw(tms_ptr, buffer, &result[byteIndex], len);
    free(tms_ptr);
    free(tdi_ptr);
#if 0
    while (bytesLeft > 0)
    {
      tms = 0;
      tdi = 0;
      tdo = 0;
      if (bytesLeft >= 4)
      {
        memcpy(&tms, &buffer[byteIndex], 4);
        memcpy(&tdi, &buffer[byteIndex + nr_bytes], 4);
        /*
        ptr->length_offset = 32;
        dsb(st);
        ptr->tms_offset = tms;
        dsb(st);
        ptr->tdi_offset = tdi;
        dsb(st);
        ptr->ctrl_offset = 0x01;

        while (ptr->ctrl_offset)
        {
        }

        tdo = ptr->tdo_offset;
        */
            memcpy(&result[byteIndex], &tdo, 4);

        bytesLeft -= 4;
        bitsLeft -= 32;
        byteIndex += 4;
      }
      else
      {
        memcpy(&tms, &buffer[byteIndex], bytesLeft);
        memcpy(&tdi, &buffer[byteIndex + nr_bytes], bytesLeft);
        /*
                ptr->length_offset = bitsLeft;
                dsb(st);
                ptr->tms_offset = tms;
                dsb(st);
                ptr->tdi_offset = tdi;
                dsb(st);
                ptr->ctrl_offset = 0x01;
                while (ptr->ctrl_offset)
                {
                }

                tdo = ptr->tdo_offset;
        */
        memcpy(&result[byteIndex], &tdo, bytesLeft);

        break;
      }
    }
#endif
    if (write(fd, result, nr_bytes) != nr_bytes)
    {
      perror("write");
      return 1;
    }

  } while (1);
  /* Note: Need to fix JTAG state updates, until then no exit is allowed */
  return 0;
}

/* This function initializes this lwIP test. When NO_SYS=1, this is done in
 * the main_loop context (there is no other one), when NO_SYS=0, this is done
 * in the tcpip_thread context */
static void
test_init(void *arg)
{ /* remove compiler warning */
#if NO_SYS
  LWIP_UNUSED_ARG(arg);
#else  /* NO_SYS */
  sys_sem_t *init_sem;
  LWIP_ASSERT("arg != NULL", arg != NULL);
  init_sem = (sys_sem_t *)arg;
#endif /* NO_SYS */

  /* init randomizer again (seed per thread) */
  srand((unsigned int)time(NULL));
  printf("task %s,%d\n", __func__, __LINE__);
  /* init network interfaces */
  // test_netif_init();

  struct netif *netif = &netif_data;
  /* the lwip virtual MAC address must be different from the host's; to ensure this, we toggle the LSbit */
  netif->hwaddr_len = sizeof(tud_network_mac_address);
  memcpy(netif->hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
  netif->hwaddr[5] ^= 0x01;
  netif = netif_add(netif, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ip_input);
  netif_set_default(netif);

  /* init apps */
  // apps_init();

#if !NO_SYS
  printf("task %s,%d\n", __func__, __LINE__);
  sys_sem_signal(init_sem);
  printf("task %s,%d\n", __func__, __LINE__);
#endif /* !NO_SYS */
}

// USB Device Driver task
// This top level thread process all usb events and invoke callbacks
void usb_device_task(void *param)
{
  (void)param;

  // This should be called after scheduler/kernel is started.
  // Otherwise it could cause kernel issue since USB IRQ handler does use RTOS queue API.
  tusb_init();

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
  printf("%s,%d\n", __func__, __LINE__);
  err_t err;
  sys_sem_t init_sem;
  err = sys_sem_new(&init_sem, 0);
  tcpip_init(test_init, &init_sem);
  /* we have to wait for initialization to finish before
   * calling update_adapter()! */
  sys_sem_wait(&init_sem);
  sys_sem_free(&init_sem);

  while (!netif_is_up(&netif_data))
    ;
  while (dhserv_init(&dhcp_config) != ERR_OK)
    ;
  while (dnserv_init(&ipaddr, 53, dns_query_proc) != ERR_OK)
    ;

  // tcp_app();

  int i;
  int s;
  int c;
  int port = 2542;
  struct sockaddr_in address;
  s = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
  {
    perror("socket");
    return 1;
  }
  i = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &i, sizeof i);
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);
  address.sin_family = AF_INET;
  if (bind(s, (struct sockaddr *)&address, sizeof(address)) < 0)
  {
    perror("bind");
    return 1;
  }
  printf("%s,%d\n", __func__, __LINE__);
  if (listen(s, 1) < 0)
  {
    perror("listen");
    return 1;
  }

  fd_set conn;
  int maxfd = 0;
  FD_ZERO(&conn);
  FD_SET(s, &conn);
  maxfd = s;
  pio_xfer_init();
  printf("%s,%d\n", __func__, __LINE__);
  while (1)
  {
    fd_set read = conn, except = conn;
    int fd;
    printf("%s,%d\n",__func__,__LINE__);
    if (select(maxfd + 1, &read, 0, &except, 0) < 0)
    {
      printf("%s,%d\n",__func__,__LINE__);
      perror("select");
      break;
    }
    for (fd = 0; fd <= maxfd; ++fd)
    {
      printf("%s,%d\n",__func__,__LINE__);
      if (FD_ISSET(fd, &read))
      {
        if (fd == s)
        {
          printf("%s,%d\n",__func__,__LINE__);
          int newfd;
          socklen_t nsize = sizeof(address);

          newfd = accept(s, (struct sockaddr *)&address, &nsize);

          //               if (verbose)
          printf("connection accepted - fd %d\n", newfd);
          if (newfd < 0)
          {
            perror("accept");
          }
          else
          {
            printf("setting TCP_NODELAY to 1\n");
            int flag = 1;
            int optResult = setsockopt(newfd,
                                       IPPROTO_TCP,
                                       TCP_NODELAY,
                                       (char *)&flag,
                                       sizeof(int));
            if (optResult < 0)
              perror("TCP_NODELAY error");
            if (newfd > maxfd)
            {
              maxfd = newfd;
            }
            FD_SET(newfd, &conn);
          }
        }
        else if (handle_data(fd, NULL))
        {
          close(fd);
          FD_CLR(fd, &conn);
        }
      }
      else if (FD_ISSET(fd, &except))
      {
        printf("%s,%d\n",__func__,__LINE__);
        close(fd);
        FD_CLR(fd, &conn);
        if (fd == s)
          break;
      }
    }
  }

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
  // printf("task led\n");
}

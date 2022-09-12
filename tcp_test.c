/**
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "pio_xfer.h"
#include "tusb.h"
#include "pico/multicore.h"
#include "hardware/irq.h"
#define TCP_PORT 2542
#define DEBUG_printf printf
#define BUF_SIZE 2048
#define TEST_ITERATIONS 10
#define POLL_TIME_S 3

typedef struct TCP_SERVER_T_
{
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
    bool complete;
    uint8_t buffer_sent[BUF_SIZE];
    uint8_t buffer_recv[BUF_SIZE];
    int sent_len;
    int recv_len;
    int run_count;
} TCP_SERVER_T;

static TCP_SERVER_T *tcp_server_init(void)
{
    TCP_SERVER_T *state = calloc(1, sizeof(TCP_SERVER_T));
    if (!state)
    {
        DEBUG_printf("failed to allocate state\n");
        return NULL;
    }
    return state;
}

static err_t tcp_server_close(void *arg)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    err_t err = ERR_OK;
    if (state->client_pcb != NULL)
    {
        tcp_arg(state->client_pcb, NULL);
        tcp_poll(state->client_pcb, NULL, 0);
        tcp_sent(state->client_pcb, NULL);
        tcp_recv(state->client_pcb, NULL);
        tcp_err(state->client_pcb, NULL);
        err = tcp_close(state->client_pcb);
        if (err != ERR_OK)
        {
            DEBUG_printf("close failed %d, calling abort\n", err);
            tcp_abort(state->client_pcb);
            err = ERR_ABRT;
        }
        state->client_pcb = NULL;
    }
    else if (state->server_pcb)
    {
        tcp_arg(state->server_pcb, NULL);
        tcp_close(state->server_pcb);
        state->server_pcb = NULL;
    }
    return err;
}

static err_t tcp_server_result(void *arg, int status)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    if (status == 0)
    {
        DEBUG_printf("test success\n");
    }
    else
    {
        DEBUG_printf("test failed %d\n", status);
    }
    state->complete = true;
    return tcp_server_close(arg);
}

static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    DEBUG_printf("tcp_server_sent %u\n", len);
    state->sent_len += len;

    if (state->sent_len >= BUF_SIZE)
    {

        // We should get the data back from the client
        state->recv_len = 0;
        DEBUG_printf("Waiting for buffer from client\n");
    }

    return ERR_OK;
}

err_t tcp_server_send_data(void *arg, struct tcp_pcb *tpcb)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    for (int i = 0; i < BUF_SIZE; i++)
    {
        state->buffer_sent[i] = rand();
    }

    state->sent_len = 0;
    DEBUG_printf("Writing %ld bytes to client\n", BUF_SIZE);
    // this method is callback from lwIP, so cyw43_arch_lwip_begin is not required, however you
    // can use this method to cause an assertion in debug mode, if this method is called when
    // cyw43_arch_lwip_begin IS needed
    // cyw43_arch_lwip_check();
    err_t err = tcp_write(tpcb, state->buffer_sent, BUF_SIZE / 16, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
    {
        DEBUG_printf("Failed to write data %d\n", err);
        return tcp_server_result(arg, -1);
    }
    return ERR_OK;
}
typedef struct tcp_test_data_s
{
    struct tcp_pcb *tpcb;
    unsigned char *data;
    int len;
} tcp_test_data;
tcp_test_data *data_buffer = NULL;

typedef struct tcp_test_fifo
{
    int len;
    unsigned char *data;
    struct tcp_test_fifo *next;
} tcp_test_fifo_t;
tcp_test_fifo_t *fifo_data = NULL;
int flag = 0;
int fifo_push(unsigned char *data, int len)
{
    while (flag)
    {
        sleep_us(100);
    }
    flag = 1;
    if (fifo_data == NULL)
    {
        fifo_data = malloc(sizeof(tcp_test_fifo_t));
        fifo_data->next = NULL;
        fifo_data->data = data;
        fifo_data->len = len;
    }
    else
    {
        tcp_test_fifo_t *tmp = fifo_data;
        while (tmp->next != NULL)
            tmp = tmp->next;
        tmp->next = malloc(sizeof(tcp_test_fifo_t));
        tmp = tmp->next;
        tmp->next = NULL;
        tmp->data = data;
        tmp->len = len;
    }
    flag = 0;
    return 0;
}
tcp_test_fifo_t *fifo_get()
{
    while (flag)
    {
        sleep_us(100);
    }
    flag = 1;
    if (fifo_data)
    {
        tcp_test_fifo_t *tmp = fifo_data;
        fifo_data = fifo_data->next;
        flag = 0;
        return tmp;
    }
    else
        flag = 0;
    return NULL;
}
int len = 0;
int flag_len = 0;
char *buf_tmp = NULL;
int shift_handle(struct tcp_pcb *tpcb, unsigned char *buf)
{
    int len = *(int *)(buf + 6);
    int nrbits = (len + 7) / 8;
    uint8_t *buf_read = malloc(nrbits + 8);
    uint8_t *data_tms = malloc(nrbits + 8);
    uint8_t *data_tdata = malloc(nrbits + 8);

    memcpy(data_tms, buf + 10, nrbits);
    memcpy(data_tdata, buf + 10 + nrbits, nrbits);
    pio_xfer_rw(data_tdata, data_tms, buf_read, len);

    free(data_tms);
    free(data_tdata);
    tcp_write(tpcb, buf_read, nrbits, TCP_WRITE_FLAG_COPY);
    free(buf_read);
    return nrbits * 2 + 10;
}
err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    printf("%s,%d\n",__func__,__LINE__);
    unsigned char *buf;
    int buf_left = p->tot_len;
    buf = malloc(buf_left + ((buf_left + 3) % 4) / 4);
    int ret = 0, tmp_count = p->tot_len;
    int len = 0;
    int nrbits = 0;
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    if (!p)
    {
        return tcp_server_result(arg, -1);
    }
#define TEST_XFER_LEN 1024
    // this method is callback from lwIP, so cyw43_arch_lwip_begin is not required, however you
    // can use this method to cause an assertion in debug mode, if this method is called when
    // cyw43_arch_lwip_begin IS needed
    // cyw43_arch_lwip_check();
Regain:

    if (tmp_count)
    {
        DEBUG_printf("tcp_server_recv %d/%d err %d\n", p->tot_len, state->recv_len, err);

        // Receive the buffer

        ret = pbuf_copy_partial(p, buf, p->tot_len, 0);
        tmp_count -= ret;
        if (!ret)
        {
            pbuf_free(p);
            return 0;
        }
        tcp_recved(tpcb, ret);
    }
    else
    {
        pbuf_free(p);
        return 0;
    }

    const char xvcInfo[] = "xvcServer_v1.0:1024\n";
    err_t err_w = 0;
    tcp_write(tpcb, buf, p->tot_len, TCP_WRITE_FLAG_MORE);
    if(0)
    if (memcmp(buf, "ge", 2) == 0)
    {
        tcp_write(tpcb, xvcInfo, strlen(xvcInfo), TCP_WRITE_FLAG_COPY);
        if (err_w != ERR_OK)
        {
            DEBUG_printf("Failed to write data %d\n", err);
            return tcp_server_result(arg, -1);
        }
        free(buf);
    }
    else if (memcmp(buf, "se", 2) == 0)
    {
        tcp_write(tpcb, buf + 7, 4, TCP_WRITE_FLAG_COPY);
        if (err_w != ERR_OK)
        {
            DEBUG_printf("Failed to write data %d\n", err);
            return tcp_server_result(arg, -1);
        }
        free(buf);
    }
    else if (memcmp(buf, "sh", 2) == 0)
    {
        int j = 0;
        uint8_t *buf_read;
        uint8_t *data_tms;
        uint8_t *data_tdata;
#if 1
        for (j=0; j < ret; j += (nrbits * 2 + 10))
        {
            /* code */

            len = *(int *)(buf + 6);
            nrbits = (len + 7) / 8;

            buf_read = malloc(nrbits + 8);
            data_tms = malloc(nrbits + 8);
            data_tdata = malloc(nrbits + 8);

            memcpy(data_tms, buf + 10, nrbits);
            memcpy(data_tdata, buf + 10 + nrbits, nrbits);
            pio_xfer_rw(data_tdata, data_tms, buf_read, len);

            free(data_tms);
            free(data_tdata);
            tcp_write(tpcb, buf_read, nrbits, TCP_WRITE_FLAG_COPY);
            if (err_w != ERR_OK)
            {
                DEBUG_printf("Failed to write data %d\n", err);
                return tcp_server_result(arg, -1);
            }
            free(buf_read);
        }

#endif

        free(buf);
    }
    else
    {
    }
    // tcp_server_send_data(arg, state->client_pcb);

CleanUP:
    // if(p->tot_len > (TEST_XFER_LEN))

    goto Regain;

    pbuf_free(p);
    return ERR_OK;
}
static err_t tcp_server_poll(void *arg, struct tcp_pcb *tpcb)
{
    gpio_put(25, !gpio_get(25));
    DEBUG_printf("tcp_server_poll_fn\n");
    // return 0;
    return tcp_server_result(arg, -1); // no response is an error?
}

static void tcp_server_err(void *arg, err_t err)
{
    if (err != ERR_ABRT)
    {
        DEBUG_printf("tcp_client_err_fn %d\n", err);
        tcp_server_result(arg, err);
    }
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    if (err != ERR_OK || client_pcb == NULL)
    {
        DEBUG_printf("Failure in accept\n");
        tcp_server_result(arg, err);
        return ERR_VAL;
    }
    DEBUG_printf("Client connected\n");

    state->client_pcb = client_pcb;
    tcp_arg(client_pcb, state);
    tcp_sent(client_pcb, tcp_server_sent);
    tcp_recv(client_pcb, tcp_server_recv);
    tcp_poll(client_pcb, tcp_server_poll, POLL_TIME_S * 2);
    tcp_err(client_pcb, tcp_server_err);
    return 0;
    // debug there will erro
    return tcp_server_send_data(arg, state->client_pcb);
}

static bool tcp_server_open(void *arg)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    // DEBUG_printf("Starting server at %s on port %u\n", ip4addr_ntoa(netif_ip4_addr(netif_list)), TCP_PORT);

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb)
    {
        DEBUG_printf("failed to create pcb\n");
        return false;
    }

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, TCP_PORT);
    if (err)
    {
        DEBUG_printf("failed to bind to port %d\n");
        return false;
    }

    state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!state->server_pcb)
    {
        DEBUG_printf("failed to listen\n");
        if (pcb)
        {
            tcp_close(pcb);
        }
        return false;
    }

    tcp_arg(state->server_pcb, state);
    tcp_accept(state->server_pcb, tcp_server_accept);

    return true;
}

int pio_xfer_task(void);

#define FLAG_VALUE1 123
#define FLAG_VALUE2 321

static int core0_rx_val = 0, core1_rx_val = 0;

void core0_sio_irq()
{
    // Just record the latest entry
    while (multicore_fifo_rvalid())
        core0_rx_val = multicore_fifo_pop_blocking();

    multicore_fifo_clear_irq();
}

void core1_sio_irq()
{
    // Just record the latest entry
    while (multicore_fifo_rvalid())
        core1_rx_val = multicore_fifo_pop_blocking();

    multicore_fifo_clear_irq();
}
void core1_entry()
{

    multicore_fifo_clear_irq();
#ifdef USE_IRQ
    irq_set_exclusive_handler(SIO_IRQ_PROC1, core1_sio_irq);

    irq_set_enabled(SIO_IRQ_PROC1, true);
#endif
    // Send something to Core0, this should fire the interrupt.
    int len = 0;
    unsigned char *data_rx;
    unsigned char *data_tx;

    while (1)
    {
        data_tx = (unsigned char *)multicore_fifo_pop_blocking();
        memcpy(&len, data_tx + 6, 4);
        // len = *(int *)(data_tx + 6);
        //  pio_xfer_rw(buf + 10 + (len + 7) / 8, buf + 10, buf_read, len); // tms tdi tdo
        if (len < 32 * 8)
        {
            unsigned char *data_tdi = malloc(32);
            unsigned char *data_tms = malloc(32); //(len + 7) / 8
            data_rx = malloc(32);
            // memcpy((unsigned char *)data_tms, (unsigned char *)(data_tx + 10),(int) (len + 7) / 8);
            // memcpy((unsigned char *)data_tdi, (unsigned char *)(data_tx + 10 + (int) (len + 7) / 8), (int) (len + 7) / 8);
            // pio_xfer_rw((uint32_t *)data_tdi, (uint32_t *)data_tms, (uint32_t *)data_rx, len);
            free(data_tdi);
            free(data_tdi);
            multicore_fifo_push_blocking(data_rx);
        }
        else
        {

            // memcpy(data_tms,data_tx+10,(len + 7) / 8);
            // memcpy(data_tdi,data_tx+10 + (len + 7) / 8 ,(len + 7) / 8);
            unsigned char *data_tdi = malloc((len + 31) / 8 + 8);
            unsigned char *data_tms = malloc((len + 31) / 8 + 8); //(len + 7) / 8
            // memcpy((unsigned char *)data_tms, (unsigned char *)(data_tx + 10), (len + 7) / 8);
            // memcpy((unsigned char *)data_tdi, (unsigned char *)(data_tx + 10 + (len + 7) / 8), (len + 7) / 8);
            data_rx = malloc((len + 31) / 8 + 8);
            // pio_xfer_rw((uint32_t *)data_tdi, (uint32_t *)data_tms, (uint32_t *)data_rx, len);
            free(data_tdi);
            free(data_tdi);
            multicore_fifo_push_blocking((uint32_t)data_rx);
        }
        len = 0;
        // free(data_rx);
        // data_rx=NULL;
    }
    pio_xfer_task();
    while (1)
        tight_loop_contents();
}

void run_tcp_server_test(void)
{
    pio_xfer_init();
    // multicore_launch_core1(core1_entry);
#ifdef USE_IRQ
    irq_set_exclusive_handler(SIO_IRQ_PROC0, core0_sio_irq);
    irq_set_enabled(SIO_IRQ_PROC0, true);
#endif
    const uint LED_PIN = PICO_DEFAULT_LED_PIN;
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    {
        gpio_put(LED_PIN, 1);
        sleep_ms(250);
        gpio_put(LED_PIN, 0);
        sleep_ms(250);
    }
    TCP_SERVER_T *state = tcp_server_init();
    if (!state)
    {
        return;
    }
    if (!tcp_server_open(state))
    {
        tcp_server_result(state, -1);
        return;
    }
    // gpio_xor_mask(LED_PIN);
    // free(state);
    while (1)
    {
        tud_task(); // tinyusb device task
        extern void service_traffic(void);
        service_traffic();
        tcp_test_fifo_t *tmp = fifo_get();
        if (tmp)
        {
            // int len = *(int *)(tmp->data + 6);
            int nrbits = (len + 7) / 8;
            uint8_t *buf_read = malloc(nrbits + 8);
            uint8_t *data_tms = malloc(nrbits + 8);
            uint8_t *data_tdata = malloc(nrbits + 8);

            // memcpy(data_tms, tmp->data + 10, nrbits);
            // memcpy(data_tdata, tmp->data + 10 + nrbits, nrbits);
            // pio_xfer_rw(data_tdata, data_tms, buf_read, len);
            memcpy(buf_read, *(tmp->data) + 6, 4);
            free(data_tms);
            free(data_tdata);
            tcp_write(state->client_pcb, buf_read, 4, TCP_WRITE_FLAG_COPY);
            free(buf_read);
            free(tmp->data);
            free(tmp);
        }
    }
}

/*
 * Copyright 2023 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/ipm.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/init.h>

#include "common.h"
#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "rpmsg_queue.h"

#define RPMSG_LITE_NS_ANNOUNCE_STRING "rpmsg-openamp-demo-channel"

#define LOCAL_EPT_ADDR                (30U)
#define APP_RPMSG_READY_EVENT_DATA    (1U)
#define APP_RPMSG_EP_READY_EVENT_DATA (2U)

#define SHM_MEM_ADDR DT_REG_ADDR(DT_CHOSEN(zephyr_ipc_shm))
#define SHM_MEM_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_ipc_shm))

#define APP_THREAD_STACK_SIZE (1024)
K_THREAD_STACK_DEFINE(thread_stack, APP_THREAD_STACK_SIZE);
static struct k_thread thread_data;

struct rpmsg_lite_instance *gp_rpmsg_dev_inst;
struct rpmsg_lite_endpoint *gp_rpmsg_ept;
struct rpmsg_lite_instance g_rpmsg_ctxt;
struct rpmsg_lite_ept_static_context g_ept_context;

static volatile rpmsg_queue_handle rpmsg_queue = NULL;
static char helloMsg[13];

uint32_t *shared_memory = (uint32_t *)SHM_MEM_ADDR;

typedef struct the_message
{
    char DATA[100];
    uint8_t cnt;
} THE_MESSAGE, *THE_MESSAGE_PTR;

static THE_MESSAGE g_msg = {0};
static uint32_t g_remote_addr = 40;

/* Internal functions */
int32_t priv_rpmsg_queue_rx_cb(void *payload, uint32_t payload_len, uint32_t src, void *priv)
{
    rpmsg_queue_rx_cb_data_t msg;

    RL_ASSERT(priv != RL_NULL);

    msg.data = payload;
    msg.len  = payload_len;
    msg.src  = src;

    printk("msg.data: %s \n", (char*)msg.data);
    printk("msg.len: %d \n", msg.len);
    printk("msg.src: %d \n", msg.src);

    /* if message is successfully added into queue then hold rpmsg buffer */
    if (0 != env_put_queue(priv, &msg, 0))
    {
        /* hold the rx buffer */
        return RL_HOLD;
    }

    return RL_RELEASE;
}

static void app_nameservice_isr_cb(uint32_t new_ept, const char *new_ept_name, uint32_t flags, void *user_data)
{
}

static void application_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    volatile rpmsg_ns_handle ns_handle;

    gp_rpmsg_dev_inst = rpmsg_lite_remote_init(shared_memory, RPMSG_LITE_LINK_ID, RL_NO_FLAGS);

    printk("Wait for link up!\n");
    rpmsg_lite_wait_for_link_up(gp_rpmsg_dev_inst, 600000);
    printk("Link is up: done!\n");

    rpmsg_queue = rpmsg_queue_create(gp_rpmsg_dev_inst);
    gp_rpmsg_ept = rpmsg_lite_create_ept(gp_rpmsg_dev_inst, LOCAL_EPT_ADDR,
                                         priv_rpmsg_queue_rx_cb, rpmsg_queue);
    ns_handle = rpmsg_ns_bind(gp_rpmsg_dev_inst, app_nameservice_isr_cb,
                             ((void *)0));

    /* Introduce some delay to avoid NS announce message not being captured by the master side.
       This could happen when the remote side execution is too fast and the NS announce message is triggered
       before the nameservice_isr_cb is registered on the master side. */
    k_sleep(K_MSEC(1000));

    rpmsg_ns_announce(gp_rpmsg_dev_inst, gp_rpmsg_ept,
                      RPMSG_LITE_NS_ANNOUNCE_STRING, (uint32_t)0);
    printk("Nameservice announce sent.\n");

    printk("Wait Hello handshake message from Remote Core...\r\n");
    rpmsg_queue_recv(gp_rpmsg_dev_inst, rpmsg_queue,
                     (uint32_t *)&g_remote_addr, helloMsg, sizeof(helloMsg),
                     ((void *)0), RL_BLOCK);

    while (g_msg.cnt <= 100U)
    {
        rpmsg_queue_recv(gp_rpmsg_dev_inst, rpmsg_queue,
                         (uint32_t *)&g_remote_addr, g_msg.DATA,
                         sizeof(g_msg.DATA), ((void *)0), RL_BLOCK);
        printk("msg_received: %s\n", g_msg.DATA);
        g_msg.cnt++;
        (void)rpmsg_lite_send(gp_rpmsg_dev_inst, gp_rpmsg_ept, g_remote_addr,
                              g_msg.DATA, sizeof(g_msg.DATA), RL_BLOCK);
    }

    (void)rpmsg_lite_destroy_ept(gp_rpmsg_dev_inst, gp_rpmsg_ept);
    gp_rpmsg_ept = ((void *)0);
    (void)rpmsg_queue_destroy(gp_rpmsg_dev_inst, rpmsg_queue);
    rpmsg_queue = ((void *)0);
    (void)rpmsg_ns_unbind(gp_rpmsg_dev_inst, ns_handle);
    (void)rpmsg_lite_deinit(gp_rpmsg_dev_inst);

    memset(g_msg.DATA, 0, sizeof(g_msg.DATA));
    g_msg.cnt = 0;
}

int main(void)
{
    printk("Starting application thread on Remote Core!\n");
    k_thread_create(&thread_data, thread_stack, APP_THREAD_STACK_SIZE, application_thread, NULL, NULL, NULL,
                    K_PRIO_COOP(7), 0, K_NO_WAIT);

    return 0;
}

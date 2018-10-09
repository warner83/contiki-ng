/*
 * Copyright (c) 2018, ASSA ABLOY AB.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */
/**
 * \file   UDP Tx-Rx forwarding test using the simple UDP API
 *         In the end, root node prints statistics per source node per test run
 *
 * \author Zhitao He <zhitao.he@assaabloy.com>
 */

#include "contiki.h"
#include "sys/node-id.h"
#include "sys/log.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-sr.h"
#include "net/mac/tsch/tsch.h"
#include "net/routing/routing.h"

#define DEBUG DEBUG_PRINT
#include "net/ipv6/uip-debug.h"

#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"

#include <string.h>
#include <stdlib.h>
#include "memb.h"
#include "node.h"

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT	8765
#define UDP_SERVER_PORT	5678
#define MAX_SOURCE_NODES 3
#define NSEQNOS 4

enum role {ROOT, FORWARDER, SOURCE};

static struct simple_udp_connection udp_conn;
static uip_ipaddr_t root_ipaddr;
static enum role my_role = ROOT;
static int run;

static void
root_et_handler(struct etimer *et);

struct stats {
  uip_ipaddr_t src_addr;
  bool started;
  bool finished;
  clock_time_t run_started_at;
  int seqnos_received[NSEQNOS];
  int run;
  struct etimer et;
  int pkts_received;
  clock_time_t run_duration[MAX_RUNS];
  int total_pkts_received[MAX_RUNS];
};

MEMB(stats_tbl, struct stats, MAX_SOURCE_NODES);

/*---------------------------------------------------------------------------*/
PROCESS(node_process, "RPL Node");
PROCESS(udp_client_process, "UDP client");
PROCESS(root_process, "Root app");
AUTOSTART_PROCESSES(&node_process);
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  struct stats *stats = NULL;

  // does source node exist already?
  int i;
  for(i = 0;i < MAX_SOURCE_NODES;i++) {
    if(stats_tbl.count[i] > 0 &&
       uip_ipaddr_cmp(&stats_tbl_memb_mem[i].src_addr, sender_addr)) {
      stats = &stats_tbl_memb_mem[i];
      break;
    }
  }

  // add new source node
  if(stats == NULL) {
    stats = memb_alloc(&stats_tbl);
    if(stats != NULL) {
      uip_ipaddr_copy(&stats->src_addr, sender_addr);
      LOG_INFO("new source node ");
      LOG_INFO_6ADDR(&stats->src_addr);
      LOG_INFO_("\n");
      LOG_INFO("memb slots available = %d\n", memb_numfree(&stats_tbl));
    } else {
      LOG_INFO("memb_alloc returns NULL\n");
    }
  }

  if(stats == NULL) {
    return;
  }

  // create a visual offset based on addr index in source addr memory block
  char spaces[5 * MAX_SOURCE_NODES];
  memset(spaces, ' ', sizeof(spaces));
  i = stats - &stats_tbl_memb_mem[0];
  spaces[i*5] = '\0';

  LOG_INFO("%s <-- %s :%02x%02x\n",
         (char*)data,
         spaces,
         sender_addr->u8[14],
         sender_addr->u8[15]);

  /* received a redundant packet? skip */
  int seqno = atoi((const char*) data);
  for(i = 0;i < NSEQNOS;i++) {
    if(seqno == stats->seqnos_received[i]) {
      LOG_INFO("redundant seqno %d\n", seqno);
      break;
    }
  }
  /* new seqno: insert it to last N received seqnos */
  if(i == NSEQNOS) {
    stats->pkts_received++;
    for(i = 0;i < NSEQNOS-1;i++) {
      stats->seqnos_received[i] = stats->seqnos_received[i+1];
    }
    stats->seqnos_received[NSEQNOS-1] = seqno;
  }

  /* a seqno smaller than the previous one indicates packet reordering */
  if(seqno < stats->seqnos_received[NSEQNOS-2]) {
    LOG_INFO("out-of-order %d - %d = %d\n",
           seqno,
           stats->seqnos_received[NSEQNOS-2],
           seqno - stats->seqnos_received[NSEQNOS-2]);
  }

  /* new run? */
  if(!stats->started) {
    stats->started = 1;
    stats->run_started_at = clock_time();
  }

  /* end of run? */
  if(seqno >= MAX_PKTS * (stats->run + 1)) {
    etimer_stop(&(stats->et));
    root_et_handler(&(stats->et));
  } else {
    /* estimate time to the end of run, with extra margin */
    clock_time_t expiration_time;
    PROCESS_CONTEXT_BEGIN(&root_process);
    expiration_time = etimer_expiration_time(&stats->et) - clock_time();
    LOG_DBG("was expiring in %lu\n", expiration_time);
    expiration_time = (MAX_PKTS * (stats->run + 1) - seqno + 1 + 5) * SEND_INTERVAL_MAX;
    LOG_DBG("now expiring in %lu\n", expiration_time);
    etimer_set(&stats->et, expiration_time);
    PROCESS_CONTEXT_END(&root_process);
  }
}
/*---------------------------------------------------------------------------*/
static void
set_role(void)
{
#if CONTIKI_TARGET_COOJA
  my_role = node_id == 1 ? ROOT : SOURCE;
#endif
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(node_process, ev, data)
{
  static struct etimer et;

  PROCESS_BEGIN();

  /* role = ROOT/FORWARDER/SOURCE */
  set_role();

  /* root */
  if(my_role == ROOT) {
    LOG_PRINT("I am a root\n");
    /* start routing root; start UDP server */
    NETSTACK_ROUTING.root_start();
    simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                        UDP_CLIENT_PORT, udp_rx_callback);
    /* packet stats per source node */
    memb_init(&stats_tbl);
    /* end-of-run handling */
    process_start(&root_process, NULL);
  }

  NETSTACK_MAC.on();

  /* source/router node: find root route */
  if(my_role != ROOT) {
    static unsigned long t0, t1;
    t0 = clock_seconds();
    LOG_INFO("Get root address");
    while (!NETSTACK_ROUTING.node_is_reachable()) {
      etimer_set(&et, CLOCK_SECOND);
      PROCESS_YIELD_UNTIL(etimer_expired(&et));
      LOG_INFO_(".");
      fflush(stdout);
    }
    LOG_INFO_("\n");
    NETSTACK_ROUTING.get_root_ipaddr(&root_ipaddr);
    LOG_INFO_6ADDR(&root_ipaddr);
    LOG_INFO_("\n");
    t1 = clock_seconds();
    LOG_INFO("It took %lu s\n", t1 - t0);

    /* TODO EXPERIMENTAL: wait until link quality to parent stablizes */

    /* TODO Show parent */

    /* source node */
    if(my_role == SOURCE) {
      LOG_PRINT("I am a source\n");
      /* Initialize UDP connection */
      simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                          UDP_SERVER_PORT, udp_rx_callback);
      /*Start periodic sending */
      process_start(&udp_client_process, NULL);
    } else {
      LOG_PRINT("I am a forwarder\n");
    }
  }

#if WITH_PERIODIC_ROUTES_PRINT
  {
    static struct etimer et;
    /* Print out routing tables every minute */
    etimer_set(&et, CLOCK_SECOND * 60);
    while(1) {
      /* Used for non-regression testing */
      #if (UIP_MAX_ROUTES != 0)
        LOG_INFO("Routing entries: %u\n", uip_ds6_route_num_routes());
      #endif
      #if (UIP_SR_LINK_NUM != 0)
        LOG_INFO("Routing links: %u\n", uip_sr_num_nodes());
      #endif
      PROCESS_YIELD_UNTIL(etimer_expired(&et));
      etimer_reset(&et);
    }
  }
#endif /* WITH_PERIODIC_ROUTES_PRINT */

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
static void
print_run_stats(struct stats *stats)
{
  LOG_INFO("***** Node %02X%02X Run %d received %d *****\n",
         stats->src_addr.u8[14], stats->src_addr.u8[15],
         stats->run, stats->pkts_received);
}

/* print test runs result in table form */
static void
print_all_stats(struct stats *stats)
{
  int i;

  LOG_PRINT("node stats ");
  LOG_PRINT_6ADDR(&stats->src_addr);
  LOG_PRINT_("\n");
  LOG_PRINT("run rcv duration\n");
  for(i = 0;i < MAX_RUNS;i++) {
    LOG_PRINT("%3d %3d %4lu (%lu.%1lus)\n",
           i,
           stats->total_pkts_received[i],
           stats->run_duration[i],
           stats->run_duration[i] / CLOCK_SECOND,
           (stats->run_duration[i] % CLOCK_SECOND) * 10 / CLOCK_SECOND);
  }
}
/*---------------------------------------------------------------------------*/
/* create a message encoding seqno, padded to len bytes */
static size_t
create_message(int seqno, char *buffer, size_t buflen)
{
  memset(buffer, 'X', buflen);

  int ret;

  if(buffer == NULL || buflen < 1) return -1;

  ret = snprintf(buffer, buflen, "%03d", seqno);

  if(ret < buflen) {
    return buflen;
  } else {
    return -1;
  }
}
/*---------------------------------------------------------------------------*/
/* End-of-run time-out */
static void
root_et_handler(struct etimer *et)
{
  struct stats *stats = NULL;
  int i;
  for(i = 0;i < MAX_SOURCE_NODES;i++) {
    if(et == &stats_tbl_memb_mem[i].et) {
      stats = &stats_tbl_memb_mem[i];
      break;
    }
  }
  if(stats == NULL) {
    LOG_ERR("stats pointer out of range\n");
    return;
  }

  stats->finished = 1;

  /* print run stats, clean up for next run; */
  if(stats->finished) {
    print_run_stats(stats);

    stats->finished = 0;
    stats->started = 0;
    stats->run_duration[stats->run] = clock_time() - stats->run_started_at;
    memset(stats->seqnos_received, 0, sizeof(stats->seqnos_received));
    (stats->total_pkts_received)[stats->run] = stats->pkts_received;
    stats->pkts_received = 0;
    stats->run++; // last step: increment run no.

    /* if this was the last run, print final stats */
    if(stats->run == MAX_RUNS) {
      print_all_stats(stats);
      stats->run = 0;
      /* memb_free(&stats_tbl, stats); */
    }
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;

  PROCESS_BEGIN();

  /* leaf: send R batches of N packets to root */
  static int pkts_sent = 0;
  for(run = 0;run < MAX_RUNS;run++) {
    LOG_INFO("\n-------- RUN %d --------\n", run);

    /* node: create and send a UDP message to root, until MAX_PKTS done */
    while(pkts_sent < (MAX_PKTS * (run+1))) {
      char message[DEFAULT_PAYLOAD_LEN];
      (void)create_message(pkts_sent + 1, message, sizeof(message));
      LOG_INFO_("^");
      fflush(stdout);
      simple_udp_sendto(&udp_conn, message, sizeof(message), &root_ipaddr);
      pkts_sent++;

      /* send packet and yield */
      etimer_set(&periodic_timer, SEND_INTERVAL);
      PROCESS_YIELD_UNTIL(etimer_expired(&periodic_timer));
    }
    etimer_set(&periodic_timer, SEND_INTERVAL * 8);
    LOG_INFO("\n-------- RUN %d ended -------- \n", run);
    PROCESS_YIELD_UNTIL(etimer_expired(&periodic_timer));
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(root_process, ev, data)
{
  PROCESS_BEGIN();
  while(1) {
    PROCESS_YIELD();
    if(ev == PROCESS_EVENT_TIMER) {
      root_et_handler((struct etimer *)data);
    }
  }
  PROCESS_END();
}

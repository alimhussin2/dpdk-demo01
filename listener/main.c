/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>

static volatile bool force_quit;

/* MAC updating enabled by default */
static int mac_updating = 1;

static int iCounter = 0;

#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

#define MAX_PKT_BURST 32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct rte_ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

/* mask of enabled ports */
static uint32_t l2fwd_enabled_port_mask = 0;

/* list of enabled ports */
static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];

struct port_pair_params {
#define NUM_PORTS	2
	uint16_t port[NUM_PORTS];
} __rte_cache_aligned;

static struct port_pair_params port_pair_params_array[RTE_MAX_ETHPORTS / 2];
static struct port_pair_params *port_pair_params;
static uint16_t nb_port_pair_params;

static unsigned int l2fwd_rx_queue_per_lcore = 1;

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16
struct lcore_queue_conf {
	unsigned n_rx_port;
	unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
} __rte_cache_aligned;
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

struct rte_mempool * l2fwd_pktmbuf_pool = NULL;

/* Per-port statistics struct */
struct l2fwd_port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
        uint64_t tx_bytes;
        uint64_t rx_bytes;
        uint64_t tx_error;
        uint64_t rx_error;
        uint64_t tx_burst;
        uint64_t rx_burst;
        uint64_t rx_nombuf;
        uint64_t pkt_p_s_tx;
        uint64_t pkt_p_s_rx;
        uint16_t ether_type;
        uint16_t vlan_tag;
        uint16_t vlan_id;
        uint16_t vlan_priority;
        struct rte_ether_addr d_addr;
        struct rte_ether_addr s_addr;
        uint32_t pkt_length;
        uint16_t data_len;
        char ip_protocol[6];
        unsigned char ip_s_addr[4];
        unsigned char ip_d_addr[4];
        uint64_t jitter_ns;
        double latency_us;
        uint64_t timestamp;
        uint64_t timestamp_error;
        uint64_t elapsed_tx_time;
        uint64_t elapsed_rx_time;
        uint64_t timestamp_s;
        uint64_t timestamp_us;
} __rte_cache_aligned;
struct l2fwd_port_statistics port_statistics[RTE_MAX_ETHPORTS];

static struct stats {
	uint64_t total_latency;
} latency_numbers;

#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t timer_period = 10; /* default period is 10 seconds */


static void debug0(const char* format,...){
  return;
  char buffer[1000];
  va_list args;
  va_start (args, format);
  vsnprintf (buffer, 999, format, args);
  printf ("%s",buffer); 
  va_end(args);
  
}

uint64_t get_time_nanosec(clockid_t clkid)
{
#define NSEC_PER_SEC 1000000000L
	struct timespec now;
	clock_gettime(clkid, &now);
	return now.tv_sec * NSEC_PER_SEC + now.tv_nsec;
}

typedef uint64_t tsc_t;
static inline tsc_t *tsc_field(struct rte_mbuf *mbuf, int tsc_dynfield_offset)
{
	struct rte_ether_hdr *eth_hdr;
	eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
	void *p = (struct rte_mbuf *)(eth_hdr);
	return RTE_MBUF_DYNFIELD(p, tsc_dynfield_offset, tsc_t *);
}

static void calc_sw_latency(struct rte_mbuf *m, int portid)
{
	int tsc_dynfield_offset;
	uint64_t rx_tsp = get_time_nanosec(CLOCK_REALTIME);
        uint64_t tx_tsp;
	double latency_ns;

        tsc_dynfield_offset = sizeof(struct rte_ether_hdr);
        tx_tsp = *tsc_field(m, tsc_dynfield_offset);
        uint64_t diff_tsp = rx_tsp - tx_tsp;

	port_statistics[portid].timestamp_us = rx_tsp;
	port_statistics[portid].timestamp += diff_tsp;

	latency_numbers.total_latency += diff_tsp;
	latency_ns = (double)latency_numbers.total_latency / (double)port_statistics[portid].rx;
	port_statistics[portid].latency_us = latency_ns / (1000 * 1000);
}

static void
extract_l2packet(struct rte_mbuf *m, int rx_batch_idx, int rx_batch_ttl)
{

#define TALKER_PACKET_ETH_TYPE 2048
       
       

       struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
       uint64_t pkt_id;
       char* msg = ((rte_pktmbuf_mtod(m,char*)) + sizeof(struct rte_ether_hdr)); //maybe wrong
       int datalen = rte_pktmbuf_pkt_len(m);  
       struct rte_ether_addr src01 =  eth_hdr->s_addr;
       struct rte_ether_addr dst01 =  eth_hdr->d_addr;

       
       if (eth_hdr->ether_type != TALKER_PACKET_ETH_TYPE) { 
            return; 
       }

       if (l2fwd_ports_eth_addr[0].addr_bytes[0] != dst01.addr_bytes[0] ||
           l2fwd_ports_eth_addr[0].addr_bytes[1] != dst01.addr_bytes[1] ||
           l2fwd_ports_eth_addr[0].addr_bytes[2] != dst01.addr_bytes[2] ||
           l2fwd_ports_eth_addr[0].addr_bytes[3] != dst01.addr_bytes[3] ||
           l2fwd_ports_eth_addr[0].addr_bytes[4] != dst01.addr_bytes[4] ||
           l2fwd_ports_eth_addr[0].addr_bytes[5] != dst01.addr_bytes[5] 
          ){
             return;
 
       }

         
        printf ("\n------------------------------------------");
        printf ("\nbatch: %d of %d",rx_batch_idx,rx_batch_ttl);
        printf("\nExtacting Packet:\n");

	int tsc_dynfield_offset = sizeof(struct rte_ether_hdr) + sizeof(pkt_id);
	pkt_id = *tsc_field(m, tsc_dynfield_offset);

	printf("\nPacket ID: %"PRIu64"\n", pkt_id);

        printf ("\nEthernet type=%d",eth_hdr->ether_type);


        printf ("\nPayload Data Size=%d",datalen);

        printf("\nSOURCE MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
                                src01.addr_bytes[0],
                                src01.addr_bytes[1],
                                src01.addr_bytes[2],
                                src01.addr_bytes[3],
                                src01.addr_bytes[4],
                                src01.addr_bytes[5]);

        printf("\nDESTINATION MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
                                dst01.addr_bytes[0],
                                dst01.addr_bytes[1],
                                dst01.addr_bytes[2],
                                dst01.addr_bytes[3],
                                dst01.addr_bytes[4],
                                dst01.addr_bytes[5]);



        if (RTE_ETH_IS_IPV4_HDR(m->packet_type)) {

                                 struct rte_ipv4_hdr* ipv4_hdr = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
                                 rte_be32_t ipTmp = ipv4_hdr->src_addr;
                                 printf ("\nIPV4=%d\n",ipTmp);

                                 unsigned char bytes[4];
                                 bytes[0] = ipTmp & 0xFF;
                                 bytes[1] = (ipTmp >> 8) & 0xFF;
                                 bytes[2] = (ipTmp >> 16) & 0xFF;
                                 bytes[3] = (ipTmp >> 24) & 0xFF;
                                 printf("\nIP:%d.%d.%d.%d", bytes[3], bytes[2], bytes[1], bytes[0]);
        }

       //print payload  
        printf("\nPacket Payload: "); 

        /*
	char b_tx_tsp[20]; 
        int j;
        for(j=0;j<20;j++){
           b_tx_tsp[j] = msg[j]; 
	   printf("%c",msg[j]);
        }
        printf("\n");
	*/


        //printf("rx_tsp:%s\n",b_tx_tsp);
	uint64_t rx_tsp  = get_time_nanosec(CLOCK_REALTIME);
        uint64_t tx_tsp;
        //sscanf(b_tx_tsp, "%"PRIu64, &tx_tsp);

	tsc_dynfield_offset = sizeof(struct rte_ether_hdr);
        tx_tsp = *tsc_field(m, tsc_dynfield_offset);
        uint64_t delta_tsp = rx_tsp - tx_tsp;

        printf("tx_tsp:%"PRIu64,tx_tsp);
        printf("\nnow_tsp:%"PRIu64,rx_tsp);
        printf("\ndelta:%"PRIu64,delta_tsp);        

       /*if (eth_hdr->ether_type == TALKER_PACKET_ETH_TYPE) {
            exit(-1);
       }*/





}

static long diff_us(struct timespec t1, struct timespec t2)
{
    struct timespec diff;
    if (t2.tv_nsec-t1.tv_nsec < 0) {
        diff.tv_sec  = t2.tv_sec - t1.tv_sec - 1;
        diff.tv_nsec = t2.tv_nsec - t1.tv_nsec + 1000000000;
    } else {
        diff.tv_sec  = t2.tv_sec - t1.tv_sec;
        diff.tv_nsec = t2.tv_nsec - t1.tv_nsec;
    }
    return (diff.tv_sec * 1000000.0 + diff.tv_nsec / 1000.0);
}


/* Print out statistics on packets dropped */

static void print_stats(void)
{
        uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
        unsigned portid;

        total_packets_dropped = 0;
        total_packets_tx = 0;
        total_packets_rx = 0;

        const char clr[] = { 27, '[', '2', 'J', '\0' };
        const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

                /* Clear screen and move to top left */
        printf("%s%s", clr, topLeft);
        printf("\nElapsed time Tx/Rx: %" PRIu64 "/%" PRIu64 " seconds",
                port_statistics[portid].elapsed_tx_time, port_statistics[portid].elapsed_rx_time);

        printf("\nPort statistics ====================================");

        for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
                /* skip disabled ports */
                if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
                        continue;

                printf("\nStatistics for port %u ------------------------------", portid);
                printf("\nMAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
                        l2fwd_ports_eth_addr[portid].addr_bytes[0],
                        l2fwd_ports_eth_addr[portid].addr_bytes[1],
                        l2fwd_ports_eth_addr[portid].addr_bytes[2],
                        l2fwd_ports_eth_addr[portid].addr_bytes[3],
                        l2fwd_ports_eth_addr[portid].addr_bytes[4],
                        l2fwd_ports_eth_addr[portid].addr_bytes[5]);
                printf("\nPackets Tx/Rx:       %18"PRIu64"/%"PRIu64,
                        port_statistics[portid].tx, port_statistics[portid].rx);
                printf("\nPackets dropped:     %18"PRIu64,
                        port_statistics[portid].dropped);
                printf("\nPackets Tx/Rx burst: %18"PRIu64"/%"PRIu64,
                        port_statistics[portid].tx_burst, port_statistics[portid].rx_burst);
                printf("\nPkts Bytes Tx/Rx:    %18"PRIu64"/%"PRIu64,
                        port_statistics[portid].tx_bytes, port_statistics[portid].rx_bytes);
                printf("\nPkts error Tx/Rx:    %18"PRIu64"/%"PRIu64,
                        port_statistics[portid].tx_error, port_statistics[portid].rx_error);
                printf("\nrx_nombuf:           %18"PRIu64, port_statistics[portid].rx_nombuf);
                printf("\nPkts/s Tx/Rx:        %18"PRIu64 "/%"PRIu64, port_statistics[portid].pkt_p_s_tx, port_statistics[portid].pkt_p_s_rx);
                printf("\nSrc MAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
                        port_statistics[portid].s_addr.addr_bytes[0],
                        port_statistics[portid].s_addr.addr_bytes[1],
                        port_statistics[portid].s_addr.addr_bytes[2],
                        port_statistics[portid].s_addr.addr_bytes[3],
                        port_statistics[portid].s_addr.addr_bytes[4],
                        port_statistics[portid].s_addr.addr_bytes[5]);
                printf("\nDst MAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
			port_statistics[portid].d_addr.addr_bytes[0],
                        port_statistics[portid].d_addr.addr_bytes[1],
                        port_statistics[portid].d_addr.addr_bytes[2],
                        port_statistics[portid].d_addr.addr_bytes[3],
                        port_statistics[portid].d_addr.addr_bytes[4],
                        port_statistics[portid].d_addr.addr_bytes[5]);
                printf("\nEther Type:          %18"PRIx16, port_statistics[portid].ether_type);
                printf("\nVLAN ID:             %18"PRIu16, port_statistics[portid].vlan_id);
                printf("\nVLAN Priority:       %18"PRIu16, port_statistics[portid].vlan_priority);
                printf("\nPacket Lenght:       %18"PRIu32, port_statistics[portid].pkt_length);
                printf("\nData Length:         %18"PRIu16, port_statistics[portid].data_len);
                printf("\nIP Protocol:         %18s", port_statistics[portid].ip_protocol);
                printf("\nSrc IP Address: %d.%d.%d.%d",
                        port_statistics[portid].ip_s_addr[0],
                        port_statistics[portid].ip_s_addr[1],
                        port_statistics[portid].ip_s_addr[2],
                        port_statistics[portid].ip_s_addr[3]);
                printf("\nDst IP Address: %d.%d.%d.%d",
                        port_statistics[portid].ip_d_addr[0],
                        port_statistics[portid].ip_d_addr[1],
                        port_statistics[portid].ip_d_addr[2],
                        port_statistics[portid].ip_d_addr[3]);
                printf("\nSW Jitter (ns)       %18"PRIu64, port_statistics[portid].jitter_ns);
                printf("\nSW Latency (ms):     %18.4f", port_statistics[portid].latency_us);
                printf("\nSW timestamp (s):    %18"PRIu64, port_statistics[portid].timestamp_s);
                printf("\nSW timestamp (us):   %18"PRIu64, port_statistics[portid].timestamp_us);
                printf("\ntotal timestamp (us):%18"PRIu64, port_statistics[portid].timestamp);
                printf("\nTimestamp error:     %18"PRIu64, port_statistics[portid].timestamp_error);

                total_packets_dropped += port_statistics[portid].dropped;
                total_packets_tx += port_statistics[portid].tx;
                total_packets_rx += port_statistics[portid].rx;
        }
        printf("\nAggregate statistics ==============================="
                   "\nTotal packets sent:      %14"PRIu64
                   "\nTotal packets received:  %14"PRIu64
                   "\nTotal packets dropped:   %14"PRIu64,
                   total_packets_tx,
                   total_packets_rx,
                   total_packets_dropped);
        printf("\n====================================================\n");

        fflush(stdout);
}

static void
print_stats_02(void)
{


	uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
	unsigned portid;

	total_packets_dropped = 0;
	total_packets_tx = 0;
	total_packets_rx = 0;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };



	/* Clear screen and move to top left */
	//printf("%s%s", clr, topLeft);

	printf("\nPort statistics ====================================");
 

	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		/* skip disabled ports */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		//printf ("\nyockgen=%24"PRIu64,  port_statistics[portid].yockgen);
                printf("\nPort %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
				portid,
				l2fwd_ports_eth_addr[portid].addr_bytes[0],
				l2fwd_ports_eth_addr[portid].addr_bytes[1],
				l2fwd_ports_eth_addr[portid].addr_bytes[2],
				l2fwd_ports_eth_addr[portid].addr_bytes[3],
				l2fwd_ports_eth_addr[portid].addr_bytes[4],
				l2fwd_ports_eth_addr[portid].addr_bytes[5]);

		printf("\nStatistics for port %u ------------------------------"
			   "\nPackets sent: %24"PRIu64
			   "\nPackets received: %20"PRIu64
			   "\nPackets dropped: %21"PRIu64,
			   portid,
			   port_statistics[portid].tx,
			   port_statistics[portid].rx,
			   port_statistics[portid].dropped);

		total_packets_dropped += port_statistics[portid].dropped;
		total_packets_tx += port_statistics[portid].tx;
		total_packets_rx += port_statistics[portid].rx;
	}
	printf("\nAggregate statistics ==============================="
		   "\nTotal packets sent: %18"PRIu64
		   "\nTotal packets received: %14"PRIu64
		   "\nTotal packets dropped: %15"PRIu64,
		   total_packets_tx,
		   total_packets_rx,
		   total_packets_dropped);
	printf("\n====================================================\n");

	//fflush(stdout);
}


static void
print_stats1(void)
{
        return; 
        struct rte_eth_stats eth_stats;
        unsigned int i;
        uint64_t  total_packets_sent=0, total_packets_received=0, total_packets_sent_bytes=0,total_packets_received_bytes=0;
        uint64_t  total_packets_sent_dropped=0,total_packets_received_dropped_buffer=0, total_packets_received_dropped_others=0;

        const char clr[] = { 27, '[', '2', 'J', '\0' };
        const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };
        printf("%s%s", clr, topLeft);
        printf("\nPort statistics\n====================================");

        RTE_ETH_FOREACH_DEV(i) {

	        /* skip disabled ports */
		if ((l2fwd_enabled_port_mask & (1 << i)) == 0)
			continue;

                rte_eth_stats_get(i, &eth_stats);

                printf("\nPort %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
				i,
				l2fwd_ports_eth_addr[i].addr_bytes[0],
				l2fwd_ports_eth_addr[i].addr_bytes[1],
				l2fwd_ports_eth_addr[i].addr_bytes[2],
				l2fwd_ports_eth_addr[i].addr_bytes[3],
				l2fwd_ports_eth_addr[i].addr_bytes[4],
				l2fwd_ports_eth_addr[i].addr_bytes[5]);

                printf("\nStatistics for port %u\n------------------------------"
			   "\nPackets sent: %"PRIu64
			   "\nPackets sent (bytes): %"PRIu64
			   "\nPackets sent dropped: %"PRIu64
			   "\nPackets received: %"PRIu64
			   "\nPackets received (bytes): %"PRIu64
			   "\nPackets received dropped (no RX buffer) : %"PRIu64
			   "\nPackets received dropped (other errors) : %"PRIu64,
			   i,
			   eth_stats.opackets,
                           eth_stats.obytes,
                           eth_stats.oerrors,  
			   eth_stats.ipackets,
                           eth_stats.ibytes, 
                           eth_stats.imissed,
                           eth_stats.ierrors);

                total_packets_sent += eth_stats.opackets;
		total_packets_received += eth_stats.ipackets;
                total_packets_sent_bytes += eth_stats.obytes;
                total_packets_received_bytes += eth_stats.ibytes;
                total_packets_sent_dropped += eth_stats.oerrors;
                total_packets_received_dropped_buffer += eth_stats.imissed;
                total_packets_received_dropped_others += eth_stats.ierrors;


        }

        printf("\n\nAggregate statistics (how many port!!)\n==============================="
		   "\nTotal packets sent: %"PRIu64
		   "\nTotal packets sent (bytes): %"PRIu64
		   "\nTotal packets sent dropped: %"PRIu64
		   "\nTotal packets received: %"PRIu64
		   "\nTotal packets received (bytes): %"PRIu64
		   "\nTotal packets received dropped (no RX buffer): %"PRIu64
		   "\nTotal packets received dropped (other errors): %"PRIu64,
		   total_packets_sent,
                   total_packets_sent_bytes,
                   total_packets_sent_dropped,
		   total_packets_received,
		   total_packets_received_bytes,
                   total_packets_received_dropped_buffer,
                   total_packets_received_dropped_others);

	printf("\n====================================================\n");


        //print_stats_02();


}

static void
l2fwd_mac_updating(struct rte_mbuf *m, unsigned dest_portid)
{
	struct rte_ether_hdr *eth;
	void *tmp;

	eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

	/* 02:00:00:00:00:xx */
	tmp = &eth->d_addr.addr_bytes[0];
	*((uint64_t *)tmp) = 0x000000000002 + ((uint64_t)dest_portid << 40);

	/* src addr */
	rte_ether_addr_copy(&l2fwd_ports_eth_addr[dest_portid], &eth->s_addr);
}

static void
l2fwd_simple_forward(struct rte_mbuf *m, unsigned portid)
{
	unsigned dst_port;
	int sent;
	struct rte_eth_dev_tx_buffer *buffer;


	dst_port = l2fwd_dst_ports[portid];
        //printf("\ndst_port id =%d", dst_port);

	if (mac_updating)
		l2fwd_mac_updating(m, dst_port);

	buffer = tx_buffer[dst_port];
	sent = rte_eth_tx_buffer(dst_port, 0, buffer, m);
	if (sent)
		port_statistics[dst_port].tx += sent;
}

/* main processing loop */
static void
l2fwd_main_loop(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	struct rte_mbuf *m;
	int sent;
	unsigned lcore_id;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	unsigned i, j, portid, nb_rx;
	struct lcore_queue_conf *qconf;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
			BURST_TX_DRAIN_US;
	struct rte_eth_dev_tx_buffer *buffer;

	prev_tsc = 0;
	timer_tsc = 0;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	if (qconf->n_rx_port == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
		return;
	}

	RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u\n", lcore_id);

	for (i = 0; i < qconf->n_rx_port; i++) {

		portid = qconf->rx_port_list[i];
		RTE_LOG(INFO, L2FWD, " -- lcoreid=%u portid=%u\n", lcore_id,
			portid);

	}

	while (!force_quit) {

		cur_tsc = rte_rdtsc();

		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {

			for (i = 0; i < qconf->n_rx_port; i++) {

				portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
				buffer = tx_buffer[portid];

				sent = rte_eth_tx_buffer_flush(portid, 0, buffer);
				if (sent)
					port_statistics[portid].tx += sent;

			}

			/* if timer is enabled */
			if (timer_period > 0) {

				/* advance the timer */
				timer_tsc += diff_tsc;

				/* if timer has reached its timeout */
				if (unlikely(timer_tsc >= timer_period)) {

					/* do this only on main core */
					if (lcore_id == rte_get_main_lcore()) {

						//printf ("yockgen listening....\n");
						print_stats();
						/* reset the timer */
						timer_tsc = 0;
					}
				}
			}

			prev_tsc = cur_tsc;
		}

		/*
		 * Read packet from RX queues
		 */
		for (i = 0; i < qconf->n_rx_port; i++) {

                        //struct timespec start, end;
                        //clock_gettime(CLOCK_MONOTONIC, &start);

			portid = qconf->rx_port_list[i];
			nb_rx = rte_eth_rx_burst(portid, 0, pkts_burst, MAX_PKT_BURST);

			//port_statistics[portid].rx += nb_rx;
			port_statistics[portid].rx_burst = nb_rx;

 
			for (j = 0; j < nb_rx; j++) {
				m = pkts_burst[j];
				rte_prefetch0(rte_pktmbuf_mtod(m, void *));
				port_statistics[portid].rx += 1;
                                //extract_l2packet(m,j+1,nb_rx); 
                                //l2fwd_simple_forward(m, portid);

				calc_sw_latency(m, portid);
				rte_pktmbuf_free(m);
			}

                        //clock_gettime(CLOCK_MONOTONIC, &end);
                        //long timeElapsed = diff_us(end, start);
                        //printf("\nlcore id=%d portid=%d packet#=%d elapsed=%ld(micro seconds)\n",lcore_id,portid,nb_rx,timeElapsed); 


		}
	}
}



static int
l2fwd_launch_one_lcore(__rte_unused void *dummy)
{
	l2fwd_main_loop();
	return 0;
}

/* display usage */
static void
l2fwd_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-q NQ]\n"
	       "  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
	       "  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
	       "  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n"
	       "  --[no-]mac-updating: Enable or disable MAC addresses updating (enabled by default)\n"
	       "      When enabled:\n"
	       "       - The source MAC address is replaced by the TX port MAC address\n"
	       "       - The destination MAC address is replaced by 02:00:00:00:00:TX_PORT_ID\n"
	       "  --portmap: Configure forwarding port pair mapping\n"
	       "	      Default: alternate port pairs\n\n",
	       prgname);
}

static int
l2fwd_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;

	return pm;
}

static int
l2fwd_parse_port_pair_config(const char *q_arg)
{
	enum fieldnames {
		FLD_PORT1 = 0,
		FLD_PORT2,
		_NUM_FLD
	};
	unsigned long int_fld[_NUM_FLD];
	const char *p, *p0 = q_arg;
	char *str_fld[_NUM_FLD];
	unsigned int size;
	char s[256];
	char *end;
	int i;

	nb_port_pair_params = 0;

	while ((p = strchr(p0, '(')) != NULL) {
		++p;
		p0 = strchr(p, ')');
		if (p0 == NULL)
			return -1;

		size = p0 - p;
		if (size >= sizeof(s))
			return -1;

		memcpy(s, p, size);
		s[size] = '\0';
		if (rte_strsplit(s, sizeof(s), str_fld,
				 _NUM_FLD, ',') != _NUM_FLD)
			return -1;
		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i] ||
			    int_fld[i] >= RTE_MAX_ETHPORTS)
				return -1;
		}
		if (nb_port_pair_params >= RTE_MAX_ETHPORTS/2) {
			printf("exceeded max number of port pair params: %hu\n",
				nb_port_pair_params);
			return -1;
		}
		port_pair_params_array[nb_port_pair_params].port[0] =
				(uint16_t)int_fld[FLD_PORT1];
		port_pair_params_array[nb_port_pair_params].port[1] =
				(uint16_t)int_fld[FLD_PORT2];
		++nb_port_pair_params;
	}
	port_pair_params = port_pair_params_array;
	return 0;
}

static unsigned int
l2fwd_parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	if (n == 0)
		return 0;
	if (n >= MAX_RX_QUEUE_PER_LCORE)
		return 0;

	return n;
}

static int
l2fwd_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n >= MAX_TIMER_PERIOD)
		return -1;

	return n;
}

static const char short_options[] =
	"p:"  /* portmask */
	"q:"  /* number of queues */
	"T:"  /* timer period */
	;

#define CMD_LINE_OPT_MAC_UPDATING "mac-updating"
#define CMD_LINE_OPT_NO_MAC_UPDATING "no-mac-updating"
#define CMD_LINE_OPT_PORTMAP_CONFIG "portmap"

enum {
	/* long options mapped to a short option */

	/* first long only option value must be >= 256, so that we won't
	 * conflict with short options */
	CMD_LINE_OPT_MIN_NUM = 256,
	CMD_LINE_OPT_PORTMAP_NUM,
};

static const struct option lgopts[] = {
	{ CMD_LINE_OPT_MAC_UPDATING, no_argument, &mac_updating, 1},
	{ CMD_LINE_OPT_NO_MAC_UPDATING, no_argument, &mac_updating, 0},
	{ CMD_LINE_OPT_PORTMAP_CONFIG, 1, 0, CMD_LINE_OPT_PORTMAP_NUM},
	{NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application */
static int
l2fwd_parse_args(int argc, char **argv)
{
	int opt, ret, timer_secs;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	argvopt = argv;
	port_pair_params = NULL;

	while ((opt = getopt_long(argc, argvopt, short_options,
				  lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'p':
			l2fwd_enabled_port_mask = l2fwd_parse_portmask(optarg);
			if (l2fwd_enabled_port_mask == 0) {
				printf("invalid portmask\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		/* nqueue */
		case 'q':
			l2fwd_rx_queue_per_lcore = l2fwd_parse_nqueue(optarg);
			if (l2fwd_rx_queue_per_lcore == 0) {
				printf("invalid queue number\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		/* timer period */
		case 'T':
			timer_secs = l2fwd_parse_timer_period(optarg);
			if (timer_secs < 0) {
				printf("invalid timer period\n");
				l2fwd_usage(prgname);
				return -1;
			}
			timer_period = timer_secs;
			break;

		/* long options */
		case CMD_LINE_OPT_PORTMAP_NUM:
			ret = l2fwd_parse_port_pair_config(optarg);
			if (ret) {
				fprintf(stderr, "Invalid config\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		default:
			l2fwd_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 1; /* reset getopt lib */
	return ret;
}

/*
 * Check port pair config with enabled port mask,
 * and for valid port pair combinations.
 */
static int
check_port_pair_config(void)
{
	uint32_t port_pair_config_mask = 0;
	uint32_t port_pair_mask = 0;
	uint16_t index, i, portid;

	for (index = 0; index < nb_port_pair_params; index++) {
		port_pair_mask = 0;

		for (i = 0; i < NUM_PORTS; i++)  {
			portid = port_pair_params[index].port[i];
			if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
				printf("port %u is not enabled in port mask\n",
				       portid);
				return -1;
			}
			if (!rte_eth_dev_is_valid_port(portid)) {
				printf("port %u is not present on the board\n",
				       portid);
				return -1;
			}

			port_pair_mask |= 1 << portid;
		}

		if (port_pair_config_mask & port_pair_mask) {
			printf("port %u is used in other port pairs\n", portid);
			return -1;
		}
		port_pair_config_mask |= port_pair_mask;
	}

	l2fwd_enabled_port_mask &= port_pair_config_mask;

	return 0;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;
	int ret;
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (force_quit)
				return;
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1)
					printf("Port %u link get failed: %s\n",
						portid, rte_strerror(-ret));
				continue;
			}
			/* print link status if flag set */
			if (print_flag == 1) {
				rte_eth_link_to_str(link_status_text,
					sizeof(link_status_text), &link);
				printf("Port %d %s\n", portid,
				       link_status_text);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

int
main(int argc, char **argv)
{
	struct lcore_queue_conf *qconf;
	int ret;
	uint16_t nb_ports;
	uint16_t nb_ports_available = 0;
	uint16_t portid, last_port;
	unsigned lcore_id, rx_lcore_id;
	unsigned nb_ports_in_mask = 0;
	unsigned int nb_lcores = 0;
	unsigned int nb_mbufs;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* parse application arguments (after the EAL ones) */
	ret = l2fwd_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid L2FWD arguments\n");

	printf("MAC updating %s\n", mac_updating ? "enabled" : "disabled");

	/* convert to number of cycles */
	timer_period *= rte_get_timer_hz();

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	if (port_pair_params != NULL) {
		if (check_port_pair_config() < 0)
			rte_exit(EXIT_FAILURE, "Invalid port pair config\n");
	}

	/* check port mask to possible port mask */
	if (l2fwd_enabled_port_mask & ~((1 << nb_ports) - 1))
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n",
			(1 << nb_ports) - 1);

	/* reset l2fwd_dst_ports */
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
		l2fwd_dst_ports[portid] = 0;
	last_port = 0;

	/* populate destination port details */
	if (port_pair_params != NULL) {
		uint16_t idx, p;

		for (idx = 0; idx < (nb_port_pair_params << 1); idx++) {
			p = idx & 1;
			portid = port_pair_params[idx >> 1].port[p];
			l2fwd_dst_ports[portid] =
				port_pair_params[idx >> 1].port[p ^ 1];
		}
	} else {
		RTE_ETH_FOREACH_DEV(portid) {
			/* skip ports that are not enabled */
			if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
				continue;

			if (nb_ports_in_mask % 2) {
				l2fwd_dst_ports[portid] = last_port;
				l2fwd_dst_ports[last_port] = portid;
			} else {
				last_port = portid;
			}

			nb_ports_in_mask++;
		}
		if (nb_ports_in_mask % 2) {
			printf("Notice: odd number of ports in portmask.\n");
			l2fwd_dst_ports[last_port] = last_port;
		}
	}

	rx_lcore_id = 0;
	qconf = NULL;

	/* Initialize the port/queue configuration of each logical core */
	RTE_ETH_FOREACH_DEV(portid) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		/* get the lcore_id for this port */
		while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
		       lcore_queue_conf[rx_lcore_id].n_rx_port ==
		       l2fwd_rx_queue_per_lcore) {
			rx_lcore_id++;
			if (rx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}

		if (qconf != &lcore_queue_conf[rx_lcore_id]) {
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[rx_lcore_id];
			nb_lcores++;
		}

		qconf->rx_port_list[qconf->n_rx_port] = portid;
		qconf->n_rx_port++;
		printf("Lcore %u: RX port %u TX port %u\n", rx_lcore_id,
		       portid, l2fwd_dst_ports[portid]);
	}
        
        //exit(1); 
	nb_mbufs = RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST +
		nb_lcores * MEMPOOL_CACHE_SIZE), 8192U);

	/* create the mbuf pool */
	l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id());
	if (l2fwd_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	/* Initialise each port */
	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_rxconf rxq_conf;
		struct rte_eth_txconf txq_conf;
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_dev_info dev_info;

		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", portid);
			continue;
		}
		nb_ports_available++;

		/* init port */
		printf("Initializing port %u... ", portid);
		fflush(stdout);

		ret = rte_eth_dev_info_get(portid, &dev_info);
		if (ret != 0)
			rte_exit(EXIT_FAILURE,
				"Error during getting device (port %u) info: %s\n",
				portid, strerror(-ret));

		if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
			local_port_conf.txmode.offloads |=
				DEV_TX_OFFLOAD_MBUF_FAST_FREE;
		ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				  ret, portid);

		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
						       &nb_txd);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot adjust number of descriptors: err=%d, port=%u\n",
				 ret, portid);

		ret = rte_eth_macaddr_get(portid,
					  &l2fwd_ports_eth_addr[portid]);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot get MAC address: err=%d, port=%u\n",
				 ret, portid);

		/* init one RX queue */
		fflush(stdout);
		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = local_port_conf.rxmode.offloads;
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
					     rte_eth_dev_socket_id(portid),
					     &rxq_conf,
					     l2fwd_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
				  ret, portid);

		/* init one TX queue on each port */
		fflush(stdout);
		txq_conf = dev_info.default_txconf;
		txq_conf.offloads = local_port_conf.txmode.offloads;
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid),
				&txq_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
				ret, portid);

		/* Initialize TX buffers */
		tx_buffer[portid] = rte_zmalloc_socket("tx_buffer",
				RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
				rte_eth_dev_socket_id(portid));
		if (tx_buffer[portid] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
					portid);

		rte_eth_tx_buffer_init(tx_buffer[portid], MAX_PKT_BURST);

		ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[portid],
				rte_eth_tx_buffer_count_callback,
				&port_statistics[portid].dropped);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
			"Cannot set error callback for tx buffer on port %u\n",
				 portid);

		ret = rte_eth_dev_set_ptypes(portid, RTE_PTYPE_UNKNOWN, NULL,
					     0);
		if (ret < 0)
			printf("Port %u, Failed to disable Ptype parsing\n",
					portid);
		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				  ret, portid);

		printf("done: \n");

		ret = rte_eth_promiscuous_enable(portid);
		if (ret != 0)
			rte_exit(EXIT_FAILURE,
				 "rte_eth_promiscuous_enable:err=%s, port=%u\n",
				 rte_strerror(-ret), portid);

		printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
				portid,
				l2fwd_ports_eth_addr[portid].addr_bytes[0],
				l2fwd_ports_eth_addr[portid].addr_bytes[1],
				l2fwd_ports_eth_addr[portid].addr_bytes[2],
				l2fwd_ports_eth_addr[portid].addr_bytes[3],
				l2fwd_ports_eth_addr[portid].addr_bytes[4],
				l2fwd_ports_eth_addr[portid].addr_bytes[5]);

		/* initialize port stats */
		memset(&port_statistics, 0, sizeof(port_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
			"All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(l2fwd_enabled_port_mask);



	ret = 0;
	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, CALL_MAIN);
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}



	RTE_ETH_FOREACH_DEV(portid) {
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		ret = rte_eth_dev_stop(portid);
		if (ret != 0)
			printf("rte_eth_dev_stop: err=%d, port=%d\n",
			       ret, portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}
	printf("Bye...\n");

	return ret;
}

#include <cos_types.h>
#include <string.h>
#include <arpa/inet.h>
#include <netmgr.h>
#include <netshmem.h>
#include <perfdata.h>


void
cos_init(void)
{
	shm_bm_objid_t objid;
	struct netshmem_pkt_buf *obj;

	/* create current component's shmem */
	netshmem_create();

	netmgr_shmem_map(netshmem_get_shm_id());

	printc("Benchmark for the sync_lock_lo .\n");
}

#define ITERATION 1000
struct perfdata perf;
cycles_t result[ITERATION] = {};

int
main(void)
{
	printc("Running benchmark lock_lo...\n");

	int ret;
	u32_t ip;
	compid_t compid;
	u16_t port;
	shm_bm_objid_t objid;
	struct netshmem_pkt_buf *rx_obj;
	struct netshmem_pkt_buf *tx_obj;
	char *data;
	u16_t data_offset, data_len;
	u16_t remote_port;
	u32_t remote_addr;

	ret = 0;
	ip = inet_addr("10.10.1.2");
	compid = cos_compid();

    remote_addr = inet_addr("10.10.1.1"); 
    remote_port = 12345; 
	data_len = sizeof(rawData);
	/* we use comp id as UDP port, representing tenant id */
	assert(compid < (1 << 16));
	port	= (u16_t)compid;

	printc("tenant id:%d\n", port);
	ret = netmgr_udp_bind(ip, port);
	assert(ret == NETMGR_OK);

	int cnt = 0;
	int time_var = 0;
	perfdata_init(&perf, "lock interference worst case test", result, ITERATION);

	while(cnt != ITERATION ){
		tx_obj = shm_bm_alloc_net_pkt_buf(netshmem_get_shm(), &objid);
		assert(tx_obj);

		memcpy(netshmem_get_data_buf(tx_obj), rawData, data_len);

		time_var = netmgr_udp_shmem_write(objid, netshmem_get_data_offset(), data_len, remote_addr, remote_port);
		perfdata_add(&perf, time_var);

		shm_bm_free_net_pkt_buf(tx_obj);
		cnt++;
	}

	perfdata_calc(&perf);
	perfdata_print(&perf);
	printc("exiting main thread of lock_lo...\n");
	return 0;
}
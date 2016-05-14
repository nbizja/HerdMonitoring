#include "contiki.h"
#include "net/rime/rime.h"
#include "dev/leds.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

#define TMP102_READ_INTERVAL (CLOCK_SECOND)  // Poll the sensor every second
#define NUMBER_OF_COWS 5 //Number of cows
#define PACKET_TIME 0.15

PROCESS (herd_monitor_gateway, "Herd monitor - gateway");
AUTOSTART_PROCESSES (&herd_monitor_gateway);

	

static int RSSIarray[NUMBER_OF_COWS][NUMBER_OF_COWS];

static int int_cmp(int a[], int b[]) 
{ 
	int x = a[1];
    int y = b[1];
    if (x < y) return 1;
    else if (x > y) return -1;
    return 0;
} 

static void bsort(int arr[][2]) {
	int n = NUMBER_OF_COWS;
	int i,j;
	for (i = 0; i < (n-1); ++i)
	{
		for (j = 0; j < (n-i-1); ++j)
		{
			if (int_cmp(arr[j], arr[j+1]) > 0) {
				int swap1 = arr[j][0];
				int swap2 = arr[j][1];
				arr[j][0] = arr[j+1][0];
				arr[j][1] = arr[j+1][1];
				arr[j+1][0] = swap1;
				arr[j+1][1] = swap2;
			}
		}
	}
}

static int findPower(int arr[][2], int a) {
	int i;
	for (i = 0; i < NUMBER_OF_COWS; i++) {
		if (arr[i][0] == a) {
			return arr[i][1];
		}
	}
	return 0;
}

/*
	Computes clusters and broadcasts them to nodes.
*/
static void compute_clusters(int RSSI[NUMBER_OF_COWS][NUMBER_OF_COWS], struct broadcast_conn *conn)
{
	int i,j,c;
	int power[NUMBER_OF_COWS][2];

	//Creating power array. It displays number of neighbours of each node. 
	// power[2][1] --> number of neighbours of 3. node
	// power[2][0] --> 2
	for (i = 0; i < NUMBER_OF_COWS; i++) {
		power[i][0] = i;
		power[i][1] = 0;
		for (j = 0; j < NUMBER_OF_COWS; j++) {
			if (RSSI[i][j] <= 0) {
				power[i][1]++;

			}
		}
	}
	//roles: -1 --> head node ; i --> index of head of cluster that belongs to
	int roles[NUMBER_OF_COWS];

	//Sorting power by number of neighbours. First element is the one with the most neighbours.
	//After sorting: power[0][0] --> node with most neighbour (id = power[0][0] + 1)
	//               power[0][1] --> number of neighbours
	//power[0] --> possible cluster heads
	bsort(power);
	for (i = 0; i < NUMBER_OF_COWS; i++) {
		printf("%d %d\n",power[i][0],power[i][1]);
		roles[i] = -2;
	}

	//int clusters[NUMBER_OF_COWS][power[0][1]+1];
	int clusters[NUMBER_OF_COWS][NUMBER_OF_COWS];
	for (i = 0; i < NUMBER_OF_COWS; i++) {
		for (j = 0; j < NUMBER_OF_COWS; j++) {
			clusters[i][j] = -1;	
		}
	}
	int counter = 0;
	int counter2;

	for (c = 0; c < NUMBER_OF_COWS; c++) {
		counter2 = 1;
		i = power[c][0];
		if (roles[i] == -2) {
			roles[i] = -1;
			clusters[counter][0] = i;
			for (j = 0; j < NUMBER_OF_COWS; j++) {
				if (RSSI[i][j] <= 0 && j != i) {
					roles[j] = i;
					clusters[counter][counter2] = j;
					counter2++;
				}
			}
			counter++;
		}
	}
	printf("Clusters: \n");
	for (i = 0; i < NUMBER_OF_COWS; i++) {
		for (j = 0; j < NUMBER_OF_COWS; j++) {
			printf("[%d][%d] = %d ;", i,j, clusters[i][j]);	
		}
		printf("\n");
	}
	/*printf("GATEWAY broadcasted clusters....\n");
	for (i = 0; i < counter; i++) {
		printf("HEAD: %d -- NODES: ",clusters[i][0]+1);
		int p = findPower(power, clusters[i][0]);
		for (j = 1; j <= p; j++) {
			printf("%d ", clusters[i][j]+1);
		}
		printf("\n");
	} */


	packetbuf_copyfrom(clusters, sizeof(clusters));
    broadcast_send(conn);
}

static void init_power_received(struct unicast_conn *c, const linkaddr_t *from)
{
	int cow_id = from->u8[0];
	int * test = (int *)packetbuf_dataptr();

	int i;
	printf("Init message received from cow %d :", cow_id);

	for (i = 0; i < NUMBER_OF_COWS; i++) {
		RSSIarray[cow_id - 1][i] = *(test + i);
		printf("%d -> %d ; ", i, *(test + i));
	}
	printf("\n");

	char *ack = "Ack";
	packetbuf_copyfrom(ack, sizeof(ack));
  	unicast_send(c, from);
}

static void init_broadcast_recv()
{

}

static const struct unicast_callbacks unicast_callbacks = {init_power_received};
static struct unicast_conn uc;

static const struct broadcast_callbacks broadcast_call = {init_broadcast_recv};
static struct broadcast_conn broadcast;

PROCESS_THREAD (herd_monitor_gateway, ev, data)
{
  	PROCESS_EXITHANDLER(
  		unicast_close(&uc);
		broadcast_close(&broadcast);
	)

	PROCESS_BEGIN();

    static linkaddr_t addr; //nastavimo nas naslov na 0.0
    addr.u8[0] = 0;
    addr.u8[1] = 0;
	linkaddr_set_node_addr(&addr); 
	uint16_t shortaddr = (addr.u8[0] << 8) + addr.u8[1];
	uint8_t longaddr[8];
	memset(longaddr, 0, sizeof(longaddr));
	longaddr[0] = addr.u8[0];
	longaddr[1] = addr.u8[1];
	cc2420_set_pan_addr(IEEE802154_PANID, shortaddr, longaddr); //spremenimo naslov radia
  	static struct etimer et;
  	unicast_open(&uc, 146, &unicast_callbacks);
	printf("GATEWAY is waiting for initilization measurements....\n");

	etimer_set(&et, CLOCK_SECOND * 3 );
	PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

	unicast_close(&uc);

  	broadcast_open(&broadcast, 129, &broadcast_call);
	printf("GATEWAY is computing clusters....\n");
	compute_clusters(RSSIarray, &broadcast);
	broadcast_close(&broadcast);

	
  PROCESS_END ();
}		


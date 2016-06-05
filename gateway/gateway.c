#include "contiki.h"
#include "net/rime/rime.h"
#include "dev/leds.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "node-id.h"
#include <stdlib.h>

#define TMP102_READ_INTERVAL (CLOCK_SECOND)  // Poll the sensor every second
#define NUMBER_OF_COWS 15 //Number of cows
#define PACKET_TIME 0.3
#define COWS_IN_PACKET 5

PROCESS (herd_monitor_gateway, "Herd monitor - gateway");
AUTOSTART_PROCESSES (&herd_monitor_gateway);

static int battery_status_list[NUMBER_OF_COWS];
static int temperature_list[NUMBER_OF_COWS];
static int alarm[NUMBER_OF_COWS];
static int time_last_seen_array;
static int flag_last_seen;
static int alarm_mode;
static int restart_timer_last_seen;
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

float floor(float x)
{
  if(x >= 0.0f) {
    return (float)((int)x);
  } else {
    return (float)((int)x - 1);
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

    printf("RSSI ARRAY:\n");
    for (i = 0; i < NUMBER_OF_COWS; i++) {
      for (j = 0; j < NUMBER_OF_COWS; j++) {
        printf("%d ", RSSIarray[i][j]);
      }
      printf("\n");
    }

    //Creating power array. It displays number of neighbours of each node. 
    // power[2][1] --> number of neighbours of 3. node
    // power[2][0] --> 2
    for (i = 0; i < NUMBER_OF_COWS; i++) {
        power[i][0] = i;
        power[i][1] = 0;
        for (j = 0; j < NUMBER_OF_COWS; j++) {
            if (RSSI[i][j] < 0) {
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
        //printf("%d %d\n",power[i][0]+1,power[i][1]);
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

    int8_t clust[NUMBER_OF_COWS];

    for (i = 0; i < NUMBER_OF_COWS; i++) {
      clust[i] = 0;
    }


    for (i = 0; i < NUMBER_OF_COWS; i++) {
      int p = findPower(power, clusters[i][0]);
      if (p > 0) {
        printf("HEAD: %d -- NODES: ",clusters[i][0]+1);
        clust[i] = clusters[i][0] + 1;
        
        for (j = 1; j <= p; j++) {
            printf("%d ", clusters[i][j]+1);
        }
        printf("\n");
      }
        
    }
    packetbuf_copyfrom(clust, sizeof(clust));
    broadcast_send(conn);
    printf("Clusters sent  \n");
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
    //packetbuf_copyfrom(ack, sizeof(ack));
    //unicast_send(c, from);
}

static void checkForMissingData(int RSSIs[NUMBER_OF_COWS][NUMBER_OF_COWS])
{
  int i,j;
  int check[NUMBER_OF_COWS];
  for (i = 0; i < NUMBER_OF_COWS; i++) {
    check[i] = 0;
    for (j = 0; j < NUMBER_OF_COWS; j++) {
      if (RSSIs[i][j] < 0) {
        check[i] = 1;
        break;
      }
    }
    if (check[i] == 1) {
      alarm[i] = 0;
    }
  }
}

/*Method for updating RSSI array with the most recent data.
  We have to be careful about which data to copy: cluster heads / zeros.*/
static void updateRSSI(int RSSIs[NUMBER_OF_COWS][NUMBER_OF_COWS])
{
  int i,j;
  int check[NUMBER_OF_COWS];
  for (i = 0; i < NUMBER_OF_COWS; i++) {
    check[i] = 0;
    for (j = 0; j < NUMBER_OF_COWS; j++) {
      if (RSSIs[i][j] < 0) {
        check[i] = 1;
        break;
      }
    }
    if (check[i] == 1) {
      for (j = 0; j < NUMBER_OF_COWS; j++) {
        RSSIarray[i][j] = RSSIs[i][j];
      }
    }
  }
}

/*Gateway receives temperature and battery status data from heads of clusters.*/
static void init_data_received(struct unicast_conn *c, const linkaddr_t *from)
{
    int cow_id = from->u8[0];
    int i,j;
    int8_t (*data)[NUMBER_OF_COWS + 3] = (int8_t (*)[NUMBER_OF_COWS + 3])packetbuf_dataptr();
    int RSSIs[NUMBER_OF_COWS][NUMBER_OF_COWS];
    for (i = 0; i < NUMBER_OF_COWS; i++) {
      for (j = 0; j < NUMBER_OF_COWS; j++) {
        RSSIs[i][j] = 1;
      }
    }

    printf("Data received: \n");
    for (i = 0; i < COWS_IN_PACKET; i++) {
      int8_t *row = *(data + i);
      int8_t cow = *row;
      battery_status_list[cow-1] = *(row + 1);
      float mv = (battery_status_list[cow-1] * 2.500 * 2) / 4096;
      temperature_list[cow-1] = *(row + 2);
      printf("[%d] Bat:%i(%ld.%03dmV), Temp:%d, RSSI: ", cow, battery_status_list[cow-1], (long)mv,
       (unsigned)((mv - floor(mv)) * 1000), temperature_list[cow-1]);
      int j;
      for (j = 0; j < NUMBER_OF_COWS; j++) {
        RSSIs[cow-1][j] = *(row + j + 3);
        printf("%d, ", RSSIs[cow-1][j]);
        if ((j == (cow_id-1)) && (RSSIs[cow-1][j] < 0)) {
          RSSIs[cow_id-1][cow-1] = RSSIs[cow-1][j];
        }
      }
      printf("\n");
    }
    /*
    for (i = 0; i < NUMBER_OF_COWS; i++) {
      for (j = 0; j < NUMBER_OF_COWS; j++) {
        printf("%d ", RSSIs[i][j]);
      }
      printf("\n");
    }*/

    //Next 3 lines order must not change!
    checkForMissingData(RSSIs);
    updateRSSI(RSSIs);

    //Head cow sent the data, so we assume it is not lost.
    alarm[cow_id-1] = 0;
    restart_timer_last_seen = 1;
    for (i = 0; i < NUMBER_OF_COWS; i++) {
      //If the cow is not found, we do not reset.
      if (alarm[i] == 1) {
        restart_timer_last_seen = 0;
        if (alarm_mode == 1) {
          printf("Alarm mode: cow %d is missing!\n", i+1);
        }
        break;
      }
    }

    //Timer for alarm expired.
    if (flag_last_seen == 1) {
      if (restart_timer_last_seen == 0 && alarm_mode == 0) {
          printf("Alarm set due to missing cow!\n");
          alarm_mode = 1;
      }
      flag_last_seen = 0;
    }

   /*
  //if (battery_status_list[0] >= 0) {
    printf("Data received from head cow %d.\n", cow_id);
    for (i = 0; i < NUMBER_OF_COWS; i++) {
      printf("%d %d \n", battery_status_list[i], temperature_list[i]);
    }
  
  //}*/ 
}

static void init_broadcast_recv()
{

}

static const struct unicast_callbacks unicast_callbacks = {init_power_received};
static const struct unicast_callbacks unicast_callbacks_data = {init_data_received};
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

    restart_timer_last_seen = 1;
    flag_last_seen = 0;
    alarm_mode = 0;
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
    static struct etimer time_last_seen;
    static struct etimer round_timer;
    static struct etimer reclustering_timer;
    static struct etimer init_broadcast_timer;
    //etimer_set(&round_timer, CLOCK_SECOND *  PACKET_TIME * (NUMBER_OF_COWS + 1));
    static int flag = 0;
    unicast_open(&uc, 146, &unicast_callbacks);
    printf("GATEWAY is waiting for initilization measurements....\n");

    etimer_set(&et, CLOCK_SECOND * 2 * PACKET_TIME * (NUMBER_OF_COWS + 1));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    //Initializing alarm and time_last_seen array.
    int i;
    for (i = 0; i < NUMBER_OF_COWS; i++) {
      alarm[i] = 1;
    }

    unicast_close(&uc);

    while (1) {
      unicast_open(&uc, 146, &unicast_callbacks_data);

      etimer_set(&round_timer, CLOCK_SECOND *  PACKET_TIME * (NUMBER_OF_COWS + 1));
      etimer_set(&init_broadcast_timer, CLOCK_SECOND * PACKET_TIME * node_id);

      if (flag % 20 == 0) {

        unicast_close(&uc);
        broadcast_open(&broadcast, 129, &broadcast_call);
        printf("GATEWAY is computing clusters....\n");
        compute_clusters(RSSIarray, &broadcast);
        broadcast_close(&broadcast);
      }
      if (restart_timer_last_seen == 1) {
        printf("Alarm timer reset.\n");
        for (i = 0; i < NUMBER_OF_COWS; i++) {
          alarm[i] = 1;
        }
        alarm_mode = 0;
        etimer_set(&time_last_seen, CLOCK_SECOND * 30);
        restart_timer_last_seen = 0;
        flag_last_seen = 0;
      }
      if(etimer_expired(&time_last_seen))
        flag_last_seen = 1;

      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&round_timer));
      flag ++;
    }
    
  PROCESS_END ();
}       


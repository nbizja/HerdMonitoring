#include "contiki.h"
#include "dev/i2cmaster.h"  // Include IC driver
#include "dev/tmp102.h"     // Include sensor driver
#include "dev/cc2420/cc2420.h"
#include "net/rime/rime.h"
#include "net/rime/mesh.h"
#include "dev/button-sensor.h"
#include "dev/battery-sensor.h"
#include "dev/leds.h"
#include "node-id.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdbool.h>


#define TMP102_READ_INTERVAL (CLOCK_SECOND)  // Poll the sensor every second
#define NUMBER_OF_COWS 5 //Number of cows
#define NUMBER_OF_INIT_BROADCASTS 3 //Each node sends 3 broadcasts in the initialization phase
#define PACKET_TIME 0.15

PROCESS (herd_monitor_node, "Herd monitor - node");
AUTOSTART_PROCESSES (&herd_monitor_node);


static int neighbour_list[NUMBER_OF_COWS];

//0 -> node; 1 -> cluster head;
static int role = 0;
static int my_clusters[NUMBER_OF_COWS - 1];
static int num_of_my_clusters = 0;

float floor(float x)
{
  if(x >= 0.0f) {
    return (float)((int)x);
  } else {
    return (float)((int)x - 1);
  }
}

static void reset_neighbour_list()
{
  printf("Reseting list... \n");
  int i;

  for (i = 0; i < NUMBER_OF_COWS; i++) {
      neighbour_list[i] = 1; //Not a RSSI value (-100 - 0)
  }
}


static void init_broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
  int cow_id = from->u8[0];
  int rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
  neighbour_list[cow_id - 1] = rssi;

  printf("broadcast message received from cow %d with rssi %d \n",
         cow_id, rssi);
}

static void data_broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
  if (role == 1) {
      int cow_id = from->u8[0];

      printf("Battery status and temperature received from cow %d \n", cow_id);
  }

}

static void clustering_broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
    printf("Clustering results received!\n");

    int (*clusters)[NUMBER_OF_COWS] = (int (*)[NUMBER_OF_COWS])packetbuf_dataptr();

    int i,j;
    int k = 0;
    //Checking if am cluster head.
    for (i = 0; i < NUMBER_OF_COWS; i++) {
      int *cluster = *(clusters + i);
      int cluster_head = *(cluster) + 1;
      if (cluster_head == node_id) {
          role = 1;
          printf("I, node %d, am Cluster head and these are my nodes: ", node_id);
          for (j = 1; j < NUMBER_OF_COWS; j++) {
            int node = *(cluster + j) + 1;
            if (node == 0) {
              break;
            }
            printf("%d,",node);
            my_clusters[j] = node;
          }
          printf("\n");   
          break;
      }
      
      /*for (j = 1; j < NUMBER_OF_COWS; j++) {
        int node = *(cluster + j) + 1;
        if (node == 0 ) {
          break;
        }
        if (node == node_id) { //We save cluster heads of my clusters.
          my_clusters[k] = cluster_head;
          printf("cluster %d \n",cluster_head);
          k++;
          break;
        }
      }*/

    }
    broadcast_close(c);
    
}

static void init_send_to_gateway(struct unicast_conn *c)
{
    static linkaddr_t addr;
    addr.u8[0] = 0;
    addr.u8[1] = 0;
    packetbuf_copyfrom(neighbour_list, sizeof(neighbour_list));
    unicast_send(c, &addr);
 
    printf("Neighbour list sent to the gateway\n");
}

static bool init_timedout = true;

static void init_ack_received()
{
  init_timedout = false;  
}


static const struct broadcast_callbacks broadcast_call = {init_broadcast_recv};
static const struct broadcast_callbacks broadcast_data_call = {data_broadcast_recv};
static const struct broadcast_callbacks broadcast_clustering_call = {clustering_broadcast_recv};
static struct broadcast_conn broadcast;
static struct broadcast_conn broadcast_clustering;
static struct broadcast_conn broadcast_data;


static const struct unicast_callbacks unicast_callbacks = {init_ack_received};
static struct unicast_conn uc;

PROCESS_THREAD (herd_monitor_node, ev, data)
{
  PROCESS_EXITHANDLER(
    unicast_close(&uc);
    broadcast_close(&broadcast);
    broadcast_close(&broadcast_clustering);
    broadcast_close(&broadcast_data);
  )
  
  PROCESS_BEGIN();
    //Time [ms] for whole round of slots (+1 is for gateway)
  static int slot_round_time = CLOCK_SECOND *  PACKET_TIME * (NUMBER_OF_COWS + 1);

  static struct etimer et;
  static struct etimer round_timer;
  static struct etimer init_broadcast_timer;
  //printf("timer: %d \n", CLOCK_SECOND * PACKET_TIME * 0.00001 * node_id); 

  //Timer for battery and temperature. We send them every 30 seconds.
  static struct etimer battery_temp_timer;


  static int init_phase = 1;
  static int init_gateway_phase = 1;
  static int clustering_phase = 1;

  //We use this variable for deciding, if 30 seconds expired (we have to mesaure and send the data.)
  static int battery_temp_status = 1;
  static uint16_t battery_status = -1;
  static uint16_t temperature = -1;

  static int broadcast_data_open = 0;

  reset_neighbour_list();
  while(1) {
    etimer_set(&round_timer, slot_round_time);
    etimer_set(&init_broadcast_timer, CLOCK_SECOND * PACKET_TIME * node_id);

    //If 30 seconds expired; we have to set batteryStatus and
    if (battery_temp_status == 1) {
      etimer_set(&battery_temp_timer, 30 * CLOCK_SECOND);
      battery_temp_status = 0;
    }


    /**********************************************************
                    BATTERY AND TEMPERATURE
    ***********************************************************/
    if (etimer_expired(&battery_temp_timer)) {
      //Activating battery sensor.
      SENSORS_ACTIVATE(battery_sensor);
      battery_status = battery_sensor.value(0);


      float mv = (battery_status * 2.500 * 2) / 4096;
      temperature = (uint16_t)tmp102_read_temp_raw();
      battery_temp_status = 1;

      printf("Battery: %i (%ld.%03d mV),   temperature: %d  \n", battery_status, (long)mv,
       (unsigned)((mv - floor(mv)) * 1000), temperature);

      SENSORS_DEACTIVATE(battery_sensor);
    }

    if (init_phase == 1) {
      broadcast_open(&broadcast, 129, &broadcast_call);
    } else if (clustering_phase == 1) {
        //LISTENING FOR CLUSTERING RESULTS FROM GATEWAY
        printf("Listening for clustering results...\n");      
        broadcast_open(&broadcast_clustering, 129, &broadcast_clustering_call);
        clustering_phase = 0;
    }

    /***********************************************************
                    SENDING STUFF
    ************************************************************/
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&init_broadcast_timer));


    /**********************************************************
                    INITIALIZATION PHASE
    ***********************************************************/
    if (init_phase == 1) {

      printf("Waiting for my slot...\n"); 

      printf("Timer expired... \n");
      packetbuf_copyfrom("Initialization...\n", 6);
      broadcast_send(&broadcast);
      printf("broadcast message sent\n"); 
    
    } else if (init_gateway_phase == 1) {
      printf("Sending data to the gateway\n"); 

      unicast_open(&uc, 146, &unicast_callbacks);
     
      init_send_to_gateway(&uc);
      unicast_close(&uc);
      init_gateway_phase = 0;
    }

    /**********************************************************
                    INITIALIZATION FINISHED
    ***********************************************************/

    /*Waiting until node's time slot is on; 
      then it checks if it is the time to send the data (battery, temperature).
    ;*/
    if (role == 0) {
      if (battery_temp_status == 1) {
        broadcast_open(&broadcast_data, 129, &broadcast_data_call);
        uint16_t packet[2];
        packet[0] = battery_status;
        packet[1] = temperature;
        packetbuf_copyfrom(packet, sizeof(packet));
        broadcast_send(&broadcast_data);
        broadcast_close(&broadcast_data);  
        printf("Temperature and battery status broadcast message sent.\n"); 
      }
    }


    /* Optimization option: listening only, when members of cluster have their time slot.
       Array has to be sorted for that purpose.
       If 2 members are sequential, connection should not close in beetwen.*/
    if (role == 1 && broadcast_data_open == 0) {     
        printf("Listening to broadcast data (temperature, battery).\n"); 
        broadcast_open(&broadcast_data, 129, &broadcast_data_call);
        broadcast_data_open = 1;
    }

    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&round_timer));
    if (init_phase == 1) {
      printf("Closing broadcast\n");

      broadcast_close(&broadcast);  
      init_phase = 0;
    }
  }
  
  PROCESS_END ();
}


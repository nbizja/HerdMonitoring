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
#define PACKET_TIME 0.15

PROCESS (herd_monitor_node, "Herd monitor - node");
AUTOSTART_PROCESSES (&herd_monitor_node);

  /*
  values of TXPOWER
  0x03 -> -18 dBm
  0x2C -> -7 dBm
  0x88 -> -4 dBm
  0x81 -> -2 dBm
  0x32 -> 0 dBm
  0x13 -> 1 dBm
  0xAB -> 2 dBm
  0xF2 -> 3 dBm
  0xF7 -> 5 dBm
*/
static int TXPOWER[9] = {0x03, 0x2C, 0x88, 0x81, 0x32, 0x13, 0xAB, 0xF7}; 
static int power_index = 5; //Default tx power is 1dBm

static int neighbour_list[NUMBER_OF_COWS];

// 1 = init_phase, 2 = init_gateway_phase, 3 = clustering phase, 4 = normal mode, 5 = panic
static int mode_of_operation = 1;
//0 -> node; 1 -> cluster head;
static int role = 0;
static int my_clusters[NUMBER_OF_COWS - 1];
static int num_of_my_clusters = 0;

//Cluster head saves received data from nodes in these arrays
static int cluster_head_rssi_data[NUMBER_OF_COWS][NUMBER_OF_COWS];
static int cluster_head_battery_data[NUMBER_OF_COWS];
static int cluster_head_temperature_data[NUMBER_OF_COWS];

// 1 = standing, 2 = moving slowly, 3 = running 
static int motion_status = 0;

static int cluster_head_sends_only_rssi = 0;

//Every five rounds we listen for whole interval so we can gather RSSI from neighbours
static int rssi_round_counter = 0;
static int is_broadcast_open = 0;
static struct broadcast_conn broadcast;

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

//Cluster head receiving data from nodes
static void data_broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
    int cow_id = from->u8[0];
    printf("Battery, temperat and rssi list received from cow %d \n", cow_id);

    int * bat_temp = (int *)packetbuf_dataptr();

    cluster_head_battery_data[cow_id - 1] = *(bat_temp + 0);
    cluster_head_temperature_data[cow_id - 1] = *(bat_temp + 1);
    printf("CLUSTER HEAD RECEIVED DATA: %d, %d", 
      cluster_head_battery_data[cow_id - 1], 
      cluster_head_temperature_data[cow_id - 1]);
    int i;
    for (i = 2; i < NUMBER_OF_COWS + 2; i++) {
      printf(", %d", *(bat_temp + i));
      cluster_head_rssi_data[cow_id - 1][i - 2] = *(bat_temp + i);
    }
    printf("\n");
}

static void neighour_data_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
    printf("RSSI data received received!\n");

    int *neighbour_data = (int *)packetbuf_dataptr();
    int i;
    for (i = 0; i < NUMBER_OF_COWS; i++) {
      cluster_head_rssi_data[from->u8[0]][i] = *(neighbour_data + i);      
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
    mode_of_operation = 4;
    
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

static void cluster_head_sends_all_data_to_gateway(struct unicast_conn *c)
{
    static linkaddr_t addr;
    addr.u8[0] = 0;
    addr.u8[1] = 0;
    int toSend[NUMBER_OF_COWS][NUMBER_OF_COWS + 2];
    int i;
    for (i = 0; i < NUMBER_OF_COWS; i++) {
        toSend[i][0] = cluster_head_battery_data[i];
        toSend[i][1] = cluster_head_temperature_data[i];
        cluster_head_battery_data[i] = -1;
        cluster_head_temperature_data[i] = -1;
        int j;
        for(j = 0; j < NUMBER_OF_COWS; j++) {
          toSend[i][j + 2] = cluster_head_rssi_data[i][j];
          cluster_head_rssi_data[i][j] = 0;
        }
    }
    //We also have to send head's data.
    //toSend[node_id - 1][0] = b;
    //toSend[node_id - 1][1] = t;

    packetbuf_copyfrom(toSend, sizeof(toSend));
    unicast_send(c, &addr);
         
    printf("Cluster head sending temperature, battery and rssi data to the gateway.\n"); 
}

static void rssi_neighbours_send_to_gateway(struct unicast_conn *c)
{
    static linkaddr_t addr;
    addr.u8[0] = 0;
    addr.u8[1] = 0;
    packetbuf_copyfrom(cluster_head_rssi_data, sizeof(cluster_head_rssi_data));
    unicast_send(c, &addr);
}


static bool init_timedout = true;

static void init_ack_received()
{
  init_timedout = false;  
}

static void data_ack_received()
{

}

static void increase_txpower()
{
    if (power_index < 8) {
      power_index += 2;
      set_txpower(TXPOWER[power_index]);
    }
}

static void decrease_txpower()
{
    if (power_index > 0) {
      power_index--;
      set_txpower(TXPOWER[power_index]);
    }
}

static void open_broadcast(struct broadcast_callbacks *cl)
{
  if (is_broadcast_open == 0) {
    is_broadcast_open = 1;
    broadcast_open(&broadcast, 129, cl);
    printf("Broadcast is open... \n");    
  }
}
static void close_broadcast()
{
     broadcast_close(&broadcast);
     is_broadcast_open = 0;
     printf("Broadcast closed... \n");
    
}

static void normal_mode_rssi_call() 
{
    printf("Broadcast is open... \n");    

}

static const struct broadcast_callbacks broadcast_call = {init_broadcast_recv};
static const struct broadcast_callbacks broadcast_data_call = {data_broadcast_recv};
static const struct broadcast_callbacks broadcast_clustering_call = {clustering_broadcast_recv};


static const struct unicast_callbacks unicast_callbacks = {init_ack_received};
static const struct unicast_callbacks unicast_callbacks_data = {data_ack_received};
static struct unicast_conn uc;

PROCESS_THREAD (herd_monitor_node, ev, data)
{
  PROCESS_EXITHANDLER(
    unicast_close(&uc);
    broadcast_close(&broadcast);
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



  //We use this variable for deciding, if 30 seconds expired (we have to mesaure and send the data.)
  static int battery_temp_status = 1;
  static int battery_status = -1;
  static int temperature = -1;
  static int i;



  //Initializing tables of data. Head cluster uses this table.
  for (i = 0; i < NUMBER_OF_COWS; i++)
  {
    cluster_head_battery_data[i] = -1;
    cluster_head_temperature_data[i] = -1;
  } 

  static int broadcast_data_open = 0;

  reset_neighbour_list();
  while(1) {
    etimer_set(&round_timer, slot_round_time);
    etimer_set(&init_broadcast_timer, CLOCK_SECOND * PACKET_TIME * node_id);

    //If 30 seconds expired; we have to set batteryStatus and
    if (battery_temp_status == 1 && mode_of_operation == 4) {
      etimer_set(&battery_temp_timer, 30 * CLOCK_SECOND);
      battery_temp_status = 0;
    }

    //Every 5 intervals we listen for whole interval
    if (rssi_round_counter == 5 && mode_of_operation == 4) {
      open_broadcast(&broadcast_call);
    }

    /**********************************************************
                    MOTION SENSING
    ***********************************************************/
    if (rssi_round_counter == 5) {
      motion_status = node_id % 3;
    }
    /**********************************************************
                    BATTERY AND TEMPERATURE
    ***********************************************************/
    if (etimer_expired(&battery_temp_timer) && mode_of_operation == 4) {
      //Activating battery sensor.
      SENSORS_ACTIVATE(battery_sensor);
      battery_status = battery_sensor.value(0);


      float mv = (battery_status * 2.500 * 2) / 4096;
      temperature = (int)tmp102_read_temp_raw();
      battery_temp_status = 1;

      printf("Battery: %i (%ld.%03d mV),   temperature: %d  \n", battery_status, (long)mv,
       (unsigned)((mv - floor(mv)) * 1000), temperature);

      SENSORS_DEACTIVATE(battery_sensor);
    }
    //Init phase
    if (mode_of_operation == 1) {
      open_broadcast(&broadcast_call);
    }

    /***********************************************************
                    SENDING STUFF
    ************************************************************/
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&init_broadcast_timer));


    /**********************************************************
                    INITIALIZATION PHASE
    ***********************************************************/
    if (mode_of_operation == 1) {

      printf("Waiting for my slot...\n"); 

      printf("Timer expired... \n");
      packetbuf_copyfrom("Initialization...\n", 6);
      broadcast_send(&broadcast);
      printf("broadcast message sent\n"); 
    
    } else if (mode_of_operation == 2) {
      printf("Sending data to the gateway\n"); 

      unicast_open(&uc, 146, &unicast_callbacks);
     
      init_send_to_gateway(&uc);
      unicast_close(&uc);
      
      //LISTENING FOR CLUSTERING RESULTS FROM GATEWAY
      printf("Listening for clustering results...\n");    
      open_broadcast(&broadcast_clustering_call);
      mode_of_operation = 0;
    }

    /**********************************************************
                    INITIALIZATION FINISHED
    ***********************************************************/

    /**********************************************************
                    SENDING DATA TO HEAD CLUSTER
    ***********************************************************/
    /*Waiting until node's time slot is on; 
      then it checks if it is the time to send the data (battery, temperature). ;*/
    if (role == 0 && mode_of_operation == 4) {
      // Fixed packet size. packet[0] = battery, packet[1] = temp status, 
      // packet[2] = motion status, packet[3: 3+NUMBER_OF_COWS] = RSSI of neighbours 
      int packet[NUMBER_OF_COWS + 3];

      packet[0] = battery_status;
      packet[1] = temperature;
      packet[2] = motion_status;

      int ri;
      for (ri = 3; ri < NUMBER_OF_COWS + 3; ri++) {
         packet[ri] = neighbour_list[ri - 3];
      }

      open_broadcast(&broadcast_data_call);
      packetbuf_copyfrom(packet, sizeof(packet));
      broadcast_send(&broadcast);
      printf("Normal mode - broadcast message sent. %d, %d, %d \n",
       battery_status, temperature, motion_status);

      close_broadcast(); 
    }

    /**********************************************************
                    HEAD CLUSTER IS LISTENING
    ***********************************************************/
    /* Optimization option: listening only, when members of cluster have their time slot.
       Array has to be sorted for that purpose.
       If 2 members are sequential, connection should not close in beetwen.*/
    if (role == 1 && mode_of_operation == 4) {     
        printf("Listening to broadcast data (temperature, battery).\n"); 
        open_broadcast(&broadcast_data_call);
    }

    /**********************************************************
          HEAD CLUSTER CHECKS FOR DATA TO SEND TO GATEWAY
    ***********************************************************/
    if (role == 1 && mode_of_operation == 4) {
      if (rssi_round_counter == 5) {
        cluster_head_sends_only_rssi = 1;
      }
      int ctr = 0;
      for (i = 0; i < NUMBER_OF_COWS; i++) {
        if (cluster_head_temperature_data[i] != -1) {
          ctr++;
        }
        if (cluster_head_battery_data[i] != -1) {
          ctr++;
        }
      }
      close_broadcast();
      printf("Unicast - Sending data to the gateway.\n"); 

      unicast_open(&uc, 146, &unicast_callbacks_data);

      cluster_head_sends_all_data_to_gateway(&uc);
      
      //We close the connection to the gateway and start listening again for
      //broadcasts of nodes.
      unicast_close(&uc);
      open_broadcast(&broadcast_data_call);
    }


    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&round_timer));
    if (mode_of_operation == 1) {
      close_broadcast(); 
      mode_of_operation++;
    } else if (mode_of_operation == 4) {
      if (rssi_round_counter == 5) {
        rssi_round_counter = 0;
      } else {
        rssi_round_counter++;
      }
    }

  }
  
  PROCESS_END ();
}


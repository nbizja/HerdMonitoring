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
#define NUMBER_OF_COWS 15 //Number of cows
#define PACKET_TIME 0.3
#define COWS_IN_PACKET 5

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
static int TXPOWER[9] = {3, 7, 11, 15, 19, 23, 27, 31}; 
static int power_index = 7; //Default and max tx power is 0dBm

static int neighbour_list[NUMBER_OF_COWS];

// 1 = init_phase, 2 = init_gateway_phase, 3 = clustering phase, 4 = normal mode, 5 = panic
static int mode_of_operation = 1;
//0 -> node; 1 -> cluster head;
static int role = 0;

//Cluster head saves received data from nodes in these arrays
static int cluster_head_rssi_data[NUMBER_OF_COWS][NUMBER_OF_COWS];
static int cluster_head_battery_data[NUMBER_OF_COWS];
static int cluster_head_temperature_data[NUMBER_OF_COWS];
//Cluster head counts received packets from each cow. 
//This is broadcasted every 5th round (when nodes are listening for rssi) as one global acknowledgment.
static int cluster_head_ack_data[NUMBER_OF_COWS];
static int node_power_management_flag; //-1 = decrease, 0 = leave as is, 1 = increase, 2 = full power

// 1 = standing, 2 = moving slowly, 3 = running 
//static int motion_status = 0;

//Every five rounds we listen for whole interval so we can gather RSSI from neighbours
static int rssi_round_counter;
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

static void open_broadcast(const struct broadcast_callbacks *cl)
{
  if (is_broadcast_open == 0) {
    is_broadcast_open = 1;
    broadcast_open(&broadcast, 129, cl);
  }
}

static void close_broadcast()
{
  broadcast_close(&broadcast);
  is_broadcast_open = 0;
}

static void init_neighbour_bat_temp_rssi_list()
{
  printf("Reseting list... \n");
  int i;

  for (i = 0; i < NUMBER_OF_COWS; i++) {
      neighbour_list[i] = 1; //Not a RSSI value (-100 - 0)
      cluster_head_battery_data[i] = -1;
      cluster_head_temperature_data[i] = -1;
      int j;
      for (j = 0; j < NUMBER_OF_COWS; j++) {
        cluster_head_rssi_data[i][j] = 1;
      }
  }
}

static void full_txpower()
{
    power_index = 7;
    cc2420_set_txpower(TXPOWER[power_index]);
}

static void increase_txpower()
{
    if (++power_index > 7) {
      power_index = 7;
    }
    cc2420_set_txpower(TXPOWER[power_index]);
}

static void decrease_txpower()
{
    if (--power_index < 0) {
      power_index = 0;
    }
    cc2420_set_txpower(TXPOWER[power_index]);
}

static void parse_clustering_results(int8_t *cluster)
{
    int i;
    //Default role is 0 = regular node;
    role = 0;

    //Checking if am cluster head.
    for (i = 0; i < NUMBER_OF_COWS; i++) {
      //printf("CLUSTERS RECVD: %d\n", *(cluster+i));
      if (*(cluster + i) == node_id) {
          role = 1;
          printf("I, node %d, am Cluster head!\n", node_id);
      }
    }
    if (role == 0) {
        printf("I, node %d, am regular node!\n", node_id);
    }
}

//This function serves two purposes. It measures rssi from neighbours and
//it accepts acknowledgment from cluster head.
//This happens every 5th round.
static void node_receiving_rssi_and_acknowledgment(const struct broadcast_conn *c, const linkaddr_t *from)
{
  int cow_id = from->u8[0];

  if (cow_id == 0) {
    int8_t *cluster = (int8_t *)packetbuf_dataptr();
    parse_clustering_results(cluster);

  }
  else {
    int rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
    neighbour_list[cow_id - 1] = rssi;

    int * ack_pointer = (int *)packetbuf_dataptr();
    node_power_management_flag = 0;

    int acknowledged_rssi = *(ack_pointer + node_id - 1);
    if (acknowledged_rssi > -110 && acknowledged_rssi < 0) { //This is almost 100% acknowledgment
      //With what RSSI did cluster head hear our packet
      //printf("Current txpower: %d\n", cc2420_get_txpower());
      printf("Reported rssi is: %d\n", acknowledged_rssi);
      if (acknowledged_rssi == -101) {
        node_power_management_flag = 2; //Packet failed. Full power!
        printf("Some packets failed. Full power!\n");

      } else if (acknowledged_rssi < -75) { //If some packet was not received successfully, then increase power.
        printf("Increasing tx power.\n");
        node_power_management_flag = 1;
      } else if (acknowledged_rssi > -65) {
        printf("Decreasing tx power.\n");
        node_power_management_flag = -1;
      } else {
        printf("Tx power at optimal level: %d\n", acknowledged_rssi);
      }
    }
    
  }
}

static void node_manage_power()
{
   if (node_power_management_flag == 2) {
      full_txpower();
   } else if (node_power_management_flag == 1) {
      increase_txpower();
   } else if (node_power_management_flag == -1) {
      decrease_txpower();
   }
   node_power_management_flag = 0;
}
//Cluster head receiving data from nodes
static void cluster_head_broadcast_recv(const struct broadcast_conn *c, const linkaddr_t *from)
{
    int cow_id = from->u8[0];
    if (cow_id == 0) {

      //printf("Clustering results received!\n");
      int8_t *clusters = (int8_t *)packetbuf_dataptr();
      parse_clustering_results(clusters);

    } else if (rssi_round_counter > 0) {
      //We increment received packet count. This will be broadcasted on every 5th interval.
      cluster_head_ack_data[cow_id - 1] = (int)packetbuf_attr(PACKETBUF_ATTR_RSSI);
      //printf("Battery, temperat and rssi list received from cow %d \n", cow_id);

      int8_t * bat_temp = (int8_t *)packetbuf_dataptr();

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
    } else {
      cluster_head_ack_data[cow_id - 1] = (int)packetbuf_attr(PACKETBUF_ATTR_RSSI);
    }
}

static void clustering_broadcast_recv(const struct broadcast_conn *c, const linkaddr_t *from)
{
    //printf("Clustering results received!\n");

    int8_t *cluster = (int8_t *)packetbuf_dataptr();

    parse_clustering_results(cluster);

    close_broadcast();
    mode_of_operation = 4;
}

static void init_send_to_gateway(const struct unicast_conn *c)
{
    static linkaddr_t addr;
    addr.u8[0] = 0;
    addr.u8[1] = 0;
    packetbuf_copyfrom(neighbour_list, sizeof(neighbour_list));
    unicast_send(c, &addr);
 
    printf("Neighbour list sent to the gateway\n");
}

static void cluster_head_sends_all_data_to_gateway(const struct unicast_conn *c, struct unicast_callbacks *callback)
{
    static linkaddr_t addr;
    addr.u8[0] = 0;
    addr.u8[1] = 0;

    //We send the data from five cows only, otherwise the packet is too big.
    //In each round we send data from different cows.
    //[0][0] = cow id,  [0][1] = bat, [0][2] = temp [0][3-15] = RSSI
    int8_t toSend[COWS_IN_PACKET][NUMBER_OF_COWS + 3];
    printf("Sending data: \n");

    int mod = rssi_round_counter % (NUMBER_OF_COWS / COWS_IN_PACKET);
    int8_t last_processed_cow = COWS_IN_PACKET * (mod + 1);
    if (last_processed_cow > NUMBER_OF_COWS) {
      last_processed_cow = NUMBER_OF_COWS;
    }
    
    int8_t j;
    int i = COWS_IN_PACKET * mod;

      for (j=0; i < last_processed_cow; i++,j++) {
        toSend[j][0] = (int8_t)(i + 1);
        toSend[j][1] = (int8_t)cluster_head_battery_data[i];
        toSend[j][2] = (int8_t)cluster_head_temperature_data[i];
        cluster_head_battery_data[i] = -1;
        cluster_head_temperature_data[i] = -1;
        printf("[%d] Bat: %d; Temp: %d, RSSI: ", toSend[j][0], toSend[j][1], toSend[j][2]);
        int k;
        for(k = 0; k < NUMBER_OF_COWS; k++) {
          toSend[j][k + 3] = (int8_t)cluster_head_rssi_data[i][k];
          printf("%d, ", toSend[j][k + 3]);
          
          cluster_head_rssi_data[i][k] = 1;
        }
        printf("\n");
      }

      unicast_open(c, 146, callback);
      packetbuf_copyfrom(toSend, sizeof(toSend));
      unicast_send(c, &addr);
      unicast_close(c);

}

static void cluster_head_sends_acknowledgments()
{
    packetbuf_copyfrom(cluster_head_ack_data, sizeof(cluster_head_ack_data));
    broadcast_send(&broadcast);
    //We reset the acknowledgments
    int i;
    for (i = 0; i < NUMBER_OF_COWS; i++)
    {
      cluster_head_ack_data[i] = -101;
    }
}

static bool init_timedout = true;

static void init_ack_received()
{
  init_timedout = false;  
}

static void data_ack_received()
{

}

static const struct broadcast_callbacks broadcast_call = {node_receiving_rssi_and_acknowledgment};
static const struct broadcast_callbacks broadcast_data_call = {cluster_head_broadcast_recv};
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
  rssi_round_counter = 0;
  //Time [ms] for whole round of slots (+1 is for gateway)
  static int slot_round_time = CLOCK_SECOND *  PACKET_TIME * (NUMBER_OF_COWS + 1);

  static struct etimer round_timer;
  static struct etimer init_broadcast_timer;
  //printf("timer: %d \n", CLOCK_SECOND * PACKET_TIME * 0.00001 * node_id); 

  //Timer for battery and temperature. We send them every 30 seconds.
  static struct etimer battery_temp_timer;

  static int battery_temp_status = 1;
  static int battery_status = -1;
  static int temperature = -1;
  node_power_management_flag = 0;

  static int not_a_rssi = 55;

  init_neighbour_bat_temp_rssi_list();
  while(1) {

    etimer_set(&round_timer, slot_round_time);
    etimer_set(&init_broadcast_timer, CLOCK_SECOND * PACKET_TIME * node_id);

    //If 30 seconds expired; we have to set batteryStatus and
    if (battery_temp_status == 1 && mode_of_operation == 4) {
      etimer_set(&battery_temp_timer, 30 * CLOCK_SECOND);
      battery_temp_status = 0;
    }

    //Every 5 intervals we listen for whole interval
    if (rssi_round_counter == 0 && mode_of_operation == 4) {
      
      int i;
      for(i = 0; i > NUMBER_OF_COWS; i++) {
        neighbour_list[i] = 1;
      }
      //We listen and compute RSSI 
      open_broadcast(&broadcast_call);
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

      //printf("Battery: %i (%ld.%03d mV),   temperature: %d  \n", battery_status, (long)mv,
      // (unsigned)((mv - floor(mv)) * 1000), temperature);

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
    node_manage_power();

    /**********************************************************
                    INITIALIZATION PHASE
    ***********************************************************/
    if (mode_of_operation == 1) {

      printf("Waiting for my slot...\n"); 
      printf("Timer expired... \n");
      packetbuf_copyfrom(node_manage_power, sizeof(not_a_rssi));
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
      open_broadcast(&broadcast_data_call);

      //5th we are sending only to gather rssi.
      if (rssi_round_counter == 0) {
        packetbuf_copyfrom(not_a_rssi, sizeof(not_a_rssi));
      } else {
        int packet[NUMBER_OF_COWS + 2];
        packet[0] = battery_status;
        packet[1] = temperature;
    
        printf("Node sends: Bat: %d, Temp: %d, RSSI: ",
         battery_status, temperature);

        int ri;
        for (ri = 2; ri < NUMBER_OF_COWS + 2; ri++) {
           packet[ri] = neighbour_list[ri - 2];
           printf("%d, ", packet[ri]);
        }
        printf("\n");
        packetbuf_copyfrom(packet, sizeof(packet));
      }

      broadcast_send(&broadcast);
      close_broadcast(); 
    }

    /**********************************************************
                    HEAD CLUSTER IS LISTENING
    ***********************************************************/
    /* Optimization option: listening only, when members of cluster have their time slot.
       Array has to be sorted for that purpose.
       If 2 members are sequential, connection should not close in beetwen.*/
    if (role == 1 && mode_of_operation == 4) {     
        //printf("Listening to broadcast data (temperature, battery).\n"); 
        open_broadcast(&broadcast_data_call);
    }

    /**********************************************************
          HEAD CLUSTER SENDS ALL DATA TO GATEWAY
    ***********************************************************/
    if (role == 1 && mode_of_operation == 4) {
      if (rssi_round_counter == 0) {
        printf("Broadcast - Cluster head sending acknowledgments.\n"); 
        cluster_head_sends_acknowledgments();
      } else {
          close_broadcast();
          printf("Unicast - Sending data to the gateway.\n"); 

          cluster_head_sends_all_data_to_gateway(&uc, &unicast_callbacks_data);
                    //We close the connection to the gateway and start listening again for
          //broadcasts of nodes.
          open_broadcast(&broadcast_data_call);
      }
    }


    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&round_timer));
    if (mode_of_operation == 1) { //Init mode
      close_broadcast(); 
      mode_of_operation++;
    } else if (mode_of_operation == 4) { // Normal mode. Increment counter for RSSI listening round
        rssi_round_counter = ++rssi_round_counter % 5;
    }

  }
  
  PROCESS_END ();
}

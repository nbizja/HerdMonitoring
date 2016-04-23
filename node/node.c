#include "contiki.h"
#include "dev/i2cmaster.h"  // Include IC driver
#include "dev/tmp102.h"     // Include sensor driver
#include "dev/cc2420/cc2420.h"
#include "net/rime/rime.h"
#include "net/rime/mesh.h"
#include "dev/button-sensor.h"
#include "dev/leds.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdbool.h>


#define TMP102_READ_INTERVAL (CLOCK_SECOND)  // Poll the sensor every second
#define NUMBER_OF_COWS 15 //Number of cows
#define NUMBER_OF_INIT_BROADCASTS 3 //Each node sends 3 broadcasts in the initialization phase

PROCESS (herd_monitor_node, "Herd monitor - node");
AUTOSTART_PROCESSES (&herd_monitor_node);

static int8_t neighbour_list[NUMBER_OF_COWS + 1];

static void reset_neighbour_list()
{
	int i;

	for (i = 0; i < NUMBER_OF_COWS; i++) {
			neighbour_list[i] = 1; //Not a RSSI value (-100 - 0)
	}
}


static void init_broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
	int cow_id = from->u8[0];
	int8_t rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
	neighbour_list[cow_id - 1] = rssi;

  /*printf("broadcast message received from cow %d.%d with rssi %d : '%s'\n",
         from->u8[0],from->u8[1], rssi, (char *)packetbuf_dataptr());
   */	
}

static void init_send_to_gateway(struct unicast_conn *c)
{
    linkaddr_t addr;
    addr.u8[0] = 0;
    addr.u8[1] = 0;
    if(!linkaddr_cmp(&addr, &linkaddr_node_addr)) {
    	packetbuf_copyfrom(neighbour_list, 5);
      unicast_send(c, &addr);
    }
  printf("Neighbour list sent to the gateway\n");

}

static bool init_timedout = true;

static void init_ack_received()
{
	init_timedout = false;	
}


PROCESS_THREAD (herd_monitor_node, ev, data)
{
  static struct etimer et;

	reset_neighbour_list();

  //PROCESS_EXITHANDLER(broadcast_close(&broadcast);)
	
	PROCESS_BEGIN();


  /**********************************************************
									INITIALIZATION PHASE
  ***********************************************************/
	static const struct broadcast_callbacks broadcast_call = {init_broadcast_recv};
	static struct broadcast_conn broadcast;

  broadcast_open(&broadcast, 129, &broadcast_call);

  static int i;
  for (i = 0; i < NUMBER_OF_INIT_BROADCASTS; i++) {
    etimer_set(&et, CLOCK_SECOND * 4 + random_rand() % 10);

  	PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    packetbuf_copyfrom("Initialization...", 6);
    broadcast_send(&broadcast);
    printf("broadcast message sent\n");
  }
  broadcast_close(&broadcast);
  printf("initialization broadcasting completed\n");

  // WAIT 5 SECONDS TO SEND GATHERED RSSI VALUES
  etimer_set(&et, CLOCK_SECOND * 5);
	PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

	static const struct unicast_callbacks unicast_callbacks = {init_ack_received};
	static struct unicast_conn uc;
  unicast_open(&uc, 146, &unicast_callbacks);
	init_send_to_gateway(&uc);

	//WAIT FOR ACK
  etimer_set(&et, CLOCK_SECOND * 3);
  if (init_timedout) {
  	printf("Failed to send neighbour_list. Retrying....\n");

		init_send_to_gateway(&uc);
  }



  while(1) {

  }


  PROCESS_END ();
}


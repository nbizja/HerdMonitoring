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

#define TMP102_READ_INTERVAL (CLOCK_SECOND)  // Poll the sensor every second
#define NUMBER_OF_COWS 15 //Number of cows
#define NUMBER_OF_INIT_BROADCASTS 3 //Each node sends 3 broadcasts in the initialization phase

PROCESS (herd_monitor_node, "Herd monitor - node");
AUTOSTART_PROCESSES (&herd_monitor_node);

static int8_t neighbour_list[NUMBER_OF_COWS + 1];

static void reset_neighbour_list()
{
	int i;

	for (i = 0; i <= NUMBER_OF_COWS; i++) {
			neighbour_list[i] = -100;
	}
}


static void
broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
	int cow_id = from->u8[0];
	int8_t rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
	neighbour_list[cow_id] = rssi;

  /*printf("broadcast message received from cow %d.%d with rssi %d : '%s'\n",
         from->u8[0],from->u8[1], rssi, (char *)packetbuf_dataptr());
   */	
}

static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
static struct broadcast_conn broadcast;



PROCESS_THREAD (herd_monitor_node, ev, data)
{
  static struct etimer et;

	reset_neighbour_list();

  PROCESS_EXITHANDLER(broadcast_close(&broadcast);)
	
	PROCESS_BEGIN();

  broadcast_open(&broadcast, 129, &broadcast_call);

  /**********************************************************
									INITIALIZATION PHASE
  ***********************************************************/

  static int i;
  for (i = 0; i < NUMBER_OF_INIT_BROADCASTS; i++) {
    etimer_set(&et, CLOCK_SECOND * 4 + random_rand() % 10);

  	PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    packetbuf_copyfrom("Initialization...", 6);
    broadcast_send(&broadcast);
    printf("broadcast message sent\n");
  }
  printf("initialization broadcasting completed\n");

  while(1) {

  }


  PROCESS_END ();
}


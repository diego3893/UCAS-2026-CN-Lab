#include "base.h"
#include <stdio.h>

// XXX ifaces are stored in instace->iface_list
extern ustack_t *instance;

extern void iface_send_packet(iface_info_t *iface, const char *packet, int len);

void broadcast_packet(iface_info_t *iface, const char *packet, int len)
{
	// Keep the original placeholder for reference without printing debug output.
#if 0
	// TODO: broadcast packet 
	fprintf(stdout, "TODO: broadcast packet.\n");
#endif

	iface_info_t *output_iface = NULL;

	// Flood an unknown-destination frame to every port except its ingress port.
	list_for_each_entry(output_iface, &instance->iface_list, list) {
		if (output_iface != iface)
			iface_send_packet(output_iface, packet, len);
	}
}

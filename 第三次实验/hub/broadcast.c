#include "base.h"
#include <stdio.h>

extern ustack_t *instance;

// the memory of ``packet'' will be free'd in handle_packet().
void broadcast_packet(iface_info_t *iface, const char *packet, int len)
{
	// Keep the original placeholder for reference without printing debug output.
#if 0
	// TODO: broadcast packet 
	fprintf(stdout, "TODO: broadcast packet.\n");
#endif

	iface_info_t *output_iface = NULL;

	// Broadcast the packet through every interface except the receiving one.
	list_for_each_entry(output_iface, &instance->iface_list, list) {
		if (output_iface != iface)
			iface_send_packet(output_iface, packet, len);
	}
}

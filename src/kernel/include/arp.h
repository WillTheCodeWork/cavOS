#include "nic_controller.h"
#include "types.h"

#ifndef ARP_H
#define ARP_H

typedef struct arpPacket {
  uint16_t hardware_type;
  uint16_t protocol_type;
  uint8_t  hardware_size;
  uint8_t  protocol_size;
  uint16_t opcode;
  uint8_t  sender_mac[6];
  uint8_t  sender_ip[4];
  uint8_t  target_mac[6];
  uint8_t  target_ip[4];
} __attribute__((packed)) arpPacket;

enum ARPOperation {
  ARP_OP_REQUEST = 0x01,
  ARP_OP_REPLY = 0x02,
};

#define ARP_HARDWARE_TYPE 0x0001
#define ARP_PROTOCOL_TYPE 0x0800

#define MAC_BYTE_SIZE 6  // MAC addresses are always 6 bytes long
#define IPv4_BYTE_SIZE 4 // IP(v4) addresses are always 4 bytes long

#define ARP_HARDWARE_SIZE MAC_BYTE_SIZE
#define ARP_PROTOCOL_SIZE IPv4_BYTE_SIZE

void netArpSend(NIC *nic, uint8_t *ip);
void netArpHandle(NIC *nic, arpPacket *packet);

#endif
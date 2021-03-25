#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

typedef unsigned char uchar;
#define BYTES_ICRC 4
#define BYTES_FCS 1


static inline void print_usage() {
    printf("Usage: proxy src_port dst_port\n");
}


struct ib_bth {
  uchar opcode; // indicates iba packet type, also extension headers to follow
  int solicited_event:1; // event should be generated by responder
  int mig_reg:1; // communicates 'migration state'
  int pad_count:2; // how many extra bytes to align payload to 4-byte boundary
  int tver:4; // transport header version
  unsigned short p_key;
  uchar fr_br_reserved;
  uchar dest_qp_1;
  uchar dest_qp_2;
  uchar dest_qp_3;
  int ack_rq:1;
  int reserved:7;
  uchar psn_1;
  uchar psn_2;
  uchar psn_3;
};


struct ib_pkt {
  struct ib_bth hdr;
  char payload[];
};

struct pkt_tail {
  unsigned int icrc;
  uchar fcs;
};

void print_ib_hdr(struct ib_bth* ib_hdr) {
  puts("IB Base Transport Header:");
  printf("\topcode: %d\n", ib_hdr->opcode);
  printf("\tsolicited event: %d, mig_req: %d, pad_count: %d, tver: %d\n", ib_hdr->solicited_event, \
         ib_hdr->mig_reg, ib_hdr->pad_count, ib_hdr->tver);
  printf("\tp_key: %d\n", ib_hdr->p_key);
  printf("\tfr_br_reserved: %d\n", ib_hdr->fr_br_reserved);
  printf("\tdest_qp_1: %d\n", ib_hdr->dest_qp_1);
  printf("\tdest_qp_2: %d\n", ib_hdr->dest_qp_2);
  printf("\tdest_qp_3: %d\n", ib_hdr->dest_qp_3);
  printf("\tack_rq: %d, reserved: %d\n", ib_hdr->ack_rq, ib_hdr->reserved);
  printf("\tpsn_1: %d\n", ib_hdr->psn_1);
  printf("\tpsn_2: %d\n", ib_hdr->psn_2);
  printf("\tpsn_3: %d\n", ib_hdr->psn_3);
}


char* deserialize_ib_pkt(char* buffer, int buffer_len) {
  struct ib_bth ib_header;
  char *start_addr = buffer;
  ib_header.opcode = *buffer;
  buffer++;
  ib_header.solicited_event = (*buffer) & 0x80;
  ib_header.mig_reg = (*buffer) & 0x40;
  ib_header.pad_count = (*buffer) & 0x30;
  ib_header.tver = (*buffer) & 0x0F;
  buffer++;
  memcpy(&(ib_header.p_key), buffer, 2);
  buffer += 2;
  ib_header.fr_br_reserved = *buffer;
  buffer++;
  ib_header.dest_qp_1 = *buffer;
  buffer++;
  ib_header.dest_qp_2 = *buffer;
  buffer++;
  ib_header.dest_qp_3 = *buffer;
  buffer++;
  ib_header.ack_rq = (*buffer) & 0x80;
  ib_header.reserved = (*buffer) & 0x7F;
  buffer++;
  ib_header.psn_1 = *buffer;
  buffer++;
  ib_header.psn_2 = *buffer;
  buffer++;
  ib_header.psn_3 = *buffer;
  buffer++;
  print_ib_hdr(&ib_header);
  int padding_bytes = ib_header.pad_count;
  char* padding = malloc(padding_bytes);
  for (int i = 0; i < padding_bytes; i++) buffer++;
  /*
   *  Now buffer should point to payload
   */
  int ib_payload_len = buffer_len - (sizeof(ib_header) + padding_bytes + \
      BYTES_ICRC + BYTES_FCS);
  printf("\tbuffer_len: %d, payload_len: %d\n", buffer_len, ib_payload_len);
  fflush(stdout);
  char* payload = malloc(ib_payload_len);
  memcpy(payload, buffer, ib_payload_len);

  return start_addr;
}


/**
 * basic idea: open two sockets 
 * -one for listening bound to addr1
 * -two for forwarding to addr2
 *
 *
 * */

int main (int argc, char** argv) {

    if (argc != 3) {
       print_usage();
       exit(1); 
    }

    int status;
    int sd_listen, sd_forward;
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    // AF_UNSPEC: use either IPv4 or IPv6
    // AF_INET: use IPv4
    // AF_INET6: use IPv6 
    hints.ai_family = AF_INET; 
    // use UDP
    hints.ai_socktype = SOCK_DGRAM;
    // fill in my own IP for me
    hints.ai_flags = AI_PASSIVE;

    status = getaddrinfo(NULL, argv[1], &hints, &res);
    if (status != 0) {
      perror("getaddrinfo");
      exit(1);
    }

    sd_listen = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sd_listen == -1) {
      perror("socket");
      exit(1);
    }
  
    status = bind(sd_listen, res->ai_addr, res->ai_addrlen);
    if (status != 0) {
      perror("bind");
      exit(1);
    }

    printf("ready to receive on port %s\n", argv[1]);
    /*
     *
     * now prepare the data structures to forward the message
     *
     **/
    freeaddrinfo(res);
    status = getaddrinfo(NULL, argv[2], &hints, &res);
    if (status != 0) {
      perror("getaddrinfo");
      exit(1);
    }
    sd_forward = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sd_listen == -1) {
      perror("socket");
      exit(1);
    }


    fflush(stdout);
    char buf[4096];
    char* msg;
    memset(&buf, 0, sizeof(buf));
    int received;
    while ((received = recvfrom(sd_listen, &buf, sizeof buf, 0, NULL, NULL)) != -1) {
      char* p = &buf[0];
      //while(*p != '\0') {
       // printf(" %x ", *p);
        //p++;
      //}
      fflush(stdout);

      /**
       *
       * parse the IB packet before forwarding it again
       *
       **/
      char* buffer_cp = &buf[0];
      deserialize_ib_pkt(buffer_cp, received);


      sendto(sd_forward, &buf, sizeof buf, 0, (struct sockaddr*) res->ai_addr, sizeof *(res->ai_addr));
      memset(&buf, 0, sizeof(buf));
    } 


    return 0;
}

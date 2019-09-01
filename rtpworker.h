#ifndef _RTP_WORKER_
#define _RTP_WORKER_

#include <stdint.h>
#include <srtp.h>
#include <stddef.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <netinet/ip.h> 

#define RTP_PKT_SIZE 1378
#define RTP_HEADER_LEN 12
#define RTP_PKT_BODY_SIZE (RTP_PKT_SIZE-RTP_HEADER_LEN)

struct srtp_hdr_t{
    unsigned char cc : 4;      /* CSRC count             */
    unsigned char x : 1;       /* header extension flag  */
    unsigned char p : 1;       /* padding flag           */
    unsigned char version : 2; /* protocol version       */
    unsigned char pt : 7;      /* payload type           */
    unsigned char m : 1;       /* marker bit             */
    uint16_t seq;              /* sequence number        */
    uint32_t ts;               /* timestamp              */
    uint32_t ssrc;             /* synchronization source */
};

struct rtp_msg_t {
    struct srtp_hdr_t header;
    uint8_t body[RTP_PKT_BODY_SIZE];
};

struct srtp_sender_context{
    struct rtp_msg_t message;//the message we want to send
    srtp_t srtp_ctx;
    int sock;
    struct sockaddr_in raddr;//receiver's address, need to parser from receiver_ip and receiver_port
};

void prepare_srtp_sender(const char* receiver_ip, const int receiver_port,
        const int ssrc, const uint8_t * input_key);
void destroy_srtp_sender();

/*
 * call this in camera encoder output callback
 */
int srtp_sender_callback(uint8_t* data, size_t length);

void srtp_backend_init();

#endif

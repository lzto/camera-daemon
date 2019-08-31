#ifndef _RTP_WORKER_
#define _RTP_WORKER_

#include <stdint.h>
#include <srtp.h>
#include <stddef.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <netinet/ip.h> 

struct srtp_sender_context{
    char* receiver_ip;
    int receiver_port;
    int ssrc;
    uint8_t* input_key;
    
    uint8_t* message;//the message we want to send
    size_t message_len;
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

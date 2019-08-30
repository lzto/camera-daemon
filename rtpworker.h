#ifndef _RTP_WORKER_
#define _RTP_WORKER_

#include <stdint.h>
#include <srtp.h>
#include <stddef.h>
#include <sys/types.h>

void prepare_srtp_sender(char* url, int ssrc, uint8_t * input_key,
        uint8_t* video_header, size_t video_header_length);
void destroy_srtp_sender();

/*
 * call this in camera encoder output callback
 */
int rtsp_sender_callback(uint8_t* data, size_t length);

#endif

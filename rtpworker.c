#include "rtpworker.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>


#include "util.h"

#define MAX_KEY_LEN 96

void prepare_srtp_sender(char* url, int ssrc, uint8_t * input_key,
        uint8_t* video_header, size_t video_header_length)
{
    //rtp_sender_t snd;
    srtp_policy_t policy;
    srtp_err_status_t status;
    char key[MAX_KEY_LEN];

#if 0
    /* initialize sender's rtp and srtp contexts */
    snd = rtp_sender_alloc();
    if (snd == NULL) {
        fprintf(stderr, "error: malloc() failed\n");
        exit(1);
    }
    rtp_sender_init(snd, sock, name, ssrc);
#endif
    /* set up the srtp policy and master key */

    srtp_crypto_policy_set_aes_cm_128_null_auth(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);

    policy.ssrc.type = ssrc_specific;
    policy.ssrc.value = ssrc;
    policy.key = (uint8_t *)key;
    policy.ekt = NULL;
    policy.next = NULL;
    policy.window_size = 128;
    policy.allow_repeat_tx = 0;
    policy.rtp.sec_serv = sec_serv_conf;
    policy.rtcp.sec_serv = sec_serv_none; /* we don't do RTCP anyway */

    /*
     * read key from base64 into an octet string
     */
    int pad;
    int expected_len = (policy.rtp.cipher_key_len * 4) / 3;
    int len = base64_string_to_octet_string(key, &pad, input_key,
            expected_len);
    if (pad != 0) {
        fprintf(stderr, "error: padding in base64 unexpected\n");
        exit(1);
    }

    /* check that hex string is the right length */
    if (len < expected_len) {
        fprintf(stderr, "error: too few digits in key/salt "
                "(should be %d digits, found %d)\n",
                expected_len, len);
        exit(1);
    }
    if ((int)strlen(input_key) > policy.rtp.cipher_key_len * 2) {
        fprintf(stderr, "error: too many digits in key/salt "
                "(should be %d hexadecimal digits, found %u)\n",
                policy.rtp.cipher_key_len * 2, (unsigned)strlen(input_key));
        exit(1);
    }

    printf("set master key/salt to %s/", octet_string_hex_string(key, 16));
    printf("%s\n", octet_string_hex_string(key + 16, 14));


    /*status = rtp_sender_init_srtp(snd, &policy);
    if (status) {
        fprintf(stderr, "error: srtp_create() failed with code %d\n",
                status);
        exit(1);
    }*/

}
void destroy_srtp_sender()
{
    //rtp_sender_deinit_srtp(snd);
    //rtp_sender_dealloc(snd);
}

/*
 * call this in camera encoder output callback
 */
int rtsp_sender_callback(uint8_t* data, size_t length)
{
    //rtp_sendto(snd, word, len);
}


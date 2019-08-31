#include "rtpworker.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "util.h"

#define MAX_KEY_LEN 96

static struct srtp_sender_context* srtpctx;

void prepare_srtp_sender(const char* receiver_ip, const int receiver_port,
        const int ssrc, const uint8_t * input_key,
        const uint8_t* video_header, const size_t video_header_length)
{
    //rtp_sender_t snd;
    srtp_policy_t policy;
    srtp_err_status_t status;
    uint8_t key[MAX_KEY_LEN];
    struct in_addr rcvr_addr;

    struct sockaddr_in local;

    printf("prepare srtp stream: %s:%d ssrc=%d, key=%s\n",
            receiver_ip, receiver_port, ssrc, input_key);

    srtpctx->receiver_ip = strdup(receiver_ip);
    srtpctx->receiver_port = receiver_port;
    srtpctx->ssrc = ssrc;
    srtpctx->input_key = strdup(input_key);
    srtpctx->video_header = malloc(video_header_length);
    memcpy(srtpctx->video_header, video_header, video_header_length);
    srtpctx->video_header_length = video_header_length;

   
    /* set up the srtp policy and master key */

    memset(&policy, 0, sizeof(srtp_policy_t));
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);

    policy.ssrc.type = ssrc_specific;
    policy.ssrc.value = ssrc;
    policy.key = key;
    policy.ekt = NULL;
    policy.next = NULL;

    //sec_serv_conf
    //sec_serv_auth
    //sec_serv_conf_and_auth

    policy.rtp.sec_serv = sec_serv_conf;
    policy.rtcp.sec_serv = sec_serv_conf;

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

    //printf("set master key/salt to %s/", octet_string_hex_string(key, 16));
    //printf("%s\n", octet_string_hex_string(key + 16, 14));


    inet_aton(receiver_ip, &rcvr_addr);
    srtpctx->sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    srtpctx->raddr.sin_addr = rcvr_addr;
    srtpctx->raddr.sin_family = PF_INET;
    srtpctx->raddr.sin_port = htons(receiver_port);

    //bind local port
    memset(&local, 0, sizeof(struct sockaddr_in));
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(receiver_port);
    if (bind(srtpctx->sock, (struct sockaddr *)&local,
                sizeof(struct sockaddr_in))<0) {
        perror("local port bind");
        exit(1);
    }


    srtp_err_status_t ret = srtp_create(&srtpctx->srtp_ctx, &policy);

    if (ret!=srtp_err_status_ok)
    {
        fprintf(stderr,"can't create srtp session: %s\n",
                (ret==srtp_err_status_alloc_fail)?
                "srtp_err_status_alloc_fail":"srtp_err_status_init_fail");
        exit(-1);
    }
}
void destroy_srtp_sender()
{
    srtp_dealloc(srtpctx->srtp_ctx);
}

/*
 * call this in camera encoder output callback
 */
int srtp_sender_callback(uint8_t* data, size_t length)
{
    int len = length;
    if (srtpctx->message_len<length)
        srtpctx->message = realloc(srtpctx->message ,length);
    memcpy(srtpctx->message, data, length);
    srtp_protect(srtpctx->srtp_ctx, srtpctx->message, &len);
    //printf("send udp msg len=%d\n", length);
    sendto(srtpctx->sock, srtpctx->message, length, 0,
            &srtpctx->raddr, sizeof(struct sockaddr_in));
}

void srtp_backend_init()
{
    printf("called srtp_init()\n");
    srtp_init();
    srtpctx = calloc(1,sizeof(struct srtp_sender_context));
    srtpctx->message = calloc(1,1024*1024);
    srtpctx->message_len = 1024*1024;

}


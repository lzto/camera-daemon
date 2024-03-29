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
        const int ssrc, const uint8_t * input_key)
{
    //rtp_sender_t snd;
    srtp_policy_t policy;
    srtp_err_status_t status;
    uint8_t key[MAX_KEY_LEN];
    struct in_addr rcvr_addr;

    struct sockaddr_in local;

    printf("prepare srtp stream: %s:%d ssrc=%d, key=%s\n",
            receiver_ip, receiver_port, ssrc, input_key);

    srtpctx->message.header.ssrc = htonl(ssrc);
    srtpctx->message.header.ts = 0;
    srtpctx->message.header.seq = 0;
    srtpctx->message.header.m = 0;
    srtpctx->message.header.pt = 99;//magic number
    srtpctx->message.header.version = 2;
    srtpctx->message.header.p = 0;
    srtpctx->message.header.x = 0;
    srtpctx->message.header.cc = 0;


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
 * breakdown data into multiple segments of size RTP_PKT_BODY_SIZE
 */
#if 1
int srtp_sender_callback(uint8_t* data, size_t length)
{
    int offset = 0;
    while (offset<length)
    {
        int body_len;
        int pkt_len;
        if ((offset+RTP_PKT_BODY_SIZE)<length)
        {
            body_len = RTP_PKT_BODY_SIZE;
            offset += RTP_PKT_BODY_SIZE;
        }
        else
        {
            body_len = length - offset;
            offset = length;
        }
        pkt_len = body_len + RTP_HEADER_LEN;
        memcpy(srtpctx->message.body, &data[offset], body_len);

        //update header
        struct srtp_hdr_t* hdr = &srtpctx->message.header;
        hdr->seq = ntohs(hdr->seq) + 1;
        hdr->seq = htons(hdr->seq);
        hdr->ts = ntohs(hdr->ts) + 1;
        hdr->ts = htons(hdr->ts);

        srtp_protect(srtpctx->srtp_ctx, &srtpctx->message.header, &pkt_len);
        sendto(srtpctx->sock, (void*)&srtpctx->message, pkt_len, 0,
                &srtpctx->raddr, sizeof(struct sockaddr_in));
    }
}

#else
int srtp_sender_callback(uint8_t* data, size_t length)
{
    int len = length;
    if (srtpctx->message_len<length)
        srtpctx->message = realloc(srtpctx->message ,length);
    memcpy(srtpctx->message, data, length);
    srtp_protect(srtpctx->srtp_ctx, srtpctx->message, &len);
    //printf("send udp msg len=%d\n", length);
    int ret = sendto(srtpctx->sock, srtpctx->message, length, 0,
            &srtpctx->raddr, sizeof(struct sockaddr_in));
    return ret;
}
#endif

void srtp_backend_init()
{
    printf("called srtp_init()\n");
    srtp_init();
    srtpctx = calloc(1,sizeof(struct srtp_sender_context));
}


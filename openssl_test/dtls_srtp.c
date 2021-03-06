
#include <tinylib/net/loop.h>
#include <tinylib/net/buffer.h>
#include <tinylib/util/log.h>

#include <srtp2/srtp.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#define _GNU_SOURCE
#define __USE_GNU
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <sys/epoll.h>
#define POLLIN EPOLLIN
#define POLLOUT EPOLLOUT
#define POLLHUP EPOLLHUP
#define POLLERR EPOLLERR

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/************************************************************/

typedef struct rtp_head
{
    uint16_t CC:4;
    uint16_t X:1;
    uint16_t P:1;
    uint16_t V:2;
    
    uint16_t PT:7;
    uint16_t M:1;            
    
    uint16_t SN;
    
    uint32_t timestamp;
    uint32_t ssrc;
}rtp_head_t;

static
int enable_dtls_srtp(SSL *ssl)
{
    int ssl_ret;
    const char *srtp_profiles = "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32";

    ssl_ret  = SSL_set_tlsext_use_srtp(ssl, srtp_profiles);
    if (ssl_ret != 0)
    {
        printf("SSL_set_tlsext_use_srtp() failed, ret: %d\n", ssl_ret);
        return -1;
    }

    return 0;
}

static
int setup_dtls_srtp(int is_client, SSL *ssl, srtp_t *srtp_tx, srtp_t *srtp_rx)
{
    unsigned char key_material[(SRTP_AES_128_KEY_LEN + SRTP_SALT_LEN) * 2];
    /* 前期协商候选的 profile 是 SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32
     * 都是 AES128 的 cipher, 其key和salt的长度事先已知
     *
     * (16 + 14) * 2
     */
    size_t key_material_len = (SRTP_AES_128_KEY_LEN + SRTP_SALT_LEN) * 2;

    unsigned char client_master_key_with_salt[SRTP_AES_128_KEY_LEN + SRTP_SALT_LEN];
    unsigned char server_master_key_with_salt[SRTP_AES_128_KEY_LEN + SRTP_SALT_LEN];

    const char *key_label = "EXTRACTOR-dtls_srtp";
    SSL_export_keying_material(ssl, key_material, key_material_len, key_label, strlen(key_label), NULL, 0, 0);
    
  #if defined(DTLS_SRTP_KEY_DUMP)
    printf("  dtls key material: 0x");
    for (int i = 0; i < key_material_len; ++i)
    {
        printf("%02X", key_material[i]);
    }
    printf("\n");
  #endif

    SRTP_PROTECTION_PROFILE *srtp_profile = SSL_get_selected_srtp_profile(ssl);
  #if defined(DTLS_SRTP_KEY_DUMP)
    printf("  negotiated srtp profile, id: %lu, name: %s\n", srtp_profile->id, srtp_profile->name);
  #endif

    /* key_material内容如下
     * client_master_key  16 bytes
     * server_master_key  16 bytes
     * client_salt        14 bytes
     * server_salt        14 bytes
     */

    memcpy(client_master_key_with_salt, key_material, SRTP_AES_128_KEY_LEN);
    memcpy(server_master_key_with_salt, (key_material+SRTP_AES_128_KEY_LEN), SRTP_AES_128_KEY_LEN);

    memcpy((client_master_key_with_salt+SRTP_AES_128_KEY_LEN), 
        (key_material+SRTP_AES_128_KEY_LEN+SRTP_AES_128_KEY_LEN), SRTP_SALT_LEN);
    memcpy(server_master_key_with_salt+SRTP_AES_128_KEY_LEN, 
        (key_material+SRTP_AES_128_KEY_LEN+SRTP_AES_128_KEY_LEN+SRTP_SALT_LEN), SRTP_SALT_LEN);

  #if defined(DTLS_SRTP_KEY_DUMP)
    printf("  client key_width_salt: ");
    for (int i = 0; i < sizeof(client_master_key_with_salt); ++i)
    {
        printf("%02X", client_master_key_with_salt[i]);
    }
    printf("\n");
    printf("  server key_width_salt: ");
    for (int i = 0; i < sizeof(server_master_key_with_salt); ++i)
    {
        printf("%02X", server_master_key_with_salt[i]);
    }
    printf("\n");
  #endif

    /**************************************************/

    srtp_err_status_t srtp_status;
    srtp_policy_t srtp_policy;
    memset(&srtp_policy, 0, sizeof(srtp_policy));
    if (srtp_profile->id == SRTP_AES128_CM_SHA1_80)
    {
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&srtp_policy.rtp);
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&srtp_policy.rtcp);
    }
    else /* if (srtp_profile->id == SRTP_AES128_CM_SHA1_32) */
    {
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&srtp_policy.rtp);
        /* srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&srtp_policy.rtcp); */
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&srtp_policy.rtcp);
    }

    if (is_client)
    {
        srtp_policy.ssrc.type = ssrc_any_outbound;
        srtp_policy.key = client_master_key_with_salt;
        srtp_status = srtp_create(srtp_tx, &srtp_policy);
        if (srtp_status != srtp_err_status_ok)
        {
            log_error("setup_dtls_srtp, client, srtp_create(srtp_tx) failed, ret: %d", srtp_status);
            *srtp_tx = NULL;
            return -1;
        }

        srtp_policy.ssrc.type = ssrc_any_inbound;
        srtp_policy.key = server_master_key_with_salt;
        srtp_status = srtp_create(srtp_rx, &srtp_policy);
        if (srtp_status != srtp_err_status_ok)
        {
            log_error("setup_dtls_srtp, client, srtp_create(srtp_rx) failed, ret: %d", srtp_status);
            srtp_dealloc(*srtp_tx);
            *srtp_tx = NULL;
            *srtp_rx = NULL;
            return -1;
        }
    }
    else
    {
        srtp_policy.ssrc.type = ssrc_any_outbound;
        srtp_policy.key = server_master_key_with_salt;
        srtp_status = srtp_create(srtp_tx, &srtp_policy);
        if (srtp_status != srtp_err_status_ok)
        {
            log_error("setup_dtls_srtp, server, srtp_create(srtp_tx) failed, ret: %d", srtp_status);
            *srtp_tx = NULL;
            return -1;
        }

        srtp_policy.ssrc.type = ssrc_any_inbound;
        srtp_policy.key = client_master_key_with_salt;
        srtp_status = srtp_create(srtp_rx, &srtp_policy);
        if (srtp_status != srtp_err_status_ok)
        {
            log_error("setup_dtls_srtp, server, srtp_create(srtp_rx) failed, ret: %d", srtp_status);
            srtp_dealloc(*srtp_tx);
            *srtp_tx = NULL;
            *srtp_rx = NULL;
            return -1;
        }
    }

    return 0;
}

void send_srtp_packet(srtp_t srtp, int fd, int sn, int pt, int ssrc)
{
    char packet[1024+SRTP_MAX_TRAILER_LEN];
    int len = 1024;

    RAND_bytes((unsigned char*)packet, len);

    rtp_head_t *rtp_head = (rtp_head_t*)packet;
    rtp_head->V = 2;
    rtp_head->P = 0;
    rtp_head->X = 0;
    rtp_head->CC = 0;
    rtp_head->M = 0;
    rtp_head->PT = pt;
    rtp_head->SN = sn;
    rtp_head->timestamp = 0;    
    rtp_head->ssrc = ssrc;

    srtp_err_status_t srtp_status = srtp_protect(srtp, packet, &len);
    if (srtp_status == srtp_err_status_ok)
    {
        write(fd, packet, len);
    }
    else
    {
        log_error("srtp_protect() failed, ret: %d", srtp_status);
    }

    return;
}

/************************************************************/

enum dtls_state
{
    DTLS_STATE_HANDSHAKE,
    DTLS_STATE_SNDRCV,
};

/************************************************************/

typedef struct dtls_client{
    loop_t *loop;

    int dtls_fd;
    channel_t *dtls_channel;

    int io_fd;
    channel_t *io_channel;

    SSL_CTX *ssl_ctx;
    SSL *ssl;
    enum dtls_state dtls_state;
    
    srtp_t srtp_tx;
    srtp_t srtp_rx;
    
    int rtp_packet_sn;
} dtls_client_t;

static
void on_dtls_client_dtls_io_event(int fd, int event, void* userdata)
{
    dtls_client_t *dtls_client = (dtls_client_t*)userdata;
    if (dtls_client->dtls_state == DTLS_STATE_HANDSHAKE)
    {
        if (event & (POLLHUP | POLLERR))
        {
            log_error("dtls client handshake failed: IO failure, sys error: %d", errno);
        }
        else
        {
            int ssl_ret;
            int ssl_error;

            ssl_ret = SSL_do_handshake(dtls_client->ssl);
            ssl_error = SSL_get_error(dtls_client->ssl, ssl_ret);
            if (ssl_ret == 1)
            {
                dtls_client->dtls_state = DTLS_STATE_SNDRCV;

                if (setup_dtls_srtp(1, dtls_client->ssl, &dtls_client->srtp_tx, &dtls_client->srtp_rx) == 0)
                {
            		log_info("=== dtls-srtp client handshake and setup OK ===");

                    send_srtp_packet(dtls_client->srtp_tx, dtls_client->io_fd, dtls_client->rtp_packet_sn, 96, 123456);
                    dtls_client->rtp_packet_sn++;
                }
            }
            else if (ssl_ret < 0 && (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE))
            {
                /* keep going and nothing todo */
            }
            else
            {
                log_error("dtls client handshake failed: fatal ssl error: %d, sys error: %d", ssl_error, errno);
            }
        }
    }
    else if (dtls_client->dtls_state == DTLS_STATE_SNDRCV)
    {
        /* TODO: read and unprotect srtp packet */
    }

    return;
}

static
void on_dtls_client_srtp_io_event(int fd, int event, void* userdata)
{
    dtls_client_t *dtls_client = (dtls_client_t*)userdata;
    char packet[1500];
    ssize_t ret;

    ret = read(dtls_client->io_fd, packet, sizeof(packet));
    if (ret <= 0)
    {
        printf("client read(%d), ret: %ld, errno: %d\n", dtls_client->io_fd, ret, errno);
        return;
    }

    int packet_len = ret;
    srtp_err_status_t srtp_status = srtp_unprotect(dtls_client->srtp_rx, packet, &packet_len);
    if (srtp_status == srtp_err_status_ok)
    {
        rtp_head_t *rtp_head = (rtp_head_t*)packet;
        log_info("client, rx srtp_len: %d, rtp_len: %d, RTP, PT: %u, SN: %u, TS: %u, SSRC: %u", ret, packet_len, 
            rtp_head->PT, rtp_head->SN, rtp_head->timestamp, rtp_head->ssrc);
    }
    else
    {
        log_error("client, srtp_unprotect() failed, ret: %d", srtp_status);
    }

    if (dtls_client->rtp_packet_sn < 3)
    {
        send_srtp_packet(dtls_client->srtp_tx, dtls_client->io_fd, dtls_client->rtp_packet_sn, 96, 123456);
        dtls_client->rtp_packet_sn++;
    }

    return;
}

static
int dtls_client_start(dtls_client_t* dtls_client, int dtls_fd, int io_fd, loop_t *loop)
{
    int ssl_ret;
    int ssl_error;

    dtls_client->loop = loop;

    dtls_client->dtls_fd = dtls_fd;
    dtls_client->dtls_channel = channel_new(dtls_fd, loop, on_dtls_client_dtls_io_event, dtls_client);
    channel_setevent(dtls_client->dtls_channel, POLLIN);

    dtls_client->io_fd = io_fd;
    dtls_client->io_channel = channel_new(io_fd, loop, on_dtls_client_srtp_io_event, dtls_client);
    channel_setevent(dtls_client->io_channel, POLLIN);

    dtls_client->dtls_state = DTLS_STATE_HANDSHAKE;

    dtls_client->ssl_ctx = SSL_CTX_new(DTLS_client_method());
    SSL_CTX_set_mode(dtls_client->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_AUTO_RETRY);
    dtls_client->ssl = SSL_new(dtls_client->ssl_ctx);
    SSL_set_fd(dtls_client->ssl, dtls_fd);
    
    enable_dtls_srtp(dtls_client->ssl);

    SSL_set_connect_state(dtls_client->ssl);
    ssl_ret = SSL_do_handshake(dtls_client->ssl);
    ssl_error = SSL_get_error(dtls_client->ssl, ssl_ret);
    if (ssl_ret == 1)
    {
        dtls_client->dtls_state = DTLS_STATE_SNDRCV;
        log_info("=== dtls client handshake OK ===");
        setup_dtls_srtp(1, dtls_client->ssl, &dtls_client->srtp_tx, &dtls_client->srtp_rx);
    }
    else if (ssl_ret < 0 && (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE))
    {
        /* keep going and nothing todo */
    }
    else
    {
        log_error("failed to start dtls client handshake: fatal ssl error: %d, sys error: %d", ssl_error, errno);
        return -1;
    }

    return 0;
}

static
void dtls_client_stop(dtls_client_t* dtls_client)
{
    if (dtls_client->dtls_state == DTLS_STATE_SNDRCV)
    {
        SSL_shutdown(dtls_client->ssl);
    }

    SSL_free(dtls_client->ssl);
    SSL_CTX_free(dtls_client->ssl_ctx);
    channel_detach(dtls_client->dtls_channel);
    channel_destroy(dtls_client->dtls_channel);
    channel_detach(dtls_client->io_channel);
    channel_destroy(dtls_client->io_channel);

    srtp_dealloc(dtls_client->srtp_tx);
    srtp_dealloc(dtls_client->srtp_rx);

    return;
}

/************************************************************/

typedef struct dtls_server{
    loop_t *loop;
    int dtls_fd;
    channel_t *dtls_channel;
    
    int io_fd;
    channel_t *io_channel;

    SSL_CTX *ssl_ctx;
    SSL *ssl;
    enum dtls_state dtls_state;

    srtp_t srtp_tx;
    srtp_t srtp_rx;
    int rtp_packet_sn;
} dtls_server_t;

static
void on_dtls_server_dtls_io_event(int dtls_fd, int event, void* userdata)
{
    dtls_server_t *dtls_server = (dtls_server_t*)userdata;
    int ssl_ret;
    int ssl_error;

    if (dtls_server->dtls_state == DTLS_STATE_HANDSHAKE)
    {
        if (event & (POLLHUP | POLLERR))
        {
            log_error("dtls server handshake failed: IO failure, sys error: %d", errno);
        }
        else
        {
            ssl_ret = SSL_do_handshake(dtls_server->ssl);
            ssl_error = SSL_get_error(dtls_server->ssl, ssl_ret);
            if (ssl_ret == 1)
            {
                dtls_server->dtls_state = DTLS_STATE_SNDRCV;
                if (setup_dtls_srtp(0, dtls_server->ssl, &dtls_server->srtp_tx, &dtls_server->srtp_rx) == 0)
                {
                    log_info("=== dtls-srtp server handshake and setup OK ===");
                    //send_srtp_packet(dtls_server->srtp_tx, dtls_server->io_fd, dtls_server->rtp_packet_sn, 97, 123457);
                    //dtls_server->rtp_packet_sn++;
                }
                else
                {
                    log_error("dtls server, failed to setup srtp");
                }
            }
            else if (ssl_ret < 0 && (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE))
            {
                /* keep going and nothing todo */
            }
            else
            {
                log_error("dtls server handshake failed: fatal ssl error: %d, sys error: %d", ssl_error, errno);
            }
        }
    }
    else if (dtls_server->dtls_state == DTLS_STATE_SNDRCV)
    {
        /* TODO */
    }

    return;
}

static
void on_dtls_server_srtp_io_event(int dtls_fd, int event, void* userdata)
{
    dtls_server_t *dtls_server = (dtls_server_t*)userdata;
    char packet[1500];
    ssize_t ret;

    ret = read(dtls_server->io_fd, packet, sizeof(packet));
    if (ret <= 0)
    {
        printf("server read(%d), ret: %ld, errno: %d\n", dtls_server->io_fd, ret, errno);
        return;
    }

    int packet_len = ret;
    srtp_err_status_t srtp_status = srtp_unprotect(dtls_server->srtp_rx, packet, &packet_len);
    if (srtp_status == srtp_err_status_ok)
    {
        rtp_head_t *rtp_head = (rtp_head_t*)packet;
        log_info("server, rx srtp_len: %d, rtp_len: %d, RTP, PT: %u, SN: %u, TS: %u, SSRC: %u", ret, packet_len, 
            rtp_head->PT, rtp_head->SN, rtp_head->timestamp, rtp_head->ssrc);
    }
    else
    {
        log_error("server, srtp_unprotect() failed, ret: %d", srtp_status);
    }

    if (dtls_server->rtp_packet_sn < 3)
    {
        send_srtp_packet(dtls_server->srtp_tx, dtls_server->io_fd, dtls_server->rtp_packet_sn, 97, 123457);
        dtls_server->rtp_packet_sn++;
    }

    return;
}

static
int dtls_server_start
(
    dtls_server_t* dtls_server, int dtls_fd, int io_fd, loop_t *loop,
    const char* ca_file, const char *private_key_file, const char *ca_pwd
)
{
    int ssl_ret;
    int ssl_error;
    
    dtls_server->loop = loop;

    dtls_server->dtls_fd = dtls_fd;
    dtls_server->dtls_channel = channel_new(dtls_fd, loop, on_dtls_server_dtls_io_event, dtls_server);
    channel_setevent(dtls_server->dtls_channel, POLLIN);

    dtls_server->io_fd = io_fd;
    dtls_server->io_channel = channel_new(io_fd, loop, on_dtls_server_srtp_io_event, dtls_server);
    channel_setevent(dtls_server->io_channel, POLLIN);

    dtls_server->dtls_state = DTLS_STATE_HANDSHAKE;

    dtls_server->ssl_ctx = SSL_CTX_new(DTLS_server_method());
    SSL_CTX_set_mode(dtls_server->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_AUTO_RETRY);
    dtls_server->ssl = SSL_new(dtls_server->ssl_ctx);

    SSL_set_fd(dtls_server->ssl, dtls_fd);
    enable_dtls_srtp(dtls_server->ssl);

    SSL_set_accept_state(dtls_server->ssl);

    ssl_ret = SSL_use_certificate_file(dtls_server->ssl, ca_file, SSL_FILETYPE_PEM);
    if (ssl_ret != 1)
    {
        ssl_error = SSL_get_error(dtls_server->ssl, ssl_ret);
        log_error("tls server: failed to load CA, ssl errno: %d", ssl_error);
        return -1;
    }

    if (private_key_file && (ssl_ret = SSL_use_PrivateKey_file(dtls_server->ssl, private_key_file, SSL_FILETYPE_PEM)) != 1)
    {
        ssl_error = SSL_get_error(dtls_server->ssl, ssl_ret);
        log_error("tls server: failed to load CA private key, ssl errno: %d", ssl_error);
        return -1;
    }

    ssl_ret = SSL_check_private_key(dtls_server->ssl);
    if (ssl_ret != 1)
    {
        ssl_error = SSL_get_error(dtls_server->ssl, ssl_ret);
        log_error("tls server: check ssl private key failed, ssl errno: %d", ssl_error);
        return -1;
    }

    return 0;
}

static
void dtls_server_stop(dtls_server_t* dtls_server)
{
    if (dtls_server->dtls_state == DTLS_STATE_SNDRCV)
    {
        SSL_shutdown(dtls_server->ssl);
    }

    SSL_free(dtls_server->ssl);
    SSL_CTX_free(dtls_server->ssl_ctx);
    channel_detach(dtls_server->dtls_channel);
    channel_destroy(dtls_server->dtls_channel);
    channel_detach(dtls_server->io_channel);
    channel_destroy(dtls_server->io_channel);

    srtp_dealloc(dtls_server->srtp_tx);
    srtp_dealloc(dtls_server->srtp_rx);

    return;
}

/************************************************************/

#include <signal.h>

loop_t *g_loop = NULL;

void on_interrupt(int signo)
{
    loop_quit(g_loop);
    return;
}

int main(int argc, char *argv[])
{
    int dtls_fds[2];
    int io_fds[2];
    loop_t *loop;

    dtls_client_t dtls_client;
    dtls_server_t dtls_server;

    const char *ca_file;
    const char *key_file;
    const char *ca_pwd;

  #if !defined(NDEBUG)
    ca_file = "/mnt/d/ca/cert.pem";
    key_file = "/mnt/d/ca/key.pem";
    ca_pwd = "sslselftest";
  #else
    if (argc < 4)
    {
        printf("usage: %s <ca file> <key file> <ca pwd>\n", argv[0]);
        return -1;
    }
    ca_file = argv[1]；
    key_file = argv[2];
    ca_pwd = argv[3];
  #endif

    //log_setlevel(LOG_LEVEL_NONE);

    SSL_library_init();
    SSL_load_error_strings();

    srtp_init();

    loop = loop_new(64);

    socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, dtls_fds);
    socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, io_fds);

    memset(&dtls_client, 0, sizeof(dtls_client));
    dtls_client_start(&dtls_client, dtls_fds[0], io_fds[0], loop);

    memset(&dtls_server, 0, sizeof(dtls_server));
    dtls_server_start(&dtls_server, dtls_fds[1], io_fds[1], loop, ca_file, key_file, ca_pwd);

    g_loop = loop;
    signal(SIGINT, on_interrupt);
    loop_loop(loop);

    dtls_client_stop(&dtls_client);
    dtls_server_stop(&dtls_server);

    close(dtls_fds[0]);
    close(dtls_fds[1]);
    close(io_fds[0]);
    close(io_fds[1]);
    
    loop_destroy(loop);

    srtp_shutdown();
    ERR_free_strings();

    return 0;
}
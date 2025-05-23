/* Copyright (C) The libssh2 project and its contributors.
 *
 * Sample showing how to do SCP transfers in a non-blocking manner.
 *
 * The sample code has default values for host name, user name, password
 * and path to copy, but you can specify them on the command line like:
 *
 * $ ./scp_nonblock 192.168.0.1 user password /tmp/secrets
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "libssh2_setup.h"
#include <libssh2.h>

#ifdef _WIN32
#define write(f, b, c)  write((f), (b), (unsigned int)(c))
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <stdio.h>

static const char *pubkey = "/home/username/.ssh/id_rsa.pub";
static const char *privkey = "/home/username/.ssh/id_rsa";
static const char *username = "username";
static const char *password = "password";
static const char *scppath = "/tmp/TEST";

#ifdef HAVE_GETTIMEOFDAY
/* diff in ms */
static long tvdiff(struct timeval newer, struct timeval older)
{
    return (newer.tv_sec - older.tv_sec) * 1000 +
        (newer.tv_usec - older.tv_usec) / 1000;
}
#endif

static int waitsocket(libssh2_socket_t socket_fd, LIBSSH2_SESSION *session)
{
    struct timeval timeout;
    int rc;
    fd_set fd;
    fd_set *writefd = NULL;
    fd_set *readfd = NULL;
    int dir;

    timeout.tv_sec = 10;
    timeout.tv_usec = 0;

    FD_ZERO(&fd);

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
    FD_SET(socket_fd, &fd);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    /* now make sure we wait in the correct direction */
    dir = libssh2_session_block_directions(session);

    if(dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        readfd = &fd;

    if(dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        writefd = &fd;

    rc = select((int)(socket_fd + 1), readfd, writefd, NULL, &timeout);

    return rc;
}

int main(int argc, char *argv[])
{
    uint32_t hostaddr;
    libssh2_socket_t sock;
    int i, auth_pw = 1;
    struct sockaddr_in sin;
    const char *fingerprint;
    int rc;
    LIBSSH2_SESSION *session = NULL;
    LIBSSH2_CHANNEL *channel;
    libssh2_struct_stat fileinfo;
#ifdef HAVE_GETTIMEOFDAY
    struct timeval start;
    struct timeval end;
    long time_ms;
#endif
    int spin = 0;
    libssh2_struct_stat_size got = 0;
    libssh2_struct_stat_size total = 0;

#ifdef _WIN32
    WSADATA wsadata;

    rc = WSAStartup(MAKEWORD(2, 0), &wsadata);
    if(rc) {
        fprintf(stderr, "WSAStartup failed with error: %d\n", rc);
        return 1;
    }
#endif

    if(argc > 1) {
        hostaddr = inet_addr(argv[1]);
    }
    else {
        hostaddr = htonl(0x7F000001);
    }
    if(argc > 2) {
        username = argv[2];
    }
    if(argc > 3) {
        password = argv[3];
    }
    if(argc > 4) {
        scppath = argv[4];
    }

    rc = libssh2_init(0);
    if(rc) {
        fprintf(stderr, "libssh2 initialization failed (%d)\n", rc);
        return 1;
    }

    /* Ultra basic "connect to port 22 on localhost".  Your code is
     * responsible for creating the socket establishing the connection
     */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock == LIBSSH2_INVALID_SOCKET) {
        fprintf(stderr, "failed to create socket.\n");
        goto shutdown;
    }

    sin.sin_family = AF_INET;
    sin.sin_port = htons(22);
    sin.sin_addr.s_addr = hostaddr;
    if(connect(sock, (struct sockaddr*)(&sin), sizeof(struct sockaddr_in))) {
        fprintf(stderr, "failed to connect.\n");
        goto shutdown;
    }

    /* Create a session instance */
    session = libssh2_session_init();
    if(!session) {
        fprintf(stderr, "Could not initialize SSH session.\n");
        goto shutdown;
    }

    /* Since we have set non-blocking, tell libssh2 we are non-blocking */
    libssh2_session_set_blocking(session, 0);

#ifdef HAVE_GETTIMEOFDAY
    gettimeofday(&start, NULL);
#endif

    /* ... start it up. This will trade welcome banners, exchange keys,
     * and setup crypto, compression, and MAC layers
     */
    while((rc = libssh2_session_handshake(session, sock)) ==
          LIBSSH2_ERROR_EAGAIN);
    if(rc) {
        fprintf(stderr, "Failure establishing SSH session: %d\n", rc);
        goto shutdown;
    }

    /* At this point we have not yet authenticated.  The first thing to do
     * is check the hostkey's fingerprint against our known hosts Your app
     * may have it hard coded, may go to a file, may present it to the
     * user, that's your call
     */
    fingerprint = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA1);
    fprintf(stderr, "Fingerprint: ");
    for(i = 0; i < 20; i++) {
        fprintf(stderr, "%02X ", (unsigned char)fingerprint[i]);
    }
    fprintf(stderr, "\n");

    if(auth_pw) {
        /* We could authenticate via password */
        while((rc = libssh2_userauth_password(session, username, password)) ==
              LIBSSH2_ERROR_EAGAIN);
        if(rc) {
            fprintf(stderr, "Authentication by password failed.\n");
            goto shutdown;
        }
    }
    else {
        /* Or by public key */
        while((rc = libssh2_userauth_publickey_fromfile(session, username,
                                                        pubkey, privkey,
                                                        password)) ==
              LIBSSH2_ERROR_EAGAIN);
        if(rc) {
            fprintf(stderr, "Authentication by public key failed.\n");
            goto shutdown;
        }
    }

#if 0
    libssh2_trace(session, LIBSSH2_TRACE_CONN);
#endif

    /* Request a file via SCP */
    fprintf(stderr, "libssh2_scp_recv2().\n");
    do {
        channel = libssh2_scp_recv2(session, scppath, &fileinfo);

        if(!channel) {
            if(libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
                char *err_msg;

                libssh2_session_last_error(session, &err_msg, NULL, 0);
                fprintf(stderr, "%s\n", err_msg);
                goto shutdown;
            }
            else {
                fprintf(stderr, "libssh2_scp_recv2() spin\n");
                waitsocket(sock, session);
            }
        }
    } while(!channel);
    fprintf(stderr, "libssh2_scp_recv2() is done, now receive data.\n");

    while(got < fileinfo.st_size) {
        char mem[1024 * 24];
        ssize_t nread;

        do {
            int amount = sizeof(mem);

            if((fileinfo.st_size - got) < amount) {
                amount = (int)(fileinfo.st_size - got);
            }

            /* loop until we block */
            nread = libssh2_channel_read(channel, mem, (size_t)amount);
            if(nread > 0) {
                write(1, mem, (size_t)nread);
                got += nread;
                total += nread;
            }
        } while(nread > 0);

        if(nread == LIBSSH2_ERROR_EAGAIN && got < fileinfo.st_size) {
            /* this is due to blocking that would occur otherwise
            so we loop on this condition */

            spin++;
            waitsocket(sock, session); /* now we wait */
            continue;
        }
        break;
    }

#ifdef HAVE_GETTIMEOFDAY
    gettimeofday(&end, NULL);

    time_ms = tvdiff(end, start);
    fprintf(stderr, "Got %ld bytes in %ld ms = %.1f bytes/sec spin: %d\n",
            (long)total, time_ms,
            (double)total / ((double)time_ms / 1000.0), spin);
#else
    fprintf(stderr, "Got %ld bytes spin: %d\n", (long)total, spin);
#endif

    libssh2_channel_free(channel);
    channel = NULL;

shutdown:

    if(session) {
        libssh2_session_disconnect(session, "Normal Shutdown");
        libssh2_session_free(session);
    }

    if(sock != LIBSSH2_INVALID_SOCKET) {
        shutdown(sock, 2);
        LIBSSH2_SOCKET_CLOSE(sock);
    }

    fprintf(stderr, "all done\n");

    libssh2_exit();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}

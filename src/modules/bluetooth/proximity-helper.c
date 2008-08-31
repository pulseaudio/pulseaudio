/*
 * Small SUID helper that allows us to ping a BT device. Borrows
 * heavily from bluez-utils' l2ping, which is licensed as GPL2+
 * and comes with a copyright like this:
 *
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2007  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/select.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>

#define PING_STRING "PulseAudio"
#define IDENT 200
#define TIMEOUT 4
#define INTERVAL 2

static void update_status(int found) {
    static int status = -1;

    if (!found && status != 0)
        printf("-");
    if (found && status <= 0)
        printf("+");

    fflush(stdout);
    status = !!found;
}

int main(int argc, char *argv[]) {
    struct sockaddr_l2 addr;
    union {
        l2cap_cmd_hdr hdr;
        uint8_t buf[L2CAP_CMD_HDR_SIZE + sizeof(PING_STRING)];
    }  packet;
    int fd = -1;
    uint8_t id = IDENT;
    int connected = 0;

    assert(argc == 2);

    for (;;) {
        fd_set fds;
        struct timeval end;
        ssize_t r;

        if (!connected) {

            if (fd >= 0)
                close(fd);

            if ((fd = socket(PF_BLUETOOTH, SOCK_RAW, BTPROTO_L2CAP)) < 0) {
                fprintf(stderr, "socket(PF_BLUETOOTH, SOCK_RAW, BTPROTO_L2CAP) failed: %s", strerror(errno));
                goto finish;
            }

            memset(&addr, 0, sizeof(addr));
            addr.l2_family = AF_BLUETOOTH;
            bacpy(&addr.l2_bdaddr, BDADDR_ANY);

            if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
                fprintf(stderr, "bind() failed: %s", strerror(errno));
                goto finish;
            }

            memset(&addr, 0, sizeof(addr));
            addr.l2_family = AF_BLUETOOTH;
            str2ba(argv[1], &addr.l2_bdaddr);

            if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {

                if (errno == EHOSTDOWN || errno == ECONNRESET || errno == ETIMEDOUT) {
                    update_status(0);
                    sleep(INTERVAL);
                    continue;
                }

                fprintf(stderr, "connect() failed: %s", strerror(errno));
                goto finish;
            }

            connected = 1;
        }

        assert(connected);

        memset(&packet, 0, sizeof(packet));
        strcpy((char*) packet.buf + L2CAP_CMD_HDR_SIZE, PING_STRING);
        packet.hdr.ident = id;
        packet.hdr.len = htobs(sizeof(PING_STRING));
        packet.hdr.code = L2CAP_ECHO_REQ;

        if ((r = send(fd, &packet, sizeof(packet), 0)) < 0) {

            if (errno == EHOSTDOWN || errno == ECONNRESET || errno == ETIMEDOUT) {
                update_status(0);
                connected = 0;
                sleep(INTERVAL);
                continue;
            }

            fprintf(stderr, "send() failed: %s", strerror(errno));
            goto finish;
        }

        assert(r == sizeof(packet));

        gettimeofday(&end, NULL);
        end.tv_sec += TIMEOUT;

        for (;;) {
            struct timeval now, delta;

            gettimeofday(&now, NULL);

            if (timercmp(&end, &now, <=)) {
                update_status(0);
                connected = 0;
                sleep(INTERVAL);
                break;
            }

            timersub(&end, &now, &delta);

            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            if (select(fd+1, &fds, NULL, NULL, &delta) < 0) {
                fprintf(stderr, "select() failed: %s", strerror(errno));
                goto finish;
            }

            if ((r = recv(fd, &packet, sizeof(packet), 0)) <= 0) {

                if (errno == EHOSTDOWN || errno == ECONNRESET || errno == ETIMEDOUT) {
                    update_status(0);
                    connected = 0;
                    sleep(INTERVAL);
                    break;
                }

                fprintf(stderr, "send() failed: %s", r == 0 ? "EOF" : strerror(errno));
                goto finish;
            }

            assert(r >= L2CAP_CMD_HDR_SIZE);

            if (packet.hdr.ident != id)
                continue;

            if (packet.hdr.code == L2CAP_ECHO_RSP || packet.hdr.code == L2CAP_COMMAND_REJ) {

                if (++id >= 0xFF)
                    id = IDENT;

                update_status(1);
                sleep(INTERVAL);
                break;
            }
        }
    }

finish:

    if (fd >= 0)
        close(fd);

    return 1;
}

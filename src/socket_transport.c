/* Sim-only TCP transport. Listens on 127.0.0.1:5555, streams received bytes
 * into the portable codec seam comms_on_bytes(). Winsock; sim submodule only.
 * Runs as a FreeRTOS producer task — it never touches LVGL (comms_on_bytes →
 * codec → thread-safe ui_set_* enqueue). */
#ifdef PRODUCER_SOCKET

#include "FreeRTOS.h"
#include "task.h"
#include "comms.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define SOCK_PORT 5555

void socket_transport_task(void *pv)
{
    WSADATA wsa;
    (void)pv;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("socket: WSAStartup failed\n");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        struct sockaddr_in addr;
        SOCKET srv, cli;
        int yes = 1;
        DWORD tmo = 100;  /* recv timeout so the task stays cooperative */

        srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv == INVALID_SOCKET) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);

        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(SOCK_PORT);

        if (bind(srv, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR ||
            listen(srv, 1) == SOCKET_ERROR) {
            printf("socket: bind/listen failed on :%d\n", SOCK_PORT);
            closesocket(srv);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        printf("socket: listening on 127.0.0.1:%d\n", SOCK_PORT);

        cli = accept(srv, NULL, NULL);
        if (cli == INVALID_SOCKET) { closesocket(srv); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
        printf("socket: client connected\n");

        for (;;) {
            uint8_t buf[512];
            int r = recv(cli, (char *)buf, (int)sizeof buf, 0);
            if (r == 0) { printf("socket: client closed\n"); break; }
            if (r == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAETIMEDOUT) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
                printf("socket: recv error\n"); break;
            }
            comms_on_bytes(buf, (uint32_t)r);
        }
        closesocket(cli);
        closesocket(srv);
    }
}

#endif /* PRODUCER_SOCKET */

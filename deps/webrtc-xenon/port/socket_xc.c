/*
 * XCloud360 stage 4 — replaces libpeer src/socket.c. libpeer's UdpSocket API
 * on top of xcw_port.h (no BSD sockets on the Xbox: lwIP raw API, NO_SYS=1,
 * owned by the hw1 network thread). Non-blocking: recvfrom returns 0 when no
 * datagram is waiting (agent.c is patched to treat 0 as "nothing yet"
 * instead of calling select()).
 *
 * TCP is not used by the WebRTC path (signaling lives in the app), so the
 * tcp_* entry points only exist to satisfy the linker.
 */
#include <string.h>

#include "socket.h"
#include "utils.h"
#include "xcw_port.h"

int udp_socket_add_multicast_group(UdpSocket* udp_socket, Address* mcast_addr) {
  (void)udp_socket;
  (void)mcast_addr;
  return -1;
}

int udp_socket_open(UdpSocket* udp_socket, int family, int port) {
  uint16_t bound = 0;
  memset(&udp_socket->bind_addr, 0, sizeof(udp_socket->bind_addr));
  udp_socket->bind_addr.family = AF_INET;
  if (family != AF_INET) {
    udp_socket->fd = -1;
    return -1;
  }
  udp_socket->fd = xcw_udp_open((uint16_t)port, &bound);
  if (udp_socket->fd < 0) {
    LOGE("xcw_udp_open failed: %d", udp_socket->fd);
    udp_socket->fd = -1;
    return -1;
  }
  udp_socket->bind_addr.sin.sin_family = AF_INET;
  udp_socket->bind_addr.sin.sin_port = htons(bound);
  udp_socket->bind_addr.sin.sin_addr.s_addr = 0;
  udp_socket->bind_addr.port = bound;
  return 0;
}

int udp_socket_bind(UdpSocket* udp_socket, int port) {
  (void)udp_socket;
  (void)port;
  return -1;
}

void udp_socket_close(UdpSocket* udp_socket) {
  if (udp_socket->fd > 0) {
    xcw_udp_close(udp_socket->fd);
  }
  udp_socket->fd = -1;
}

int udp_socket_sendto(UdpSocket* udp_socket, Address* addr, const uint8_t* buf, int len) {
  if (udp_socket->fd < 0) {
    LOGE("sendto before socket init");
    return -1;
  }
  if (addr->family != AF_INET) {
    return -1;
  }
  uint32_t ip = ntohl(addr->sin.sin_addr.s_addr);
  int r = xcw_udp_send(udp_socket->fd, ip, addr->port, buf, len);
  return r < 0 ? -1 : r;
}

int udp_socket_recvfrom(UdpSocket* udp_socket, Address* addr, uint8_t* buf, int len) {
  uint32_t ip = 0;
  uint16_t port = 0;
  if (udp_socket->fd < 0) {
    return -1;
  }
  int r = xcw_udp_recv(udp_socket->fd, &ip, &port, buf, len);
  if (r > 0 && addr) {
    memset(addr, 0, sizeof(*addr));
    addr->family = AF_INET;
    addr->port = port;
    addr->sin.sin_family = AF_INET;
    addr->sin.sin_port = htons(port);
    addr->sin.sin_addr.s_addr = htonl(ip);
  }
  return r;
}

int tcp_socket_open(TcpSocket* tcp_socket, int family) {
  (void)family;
  tcp_socket->fd = -1;
  return -1;
}

int tcp_socket_connect(TcpSocket* tcp_socket, Address* addr) {
  (void)tcp_socket;
  (void)addr;
  return -1;
}

void tcp_socket_close(TcpSocket* tcp_socket) { tcp_socket->fd = -1; }

int tcp_socket_send(TcpSocket* tcp_socket, const uint8_t* buf, int len) {
  (void)tcp_socket;
  (void)buf;
  (void)len;
  return -1;
}

int tcp_socket_recv(TcpSocket* tcp_socket, uint8_t* buf, int len) {
  (void)tcp_socket;
  (void)buf;
  (void)len;
  return -1;
}

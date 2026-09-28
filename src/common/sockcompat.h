#ifndef IVMP_SOCKCOMPAT_H
#define IVMP_SOCKCOMPAT_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)

#define FD_SETSIZE 1024

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef int ivmp_ssize_t;

#define IVMP_FD_SELECTABLE(fd) 1

#define IVMP_EINTR WSAEINTR
#define IVMP_EAGAIN WSAEWOULDBLOCK
#define IVMP_EWOULDBLOCK WSAEWOULDBLOCK

#define IVMP_SOCKET(af, type, proto) \
	((int)socket((af), (type), (proto)))
#define IVMP_ACCEPT(fd, addr, addrlen) \
	((int)accept((SOCKET)(fd), (addr), (addrlen)))
#define IVMP_SEND(fd, buf, len, flags) \
	send((SOCKET)(fd), (const char *)(buf), (int)(len), (flags))
#define IVMP_RECV(fd, buf, len, flags) \
	recv((SOCKET)(fd), (char *)(buf), (int)(len), (flags))
#define IVMP_SENDTO(fd, buf, len, flags, addr, addrlen) \
	sendto((SOCKET)(fd), (const char *)(buf), (int)(len), (flags), \
		(addr), (addrlen))
#define IVMP_RECVFROM(fd, buf, len, flags, addr, addrlen) \
	recvfrom((SOCKET)(fd), (char *)(buf), (int)(len), (flags), \
		(addr), (addrlen))

#else

#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef ssize_t ivmp_ssize_t;

#define IVMP_FD_SELECTABLE(fd) ((fd) < FD_SETSIZE)

#define IVMP_EINTR EINTR
#define IVMP_EAGAIN EAGAIN
#define IVMP_EWOULDBLOCK EWOULDBLOCK

#define IVMP_SOCKET(af, type, proto) (socket((af), (type), (proto)))
#define IVMP_ACCEPT(fd, addr, addrlen) (accept((fd), (addr), (addrlen)))
#define IVMP_SEND(fd, buf, len, flags) (send((fd), (buf), (len), (flags)))
#define IVMP_RECV(fd, buf, len, flags) (recv((fd), (buf), (len), (flags)))
#define IVMP_SENDTO(fd, buf, len, flags, addr, addrlen) \
	(sendto((fd), (buf), (len), (flags), (addr), (addrlen)))
#define IVMP_RECVFROM(fd, buf, len, flags, addr, addrlen) \
	(recvfrom((fd), (buf), (len), (flags), (addr), (addrlen)))

#endif

int ivmp_sockets_init(void);
void ivmp_sockets_cleanup(void);

int ivmp_socket_errno(void);
const char *ivmp_socket_strerror(int code);
int ivmp_socket_close(int fd);
int ivmp_socket_set_nonblocking(int fd);
int ivmp_random_bytes(unsigned char *output, size_t length);
uint64_t ivmp_monotonic_milliseconds(void);

#endif

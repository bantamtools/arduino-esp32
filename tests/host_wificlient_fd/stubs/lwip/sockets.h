#pragma once
// Host stub: BSD sockets under the lwip names, with close and setsockopt routed through
// the test's seams (counted closes, a setsockopt that can be made to fail).
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int test_close(int fd);
int test_setsockopt(int fd, int level, int name, const void* value, socklen_t len);

#define lwip_ioctl ::ioctl
#define lwip_connect ::connect
#define lwip_accept ::accept
#define lwip_close test_close
#define close test_close
#define setsockopt test_setsockopt

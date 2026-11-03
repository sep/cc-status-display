#pragma once
// libssh2 build configuration for ESP-IDF: newlib + lwIP sockets.
// Pulled in by libssh2's libssh2_setup.h because we define HAVE_CONFIG_H.
// Keep this list in step with libssh2/src/libssh2_config_cmake.h.in.

#define HAVE_UNISTD_H
#define HAVE_INTTYPES_H
#define HAVE_SYS_SELECT_H
#define HAVE_SYS_TIME_H
#define HAVE_ARPA_INET_H
#define HAVE_NETINET_IN_H
#define HAVE_O_NONBLOCK
// HAVE_POLL deliberately left out: libssh2 then waits on the socket with
// select(), which goes through the ESP-IDF VFS and is the well-trodden path.

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

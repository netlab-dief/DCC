#pragma once

#include <sys/socket.h>
#include <sys/time.h>

// Bound each blocking socket send/receive, including Netlink family lookup
// and ACK reception. Failure to install either timeout is a setup failure.
inline bool set_io_timeout(int fd, int milliseconds = 80) {
    if (fd < 0 || milliseconds <= 0) return false;
    const timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

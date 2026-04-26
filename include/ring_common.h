#ifndef RING_COMMON_H
#define RING_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#define RING_DATA_MAX 240
#define RING_SOCK_PATH_MAX 108

#define RING_FLAG_STATUS   1
#define RING_FLAG_INFO_END 2
#define RING_FLAG_FILE_END 4
#define RING_FLAG_FILE_REQ 8

#define RING_BROADCAST_ID (-1)

enum ring_msg_type {
    MSG_TOKEN = 1,
    MSG_DATA,
    MSG_BROADCAST,
    MSG_INFO_REQ,
    MSG_INFO_REP,
    MSG_JOIN,
    MSG_LEAVE,
    MSG_FILE_REQ,
    MSG_FILE_DATA,
    MSG_FILE_ACK,
    MSG_NEIGHBOR_REP
};

struct ring_msg {
    int type;
    int src;
    int dst;
    int size;
    int seq;
    int flags;
    char data[RING_DATA_MAX];
};

#define FATAL(msg) do { perror(msg); exit(1); } while (0)

int write_all(int fd, const void *buf, size_t len);
int read_all(int fd, void *buf, size_t len);
int send_ring_msg(int fd, const struct ring_msg *msg);
int recv_ring_msg(int fd, struct ring_msg *msg);
void ring_msg_init(struct ring_msg *msg, int type, int src, int dst);
void ring_msg_set_text(struct ring_msg *msg, const char *text);
const char *ring_msg_type_name(int type);
void ring_make_local_path(char *path, size_t size, int machine_id);

#endif

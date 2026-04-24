#ifndef RING_COMMON_H
#define RING_COMMON_H

/* Fonctions standard d'entree/sortie (perror, snprintf). */
#include <stdio.h>
/* Fonctions utilitaires (exit). */
#include <stdlib.h>
/* Fonctions de manipulation memoire/chaines. */
#include <string.h>
/* Taille maximale de la charge utile d'une trame. */
#define RING_DATA_MAX 240

/* Valeur speciale pour une diffusion a tout l'anneau. */
#define RING_BROADCAST_ID (-1)

/* Type de message echange sur l'anneau ou en local. */
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
    MSG_FILE_ACK
};

/* Trame commune a Driver et Comm. */
struct ring_msg {
    int type;
    int src;
    int dst;
    int size;
    int seq;
    int flags;
    char data[RING_DATA_MAX];
};

/* Macro d'arret en cas d'erreur systeme. */
#define FATAL(msg) do { perror(msg); exit(1); } while (0)

int write_all(int fd, const void *buf, size_t len);
int read_all(int fd, void *buf, size_t len);
int send_ring_msg(int fd, const struct ring_msg *msg);
int recv_ring_msg(int fd, struct ring_msg *msg);
void ring_msg_init(struct ring_msg *msg, int type, int src, int dst);
void ring_msg_set_text(struct ring_msg *msg, const char *text);
const char *ring_msg_type_name(int type);

#endif

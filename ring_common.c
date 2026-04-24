/* API commune du projet. */
#include "ring_common.h"
/* Appels systeme POSIX (read, write). */
#include <unistd.h>
#include <errno.h>

/*
 * Ecrit exactement len octets sur fd.
 * Utile pour fiabiliser les echanges sur sockets de type stream.
 */
int write_all(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    const char *p = (const char *)buf;

    while (sent < len) {
        ssize_t cc = write(fd, p + sent, len - sent);
        if (cc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (cc == 0) return -1;
        sent += (size_t)cc;
    }

    return 0;
}

/*
 * Lit exactement len octets sur fd.
 * Retourne -1 si la connexion se ferme ou en cas d'erreur.
 */
int read_all(int fd, void *buf, size_t len)
{
    size_t recvd = 0;
    char *p = (char *)buf;

    while (recvd < len) {
        ssize_t cc = read(fd, p + recvd, len - recvd);
        if (cc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (cc == 0) return -1;
        recvd += (size_t)cc;
    }

    return 0;
}

/* Envoie une trame complete sur une socket stream. */
int send_ring_msg(int fd, const struct ring_msg *msg)
{
    return write_all(fd, msg, sizeof(*msg));
}

/* Recoit une trame complete sur une socket stream. */
int recv_ring_msg(int fd, struct ring_msg *msg)
{
    return read_all(fd, msg, sizeof(*msg));
}

/* Initialise proprement une trame. */
void ring_msg_init(struct ring_msg *msg, int type, int src, int dst)
{
    memset(msg, 0, sizeof(*msg));
    msg->type = type;
    msg->src = src;
    msg->dst = dst;
}

/*
 * Place une chaine dans data en respectant la taille maximale.
 * Le champ size contient le nombre d'octets utiles sans le '\0'.
 */
void ring_msg_set_text(struct ring_msg *msg, const char *text)
{
    size_t n = strlen(text);

    if (n >= RING_DATA_MAX) n = RING_DATA_MAX - 1;

    memcpy(msg->data, text, n);
    msg->data[n] = '\0';
    msg->size = (int)n;
}

/* Nom lisible d'un type de message. */
const char *ring_msg_type_name(int type)
{
    switch (type) {
    case MSG_TOKEN:      return "MSG_TOKEN";
    case MSG_DATA:       return "MSG_DATA";
    case MSG_BROADCAST:  return "MSG_BROADCAST";
    case MSG_INFO_REQ:   return "MSG_INFO_REQ";
    case MSG_INFO_REP:   return "MSG_INFO_REP";
    case MSG_JOIN:       return "MSG_JOIN";
    case MSG_LEAVE:      return "MSG_LEAVE";
    case MSG_FILE_REQ:   return "MSG_FILE_REQ";
    case MSG_FILE_DATA:  return "MSG_FILE_DATA";
    case MSG_FILE_ACK:   return "MSG_FILE_ACK";
    default:             return "MSG_UNKNOWN";
    }
}

/* Construit le chemin du socket local a partir de l'identifiant machine. */
void ring_make_local_path(char *path, size_t size, int machine_id)
{
    snprintf(path, size, "/tmp/ring_local_%d.sock", machine_id);
}

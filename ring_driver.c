/* Driver minimal de l'anneau. */
#include "ring_common.h"

/*
 * Retourne vrai si la trame doit etre livree au Comm local.
 * Dans cette premiere version, on accepte :
 * - les messages pour la machine locale
 * - les diffusions
 */
static int must_deliver_local(int machine_id, const struct ring_msg *msg)
{
    if (msg->dst == machine_id) return 1;
    if (msg->dst == RING_BROADCAST_ID) return 1;
    return 0;
}

/*
 * Retourne vrai si la trame doit etre retransmise a droite.
 * En V1, le driver retransmet tout ce qui vient de gauche.
 */
static int must_forward_right(const struct ring_msg *msg)
{
    (void)msg;
    return 1;
}

/* Affichage simple d'une trame pour le debug. */
static void print_msg(const struct ring_msg *msg)
{
    printf("Driver recoit type=%s src=%d dst=%d size=%d seq=%d\n",
           ring_msg_type_name(msg->type), msg->src, msg->dst, msg->size, msg->seq);

    if (msg->size > 0) {
        printf("Contenu: %s\n", msg->data);
    }
}

/*
 * Boucle principale du driver.
 * left_fd  : arrivee depuis le voisin gauche
 * right_fd : sortie vers le voisin droit
 * local_fd : lien avec Comm
 */
static void driver_loop(int machine_id, int left_fd, int right_fd, int local_fd)
{
    struct ring_msg msg;

    while (1) {
        if (recv_ring_msg(left_fd, &msg) == -1) FATAL("recv_ring_msg left");

        print_msg(&msg);

        if (must_deliver_local(machine_id, &msg)) {
            if (send_ring_msg(local_fd, &msg) == -1) FATAL("send_ring_msg local");
        }

        if (must_forward_right(&msg)) {
            if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg right");
        }
    }
}

int main(int argc, char *argv[])
{
    int machine_id;
    int left_fd;
    int right_fd;
    int local_fd;

    /*
     * Arguments :
     *  machine_id : identifiant logique de la machine
     *  left_fd    : descripteur lu par le driver
     *  right_fd   : descripteur ecrit par le driver
     *  local_fd   : descripteur vers le Comm local
     */
    if (argc != 5) {
        printf("Usage: %s machine_id left_fd right_fd local_fd\n", argv[0]);
        exit(1);
    }

    machine_id = atoi(argv[1]);
    left_fd = atoi(argv[2]);
    right_fd = atoi(argv[3]);
    local_fd = atoi(argv[4]);

    printf("driver pret: machine=%d left=%d right=%d local=%d\n",
           machine_id, left_fd, right_fd, local_fd);

    driver_loop(machine_id, left_fd, right_fd, local_fd);

    return 0;
}

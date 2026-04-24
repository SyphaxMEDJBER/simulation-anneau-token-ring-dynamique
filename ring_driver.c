/* Driver minimal de l'anneau. */
#include "ring_common.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

/* Affichage simple d'une trame pour le debug. */
static void print_msg(const struct ring_msg *msg)
{
    printf("Driver recoit type=%s src=%d dst=%d size=%d seq=%d\n",
           ring_msg_type_name(msg->type), msg->src, msg->dst, msg->size, msg->seq);

    if (msg->size > 0) {
        printf("Contenu: %s\n", msg->data);
    }
}

/* Cree le socket serveur local du Driver. */
static int create_local_server(int machine_id, char *path, size_t path_size)
{
    int sock;
    struct sockaddr_un addr;

    ring_make_local_path(path, path_size, machine_id);

    unlink(path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) FATAL("socket local");

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) FATAL("bind local");
    if (listen(sock, 1) == -1) FATAL("listen local");

    return sock;
}

/*
 * Traite un message recu depuis le Comm local.
 * Le routage anneau n'est pas encore branche: on livre seulement en local.
 */
static void handle_local_msg(int machine_id, int local_fd)
{
    struct ring_msg msg;

    if (recv_ring_msg(local_fd, &msg) == -1) FATAL("recv_ring_msg local");

    printf("Driver recoit depuis Comm:\n");
    print_msg(&msg);

    if (must_deliver_local(machine_id, &msg)) {
        if (send_ring_msg(local_fd, &msg) == -1) FATAL("send_ring_msg local");
    } else {
        printf("Destination distante %d: anneau externe pas encore branche\n", msg.dst);
    }
}

/* Boucle principale du Driver. */
static void driver_loop(int machine_id, int local_fd)
{
    fd_set rfds;

    while (1) {
        FD_ZERO(&rfds);
        FD_SET(local_fd, &rfds);

        if (select(local_fd + 1, &rfds, NULL, NULL, NULL) == -1) FATAL("select");

        if (FD_ISSET(local_fd, &rfds)) {
            handle_local_msg(machine_id, local_fd);
        }
    }
}

int main(int argc, char *argv[])
{
    int machine_id;
    int listen_fd;
    int local_fd;
    char local_path[RING_SOCK_PATH_MAX];

    /*
     * Argument :
     *  machine_id : identifiant logique de la machine
     */
    if (argc != 2) {
        printf("Usage: %s machine_id\n", argv[0]);
        exit(1);
    }

    machine_id = atoi(argv[1]);

    listen_fd = create_local_server(machine_id, local_path, sizeof(local_path));

    printf("driver pret: machine=%d local=%s\n", machine_id, local_path);
    printf("driver attend la connexion de Comm...\n");

    local_fd = accept(listen_fd, NULL, NULL);
    if (local_fd == -1) FATAL("accept local");

    printf("driver connecte au Comm local\n");

    close(listen_fd);

    driver_loop(machine_id, local_fd);

    close(local_fd);
    unlink(local_path);

    return 0;
}

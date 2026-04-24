/* Comm minimal du projet anneau. */
#include "ring_common.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Se connecte au socket local du Driver. */
static int connect_local_driver(int machine_id)
{
    int sock;
    struct sockaddr_un addr;
    char path[RING_SOCK_PATH_MAX];

    ring_make_local_path(path, sizeof(path), machine_id);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) FATAL("socket local");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) FATAL("connect local");

    return sock;
}

/* Affiche une trame recue depuis le Driver local. */
static void print_local_msg(const struct ring_msg *msg)
{
    printf("Comm recoit type=%s src=%d dst=%d size=%d seq=%d\n",
           ring_msg_type_name(msg->type), msg->src, msg->dst, msg->size, msg->seq);

    if (msg->size > 0) {
        if (msg->type == MSG_INFO_REP) {
            printf("Etat: %s\n", msg->data);
        } else {
            printf("Message: %s\n", msg->data);
        }
    }
}

/* Demande a l'utilisateur un message simple puis l'envoie au Driver. */
static void send_user_message(int local_fd, int machine_id)
{
    struct ring_msg msg;
    int dst;
    char text[RING_DATA_MAX];

    printf("Destination : ");
    scanf("%d", &dst);

    printf("Message : ");
    scanf("%239s", text);

    ring_msg_init(&msg, MSG_DATA, machine_id, dst);
    ring_msg_set_text(&msg, text);

    if (send_ring_msg(local_fd, &msg) == -1) FATAL("send_ring_msg local");
}

/* Attend une trame envoyee par le Driver local. */
static void receive_from_driver(int local_fd)
{
    struct ring_msg msg;

    if (recv_ring_msg(local_fd, &msg) == -1) FATAL("recv_ring_msg local");
    print_local_msg(&msg);
}

/* Menu minimal de Comm. */
static void comm_loop(int local_fd, int machine_id)
{
    int choix;

    while (1) {
        printf("\n");
        printf("1. Envoyer un message\n");
        printf("2. Recevoir depuis Driver\n");
        printf("3. Quitter\n");
        printf("Choix : ");
        scanf("%d", &choix);

        if (choix == 1) {
            send_user_message(local_fd, machine_id);
        } else if (choix == 2) {
            receive_from_driver(local_fd);
        } else if (choix == 3) {
            break;
        } else {
            printf("Choix invalide\n");
        }
    }
}

int main(int argc, char *argv[])
{
    int machine_id;
    int local_fd;

    /*
     * Argument :
     *  machine_id : identifiant logique de la machine
     */
    if (argc != 2) {
        printf("Usage: %s machine_id\n", argv[0]);
        exit(1);
    }

    machine_id = atoi(argv[1]);
    local_fd = connect_local_driver(machine_id);

    printf("comm pret: machine=%d local=%d\n", machine_id, local_fd);

    comm_loop(local_fd, machine_id);

    close(local_fd);

    return 0;
}

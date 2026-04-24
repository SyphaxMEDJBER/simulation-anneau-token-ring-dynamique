/* Driver minimal de l'anneau avec gestion du jeton. */
#include "ring_common.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Etat local du Driver. */
struct driver_state {
    int machine_id;
    int left_port;
    int right_port;
    int pending_valid;
    int next_seq;
    struct ring_msg pending_msg;
};

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
 * Retourne vrai si la trame doit continuer sur l'anneau.
 * En V1, une trame unicast s'arrete a sa destination.
 */
static int must_forward_right(int machine_id, const struct ring_msg *msg)
{
    if (msg->type == MSG_BROADCAST && msg->src == machine_id) return 0;
    if (msg->type == MSG_INFO_REQ && msg->src == machine_id) return 0;
    if (msg->dst == machine_id) return 0;
    return 1;
}

/* Envoie un message d'etat simple au Comm local. */
static void send_status_to_comm(int local_fd, int machine_id, const char *text)
{
    struct ring_msg msg;

    ring_msg_init(&msg, MSG_INFO_REP, machine_id, machine_id);
    ring_msg_set_text(&msg, text);

    if (send_ring_msg(local_fd, &msg) == -1) FATAL("send_ring_msg status");
}

/* Ajoute une ligne d'information machine dans data si la place suffit. */
static void append_machine_info(struct ring_msg *msg, const struct driver_state *state)
{
    char line[96];
    size_t cur;
    int n;

    cur = (size_t)msg->size;
    n = snprintf(line, sizeof(line), "machine=%d portE=%d portS=%d\n",
                 state->machine_id, state->left_port, state->right_port);
    if (n <= 0) return;

    if (cur + (size_t)n >= RING_DATA_MAX) return;

    memcpy(msg->data + cur, line, (size_t)n);
    msg->size += n;
    msg->data[msg->size] = '\0';
}

/* Affichage simple d'une trame pour le debug. */
static void print_msg(const char *prefix, const struct ring_msg *msg)
{
    printf("%s type=%s src=%d dst=%d size=%d seq=%d\n",
           prefix,
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

/* Cree la socket TCP d'ecoute pour recevoir depuis le voisin gauche. */
static int create_left_server(int port)
{
    int sock;
    int opt = 1;
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) FATAL("socket left");

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) FATAL("setsockopt left");
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) FATAL("bind left");
    if (listen(sock, 1) == -1) FATAL("listen left");

    return sock;
}

/* Etablit la connexion TCP vers le voisin droit. */
static int connect_right_peer(const char *host, int port)
{
    int sock;
    struct hostent *hp;
    struct sockaddr_in addr;

    hp = gethostbyname(host);
    if (hp == NULL) FATAL("gethostbyname right");

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    memcpy(&addr.sin_addr, hp->h_addr_list[0], (size_t)hp->h_length);

    while (1) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == -1) FATAL("socket right");

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return sock;
        }

        close(sock);
        sleep(1);
    }
}

/* Injecte un jeton initial sur l'anneau. */
static void inject_initial_token(int machine_id, int right_fd)
{
    struct ring_msg msg;

    ring_msg_init(&msg, MSG_TOKEN, machine_id, RING_BROADCAST_ID);

    printf("Driver injecte le jeton initial\n");

    if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg token");
}

/* Traite un message recu depuis le Comm local. */
static void handle_local_msg(struct driver_state *state, int local_fd)
{
    struct ring_msg msg;

    if (recv_ring_msg(local_fd, &msg) == -1) FATAL("recv_ring_msg local");

    print_msg("Driver recoit depuis Comm:", &msg);

    if (msg.type == MSG_DATA && must_deliver_local(state->machine_id, &msg)) {
        if (send_ring_msg(local_fd, &msg) == -1) FATAL("send_ring_msg local");
        return;
    }

    if (state->pending_valid) {
        send_status_to_comm(local_fd, state->machine_id, "Emission deja en attente");
        return;
    }

    msg.src = state->machine_id;
    msg.seq = ++state->next_seq;

    if (msg.type == MSG_INFO_REQ) {
        append_machine_info(&msg, state);
    }

    state->pending_msg = msg;
    state->pending_valid = 1;
    send_status_to_comm(local_fd, state->machine_id, "Message mis en attente du jeton");
}

/* Traite un message recu depuis le voisin gauche. */
static void handle_left_msg(struct driver_state *state, int left_fd, int right_fd, int local_fd)
{
    struct ring_msg msg;

    if (recv_ring_msg(left_fd, &msg) == -1) FATAL("recv_ring_msg left");

    print_msg("Driver recoit depuis anneau:", &msg);

    if (msg.type == MSG_TOKEN) {
        if (state->pending_valid) {
            printf("Driver emet le message en attente puis retransmet le jeton\n");

            if (send_ring_msg(right_fd, &state->pending_msg) == -1) FATAL("send_ring_msg pending");

            send_status_to_comm(local_fd, state->machine_id, "Message emis sur l anneau");

            state->pending_valid = 0;
        }

        if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg token");
        return;
    }

    if (msg.type == MSG_BROADCAST) {
        if (msg.src == state->machine_id) {
            printf("Driver recupere sa propre diffusion, fin de tour\n");
            return;
        }

        if (send_ring_msg(local_fd, &msg) == -1) FATAL("send_ring_msg local");
        if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg right");
        return;
    }

    if (msg.type == MSG_INFO_REQ) {
        if (msg.src == state->machine_id) {
            msg.type = MSG_INFO_REP;
            msg.dst = state->machine_id;
            if (send_ring_msg(local_fd, &msg) == -1) FATAL("send_ring_msg local");
            return;
        }

        append_machine_info(&msg, state);
        if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg right");
        return;
    }

    if (must_deliver_local(state->machine_id, &msg)) {
        if (send_ring_msg(local_fd, &msg) == -1) FATAL("send_ring_msg local");
    }

    if (must_forward_right(state->machine_id, &msg)) {
        if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg right");
    }
}

/* Boucle principale du Driver. */
static void driver_loop(struct driver_state *state, int left_fd, int right_fd, int local_fd)
{
    fd_set rfds;
    int maxfd;

    maxfd = left_fd;
    if (local_fd > maxfd) maxfd = local_fd;

    while (1) {
        FD_ZERO(&rfds);
        FD_SET(local_fd, &rfds);
        FD_SET(left_fd, &rfds);

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) == -1) FATAL("select");

        if (FD_ISSET(local_fd, &rfds)) {
            handle_local_msg(state, local_fd);
        }

        if (FD_ISSET(left_fd, &rfds)) {
            handle_left_msg(state, left_fd, right_fd, local_fd);
        }
    }
}

int main(int argc, char *argv[])
{
    int machine_id;
    int left_port;
    int listen_fd;
    int left_fd;
    int local_fd;
    int left_local_fd;
    int right_fd;
    const char *right_host;
    int right_port;
    int inject_token;
    char local_path[RING_SOCK_PATH_MAX];
    struct driver_state state;

    /*
     * Arguments :
     *  machine_id   : identifiant logique de la machine
     *  left_port    : port TCP d'ecoute depuis le voisin gauche
     *  right_host   : hote du voisin droit
     *  right_port   : port TCP du voisin droit
     *  inject_token : 1 si ce Driver cree le jeton initial, sinon 0
     */
    if (argc != 6) {
        printf("Usage: %s machine_id left_port right_host right_port inject_token\n", argv[0]);
        exit(1);
    }

    machine_id = atoi(argv[1]);
    left_port = atoi(argv[2]);
    right_host = argv[3];
    right_port = atoi(argv[4]);
    inject_token = atoi(argv[5]);

    memset(&state, 0, sizeof(state));
    state.machine_id = machine_id;
    state.left_port = left_port;
    state.right_port = right_port;

    listen_fd = create_local_server(machine_id, local_path, sizeof(local_path));
    left_local_fd = create_left_server(left_port);

    printf("driver pret: machine=%d local=%s left_port=%d\n",
           machine_id, local_path, left_port);
    printf("driver attend la connexion de Comm...\n");

    local_fd = accept(listen_fd, NULL, NULL);
    if (local_fd == -1) FATAL("accept local");

    printf("driver connecte au Comm local\n");
    printf("driver tente la connexion vers le voisin droit %s:%d...\n", right_host, right_port);

    right_fd = connect_right_peer(right_host, right_port);
    printf("driver connecte au voisin droit\n");

    printf("driver attend la connexion du voisin gauche...\n");

    left_fd = accept(left_local_fd, NULL, NULL);
    if (left_fd == -1) FATAL("accept left");

    printf("driver connecte au voisin gauche\n");

    close(listen_fd);
    close(left_local_fd);

    if (inject_token) {
        inject_initial_token(machine_id, right_fd);
    }

    driver_loop(&state, left_fd, right_fd, local_fd);

    close(left_fd);
    close(right_fd);
    close(local_fd);
    unlink(local_path);

    return 0;
}

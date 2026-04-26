/* Driver de l'anneau avec gestion du jeton et JOIN/LEAVE. */
#include "ring_common.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define RING_QUEUE_MAX 256
#define TOKEN_TIMEOUT_MS 5000

struct driver_state {
    int machine_id;
    int left_port;
    int right_port;
    char right_host[108];
    int local_listen_fd;
    int local_fd;
    int left_listen_fd;
    int left_fd;
    int right_fd;
    int active;
    int join_pending;
    int leave_after_send;
    int can_regen_token;
    int next_seq;
    long last_token_ms;
    long last_regen_ms;
    int q_head;
    int q_tail;
    int q_count;
    struct ring_msg queue[RING_QUEUE_MAX];
};

static long now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long)(tv.tv_sec * 1000L + tv.tv_usec / 1000L);
}

static int must_deliver_local(const struct driver_state *state, const struct ring_msg *msg)
{
    if (!state->active) return 0;
    if (msg->dst == state->machine_id) return 1;
    if (msg->dst == RING_BROADCAST_ID) return 1;
    return 0;
}

static int must_forward_right(const struct driver_state *state, const struct ring_msg *msg)
{
    if (msg->type == MSG_BROADCAST && msg->src == state->machine_id) return 0;
    if (msg->type == MSG_INFO_REQ && msg->src == state->machine_id) return 0;
    if (msg->dst == state->machine_id) return 0;
    return 1;
}

static int queue_has_pending(const struct driver_state *state)
{
    return state->q_count > 0;
}

static int queue_push(struct driver_state *state, const struct ring_msg *msg)
{
    if (state->q_count >= RING_QUEUE_MAX) return -1;

    state->queue[state->q_tail] = *msg;
    state->q_tail = (state->q_tail + 1) % RING_QUEUE_MAX;
    state->q_count++;
    return 0;
}

static int queue_pop(struct driver_state *state, struct ring_msg *msg)
{
    if (state->q_count == 0) return -1;

    *msg = state->queue[state->q_head];
    state->q_head = (state->q_head + 1) % RING_QUEUE_MAX;
    state->q_count--;
    return 0;
}

static void send_status_to_comm(int local_fd, int machine_id, const char *text)
{
    struct ring_msg msg;

    if (local_fd == -1) return;

    ring_msg_init(&msg, MSG_INFO_REP, machine_id, machine_id);
    ring_msg_set_text(&msg, text);
    msg.flags = RING_FLAG_STATUS;

    if (send_ring_msg(local_fd, &msg) == -1) {
        perror("send_ring_msg status");
    }
}

static int deliver_to_comm(struct driver_state *state, const struct ring_msg *msg)
{
    if (state->local_fd == -1) return -1;

    if (send_ring_msg(state->local_fd, msg) == -1) {
        perror("send_ring_msg local");
        close(state->local_fd);
        state->local_fd = -1;
        return -1;
    }

    return 0;
}

static void append_machine_info(struct ring_msg *msg, const struct driver_state *state)
{
    char line[96];
    size_t cur;
    int n;

    cur = (size_t)msg->size;
    n = snprintf(line, sizeof(line), "machine=%d portE=%d portS=%d active=%d\n",
                 state->machine_id, state->left_port, state->right_port, state->active);
    if (n <= 0) return;

    if (cur + (size_t)n >= RING_DATA_MAX) return;

    memcpy(msg->data + cur, line, (size_t)n);
    msg->size += n;
    msg->data[msg->size] = '\0';
}

static void print_msg(const char *prefix, const struct ring_msg *msg)
{
    printf("%s type=%s src=%d dst=%d size=%d seq=%d flags=%d\n",
           prefix,
           ring_msg_type_name(msg->type), msg->src, msg->dst, msg->size, msg->seq, msg->flags);

    if (msg->size > 0 && msg->type != MSG_FILE_DATA) {
        printf("Contenu: %s\n", msg->data);
    }
}

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
    if (listen(sock, 4) == -1) FATAL("listen left");

    return sock;
}

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

static int parse_host_port(const char *text, char *host, size_t host_size, int *port)
{
    (void)host_size;
    return sscanf(text, "%107s %d", host, port) == 2 ? 0 : -1;
}

static int parse_host_port_ref(const char *text, char *host, size_t host_size, int *port, int *ref_port)
{
    (void)host_size;
    return sscanf(text, "%107s %d %d", host, port, ref_port) == 3 ? 0 : -1;
}

static int is_detached_right(const char *host, int port)
{
    if (port <= 0) return 1;
    if (strcmp(host, "-") == 0) return 1;
    if (strcmp(host, "none") == 0) return 1;
    return 0;
}

static int accept_left_with_timeout(int listen_fd, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_SET(listen_fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (select(listen_fd + 1, &rfds, NULL, NULL, &tv) <= 0) return -1;

    return accept(listen_fd, NULL, NULL);
}

static int fd_readable_with_timeout(int fd, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    return select(fd + 1, &rfds, NULL, NULL, &tv);
}

static void reconnect_right(struct driver_state *state, int local_fd, const char *host, int port)
{
    int new_fd;

    if (state->right_fd != -1) {
        close(state->right_fd);
        state->right_fd = -1;
    }

    new_fd = connect_right_peer(host, port);
    state->right_fd = new_fd;
    state->right_port = port;
    snprintf(state->right_host, sizeof(state->right_host), "%s", host);

    send_status_to_comm(local_fd, state->machine_id, "Voisin droit reconfigure");
}

static int send_join_control_request(const char *host_b, int port_b, int machine_id, int left_port,
                                     char *next_host, size_t next_host_size, int *next_port)
{
    int fd;
    struct ring_msg msg;
    struct ring_msg rep;

    fd = connect_right_peer(host_b, port_b);

    ring_msg_init(&msg, MSG_JOIN, machine_id, RING_BROADCAST_ID);
    snprintf(msg.data, sizeof(msg.data), "127.0.0.1 %d", left_port);
    msg.size = (int)strlen(msg.data);

    if (send_ring_msg(fd, &msg) == -1) {
        close(fd);
        return -1;
    }

    if (recv_ring_msg(fd, &rep) == -1) {
        close(fd);
        return -1;
    }

    close(fd);

    if (rep.type != MSG_NEIGHBOR_REP) return -1;

    return parse_host_port(rep.data, next_host, next_host_size, next_port);
}

static void handle_left_extra_connection(struct driver_state *state, int local_fd)
{
    int fd;
    int rc;
    struct ring_msg msg;
    struct ring_msg rep;
    char host[108];
    int port;

    fd = accept(state->left_listen_fd, NULL, NULL);
    if (fd == -1) return;

    rc = fd_readable_with_timeout(fd, 300);
    if (rc <= 0) {
        if (state->left_fd != -1) {
            close(state->left_fd);
        }
        state->left_fd = fd;
        send_status_to_comm(local_fd, state->machine_id, "Nouveau voisin gauche raccorde");
        return;
    }

    if (recv_ring_msg(fd, &msg) == -1) {
        close(fd);
        return;
    }

    if (msg.type == MSG_JOIN && parse_host_port(msg.data, host, sizeof(host), &port) == 0) {
        ring_msg_init(&rep, MSG_NEIGHBOR_REP, state->machine_id, msg.src);
        snprintf(rep.data, sizeof(rep.data), "%s %d", state->right_host, state->right_port);
        rep.size = (int)strlen(rep.data);
        if (send_ring_msg(fd, &rep) == -1) {
            close(fd);
            return;
        }

        reconnect_right(state, local_fd, host, port);
        send_status_to_comm(local_fd, state->machine_id, "Nouvelle machine inseree");
    }

    close(fd);
}

static void handle_local_disconnect(struct driver_state *state)
{
    int new_local;

    if (state->local_fd != -1) {
        close(state->local_fd);
        state->local_fd = -1;
    }

    new_local = accept_left_with_timeout(state->local_listen_fd, 1500);
    if (new_local != -1) {
        state->local_fd = new_local;
        send_status_to_comm(state->local_fd, state->machine_id, "Comm local reconnecte");
        return;
    }

    printf("Driver sans Comm local, attente d'une reconnexion...\n");
}

static void handle_left_disconnect(struct driver_state *state, int local_fd)
{
    int new_left;

    if (state->left_fd != -1) {
        close(state->left_fd);
        state->left_fd = -1;
    }

    new_left = accept_left_with_timeout(state->left_listen_fd, 1500);
    if (new_left != -1) {
        state->left_fd = new_left;
        send_status_to_comm(local_fd, state->machine_id, "Nouveau voisin gauche connecte");
        return;
    }

    send_status_to_comm(local_fd, state->machine_id,
                        "Voisin gauche perdu, attente d'un nouveau raccordement");
}

static void inject_initial_token(int machine_id, int right_fd)
{
    struct ring_msg msg;

    ring_msg_init(&msg, MSG_TOKEN, machine_id, RING_BROADCAST_ID);
    printf("Driver injecte le jeton initial\n");

    if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg token");
}

static void maybe_regenerate_token(struct driver_state *state)
{
    long now;

    if (!state->can_regen_token) return;
    if (state->right_fd == -1) return;

    now = now_ms();

    if (now - state->last_token_ms < TOKEN_TIMEOUT_MS) return;
    if (now - state->last_regen_ms < TOKEN_TIMEOUT_MS) return;

    printf("Driver regenere un jeton\n");
    inject_initial_token(state->machine_id, state->right_fd);
    state->last_token_ms = now;
    state->last_regen_ms = now;
}

static void queue_local_request(struct driver_state *state, int local_fd, struct ring_msg *msg)
{
    msg->src = state->machine_id;

    if (msg->type != MSG_FILE_REQ && msg->type != MSG_FILE_DATA && msg->type != MSG_FILE_ACK) {
        msg->seq = ++state->next_seq;
    }

    if (msg->type == MSG_INFO_REQ) {
        append_machine_info(msg, state);
    }

    if (queue_push(state, msg) == -1) {
        send_status_to_comm(local_fd, state->machine_id, "File d emission pleine");
        return;
    }

    send_status_to_comm(local_fd, state->machine_id, "Message mis en attente du jeton");
}

static int handle_local_msg(struct driver_state *state, int local_fd)
{
    struct ring_msg msg;
    char host[108];
    int port;
    char next_host[108];
    int next_port;

    if (recv_ring_msg(local_fd, &msg) == -1) return -1;

    print_msg("Driver recoit depuis Comm:", &msg);

    if (msg.type == MSG_DATA && must_deliver_local(state, &msg)) {
        deliver_to_comm(state, &msg);
        return 0;
    }

    if (msg.type == MSG_JOIN) {
        if (parse_host_port(msg.data, host, sizeof(host), &port) == -1) {
            send_status_to_comm(local_fd, state->machine_id, "JOIN attend host_B port_B");
            return 0;
        }

        if (send_join_control_request(host, port, state->machine_id, state->left_port,
                                      next_host, sizeof(next_host), &next_port) == -1) {
            send_status_to_comm(local_fd, state->machine_id, "JOIN echoue");
            return 0;
        }

        state->join_pending = 1;
        state->active = 0;
        reconnect_right(state, local_fd, next_host, next_port);
        send_status_to_comm(local_fd, state->machine_id, "JOIN en attente du raccordement de B");
        return 0;
    }

    if (msg.type == MSG_LEAVE) {
        snprintf(msg.data, sizeof(msg.data), "%s %d %d",
                 state->right_host, state->right_port, state->left_port);
        msg.size = (int)strlen(msg.data);
        state->leave_after_send = 1;
        queue_local_request(state, local_fd, &msg);
        send_status_to_comm(local_fd, state->machine_id, "LEAVE en cours");
        return 0;
    }

    if (!state->active) {
        send_status_to_comm(local_fd, state->machine_id,
                            "Machine inactive: faire JOIN avant cette action");
        return 0;
    }

    if (state->right_fd == -1) {
        send_status_to_comm(local_fd, state->machine_id,
                            "Aucun voisin droit: anneau incomplet");
        return 0;
    }

    queue_local_request(state, local_fd, &msg);
    return 0;
}

static void handle_info_req(struct driver_state *state, int right_fd, int local_fd, struct ring_msg *msg)
{
    (void)right_fd;
    (void)local_fd;

    if (msg->src == state->machine_id) {
        msg->type = MSG_INFO_REP;
        msg->dst = state->machine_id;
        msg->flags = RING_FLAG_INFO_END;
        deliver_to_comm(state, msg);
        return;
    }

    append_machine_info(msg, state);
    if (send_ring_msg(right_fd, msg) == -1) FATAL("send_ring_msg right");
}

static int handle_left_msg(struct driver_state *state, int right_fd, int local_fd)
{
    struct ring_msg msg;
    struct ring_msg out;
    char host[108];
    int port;
    int ref_port;

    if (recv_ring_msg(state->left_fd, &msg) == -1) return -1;

    print_msg("Driver recoit depuis anneau:", &msg);

    if (msg.type == MSG_TOKEN) {
        state->last_token_ms = now_ms();

        if (queue_has_pending(state)) {
            if (queue_pop(state, &out) == -1) FATAL("queue_pop");

            printf("Driver emet une trame en attente puis retransmet le jeton\n");

            if (send_ring_msg(right_fd, &out) == -1) FATAL("send_ring_msg pending");
            send_status_to_comm(local_fd, state->machine_id, "Trame emise sur l anneau");

            if (out.type == MSG_LEAVE && out.src == state->machine_id && state->leave_after_send) {
                if (state->right_fd != -1) {
                    close(state->right_fd);
                    state->right_fd = -1;
                }
                if (state->left_fd != -1) {
                    close(state->left_fd);
                    state->left_fd = -1;
                }
                state->active = 0;
                state->leave_after_send = 0;
                send_status_to_comm(local_fd, state->machine_id, "Machine sortie de l anneau");
                return 0;
            }
        }

        if (right_fd != -1 && send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg token");
        return 0;
    }

    if (msg.type == MSG_BROADCAST) {
        if (msg.src == state->machine_id) {
            printf("Driver recupere sa propre diffusion, fin de tour\n");
            deliver_to_comm(state, &msg);
            return 0;
        }

        deliver_to_comm(state, &msg);
        if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg right");
        return 0;
    }

    if (msg.type == MSG_INFO_REQ) {
        handle_info_req(state, right_fd, local_fd, &msg);
        return 0;
    }

    if (msg.type == MSG_LEAVE) {
        if (parse_host_port_ref(msg.data, host, sizeof(host), &port, &ref_port) == -1) {
            return 0;
        }

        if (state->right_port == ref_port) {
            reconnect_right(state, local_fd, host, port);
            send_status_to_comm(local_fd, state->machine_id, "Machine retiree de l anneau");
            return 0;
        }

        if (msg.src == state->machine_id) {
            return 0;
        }

        if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg right");
        return 0;
    }

    if (must_deliver_local(state, &msg)) {
        deliver_to_comm(state, &msg);
    }

    if (must_forward_right(state, &msg)) {
        if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg right");
    }

    return 0;
}

static void driver_loop(struct driver_state *state)
{
    fd_set rfds;
    int maxfd;
    struct timeval tv;

    while (1) {
        FD_ZERO(&rfds);
        maxfd = -1;

        if (state->local_fd != -1) {
            FD_SET(state->local_fd, &rfds);
            maxfd = state->local_fd;
        } else {
            FD_SET(state->local_listen_fd, &rfds);
            maxfd = state->local_listen_fd;
        }

        FD_SET(state->left_listen_fd, &rfds);
        if (state->left_listen_fd > maxfd) maxfd = state->left_listen_fd;

        if (state->left_fd != -1) {
            FD_SET(state->left_fd, &rfds);
            if (state->left_fd > maxfd) maxfd = state->left_fd;
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) == -1) FATAL("select");

        if (state->local_fd == -1 && FD_ISSET(state->local_listen_fd, &rfds)) {
            state->local_fd = accept(state->local_listen_fd, NULL, NULL);
            if (state->local_fd == -1) FATAL("accept local");
            send_status_to_comm(state->local_fd, state->machine_id, "Comm local connecte");
        }

        if (state->local_fd != -1 && FD_ISSET(state->local_fd, &rfds)) {
            if (handle_local_msg(state, state->local_fd) == -1) {
                handle_local_disconnect(state);
            }
        }

        if (FD_ISSET(state->left_listen_fd, &rfds)) {
            if (state->left_fd == -1) {
                state->left_fd = accept(state->left_listen_fd, NULL, NULL);
                if (state->left_fd == -1) FATAL("accept left");
                send_status_to_comm(state->local_fd, state->machine_id, "Voisin gauche connecte");
                if (state->join_pending) {
                    state->join_pending = 0;
                    state->active = 1;
                    send_status_to_comm(state->local_fd, state->machine_id, "JOIN termine");
                }
            } else {
                handle_left_extra_connection(state, state->local_fd);
            }
        }

        if (state->left_fd != -1 && FD_ISSET(state->left_fd, &rfds)) {
            if (handle_left_msg(state, state->right_fd, state->local_fd) == -1) {
                handle_left_disconnect(state, state->local_fd);
            }
        }

        maybe_regenerate_token(state);
    }
}

int main(int argc, char *argv[])
{
    int machine_id;
    int left_port;
    int listen_fd;
    const char *right_host;
    int right_port;
    int inject_token;
    char local_path[RING_SOCK_PATH_MAX];
    struct driver_state state;

    if (argc != 6) {
        printf("Usage: %s machine_id left_port right_host right_port inject_token\n", argv[0]);
        exit(1);
    }

    machine_id = atoi(argv[1]);
    left_port = atoi(argv[2]);
    right_host = argv[3];
    right_port = atoi(argv[4]);
    inject_token = atoi(argv[5]);
    signal(SIGPIPE, SIG_IGN);

    memset(&state, 0, sizeof(state));
    state.machine_id = machine_id;
    state.left_port = left_port;
    state.right_port = right_port;
    snprintf(state.right_host, sizeof(state.right_host), "%s", right_host);
    state.local_fd = -1;
    state.left_fd = -1;
    state.right_fd = -1;
    state.active = 1;
    state.join_pending = 0;
    state.leave_after_send = 0;
    state.can_regen_token = inject_token;
    state.last_token_ms = now_ms();
    state.last_regen_ms = 0;

    listen_fd = create_local_server(machine_id, local_path, sizeof(local_path));
    state.local_listen_fd = listen_fd;
    state.left_listen_fd = create_left_server(left_port);

    printf("driver pret: machine=%d local=%s left_port=%d\n",
           machine_id, local_path, left_port);
    printf("driver attend la connexion de Comm...\n");

    state.local_fd = accept(listen_fd, NULL, NULL);
    if (state.local_fd == -1) FATAL("accept local");

    printf("driver connecte au Comm local\n");

    if (!is_detached_right(right_host, right_port)) {
        printf("driver tente la connexion vers le voisin droit %s:%d...\n", right_host, right_port);

        state.right_fd = connect_right_peer(right_host, right_port);
        printf("driver connecte au voisin droit\n");
        printf("driver attend la connexion du voisin gauche...\n");

        state.left_fd = accept(state.left_listen_fd, NULL, NULL);
        if (state.left_fd == -1) FATAL("accept left");

        printf("driver connecte au voisin gauche\n");
    } else {
        state.right_fd = -1;
        state.active = 0;
        state.join_pending = 0;
        printf("driver demarre hors anneau, en attente d'un JOIN\n");
    }

    if (inject_token && state.right_fd != -1) {
        inject_initial_token(machine_id, state.right_fd);
    }

    driver_loop(&state);

    if (state.left_fd != -1) close(state.left_fd);
    if (state.right_fd != -1) close(state.right_fd);
    if (state.left_listen_fd != -1) close(state.left_listen_fd);
    if (state.local_fd != -1) close(state.local_fd);
    if (state.local_listen_fd != -1) close(state.local_listen_fd);
    unlink(local_path);

    return 0;
}

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

/*
 * Structure principale du Driver.
 * --------------------------------
 * Elle regroupe tout l'etat necessaire a une machine logique:
 * - ses identifiants reseau (ports, hote du voisin droit),
 * - les descripteurs de sockets,
 * - son etat de participation a l'anneau,
 * - les indicateurs lies a JOIN / LEAVE,
 * - les compteurs utiles au jeton et aux messages,
 * - la file locale des trames en attente d'emission.
 */
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

    /* File circulaire des messages locaux en attente du jeton. */
    int q_head;
    int q_tail;
    int q_count;
    struct ring_msg queue[RING_QUEUE_MAX];
};

/*
 * Fonction: now_ms
 * ----------------
 * Retourne l'heure courante en millisecondes.
 * Le Driver utilise cette valeur pour mesurer le temps ecoule depuis le
 * dernier jeton recu et pour decider, si besoin, d'une regeneration.
 */
static long now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long)(tv.tv_sec * 1000L + tv.tv_usec / 1000L);
}

/*
 * Fonction: must_deliver_local
 * ----------------------------
 * Indique si une trame doit etre remise au Comm local.
 * Une machine inactive, c'est-a-dire hors anneau, ne consomme plus les
 * messages applicatifs. Sinon, on livre:
 * - tout message dont la destination vaut l'identifiant local,
 * - tout message de diffusion.
 */
static int must_deliver_local(const struct driver_state *state, const struct ring_msg *msg)
{
    /* Une machine inactive ne consomme plus les messages applicatifs. */
    if (!state->active) return 0;
    if (msg->dst == state->machine_id) return 1;
    if (msg->dst == RING_BROADCAST_ID) return 1;
    return 0;
}

/*
 * Fonction: must_forward_right
 * ----------------------------
 * Determine si une trame doit poursuivre sa circulation vers le voisin
 * droit. Certains messages s'arretent naturellement lorsqu'ils reviennent
 * a leur source, notamment les diffusions et les requetes d'information.
 */
static int must_forward_right(const struct driver_state *state, const struct ring_msg *msg)
{
    /* Un broadcast ou une requete d'info s'arrete lorsqu'il revient a sa source. */
    if (msg->type == MSG_BROADCAST && msg->src == state->machine_id) return 0;
    if (msg->type == MSG_INFO_REQ && msg->src == state->machine_id) return 0;
    if (msg->dst == state->machine_id) return 0;
    return 1;
}

/*
 * Fonction: queue_has_pending
 * ---------------------------
 * Test rapide indiquant si le Driver a au moins une trame locale en attente
 * d'emission. Cette file d'attente est necessaire car une machine ne peut
 * emettre qu'au moment ou elle recoit le jeton.
 */
static int queue_has_pending(const struct driver_state *state)
{
    return state->q_count > 0;
}

/*
 * Fonction: queue_push
 * --------------------
 * Ajoute une trame en fin de file circulaire.
 * Si la file est pleine, la fonction echoue pour eviter d'ecraser des
 * messages deja en attente.
 */
static int queue_push(struct driver_state *state, const struct ring_msg *msg)
{
    if (state->q_count >= RING_QUEUE_MAX) return -1;

    /* Insertion en fin de file. */
    state->queue[state->q_tail] = *msg;
    state->q_tail = (state->q_tail + 1) % RING_QUEUE_MAX;
    state->q_count++;
    return 0;
}

/*
 * Fonction: queue_pop
 * -------------------
 * Retire la trame la plus ancienne de la file circulaire.
 * On respecte ainsi un ordre FIFO entre les demandes d'emission locales.
 */
static int queue_pop(struct driver_state *state, struct ring_msg *msg)
{
    if (state->q_count == 0) return -1;

    /* Extraction en tete de file. */
    *msg = state->queue[state->q_head];
    state->q_head = (state->q_head + 1) % RING_QUEUE_MAX;
    state->q_count--;
    return 0;
}

/*
 * Fonction: send_status_to_comm
 * -----------------------------
 * Construit et envoie un message d'etat vers le Comm local.
 * Ce mecanisme est utilise pour rendre l'execution lisible depuis
 * l'interface utilisateur: attente du jeton, fin de JOIN, LEAVE en cours,
 * erreur de configuration, etc.
 */
static void send_status_to_comm(int local_fd, int machine_id, const char *text)
{
    struct ring_msg msg;

    /* Les messages d'etat servent a informer l'IHM locale. */
    if (local_fd == -1) return;

    ring_msg_init(&msg, MSG_INFO_REP, machine_id, machine_id);
    ring_msg_set_text(&msg, text);
    msg.flags = RING_FLAG_STATUS;

    if (send_ring_msg(local_fd, &msg) == -1) {
        perror("send_ring_msg status");
    }
}

/*
 * Fonction: deliver_to_comm
 * -------------------------
 * Livre une trame deja destinee a la machine locale.
 * Si la socket locale est cassee, le Driver invalide simplement la liaison
 * avec le Comm sans s'arreter. Cela permet au Driver de survivre a la
 * fermeture puis a la relance de l'interface utilisateur.
 */
static int deliver_to_comm(struct driver_state *state, const struct ring_msg *msg)
{
    if (state->local_fd == -1) return -1;

    /* Si le Comm local est perdu, on invalide simplement la liaison locale. */
    if (send_ring_msg(state->local_fd, msg) == -1) {
        perror("send_ring_msg local");
        close(state->local_fd);
        state->local_fd = -1;
        return -1;
    }

    return 0;
}

/*
 * Fonction: append_machine_info
 * -----------------------------
 * Ajoute dans la charge utile d'un message une ligne de description de la
 * machine courante. Cette fonction est au coeur de l'option "Recuperer"
 * car chaque Driver complete la meme trame au passage.
 */
static void append_machine_info(struct ring_msg *msg, const struct driver_state *state)
{
    char line[96];
    size_t cur;
    int n;

    /* Chaque Driver ajoute sa propre ligne a la reponse "Recuperer". */
    cur = (size_t)msg->size;
    n = snprintf(line, sizeof(line), "machine=%d portE=%d portS=%d active=%d\n",
                 state->machine_id, state->left_port, state->right_port, state->active);
    if (n <= 0) return;

    if (cur + (size_t)n >= RING_DATA_MAX) return;

    memcpy(msg->data + cur, line, (size_t)n);
    msg->size += n;
    msg->data[msg->size] = '\0';
}

/*
 * Fonction: print_msg
 * -------------------
 * Affiche une trace de debug lisible pour suivre la circulation des trames.
 * Elle sert uniquement a observer le comportement du protocole pendant les
 * tests et n'intervient pas dans la logique fonctionnelle.
 */
static void print_msg(const char *prefix, const struct ring_msg *msg)
{
    printf("%s type=%s src=%d dst=%d size=%d seq=%d flags=%d\n",
           prefix,
           ring_msg_type_name(msg->type), msg->src, msg->dst, msg->size, msg->seq, msg->flags);

    if (msg->size > 0 && msg->type != MSG_FILE_DATA) {
        printf("Contenu: %s\n", msg->data);
    }
}

/*
 * Fonction: create_local_server
 * -----------------------------
 * Cree la socket UNIX d'ecoute entre le Driver et son Comm local.
 * Le chemin est supprime au prealable pour eviter qu'un ancien fichier
 * de socket ne bloque le redemarrage du programme.
 */
static int create_local_server(int machine_id, char *path, size_t path_size)
{
    int sock;
    struct sockaddr_un addr;

    ring_make_local_path(path, path_size, machine_id);
    unlink(path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /* Socket locale UNIX entre Comm et Driver. */
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) FATAL("socket local");

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) FATAL("bind local");
    if (listen(sock, 1) == -1) FATAL("listen local");

    return sock;
}

/*
 * Fonction: create_left_server
 * ----------------------------
 * Cree la socket TCP d'ecoute correspondant au voisin gauche.
 * Dans ce projet, chaque liaison de l'anneau est orientee: le Driver lit
 * les trames depuis la gauche et les retransmet vers la droite.
 */
static int create_left_server(int port)
{
    int sock;
    int opt = 1;
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* Socket d'ecoute pour le voisin gauche de l'anneau. */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) FATAL("socket left");

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) FATAL("setsockopt left");
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) FATAL("bind left");
    if (listen(sock, 4) == -1) FATAL("listen left");

    return sock;
}

/*
 * Fonction: connect_right_peer
 * ----------------------------
 * Etablit la connexion TCP vers le voisin droit.
 * La fonction retente indefiniment tant que la connexion n'aboutit pas,
 * ce qui simplifie le lancement manuel des differents Drivers dans des
 * terminaux distincts.
 */
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

    /* La connexion vers le voisin droit est retentee tant qu'elle n'aboutit pas. */
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

/*
 * Fonction: parse_host_port
 * -------------------------
 * Extrait depuis une chaine deux informations de controle:
 * - un nom d'hote ou une adresse IP,
 * - un numero de port.
 * Ce format est utilise dans plusieurs messages d'administration.
 */
static int parse_host_port(const char *text, char *host, size_t host_size, int *port)
{
    (void)host_size;
    return sscanf(text, "%107s %d", host, port) == 2 ? 0 : -1;
}

/*
 * Fonction: parse_host_port_ref
 * -----------------------------
 * Variante de parse_host_port qui extrait en plus un port de reference.
 * Cette troisieme valeur est utile dans le message LEAVE pour permettre au
 * predecesseur de reconnaitre qu'il doit se reconnecter au successeur.
 */
static int parse_host_port_ref(const char *text, char *host, size_t host_size, int *port, int *ref_port)
{
    (void)host_size;
    return sscanf(text, "%107s %d %d", host, port, ref_port) == 3 ? 0 : -1;
}

/*
 * Fonction: is_detached_right
 * ---------------------------
 * Indique si la machine doit demarrer hors anneau.
 * Dans ce cas, aucun voisin droit initial n'est defini et la machine devra
 * utiliser l'operation JOIN avant de redevenir active.
 */
static int is_detached_right(const char *host, int port)
{
    if (port <= 0) return 1;
    if (strcmp(host, "-") == 0) return 1;
    if (strcmp(host, "none") == 0) return 1;
    return 0;
}

/*
 * Fonction: accept_left_with_timeout
 * ----------------------------------
 * Attend une connexion entrante sur une socket d'ecoute pendant un delai
 * borne. Cette attente bornee est utile lorsqu'on souhaite laisser vivre
 * le Driver sans le bloquer indefiniment sur un accept().
 */
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

/*
 * Fonction: fd_readable_with_timeout
 * ----------------------------------
 * Teste si un descripteur devient lisible dans un delai donne.
 * Le Driver s'en sert pour distinguer une vraie connexion de voisin gauche
 * d'une connexion de controle JOIN qui envoie immediatement une trame.
 */
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

/*
 * Fonction: reconnect_right
 * -------------------------
 * Remplace proprement le voisin droit courant par un nouveau voisin.
 * Cette primitive est reutilisee dans les trois cas principaux:
 * - insertion d'une nouvelle machine (JOIN),
 * - retrait d'une machine (LEAVE),
 * - reconfiguration explicite de l'anneau.
 */
static void reconnect_right(struct driver_state *state, int local_fd, const char *host, int port)
{
    int new_fd;

    /* Utilise lors d'un JOIN, d'un LEAVE ou d'une reconfiguration. */
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

/*
 * Fonction: send_join_control_request
 * -----------------------------------
 * Permet a une machine N hors anneau de contacter une machine B deja
 * presente pour lui demander l'identite de son successeur actuel C.
 *
 * Principe:
 * 1. N ouvre une connexion de controle temporaire vers B.
 * 2. N envoie un MSG_JOIN contenant son propre port d'ecoute gauche.
 * 3. B repond avec un MSG_NEIGHBOR_REP contenant son voisin droit courant.
 * 4. N peut alors se connecter a C avant que B ne bascule son lien vers N.
 */
static int send_join_control_request(const char *host_b, int port_b, int machine_id, int left_port,
                                     char *next_host, size_t next_host_size, int *next_port)
{
    int fd;
    struct ring_msg msg;
    struct ring_msg rep;

    /* La nouvelle machine demande a B quel est son successeur actuel. */
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

/*
 * Fonction: handle_left_extra_connection
 * --------------------------------------
 * Gere une connexion supplementaire arrivee sur le port gauche alors qu'un
 * voisin gauche est deja branche.
 *
 * Deux situations sont possibles:
 * - une connexion de controle JOIN, qui envoie tout de suite un MSG_JOIN;
 * - un nouveau voisin gauche qui se raccorde effectivement a l'anneau.
 *
 * La distinction est faite par une courte attente de lisibilite.
 */
static void handle_left_extra_connection(struct driver_state *state, int local_fd)
{
    int fd;
    int rc;
    struct ring_msg msg;
    struct ring_msg rep;
    char host[108];
    int port;

    /* Sur le port gauche, on peut recevoir soit un nouveau voisin, soit une
       connexion temporaire de controle pour l'operation JOIN. */
    fd = accept(state->left_listen_fd, NULL, NULL);
    if (fd == -1) return;

    rc = fd_readable_with_timeout(fd, 300);
    if (rc <= 0) {
        /* Aucun message immediat: on considere qu'il s'agit du nouveau voisin gauche. */
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
        /* B renvoie d'abord son ancien successeur, puis se reconnecte vers N. */
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

/*
 * Fonction: handle_local_disconnect
 * ---------------------------------
 * Traite la fermeture ou la perte du Comm local.
 * Le choix retenu est de conserver le Driver actif, puis de lui permettre
 * d'accepter plus tard une nouvelle connexion locale depuis un Comm relance.
 */
static void handle_local_disconnect(struct driver_state *state)
{
    int new_local;

    /* Le Driver reste vivant meme si le Comm local est ferme. */
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

/*
 * Fonction: handle_left_disconnect
 * --------------------------------
 * Gere la perte du voisin gauche.
 * Au lieu d'arreter brutalement la machine, le Driver ferme l'ancienne
 * liaison puis attend un eventuel nouveau raccordement. Cette strategie
 * est utile pendant un JOIN ou apres certaines reconfigurations.
 */
static void handle_left_disconnect(struct driver_state *state, int local_fd)
{
    int new_left;

    /* En cas de perte du voisin gauche, on reste en attente d'un nouveau raccordement. */
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

/*
 * Fonction: inject_initial_token
 * ------------------------------
 * Cree puis envoie le tout premier jeton dans l'anneau.
 * Cette operation n'est executee qu'au demarrage par la machine designee
 * comme injectrice initiale, ou plus tard lors d'une regeneration.
 */
static void inject_initial_token(int machine_id, int right_fd)
{
    struct ring_msg msg;

    ring_msg_init(&msg, MSG_TOKEN, machine_id, RING_BROADCAST_ID);
    printf("Driver injecte le jeton initial\n");

    if (send_ring_msg(right_fd, &msg) == -1) FATAL("send_ring_msg token");
}

/*
 * Fonction: maybe_regenerate_token
 * --------------------------------
 * Regeneration simple du jeton en cas de perte supposee.
 * Si la machine courante est autorisee a le faire et qu'aucun jeton n'a
 * ete observe depuis un certain delai, elle reinjecte un nouveau jeton.
 *
 * Ce mecanisme reste pedagogique: il traite surtout la disparition du
 * jeton, pas la panne robuste d'un noeud complet.
 */
static void maybe_regenerate_token(struct driver_state *state)
{
    long now;

    /* Regeneration simple: une machine autorisee recree le jeton apres timeout. */
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

/*
 * Fonction: queue_local_request
 * -----------------------------
 * Prepare puis place dans la file d'attente une requete issue du Comm.
 * Le Driver complete ici certains champs protocolaires:
 * - la source locale,
 * - le numero de sequence pour les messages classiques,
 * - la ligne locale initiale pour une requete d'information.
 */
static void queue_local_request(struct driver_state *state, int local_fd, struct ring_msg *msg)
{
    msg->src = state->machine_id;

    /* Les transferts de fichiers gardent leurs numeros de sequence propres. */
    if (msg->type != MSG_FILE_REQ && msg->type != MSG_FILE_DATA && msg->type != MSG_FILE_ACK) {
        msg->seq = ++state->next_seq;
    }

    if (msg->type == MSG_INFO_REQ) {
        /* La source ajoute deja sa ligne avant la mise en circulation. */
        append_machine_info(msg, state);
    }

    if (queue_push(state, msg) == -1) {
        send_status_to_comm(local_fd, state->machine_id, "File d emission pleine");
        return;
    }

    send_status_to_comm(local_fd, state->machine_id, "Message mis en attente du jeton");
}

/*
 * Fonction: handle_local_msg
 * --------------------------
 * Traite une trame envoyee par le Comm local.
 * C'est ici que le Driver decide:
 * - de livrer immediatement un message boucle sur soi-meme,
 * - de lancer un JOIN,
 * - de preparer un LEAVE,
 * - ou simplement d'empiler la requete en attente du jeton.
 */
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
        /* Cas particulier utile pour tester une machine qui s'envoie un message a elle-meme. */
        deliver_to_comm(state, &msg);
        return 0;
    }

    if (msg.type == MSG_JOIN) {
        /* JOIN: N contacte B, recupere C, se connecte a C puis attend que B se raccorde a elle. */
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
        /* LEAVE: la machine diffuse les informations necessaires pour que son
           predecesseure se reconnecte a son successeur. */
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

/*
 * Fonction: handle_info_req
 * -------------------------
 * Gere la propagation de la requete "Recuperer".
 * Chaque Driver ajoute ses informations dans la meme trame. Lorsque la
 * requete revient a la machine source, celle-ci la transforme en reponse
 * finale et la remet uniquement a son Comm local.
 */
static void handle_info_req(struct driver_state *state, int right_fd, int local_fd, struct ring_msg *msg)
{
    (void)right_fd;
    (void)local_fd;

    /* Lorsque la requete revient a sa source, elle devient une reponse finale. */
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

/*
 * Fonction: handle_left_msg
 * -------------------------
 * Traite toute trame entrante provenant du voisin gauche.
 * Cette fonction centralise la logique reseau principale du Driver:
 * - circulation du jeton,
 * - emission des trames locales en attente,
 * - diffusion,
 * - recuperation d'informations,
 * - retrait dynamique d'une machine,
 * - livraison locale et retransmission generale.
 */
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
        /* Le jeton donne le droit d'emettre une trame locale en attente. */
        state->last_token_ms = now_ms();

        if (queue_has_pending(state)) {
            if (queue_pop(state, &out) == -1) FATAL("queue_pop");

            printf("Driver emet une trame en attente puis retransmet le jeton\n");

            if (send_ring_msg(right_fd, &out) == -1) FATAL("send_ring_msg pending");
            send_status_to_comm(local_fd, state->machine_id, "Trame emise sur l anneau");

            if (out.type == MSG_LEAVE && out.src == state->machine_id && state->leave_after_send) {
                /* La machine sortante se debranche logiquement apres l'envoi de son LEAVE. */
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
        /* Le broadcast est livre a chaque machine active, puis stoppe a son retour a la source. */
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
        /* Seul le predecesseur direct de la machine sortante doit se reconfigurer. */
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

/*
 * Fonction: driver_loop
 * ---------------------
 * Boucle centrale du Driver, fondee sur select().
 * Elle surveille simultanement:
 * - la connexion du Comm local,
 * - le port d'ecoute gauche,
 * - la liaison effective avec le voisin gauche.
 *
 * Cette boucle donne au Driver un comportement evenementiel simple a suivre
 * et bien adapte a une simulation de protocole.
 */
static void driver_loop(struct driver_state *state)
{
    fd_set rfds;
    int maxfd;
    struct timeval tv;

    /* Boucle centrale: surveillance du Comm local, du voisin gauche et
       des connexions de controle sur le meme port d'ecoute. */
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
                /* Premiere connexion du voisin gauche, ou raccordement apres un JOIN. */
                state->left_fd = accept(state->left_listen_fd, NULL, NULL);
                if (state->left_fd == -1) FATAL("accept left");
                send_status_to_comm(state->local_fd, state->machine_id, "Voisin gauche connecte");
                if (state->join_pending) {
                    state->join_pending = 0;
                    state->active = 1;
                    send_status_to_comm(state->local_fd, state->machine_id, "JOIN termine");
                }
            } else {
                /* Si le port gauche est deja occupe, une nouvelle connexion correspond
                   soit a un JOIN, soit a un remplacement du voisin gauche. */
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

/*
 * Fonction principale du Driver.
 * ------------------------------
 * Cette fonction:
 * 1. lit les parametres de lancement,
 * 2. initialise l'etat local,
 * 3. cree les sockets d'ecoute,
 * 4. attend la connexion du Comm,
 * 5. raccorde la machine a l'anneau initial si un voisin droit est fourni,
 * 6. ou demarre hors anneau si la machine doit rejoindre plus tard via JOIN.
 */
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

/* Comm du projet anneau. */
#include "ring_common.h"
#include <errno.h>
#include <libgen.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * Etat minimal du recepteur de fichier.
 * -------------------------------------
 * Le Comm peut recevoir un fichier bloc par bloc pendant que l'utilisateur
 * reste dans le menu. Il doit donc memoriser:
 * - le fichier actuellement ouvert,
 * - le nom logique du fichier en cours de reception.
 */
struct file_recv_state {
    FILE *fp;
    char name[RING_DATA_MAX];
};

static void drain_driver_messages(int local_fd, struct file_recv_state *st, int machine_id);

/*
 * Fonction: read_line_input
 * -------------------------
 * Lit une ligne complete depuis stdin puis supprime le retour a la ligne.
 * Cette routine evite les melanges delicats entre scanf() et fgets() et
 * sert de base a toutes les lectures utilisateur du menu.
 */
static int read_line_input(char *buf, size_t size)
{
    size_t n;

    /* Lecture ligne par ligne pour eviter les melanges scanf/fgets. */
    if (fgets(buf, (int)size, stdin) == NULL) return -1;

    n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') {
        buf[n - 1] = '\0';
    } else {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) {
        }
    }

    return 0;
}

/*
 * Fonction: read_int_input
 * ------------------------
 * Lit un entier depuis le terminal en affichant eventuellement un prompt.
 * La conversion utilise strtol() pour detecter proprement les saisies
 * invalides plutot que de faire confiance a l'utilisateur.
 */
static int read_int_input(const char *prompt, int *value)
{
    char line[64];
    char *end;
    long v;

    printf("%s", prompt);
    if (read_line_input(line, sizeof(line)) == -1) return -1;

    v = strtol(line, &end, 10);
    if (end == line || *end != '\0') return -1;

    *value = (int)v;
    return 0;
}

/*
 * Fonction: read_text_input
 * -------------------------
 * Variante simple de lecture de texte avec affichage d'un prompt.
 * Elle factorise les saisies de messages, chemins de fichiers et adresses.
 */
static int read_text_input(const char *prompt, char *buf, size_t size)
{
    printf("%s", prompt);
    return read_line_input(buf, size);
}

/*
 * Fonction: connect_local_driver
 * ------------------------------
 * Etablit la connexion locale avec le Driver via la socket UNIX du projet.
 * Le Comm retente plusieurs fois car, pendant les tests manuels, l'ordre
 * exact de lancement entre Driver et Comm peut varier.
 */
static int connect_local_driver(int machine_id)
{
    int sock;
    int tries;
    struct timeval pause_time;
    struct sockaddr_un addr;
    char path[RING_SOCK_PATH_MAX];

    ring_make_local_path(path, sizeof(path), machine_id);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) FATAL("socket local");

    pause_time.tv_sec = 0;
    pause_time.tv_usec = 200000;

    /* Le Comm peut etre lance juste avant ou juste apres le Driver:
       on retente donc la connexion locale pendant un court instant. */
    for (tries = 0; tries < 10; tries++) {
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return sock;
        }

        if (errno != ECONNREFUSED && errno != ENOENT) {
            perror("connect local");
            close(sock);
            return -1;
        }

        select(0, NULL, NULL, NULL, &pause_time);
    }

    perror("connect local");
    close(sock);
    return -1;
}

/*
 * Fonction: print_local_msg
 * -------------------------
 * Affiche de maniere uniforme une trame recue depuis le Driver local.
 * Suivant le type et les drapeaux, la meme structure ring_msg peut servir
 * de message utilisateur, de message d'etat ou de reponse "Recuperer".
 */
static void print_local_msg(const struct ring_msg *msg)
{
    /* Affichage uniforme des messages recus depuis le Driver local. */
    printf("Comm recoit type=%s src=%d dst=%d size=%d seq=%d\n",
           ring_msg_type_name(msg->type), msg->src, msg->dst, msg->size, msg->seq);

    if (msg->size > 0) {
        if (msg->flags & RING_FLAG_STATUS) {
            printf("Etat: %s\n", msg->data);
        } else if (msg->type == MSG_INFO_REP) {
            printf("Infos anneau:\n%s", msg->data);
        } else if (msg->type != MSG_FILE_DATA) {
            printf("Message: %s\n", msg->data);
        }
    }
}

/*
 * Fonction: wait_for_local_msg
 * ----------------------------
 * Attend pendant un delai borne qu'un message arrive depuis le Driver.
 * Cette attente temporisee est utile pour les operations qui doivent lire
 * une reponse sans pour autant bloquer indefiniment l'interface.
 */
static int wait_for_local_msg(int local_fd, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    int rc;

    FD_ZERO(&rfds);
    FD_SET(local_fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    rc = select(local_fd + 1, &rfds, NULL, NULL, &tv);
    if (rc == 0) return 0;
    if (rc == -1) {
        perror("select local");
        return -1;
    }

    return FD_ISSET(local_fd, &rfds) ? 1 : 0;
}

/*
 * Fonction: send_file_ack
 * -----------------------
 * Envoie un accuse de reception pour un bloc de fichier.
 * Le protocole de transfert implementa ici repose sur cet ACK pour
 * confirmer l'annonce du fichier puis chacun des blocs de donnees.
 */
static int send_file_ack(int local_fd, int machine_id, int dst, int seq)
{
    struct ring_msg ack;

    /* Chaque bloc recu est confirme par un ACK. */
    ring_msg_init(&ack, MSG_FILE_ACK, machine_id, dst);
    ack.seq = seq;

    if (send_ring_msg(local_fd, &ack) == -1) {
        perror("send_ring_msg ack");
        return -1;
    }

    return 0;
}

/*
 * Fonction: send_local_request
 * ----------------------------
 * Primitive commune d'envoi d'une requete du Comm vers son Driver local.
 * Elle centralise le test d'erreur sur la socket locale.
 */
static int send_local_request(int local_fd, const struct ring_msg *msg)
{
    if (send_ring_msg(local_fd, msg) == -1) {
        perror("send_ring_msg local");
        return -1;
    }

    return 0;
}

/*
 * Fonction: handle_file_msg
 * -------------------------
 * Gere les deux types de trames lies a la reception d'un fichier:
 * - MSG_FILE_REQ ouvre ou reinitialise le fichier de destination,
 * - MSG_FILE_DATA ecrit un bloc puis ferme le fichier sur le dernier bloc.
 *
 * Le Driver n'ecrit jamais sur disque: cette responsabilite appartient
 * entierement au Comm, conforme a la separation demandee par le sujet.
 */
static void handle_file_msg(struct file_recv_state *st, const struct ring_msg *msg)
{
    if (msg->type == MSG_FILE_REQ) {
        char outname[RING_DATA_MAX + 16];

        /* Le message d'annonce cree ou reinitialise le fichier de reception. */
        if (st->fp != NULL) {
            fclose(st->fp);
            st->fp = NULL;
        }

        snprintf(st->name, sizeof(st->name), "%s", msg->data);
        snprintf(outname, sizeof(outname), "received/recv_%s", st->name);

        if (mkdir("received", 0777) == -1 && errno != EEXIST) {
            perror("mkdir received");
            return;
        }

        st->fp = fopen(outname, "wb");
        if (st->fp == NULL) {
            perror("fopen recv");
            return;
        }

        printf("Reception du fichier dans %s\n", outname);
        return;
    }

    if (msg->type == MSG_FILE_DATA) {
        /* Chaque bloc est ecrit immediatement sur disque. */
        if (st->fp == NULL) {
            printf("Aucun fichier ouvert pour recevoir les blocs\n");
            return;
        }

        if (fwrite(msg->data, 1, (size_t)msg->size, st->fp) != (size_t)msg->size) {
            perror("fwrite recv");
            fclose(st->fp);
            st->fp = NULL;
            return;
        }

        fflush(st->fp);

        if (msg->flags & RING_FLAG_FILE_END) {
            fclose(st->fp);
            st->fp = NULL;
            printf("Reception du fichier terminee\n");
        }
    }
}

/*
 * Fonction: receive_one_msg
 * -------------------------
 * Recoit puis traite une seule trame venant du Driver local.
 * Les messages de transfert de fichier sont traites immediatement pour
 * permettre l'envoi automatique des ACK, alors que les autres messages
 * sont simplement affiches a l'utilisateur.
 */
static int receive_one_msg(int local_fd, struct ring_msg *msg, struct file_recv_state *st, int machine_id)
{
    if (recv_ring_msg(local_fd, msg) == -1) {
        perror("recv_ring_msg local");
        return -1;
    }

    /* Les messages de transfert sont traites a part pour envoyer l'ACK
       sans attendre une action manuelle de l'utilisateur. */
    if (msg->type == MSG_FILE_REQ || msg->type == MSG_FILE_DATA) {
        handle_file_msg(st, msg);
        send_file_ack(local_fd, machine_id, msg->src, msg->seq);
        return 0;
    }

    print_local_msg(msg);
    return 0;
}

/*
 * Fonction: wait_for_file_ack
 * ---------------------------
 * Attend l'accuse de reception d'un numero de sequence donne.
 * Pendant cette attente, le Comm continue a traiter les autres trames
 * entrantes afin de ne pas bloquer la reception d'un autre fichier ou de
 * messages d'etat emis par le Driver.
 */
static int wait_for_file_ack(int local_fd, int machine_id, struct file_recv_state *st, int seq)
{
    fd_set rfds;
    struct timeval tv;
    struct ring_msg msg;

    /* Pendant l'attente d'un ACK, on continue a traiter les autres messages
       entrants pour ne pas bloquer la reception d'un fichier. */
    while (1) {
        FD_ZERO(&rfds);
        FD_SET(local_fd, &rfds);
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        {
            int rc = select(local_fd + 1, &rfds, NULL, NULL, &tv);
            if (rc == 0) return -1;
            if (rc == -1) FATAL("select ack");
        }

        if (!FD_ISSET(local_fd, &rfds)) continue;

        if (recv_ring_msg(local_fd, &msg) == -1) {
            perror("recv_ring_msg ack");
            return -1;
        }

        if (msg.type == MSG_FILE_REQ || msg.type == MSG_FILE_DATA) {
            handle_file_msg(st, &msg);
            send_file_ack(local_fd, machine_id, msg.src, msg.seq);
            continue;
        }

        if (msg.type == MSG_FILE_ACK && msg.seq == seq) {
            return 0;
        }

        print_local_msg(&msg);
    }
}

/*
 * Fonction: send_user_message
 * ---------------------------
 * Construit une trame unicast a partir des informations saisies au clavier
 * puis la transmet au Driver local. L'emission effective dans l'anneau
 * reste ensuite sous le controle du jeton.
 */
static void send_user_message(int local_fd, int machine_id)
{
    struct ring_msg msg;
    int dst;
    char text[RING_DATA_MAX];

    if (read_int_input("Destination : ", &dst) == -1) {
        printf("Destination invalide\n");
        return;
    }
    if (read_text_input("Message : ", text, sizeof(text)) == -1) {
        printf("Lecture du message impossible\n");
        return;
    }

    ring_msg_init(&msg, MSG_DATA, machine_id, dst);
    ring_msg_set_text(&msg, text);

    send_local_request(local_fd, &msg);
}

/*
 * Fonction: broadcast_user_message
 * --------------------------------
 * Construit une trame de diffusion a destination de toutes les machines
 * actives de l'anneau. Comme pour l'unicast, seul le Driver decidera du
 * moment effectif de l'emission lorsqu'il recevra le jeton.
 */
static void broadcast_user_message(int local_fd, int machine_id)
{
    struct ring_msg msg;
    char text[RING_DATA_MAX];

    if (read_text_input("Message a diffuser : ", text, sizeof(text)) == -1) {
        printf("Lecture du message impossible\n");
        return;
    }

    ring_msg_init(&msg, MSG_BROADCAST, machine_id, RING_BROADCAST_ID);
    ring_msg_set_text(&msg, text);

    send_local_request(local_fd, &msg);
}

/*
 * Fonction: request_ring_info
 * ---------------------------
 * Lance la commande "Recuperer" puis attend la reponse finale complete.
 * La machine source envoie une requete MSG_INFO_REQ, chaque Driver ajoute
 * sa propre ligne, puis la source recoit en retour un MSG_INFO_REP marque
 * avec le drapeau RING_FLAG_INFO_END.
 */
static void request_ring_info(int local_fd, int machine_id, struct file_recv_state *st)
{
    struct ring_msg msg;
    struct ring_msg rep;
    int rc;
    int tries;

    /* La reponse finale est reconnue par le drapeau RING_FLAG_INFO_END. */
    ring_msg_init(&msg, MSG_INFO_REQ, machine_id, RING_BROADCAST_ID);
    ring_msg_set_text(&msg, "");

    if (send_local_request(local_fd, &msg) == -1) return;

    for (tries = 0; tries < 6; tries++) {
        rc = wait_for_local_msg(local_fd, 1000);
        if (rc == 0) continue;
        if (rc == -1) {
            printf("Erreur d'attente sur la reponse de l'anneau\n");
            return;
        }

        if (receive_one_msg(local_fd, &rep, st, machine_id) == -1) return;

        if (rep.type == MSG_INFO_REP && (rep.flags & RING_FLAG_INFO_END)) {
            return;
        }
    }

    printf("Timeout: aucune reponse complete de l'anneau\n");
}

/*
 * Fonction: transfer_file
 * -----------------------
 * Envoie un fichier a une machine distante en plusieurs etapes:
 * 1. ouverture locale du fichier,
 * 2. annonce du nom distant via MSG_FILE_REQ,
 * 3. envoi de blocs MSG_FILE_DATA,
 * 4. attente d'un ACK pour chaque etape.
 *
 * Cette logique fournit un transfert plus fiable qu'un simple envoi brut,
 * meme si elle reste volontairement simple dans ce projet pedagogique.
 */
static void transfer_file(int local_fd, int machine_id)
{
    struct ring_msg msg;
    char path[256];
    char remote_name[128];
    char *base;
    FILE *fp;
    int dst;
    size_t n;
    int seq = 0;
    int tries;
    struct file_recv_state recv_state;

    /* Le transfert commence par l'annonce du nom de fichier, puis l'envoi
       des blocs avec un ACK attendu pour chaque etape. */
    memset(&recv_state, 0, sizeof(recv_state));

    if (read_int_input("Destination : ", &dst) == -1) {
        printf("Destination invalide\n");
        return;
    }
    if (read_text_input("Chemin du fichier : ", path, sizeof(path)) == -1) {
        printf("Lecture du chemin impossible\n");
        return;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        perror("fopen send");
        return;
    }

    base = basename(path);
    snprintf(remote_name, sizeof(remote_name), "%s", base);

    ring_msg_init(&msg, MSG_FILE_REQ, machine_id, dst);
    ring_msg_set_text(&msg, remote_name);
    msg.flags = RING_FLAG_FILE_REQ;
    msg.seq = 0;

    for (tries = 0; tries < 3; tries++) {
        if (send_local_request(local_fd, &msg) == -1) {
            fclose(fp);
            return;
        }
        if (wait_for_file_ack(local_fd, machine_id, &recv_state, 0) == 0) break;
    }
    if (tries == 3) {
        printf("Aucun ACK recu pour l'annonce du fichier\n");
        fclose(fp);
        return;
    }

    while (1) {
        ring_msg_init(&msg, MSG_FILE_DATA, machine_id, dst);

        n = fread(msg.data, 1, sizeof(msg.data), fp);
        if (n == 0) break;

        msg.seq = ++seq;
        msg.size = (int)n;

        /* Le dernier bloc est repere soit par une taille plus petite que le
           buffer, soit par la fin du fichier. */
        if (n < sizeof(msg.data) || fgetc(fp) == EOF) {
            msg.flags |= RING_FLAG_FILE_END;
        } else {
            if (fseek(fp, -1L, SEEK_CUR) == -1) {
                perror("fseek send");
                fclose(fp);
                return;
            }
        }

        for (tries = 0; tries < 3; tries++) {
            if (send_local_request(local_fd, &msg) == -1) {
                fclose(fp);
                return;
            }
            if (wait_for_file_ack(local_fd, machine_id, &recv_state, msg.seq) == 0) break;
        }
        if (tries == 3) {
            printf("Aucun ACK recu pour le bloc %d\n", msg.seq);
            fclose(fp);
            return;
        }
    }

    fclose(fp);
    printf("Transfert termine\n");
}

/*
 * Fonction: send_join_request
 * ---------------------------
 * Prepare une demande JOIN depuis une machine actuellement hors anneau.
 * L'utilisateur renseigne ici la machine B deja presente dans l'anneau;
 * le Driver se chargera ensuite du protocole de raccordement.
 */
static void send_join_request(int local_fd, int machine_id)
{
    struct ring_msg msg;
    char host[108];
    int port;

    /* Le JOIN fournit a une machine hors anneau les coordonnees de B. */
    ring_msg_init(&msg, MSG_JOIN, machine_id, RING_BROADCAST_ID);
    if (read_text_input("Hote de B : ", host, sizeof(host)) == -1) {
        printf("Lecture de l'hote impossible\n");
        return;
    }
    if (read_int_input("Port d'ecoute de B : ", &port) == -1) {
        printf("Port invalide\n");
        return;
    }
    snprintf(msg.data, sizeof(msg.data), "%s %d", host, port);
    msg.size = (int)strlen(msg.data);
    send_local_request(local_fd, &msg);
}

/*
 * Fonction: send_leave_request
 * ----------------------------
 * Declenche un retrait volontaire de la machine courante.
 * Le Comm n'a pas besoin de remplir tous les details du message: le Driver
 * complete lui-meme les informations de successeur necessaires au LEAVE.
 */
static void send_leave_request(int local_fd, int machine_id)
{
    struct ring_msg msg;

    /* Le contenu exact du LEAVE est ensuite complete par le Driver. */
    ring_msg_init(&msg, MSG_LEAVE, machine_id, RING_BROADCAST_ID);
    msg.size = 0;
    send_local_request(local_fd, &msg);
}

/*
 * Fonction: drain_driver_messages
 * -------------------------------
 * Vide rapidement la file des messages locaux emis par le Driver.
 * Cette routine est surtout utile juste apres un JOIN ou un LEAVE afin
 * d'afficher sans action supplementaire toute la sequence d'etats.
 */
static void drain_driver_messages(int local_fd, struct file_recv_state *st, int machine_id)
{
    /* Utilise apres JOIN/LEAVE pour vider rapidement les statuts emis par le Driver. */
    while (1) {
        int rc;
        struct ring_msg msg;

        rc = wait_for_local_msg(local_fd, 300);
        if (rc <= 0) return;

        if (receive_one_msg(local_fd, &msg, st, machine_id) == -1) {
            printf("Connexion locale au Driver perdue\n");
            return;
        }
    }
}

/*
 * Fonction: comm_loop
 * -------------------
 * Boucle principale de l'interface utilisateur.
 * Elle utilise select() pour rester reactive a deux evenements:
 * - une saisie clavier,
 * - un message entrant du Driver local.
 *
 * Ce choix permet de recevoir automatiquement les diffusions, les transferts
 * de fichiers et les statuts du Driver sans imposer une commande speciale
 * "recevoir" a l'utilisateur.
 */
static void comm_loop(int local_fd, int machine_id)
{
    int choix;
    struct file_recv_state recv_state;
    int stdin_fd;

    memset(&recv_state, 0, sizeof(recv_state));
    stdin_fd = STDIN_FILENO;

    /* Le Comm reste reactif a la fois au clavier et aux messages locaux. */
    while (1) {
        while (1) {
            fd_set rfds;
            int maxfd;

            printf("\n");
            printf("1. Emettre un message\n");
            printf("2. Diffuser un message\n");
            printf("3. Recuperer les infos de l anneau\n");
            printf("4. Transferer un fichier\n");
            printf("5. Demander JOIN\n");
            printf("6. Demander LEAVE\n");
            printf("7. Quitter\n");
            printf("Choix : ");
            fflush(stdout);

            FD_ZERO(&rfds);
            FD_SET(stdin_fd, &rfds);
            FD_SET(local_fd, &rfds);
            maxfd = stdin_fd > local_fd ? stdin_fd : local_fd;

            if (select(maxfd + 1, &rfds, NULL, NULL, NULL) == -1) FATAL("select menu");

            if (FD_ISSET(local_fd, &rfds)) {
                struct ring_msg msg;

                /* Reception automatique: utile pour broadcast, fichiers et statuts JOIN/LEAVE. */
                if (receive_one_msg(local_fd, &msg, &recv_state, machine_id) == -1) {
                    printf("Connexion locale au Driver perdue\n");
                    return;
                }
                continue;
            }

            if (FD_ISSET(stdin_fd, &rfds)) {
                if (read_int_input("", &choix) == -1) {
                    printf("Choix invalide\n");
                    continue;
                }
                break;
            }
        }

        if (choix == 1) {
            send_user_message(local_fd, machine_id);
        } else if (choix == 2) {
            broadcast_user_message(local_fd, machine_id);
        } else if (choix == 3) {
            request_ring_info(local_fd, machine_id, &recv_state);
        } else if (choix == 4) {
            transfer_file(local_fd, machine_id);
        } else if (choix == 5) {
            send_join_request(local_fd, machine_id);
            drain_driver_messages(local_fd, &recv_state, machine_id);
        } else if (choix == 6) {
            send_leave_request(local_fd, machine_id);
            drain_driver_messages(local_fd, &recv_state, machine_id);
        } else if (choix == 7) {
            break;
        } else {
            printf("Choix invalide\n");
        }
    }

    if (recv_state.fp != NULL) fclose(recv_state.fp);
}

/*
 * Fonction principale du Comm.
 * ----------------------------
 * Elle verifie les arguments, se connecte au Driver local correspondant a
 * la machine demandee puis lance la boucle interactive du menu.
 */
int main(int argc, char *argv[])
{
    int machine_id;
    int local_fd;

    if (argc != 2) {
        printf("Usage: %s machine_id\n", argv[0]);
        exit(1);
    }

    machine_id = atoi(argv[1]);
    local_fd = connect_local_driver(machine_id);
    if (local_fd == -1) return 1;

    printf("comm pret: machine=%d local=%d\n", machine_id, local_fd);

    comm_loop(local_fd, machine_id);

    close(local_fd);

    return 0;
}

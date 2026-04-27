/* API commune du projet. */
#include "ring_common.h"
#include <errno.h>
#include <unistd.h>

/*
 * Fonction: write_all
 * -------------------
 * Ecrit exactement "len" octets sur le descripteur passe en parametre.
 * Cette fonction encapsule write() pour eviter qu'une ecriture partielle
 * ne casse le protocole. C'est important ici car toutes les communications
 * du projet envoient des structures completes de type ring_msg.
 *
 * fd  : descripteur de socket ou de fichier deja ouvert.
 * buf : zone memoire contenant les donnees a ecrire.
 * len : nombre total d'octets attendus en sortie.
 *
 * Retour:
 *  0  si tous les octets ont bien ete envoyes,
 * -1  si une erreur definitive apparait ou si la connexion est fermee.
 */
int write_all(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    const char *p = (const char *)buf;

    /* On boucle jusqu'a l'ecriture complete de la trame. */
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
 * Fonction: read_all
 * ------------------
 * Lit exactement "len" octets depuis le descripteur fourni.
 * Comme pour write_all(), le but est d'obtenir une structure complete
 * avant de continuer le traitement. Le Driver et le Comm manipulent
 * ainsi des messages deja reconstitues en memoire.
 *
 * fd  : descripteur sur lequel on attend les donnees.
 * buf : tampon recevant les octets lus.
 * len : nombre exact d'octets a recuperer.
 *
 * Retour:
 *  0  si la lecture complete a reussi,
 * -1  si la lecture echoue ou si l'autre extremite ferme la connexion.
 */
int read_all(int fd, void *buf, size_t len)
{
    size_t recvd = 0;
    char *p = (char *)buf;

    /* Meme principe pour la lecture: on attend la structure complete. */
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

/*
 * Fonction: send_ring_msg
 * -----------------------
 * Envoie une trame complete de type ring_msg.
 * Cette fonction centralise le format d'emission du protocole pour que
 * toutes les parties du programme utilisent la meme primitive.
 */
int send_ring_msg(int fd, const struct ring_msg *msg)
{
    return write_all(fd, msg, sizeof(*msg));
}

/*
 * Fonction: recv_ring_msg
 * -----------------------
 * Recoit une trame complete de type ring_msg.
 * Le couple send_ring_msg / recv_ring_msg constitue l'interface de base
 * du protocole applicatif du projet.
 */
int recv_ring_msg(int fd, struct ring_msg *msg)
{
    return read_all(fd, msg, sizeof(*msg));
}

/*
 * Fonction: ring_msg_init
 * -----------------------
 * Initialise une trame avant remplissage.
 * Toute la structure est mise a zero pour repartir d'un etat sain, puis
 * les champs indispensables du protocole sont renseignes: type, source
 * et destination.
 */
void ring_msg_init(struct ring_msg *msg, int type, int src, int dst)
{
    /* Toutes les trames sont initialisees a zero avant remplissage. */
    memset(msg, 0, sizeof(*msg));
    msg->type = type;
    msg->src = src;
    msg->dst = dst;
}

/*
 * Fonction: ring_msg_set_text
 * ---------------------------
 * Copie un texte C dans la charge utile du message et met a jour le champ
 * "size". Cette routine est surtout utilisee pour les messages de statut,
 * les messages utilisateur et certaines requetes de controle.
 *
 * Si le texte est trop long, il est tronque proprement pour rester dans
 * la taille maximale definie par RING_DATA_MAX.
 */
void ring_msg_set_text(struct ring_msg *msg, const char *text)
{
    size_t n = strlen(text);

    /* La charge utile est tronquee si elle depasse la taille maximale. */
    if (n >= RING_DATA_MAX) n = RING_DATA_MAX - 1;

    memcpy(msg->data, text, n);
    msg->data[n] = '\0';
    msg->size = (int)n;
}

/*
 * Fonction: ring_msg_type_name
 * ----------------------------
 * Associe une valeur numerique de type de message a une chaine lisible.
 * Cette fonction sert uniquement au debug et aux traces d'execution afin
 * d'obtenir un affichage comprensible pendant les tests.
 */
const char *ring_msg_type_name(int type)
{
    switch (type) {
    case MSG_TOKEN:        return "MSG_TOKEN";
    case MSG_DATA:         return "MSG_DATA";
    case MSG_BROADCAST:    return "MSG_BROADCAST";
    case MSG_INFO_REQ:     return "MSG_INFO_REQ";
    case MSG_INFO_REP:     return "MSG_INFO_REP";
    case MSG_JOIN:         return "MSG_JOIN";
    case MSG_LEAVE:        return "MSG_LEAVE";
    case MSG_FILE_REQ:     return "MSG_FILE_REQ";
    case MSG_FILE_DATA:    return "MSG_FILE_DATA";
    case MSG_FILE_ACK:     return "MSG_FILE_ACK";
    case MSG_NEIGHBOR_REP: return "MSG_NEIGHBOR_REP";
    default:               return "MSG_UNKNOWN";
    }
}

/*
 * Fonction: ring_make_local_path
 * ------------------------------
 * Construit le chemin de la socket UNIX locale utilisee entre le Comm et
 * le Driver d'une meme machine logique.
 *
 * Le chemin inclut l'uid de l'utilisateur et l'identifiant de machine
 * pour eviter les collisions lorsque plusieurs etudiants executent leur
 * projet sur la meme machine pedagogique.
 */
void ring_make_local_path(char *path, size_t size, int machine_id)
{
    /* L'uid evite des collisions de sockets locales entre utilisateurs. */
    snprintf(path, size, "/tmp/ring_local_%ld_%d.sock", (long)getuid(), machine_id);
}

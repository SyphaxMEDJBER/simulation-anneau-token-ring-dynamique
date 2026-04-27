/*
 * Fichier commun a l'ensemble du projet.
 *
 * Ce header centralise :
 * - les constantes de taille ;
 * - les drapeaux utilises dans les trames ;
 * - les types de messages du protocole ;
 * - la structure unique representant une trame ;
 * - les prototypes des fonctions utilitaires partagees.
 *
 * L'objectif est que Driver et Comm manipulent exactement le meme
 * format de message et les memes primitives d'envoi / reception.
 */
#ifndef RING_COMMON_H
#define RING_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

/* Taille maximale de la charge utile d'une trame. */
#define RING_DATA_MAX 240

/* Taille maximale du chemin d'une socket UNIX locale. */
#define RING_SOCK_PATH_MAX 108

/*
 * Drapeaux de controle :
 * - STATUS   : message d'etat local envoye au Comm ;
 * - INFO_END : indique la fin de la reponse de type Recuperer ;
 * - FILE_END : indique le dernier bloc du transfert de fichier ;
 * - FILE_REQ : permet d'identifier l'annonce d'un transfert.
 */
#define RING_FLAG_STATUS   1
#define RING_FLAG_INFO_END 2
#define RING_FLAG_FILE_END 4
#define RING_FLAG_FILE_REQ 8

/* Identifiant special utilise pour les diffusions. */
#define RING_BROADCAST_ID (-1)

/*
 * Enumeration de tous les types de messages du protocole.
 *
 * Le protocole couvre :
 * - la circulation du jeton ;
 * - les messages unicast et broadcast ;
 * - les requetes d'information sur l'anneau ;
 * - l'insertion et le retrait dynamique ;
 * - le transfert de fichiers par blocs avec ACK ;
 * - une reponse technique de voisin utilisee pendant JOIN.
 */
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
    MSG_FILE_ACK,
    MSG_NEIGHBOR_REP
};

/*
 * Structure unique de toutes les trames echangees dans le projet.
 *
 * type  : nature de la trame (token, data, join, leave, etc.)
 * src   : identifiant de la machine source
 * dst   : identifiant de la machine destination
 * size  : taille utile de data
 * seq   : numero de sequence, surtout utile pour les transferts
 * flags : informations complementaires sur la trame
 * data  : charge utile de la trame
 */
struct ring_msg {
    int type;
    int src;
    int dst;
    int size;
    int seq;
    int flags;
    char data[RING_DATA_MAX];
};

/* Macro simple pour arreter le programme sur erreur critique. */
#define FATAL(msg) do { perror(msg); exit(1); } while (0)

/* Ecrit exactement len octets, sauf en cas d'erreur. */
int write_all(int fd, const void *buf, size_t len);

/* Lit exactement len octets, sauf en cas d'erreur ou fermeture. */
int read_all(int fd, void *buf, size_t len);

/* Envoie une trame complete sur le descripteur fd. */
int send_ring_msg(int fd, const struct ring_msg *msg);

/* Recoit une trame complete depuis le descripteur fd. */
int recv_ring_msg(int fd, struct ring_msg *msg);

/* Initialise proprement une trame avant remplissage. */
void ring_msg_init(struct ring_msg *msg, int type, int src, int dst);

/* Copie un texte dans data et met a jour size. */
void ring_msg_set_text(struct ring_msg *msg, const char *text);

/* Retourne le nom lisible d'un type de trame pour le debug. */
const char *ring_msg_type_name(int type);

/* Construit le chemin de la socket locale Comm <-> Driver. */
void ring_make_local_path(char *path, size_t size, int machine_id);

#endif

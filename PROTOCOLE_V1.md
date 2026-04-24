# Protocole V1

## But

Cette version 1 sert a lancer une premiere simulation simple et testable.
On cherche d'abord a faire fonctionner un anneau minimal avant d'ajouter les
fonctions avancees.

## Roles

### Driver

- gere la communication sur l'anneau
- recoit les trames venant du voisin gauche
- transmet les trames au voisin droit
- remet les trames locales au processus Comm
- gere le jeton

### Comm

- gere l'interface utilisateur
- demande l'emission d'un message
- recoit les messages livres localement
- affichage du menu et des messages recus

## Architecture V1

Pour la version 1, chaque machine logique contient :

- un processus `driver`
- un processus `comm`
- une socket locale entre `comm` et `driver`

L'anneau relie plusieurs `driver`.

## Types de messages V1

- `MSG_TOKEN` : jeton de permission d'emission
- `MSG_DATA` : message utilisateur unicast
- `MSG_BROADCAST` : message diffuse a tout l'anneau
- `MSG_INFO_REQ` : demande d'informations reseau
- `MSG_INFO_REP` : reponse a une demande d'informations
- `MSG_JOIN` : demande d'insertion dans l'anneau
- `MSG_LEAVE` : annonce de retrait de l'anneau
- `MSG_FILE_REQ` : demande de transfert de fichier
- `MSG_FILE_DATA` : bloc de fichier
- `MSG_FILE_ACK` : accuse de reception d'un bloc

## Format de trame propose

La V1 reste volontairement simple :

```c
#define RING_DATA_MAX 240

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
    MSG_FILE_ACK
};

struct ring_msg {
    int type;
    int src;
    int dst;
    int size;
    int seq;
    int flags;
    char data[RING_DATA_MAX];
};
```

## Sens des champs

- `type` : type de message
- `src` : identifiant de la machine source
- `dst` : identifiant de la machine destination
- `size` : taille utile de `data`
- `seq` : numero de sequence, utile surtout pour le transfert de fichier
- `flags` : champ reserve pour les variantes futures
- `data` : contenu du message

## Regles V1 de circulation

### Message unicast

- `comm` demande a `driver` d'envoyer une trame `MSG_DATA`
- `driver` n'emet que lorsqu'il possede le jeton
- la trame circule sur l'anneau
- chaque `driver` compare `dst` a son identifiant local
- si la trame ne lui est pas destinee, il la retransmet
- si la trame lui est destinee, il la livre a `comm`

### Jeton

- un seul jeton doit circuler
- si `driver` recoit le jeton et que `comm` n'a rien a envoyer, il le retransmet
- si `comm` a un message en attente, `driver` utilise le jeton pour emettre puis le retransmet

## Choix pour la premiere implementation

Pour commencer sans se disperser :

- anneau statique
- 2 machines minimum
- unicast seulement au debut
- socket UNIX `stream` pour le lien local `Comm` <-> `Driver`
- pas de connexion dynamique dans l'etape suivante
- pas de transfert de fichier dans l'etape suivante

## Fichiers prevus ensuite

- `ring_common.h`
- `ring_common.c`
- `ring_driver.c`
- `ring_comm.c`

Le fichier `ring_common.h` contient les declarations communes.
Le fichier `ring_common.c` contient leur implementation.
Le fichier `ring_driver.c` contient une premiere boucle minimale du Driver avec socket local UNIX.
Le fichier `ring_comm.c` contient une premiere interface locale minimale connectee au Driver.

## Critere de validation de la V1

La base est consideree correcte si :

- deux drivers peuvent etre relies en anneau
- chaque driver communique avec son comm local
- un message unicast peut partir d'une machine et arriver a l'autre
- le jeton circule sans bloquer le systeme

Simulation Anneau Token Ring Dynamique

Objectif

Simulation d’un réseau en anneau dynamique inspiré du protocole IEEE 802.5 (Token Ring).
Les machines sont représentées par des processus communicants via sockets.

Architecture

Chaque machine est composée de deux processus :

Driver : gestion du jeton et des communications sur l’anneau

Comm : interface utilisateur (émission, réception, transfert)

Communication interne via socket locale.
Communication externe via sockets unidirectionnelles formant l’anneau.

Fonctionnalites prevues

Émission unicast

Diffusion broadcast

Transfert de fichiers fiable (ASCII / binaire)

Connexion / Déconnexion dynamique

Gestion et régénération du jeton

Tolérance aux pannes

Premiere validation visee

- compiler `ring_driver` et `ring_comm`
- verifier que `Driver` et `Comm` partagent bien le meme format de trame
- valider le lien local UNIX entre `Driver` et `Comm`
- preparer ensuite la vraie mise en place de l'anneau

Compilation

```bash
make
```

Premier test local

Dans un premier terminal :

```bash
./bin/ring_driver 1 5001 127.0.0.1 5001 1
```

Dans un second terminal :

```bash
./bin/ring_comm 1
```

Si `Comm` envoie un message avec destination `1`, `Driver` le relivre localement.

Premier test a 2 drivers

Dans un premier terminal :

```bash
./bin/ring_driver 1 5001 127.0.0.1 5002 1
```

Dans un second terminal :

```bash
./bin/ring_driver 2 5002 127.0.0.1 5001 0
```

Puis lancer un `Comm` pour chaque machine :

```bash
./bin/ring_comm 1
./bin/ring_comm 2
```

Un message envoye par `Comm 1` vers destination `2` doit traverser l'anneau et
etre livre a `Comm 2`.

Gestion du jeton

- un seul `Driver` doit etre lance avec `inject_token = 1`
- si un `Comm` envoie un message distant, le `Driver` le garde en attente
- le message n'est emis sur l'anneau qu'a la reception du `MSG_TOKEN`
- apres emission, le jeton continue sa circulation

Fonctions utilisateur de base

- `Emettre` : envoi d'un message vers une machine destination
- `Diffuser` : envoi d'un message a tout l'anneau
- `Recuperer` : collecte des informations des machines de l'anneau
- la machine source recupere aussi sa propre diffusion quand elle revient
- les messages d'etat locaux sont distingues des vraies reponses d'information
- `Transferer un fichier` : envoi par blocs `MSG_FILE_REQ` / `MSG_FILE_DATA`
- `JOIN` / `LEAVE` : commandes presentes cote `Comm`
- `JOIN` / `LEAVE` : reconfiguration minimale du voisin droit en cours d'execution

Pour `Recuperer`, le message de requete circule dans l'anneau et chaque Driver
ajoute une ligne d'information avant le retour a la machine source.

Transfert de fichier

- le fichier source est lu en binaire
- un message `MSG_FILE_REQ` annonce le nom du fichier
- des blocs `MSG_FILE_DATA` suivent ensuite sur l'anneau
- le dernier bloc porte le drapeau `RING_FLAG_FILE_END`
- le recepteur cree un fichier `received/recv_<nom>`

Reconfiguration dynamique

- `JOIN` part d'une machine hors anneau `N` qui contacte une machine `B`
- `B` renvoie son voisin courant `C`, puis l'insertion devient `B -> N -> C`
- une machine hors anneau peut etre lancee avec `right_host = -` et `right_port = 0`
- `LEAVE` permet a une machine `N` de quitter l'anneau et de faire reconnecter `B -> C`
- si un voisin gauche disparait, le Driver attend automatiquement un nouveau
  rattachement pendant un court delai
- ce mecanisme permet une demonstration simple : anneau a 3 machines, insertion
  d'une 4e machine, puis retrait de cette machine

Nettoyage

```bash
make clean
```

Technologies

C

Sockets TCP

Communication inter-processus (IPC)

Linux

Concepts abordes

Conception de protocole réseau

Gestion de jeton

Architecture distribuée

Synchronisation et gestion des erreurs

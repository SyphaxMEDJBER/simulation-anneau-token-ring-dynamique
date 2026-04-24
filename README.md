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

Plan de travail en 9 etapes dans `etape.txt`.

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
./ring_driver 1
```

Dans un second terminal :

```bash
./ring_comm 1
```

Si `Comm` envoie un message avec destination `1`, `Driver` le relivre localement.

Premier test a 2 drivers

Dans un premier terminal :

```bash
./ring_driver 1 5001 127.0.0.1 5002
```

Dans un second terminal :

```bash
./ring_driver 2 5002 127.0.0.1 5001
```

Puis lancer un `Comm` pour chaque machine :

```bash
./ring_comm 1
./ring_comm 2
```

Un message envoye par `Comm 1` vers destination `2` doit traverser l'anneau et
etre livre a `Comm 2`.

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

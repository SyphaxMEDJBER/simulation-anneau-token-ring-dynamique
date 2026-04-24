Simulation Anneau Token Ring Dynamique
🎯 Objectif

Simulation d’un réseau en anneau dynamique inspiré du protocole IEEE 802.5 (Token Ring).
Les machines sont représentées par des processus communicants via sockets.

🏗 Architecture

Chaque machine est composée de deux processus :

Driver : gestion du jeton et des communications sur l’anneau

Comm : interface utilisateur (émission, réception, transfert)

Communication interne via socket locale.
Communication externe via sockets unidirectionnelles formant l’anneau.

⚙ Fonctionnalités prévues

Émission unicast

Diffusion broadcast

Transfert de fichiers fiable (ASCII / binaire)

Connexion / Déconnexion dynamique

Gestion et régénération du jeton

Tolérance aux pannes

Plan de travail en 9 etapes dans `etape.txt`.

Specification initiale du protocole dans `PROTOCOLE_V1.md`.

🛠 Technologies

C

Sockets TCP

Communication inter-processus (IPC)

Linux

📌 Concepts abordés

Conception de protocole réseau

Gestion de jeton

Architecture distribuée

Synchronisation et gestion des erreurs

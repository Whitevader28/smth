# Client server TCP - UDP

## Structura proiectului:
- server.cpp - serverul care face legatura intre clientii UDP si TCP
- server_tcp_com.cpp - fisierul care contine logica serverului
- subscriber.cpp - clientul TCP care se poate abona la server
- common.cpp - fisierul care contine structurile de date comune și funcții utilitare de rețea.
- helper.cpp - (Conținutul nu a fost furnizat, dar probabil conține funcții ajutătoare suplimentare)
- Makefile

## Logica:
1. Serverul deschide 2 socket-uri:
   - TCP pentru clientii care se aboneaza (pe care ascultă conexiuni noi)
   - UDP pentru clientii care trimit mesaje (pe care primește datagrame)
2. a. Clientii TCP se pot conecta la server, trimit un ID unic și se pot abona/dezabona la diferite topicuri folosind comenzi specifice (`subscribe <TOPIC>`, `unsubscribe <TOPIC>`)

   b. Clientii UDP trimit mesaje către server specificând un topic și un conținut
3. Serverul primește mesajele de la clientii UDP, le formatează și le retransmite către toți clienții TCP care sunt abonați la topicurile respective (inclusiv prin potrivire wildcard).

## Protocolul de comunicare TCP (între Server ski Clientul TCP):
Fiecare mesaj TCP schimbat între server și clientul TCP este structurat astfel:
1.  **Antet:**
    * `uint16_t len`: Reprezintă lungimea (în octeți) a payload-ului care urmează. Valoarea este trimisă în format network byte order (convertită folosind `htons` la trimitere și `ntohs` la primire).
    * `uint32_t type`: Un număr întreg ce identifică tipul mesajului (de ex., ID client, cerere de abonare, mesaj de la UDP, comandă de ieșire, eroare). Valorile corespund unui enum `MessageType` și sunt trimise în format network byte order (convertite folosind `htonl` la trimitere și `ntohl` la primire).
2.  **Payload:**
    * Urmează exact `len` octeți de date.
    * Conținutul payload-ului depinde de `type`-ul mesajului specificat în antet. De exemplu:
        * Pentru un mesaj de tip ID, payload-ul conține ID-ul clientului.
        * Pentru o cerere de abonare, payload-ul conține comanda completă (ex: "subscribe <TOPIC_NAME>").
        * Pentru un mesaj retransmis de la un client UDP, payload-ul conține mesajul formatat de server gata pentru afișare.

## Detalii de implementare:

### common.cpp
Acest fișier furnizează funcționalități de bază pentru operațiunile de rețea, partajate între server și client:
* **Funcții de trimitere/primire date:**
    * `recv_nonblocking` / `send_nonblocking`: Pentru operațiuni I/O pe socket-uri non-blocante.
    * `recv_all` / `send_all`: Pentru operațiuni I/O blocante, asigurând transferul complet al numărului specificat de octeți. Acestea sunt esențiale pentru protocolul TCP custom, pentru a citi antetul și apoi payload-ul complet.
* **Configurări socket:**
    * `disable_nagle(int socket_fd)`: Dezactivează algoritmul Nagle pentru socket-ul TCP specificat. Acest lucru este important pentru a reduce latența mesajelor mici, conform cerințelor.
    * `set_nonblocking(int sockfd)`: Setează un descriptor de socket în modul non-blocant.

### server.cpp
Bucla principală pentru server:
* **Inițializare:**
    * Primește portul ca argument din linia de comandă.
    * Creează socket-urile de ascultare: unul TCP (`listenfd_tcp`) și unul UDP (`listendfd_udp`) folosind funcțiile din `server_tcp_com.cpp`.
* **Bucla principală (`run_multiplexed`):**
    * Inițiază ascultarea pe socket-ul TCP.
* **Închidere:** Închide socket-urile de ascultare la terminarea programului.

### server_tcp_com.cpp
Conține logica completă a serverului pentru gestionarea clienților și a comunicării:
* **Clasa `ClientConnections`:**
    * **Multiplexare I/O:** Utilizează `poll` pentru a gestiona evenimente pe mai mulți descriptori de fișiere: `stdin`, socket-ul de ascultare TCP, socket-ul de ascultare UDP și toate socket-urile clienților TCP conectați.
    * **Gestionare FD:** Menține o listă de `pollfd` și un map (`fd_to_type`) pentru a asocia descriptori cu tipul lor.
    * **Starea Clientului (`ClientState`):** O structură/clasă ce stochează informații per client: socket-ul (`socketfd`), ID-ul (`id`), starea conexiunii (`connected`), un buffer de recepție (`recv_buffer`), o coadă de mesaje de trimis (`send_queue` de tip `std::deque<std::vector<char>>`), offset-ul curent de trimitere (`current_send_offset` pentru trimiteri parțiale), și un set de abonamente la topic-uri (`subscriptions` de tip `std::unordered_set<std::string>`).
    * **Gestionare Conexiuni TCP:**
        * `connect_tcp_client`: Acceptă noi conexiuni, creează o nouă instanță `ClientState`, o adaugă la `poll`. Așteaptă ca primul mesaj să fie ID-ul clientului.
        * `disconnect_tcp_client`: Închide socket-ul clientului, îl elimină din `poll`, marchează clientul ca deconectat în `ClientState` (dar păstrează ID-ul și abonamentele în `client_by_id`) și afișează mesajul "Client <ID_CLIENT> disconnected."
    * **Procesare Mesaje:**
        * `handle_stdin`: Citește comanda "exit" de la tastatură pentru a opri serverul.
        * `handle_udp_message`: Primește datagrame UDP. Extrage topicul (max 50 caractere), tipul de date (1 octet) și conținutul (max 1500 octeți).
        * `broadcast_udp_message`: Iterează prin toți clienții din `client_by_id`. Dacă un client este conectat și abonat la topicul mesajului UDP (verificat cu `match_topic`), îi trimite `ChatPacket`-ul.
        * `send_message_to_client`: Adaugă `ChatPacket`-ul serializat (cu antet de lungime și tip) în `send_queue` a clientului. Activează `POLLOUT` pentru socket-ul clientului dacă nu era deja activat și coada nu era goală.
        * `process_tcp_command`:
            * Dacă clientul nu este încă "conectat" (nu și-a trimis ID-ul), primul mesaj trebuie să fie de tip `MSG_ID`. Validează ID-ul. Dacă ID-ul este deja în `client_by_id` și clientul respectiv este marcat ca `connected`, trimite o eroare "EINUSE" noului socket și îl închide. Dacă ID-ul există dar clientul nu e `connected`, este o reconectare: actualizează socket-ul și starea în `ClientState`-ul existent. Dacă ID-ul e nou, îl înregistrează. Afișează "New client <ID_CLIENT> connected from IP:PORT."
    * **Gestionarea Abonamentelor:**
        * `topic_subscribers`: Un `std::unordered_map<std::string, std::unordered_set<std::string>>` care mapează un model de topic (posibil cu wildcard) la un set de ID-uri de clienți abonați la acel model.
        * `subscribe_client_to_topic`: Adaugă modelul de topic la `subscriptions` din `ClientState` și ID-ul clientului la setul corespunzător din `topic_subscribers`.
        * `unsubscribe_client_from_topic`: Elimină modelul din `ClientState` și ID-ul din `topic_subscribers`. Dacă un model nu mai are abonați, este eliminat din `topic_subscribers`.
        * `match_topic(const string& topic, const string& pattern)`: Implementează potrivirea wildcard. Desparte topicul și modelul în nivele (separate de '/'). Utilizează programare dinamică pentru a gestiona wildcard-ul `*` (poate înlocui oricâte nivele). Wildcard-ul `+` înlocuiește un singur nivel.
    * **Bucla `pollAll()`:**
        * Apelează `poll()` cu un timeout.
        * Iterează prin descriptorii de fișiere care au evenimente.
        * Gestionează `POLLERR`, `POLLHUP`, `POLLNVAL` (de obicei duc la deconectarea clientului sau oprirea serverului dacă e pe un socket de ascultare).
        * Dacă `POLLIN`:
            * Pe `listenfd_tcp`: Apelează `connect_tcp_client`.
            * Pe `listenfd_udp`: Apelează `handle_udp_message` (într-o buclă până când `recvfrom` returnează EAGAIN, deoarece UDP-ul e non-blocant).
            * Pe `STDIN_FILENO`: Apelează `handle_stdin`. Dacă se primește "exit", pregătește oprirea.
            * Pe un socket de client TCP: Apelează `handle_client_read`.
        * Dacă `POLLOUT` (și socket-ul este al unui client TCP): Apelează `handle_client_write`.
        * Returnează 1 dacă serverul trebuie să se oprească, altfel 0.
    * `onExit()`: Când serverul se oprește (comandă "exit"), trimite un mesaj `MSG_EXIT` tuturor clienților TCP conectați.

### subscriber.cpp
Implementează clientul TCP:
* **Inițializare:**
    * Primește ID_CLIENT, IP_SERVER, PORT_SERVER ca argumente.
    * Creează un socket TCP, se conectează la server la IP-ul și portul specificate.
    * Imediat după conectare, trimite un `ChatPacket` de tip `MSG_ID` cu ID-ul clientului către server folosind `send_packet`.
* **Bucla principală (`run_client`):**
    * Utilizează `poll` pentru a monitoriza evenimente pe `socket_fd` (conexiunea la server) și `STDIN_FILENO` (tastatură).
    * **Evenimente de la Server (`poll_fds[0].revents & POLLIN`):**
        * Apelează `receive_packet`.
        * Dacă primește `MSG_EXIT` de la server, se închide.
        * Dacă primește `MSG_ERROR` (ex: "EINUSE"), afișează eroarea și se închide.
        * Dacă primește `MSG_UDP_FORWARD`, afișează payload-ul (mesajul de la clientul UDP, formatat de server).
        * Dacă `receive_packet` returnează eroare sau 0 (conexiune închisă), clientul se oprește.
    * **Evenimente de la Tastatură (`poll_fds[1].revents & POLLIN`):**
        * Citește intrarea de la utilizator.
        * Dacă e "exit": trimite `MSG_EXIT` la server și se închide.
        * Dacă e "subscribe <TOPIC>": trimite `MSG_SUBSCRIBE` cu payload-ul "subscribe <TOPIC>" la server. Afișează "Subscribed to topic <TOPIC>".
        * Dacă e "unsubscribe <TOPIC>": trimite `MSG_UNSUBSCRIBE` cu payload-ul "unsubscribe <TOPIC>" la server. Afișează "Unsubscribed from topic <TOPIC>".
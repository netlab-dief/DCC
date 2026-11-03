# Generatore CAM integrato

[Panoramica del progetto](../README.md) · [Guida d'uso](../GUIDA_USO.md) ·
[Architettura](../CODEBASE_ANALYSIS.md)

`UtilsEncode/` e `Asn1_schemas/` provengono da `ETSI_stack/cam_c++`.
Il costruttore CAM è in `UtilsEncode/utils.c`, con gestione degli errori di
allocazione e codifica. `cam_message.cpp` sostituisce
il passaggio binario → esadecimale → binario e il payload fisso di 50 byte.
I valori dimostrativi del messaggio originale relativi a dimensioni veicolo,
quota, accelerazioni, curvatura, yaw rate e confidenze sono impostati come
indisponibili; i campi opzionali privi di sensore sono omessi. La sorgente RMC
fornisce posizione, velocità e direzione, senza confidenze di misura.

## Avvio su Linux

Compilare con `make` dalla cartella principale. Servono le dipendenze del DCC,
permessi per i socket raw e un'interfaccia monitor configurata per l'iniezione.
La seconda interfaccia passata al programma viene usata per trasmettere i CAM.

```bash
sudo ./dcc wlan0 wlan1 --cam-static
sudo ./dcc wlan0 wlan1 --cam-gps /dev/ttyACM0
sudo ./dcc wlan0 wlan1 --limeric --cam-static --cam-station-id 4316
```

La MAC viene letta dall'interfaccia. La modalità statica usa 44.6292708 N,
10.9479523 E, velocità zero e direzione indisponibile. La modalità GPS configura
la seriale a 9600 baud, ricompone righe parziali, verifica checksum e fix A di
GPRMC/GNRMC. Non invia con fix invalido o più vecchio di due secondi. Le
coordinate sono in 10⁻⁷ gradi, la velocità in cm/s e la direzione in 0.1 gradi.
Il tempo CAM GPS deriva da data e ora UTC del fix RMC rispetto al 2004, modulo
65536 ms. La data viene validata, compresi gli anni bisestili. Fix ripetuti o
fuori ordine non rinnovano la validità della sorgente; quelli più vecchi o più
avanti dell'orologio della macchina di oltre due secondi non generano messaggi.
La modalità statica e il timestamp GeoNetworking usano l'orologio UTC del
sistema, che deve essere sincronizzato.

Il frame contiene radiotap, QoS 802.11 broadcast, LLC/SNAP, GeoNetworking SHB,
BTP-B porta 2001 e l'intero payload CAM UPER. I campi di rete sono serializzati
esplicitamente e la lunghezza GN include il BTP. La scelta del bitrate resta
affidata alla configurazione radio del DCC; verificare sul driver che tale
configurazione sia effettivamente rispettata per i frame iniettati.

## Priorità CAM e code EDCA

Il flag opzionale `--cam-priority <0-7>` imposta la user priority nel campo TID
del QoS Control 802.11. Si applica sia alla modalità statica sia a quella GPS:

```bash
sudo ./dcc wlan0 wlan1 --cam-static --cam-priority 6
sudo ./dcc wlan0 wlan1 --cam-gps /dev/ttyACM0 --cam-priority 4
```

| Priorità / TID | Categoria EDCA |
| --- | --- |
| 0, 3 | AC_BE — Best Effort |
| 1, 2 | AC_BK — Background |
| 4, 5 | AC_VI — Video |
| 6, 7 | AC_VO — Voice |

Senza il flag viene usato il valore CAM predefinito: **TID 3, AC_BE**, con classe
GeoNetworking **TC2**. La corrispondenza TC2 → UP3 → AC_BE è definita nella
tabella 4 di [ETSI TS 102 636-4-2 V1.4.1](https://www.etsi.org/deliver/etsi_ts/102600_102699/1026360402/01.04.01_60/ts_1026360402v010401p.pdf).
Il profilo DP2 per CAM è descritto anche nell'annesso C.2 di
[ETSI TS 102 724 V1.1.1](https://www.etsi.org/deliver/etsi_ts/102700_102799/102724/01.01.01_60/ts_102724v010101p.pdf).
Il flag modifica il TID e la categoria EDCA; il messaggio rimane CAM con TC2.
Valori fuori da 0–7,
negativi, non numerici o mancanti vengono rifiutati. All'avvio il sender stampa
la priorità e la categoria selezionate.

Per i frame QoS iniettati tramite radiotap, mac80211 legge il TID per scegliere
la coda: la mappatura è quella di `ieee802_1d_to_ac` nel
[codice Linux mac80211](https://github.com/torvalds/linux/blob/master/net/mac80211/wme.c).
Non viene usato il numero della coda hardware come TID. L'effettivo uso delle
quattro categorie richiede il supporto del driver e della radio; con meno di
quattro code mac80211 ricade sulla coda disponibile. Il flag seleziona la
categoria, senza modificare AIFS/CW/TXOP o il controllo di ammissione DCC.

## Invio autonomo e stima dell'airtime

È sufficiente avviare il processo `dcc`: il generatore costruisce i CAM e il
sender li inietta sulla seconda interfaccia con `AF_PACKET/SOCK_RAW`. Non serve
un bridge esterno e non vengono inviati datagrammi sulla porta 5557.

Per ogni pacchetto il sender legge il bitrate legacy corrente del manager DCC,
lo inserisce nel campo [radiotap RATE](https://radiotap.org/fields/Rate.html)
e stima l'airtime della stessa versione del frame. Il bitrate del pacchetto
resta fissato durante la sua ammissione: un aggiornamento concorrente del DCC
vale per la selezione successiva, senza cambiare retroattivamente la stima.

La lunghezza comprende 802.11 QoS, LLC/SNAP, GeoNetworking, BTP e CAM, con tutti
i contenitori effettivamente presenti; il modello aggiunge 4 byte FCS ed esclude
l'header radiotap. Per OFDM considera SERVICE, TAIL, arrotondamento al numero
intero di simboli, preambolo e SIGNAL:

```text
L = lunghezza del frame senza radiotap, in byte
R = bitrate legacy nominale del DCC/Netlink, in Mb/s
N_DBPS = 4 × R
T_stimato_10MHz = 40 + 8 × ceil((16 + 8 × (L + 4) + 6) / N_DBPS)  [µs]
```

Il DCC usa i valori nominali della tabella legacy Linux (6, 12, 18, 24 Mb/s).
A 10 MHz il bitrate fisico della stessa modulazione/codifica è metà del valore
nominale a 20 MHz. Il modello usa i tempi a **10 MHz** (simboli da 8 µs,
preambolo + SIGNAL da 40 µs); su una radio a 20 MHz sovrastima l'airtime di un
fattore due. Non rileva automaticamente la larghezza del canale. I riferimenti
PHY sono le tabelle C.1 e C.2 di
[ETSI EN 302 663 V1.3.1](https://www.etsi.org/deliver/etsi_en/302600_302699/302663/01.03.01_60/en_302663v010301p.pdf).

La stima riguarda un singolo invio broadcast senza ACK. Non include backoff,
attesa nel driver o eventuali tentativi aggiuntivi non osservati; richiede che
il driver rispetti il RATE legacy richiesto. È una stima, non una misura PHY.

Prima di inviare si chiede `ADMIT` al gate condiviso. Dopo un `sendto()` completo,
il sender invia internamente `ACCOUNT <id> <airtime_stimato_us>` allo stesso gate:
la riserva viene chiusa e il budget viene addebitato sulla stima, senza attendere
riscontri hardware. Il gate applica ancora intervallo minimo e budget δ.
Un invio fallito o parziale interrompe il sender senza inventare un esito
positivo; il gate conserva la riserva incerta. `REPORT` resta disponibile per
client esterni che dispongano davvero di misure di completamento PHY.

## Generazione CAM per veicolo

La politica di generazione segue il §6.1.3 di
[ETSI EN 302 637-2 V1.4.1](https://www.etsi.org/deliver/etsi_en/302600_302699/30263702/01.04.01_60/en_30263702v010401p.pdf):
`T_GenCam_DCC = clamp(intervallo_DCC, 100 ms, 1000 ms)`. Se il valore non è
disponibile si usano 100 ms. Il sender legge il valore aggiornato dal manager
DCC sia in modalità reattiva sia adattativa e conta il tempo dall'inizio della
generazione del CAM precedente, senza aggiungere un ulteriore intervallo
a partire dal ritorno della chiamata di invio. Il thread di generazione valuta le condizioni su
tick da 10 ms, anche durante l'attesa del gate, purché venga
regolarmente eseguito dal sistema.

Il primo CAM è generato appena la sorgente è valida. I successivi rispettano
sempre T_GenCam_DCC e sono generati per una delle seguenti condizioni:

- Variazione rispetto all'ultimo CAM accodato con successo al driver:
  distanza **oltre 4 m**, direzione **oltre 4°**, oppure velocità **oltre 0,5 m/s**.
  La direzione usa la distanza angolare minima attraverso 0°/360°; i valori
  indisponibili non attivano i trigger di velocità/direzione.
- Scadenza di T_GenCam, inizialmente **1000 ms**. Un trigger di movimento imposta
  T_GenCam all'intervallo effettivo dall'ultima generazione. Questo intervallo
  viene mantenuto per **N_GenCam = 3** CAM temporizzati consecutivi, poi torna
  a 1000 ms. Un nuovo trigger di movimento azzera il conteggio.

Da fermo il comportamento è quindi **1 Hz**; con variazioni di movimento può
arrivare a **10 Hz**, nei limiti del DCC e della frequenza dei fix disponibili.
La modalità statica segue la stessa politica di un veicolo fermo e non simula
trigger di movimento.

### Intervalli DCC in modalità normale

La tabella indica il limite inferiore di generazione, non un periodo fisso:
con sorgente ferma il timer normale resta a 1000 ms.

| Stato reattivo | Minimo DCC fornito | Minimo di generazione CAM |
| --- | --- | --- |
| RELAXED | 50 ms | 100 ms |
| ACTIVE_1 | 100 ms | 100 ms |
| ACTIVE_2 | 200 ms | 200 ms |
| ACTIVE_3 | 400 ms | 400 ms |
| RESTRICTIVE | 1000 ms | 1000 ms |

### Trigger forzato ogni 100 ms

Con `--cam-force-100ms` la generazione avviene ogni **100 ms nominali**, anche
da fermo, indipendentemente dai trigger dinamici, da N_GenCam e dal valore di
T_GenCam_DCC. Il primo CAM è immediato appena sono disponibili dati validi.

```bash
sudo ./dcc wlan0 wlan1 --cam-static --cam-force-100ms
sudo ./dcc wlan0 wlan1 --limeric --cam-gps /dev/ttyACM0 --cam-force-100ms
```

Con questo flag il periodo nominale di generazione è 100 ms in tutti gli stati
DCC, compreso RESTRICTIVE. Il flag richiede `--cam-static` o `--cam-gps`.
Senza flag rimane attiva la politica ETSI descritta sopra. Questa opzione forza la **generazione**: il gate
DCC continua a decidere quando consentire l'invio radio, quindi la frequenza
effettiva di trasmissione può essere inferiore a 10 Hz. Restano attivi i
controlli di validità GPS, la scadenza dei messaggi, lo slot che conserva solo
il CAM più recente e il contenitore low frequency con scadenza a 500 ms.
Un ritardo del processo non produce una raffica di CAM per recuperare i tick
persi; la generazione successiva dista almeno 100 ms da quella effettiva.

### Ammissione e validità dei messaggi

Il limite di 1000 ms si applica al parametro di generazione, non forza l'apertura
del gate. Il gate conserva il proprio addebito basato sull'airtime e richiede
misure CBR valide: sotto congestione o in assenza di riscontri, gli invii effettivi
possono essere più distanziati. Generazione e trasmissione sono separate: il CAM
viene costruito quando scatta il trigger; lo slot di attesa conserva solo il
più recente. Un pacchetto più vecchio di 1000 ms o con sorgente scaduta non è
selezionato per la trasmissione. Una costruzione che impiega almeno 50 ms viene
scartata senza consumare il trigger, per ritentare con dati freschi al controllo
successivo. Un nuovo CAM sostituisce quello ancora in attesa, senza accumulare
una raffica da inviare quando il gate riapre.
La lifetime GeoNetworking iniziale è 1000 ms (§5.3.4.1, tabella 2) e viene
ridotta prima dell'invio per sottrarre l'attesa locale, con risoluzione di 50 ms;
un pacchetto con meno di 50 ms residui viene scartato.

Il contenitore low frequency è incluso nel primo CAM e nel primo CAM generato
almeno **500 ms** dopo l'ultimo che lo conteneva. Non causa un invio autonomo a
500 ms. Se il CAM che lo portava è sostituito mentre attende la radio, il
contenitore è conservato nel sostituto. Contiene ruolo veicolo ordinario, luci
indisponibili (bit a zero) e lo storico dei fix disponibili, fino a 23 punti:
ordine dal più recente, offset rispetto al punto precedente e tempo in unità
da 10 ms. Lo storico è vuoto all'avvio e viene interrotto da un gap non
rappresentabile; risoluzione e qualità dipendono dai fix forniti. Questo
storico dei fix non implementa la selezione/compressione del percorso SAE J2735.

La configurazione corrente è un **veicolo passengerCar con ruolo ordinario**;
non richiede il contenitore special vehicle. Il codice non rappresenta una
certificazione dello stack o dell'hardware e non include una sorgente CAN,
le confidenze GNSS o un misuratore hardware dell’airtime.
Le notifiche UDP storiche sulla porta 5555 restano disponibili ai processi esterni.

## Telemetria

Con CAM attivi, `[CAM MONITOR]` mostra generati, sostituiti, scaduti,
invalidati, accodati, pacchetti contabilizzati con stima, errori e frequenza
**degli invii accettati dal driver**. `cam_telemetry_estimated_log.csv` salva
questi contatori, i tempi medi di coda/gate/invio e l'airtime stimato cumulativo.
Non vengono mostrate TX confermate dal driver: `Submitted_Hz` conta l'incremento
di `Submitted`, non ricezioni o completamenti PHY. Il file nuovo evita di
mescolare questi dati con il precedente schema di `cam_telemetry_log.csv`.
Vedi [colonne e definizioni](../GUIDA_USO.md#telemetria-cam).

## Verifica

`make test` comprende parsing GPS, codifica/decodifica UPER, struttura dei frame,
ammissione condivisa, soglie dei trigger, N_GenCam, ritorno a 1 Hz, contenitore
LF e path history, modalità forzata a 100 ms, generazione durante un invio bloccato, sostituzione/scadenza
dei CAM in attesa, contabilizzazione della stima e blocco dopo TX incerta. Questi test
non richiedono radio. La compilazione dell'applicazione completa e l'iniezione
richiedono Linux e vanno verificate con il driver e la radio di destinazione.

# Guida all'uso di DCC

## Scopo del programma

Questo programma implementa un sistema di **Distributed Congestion Control (DCC)** per reti veicolari wireless. Misura il Channel Busy Ratio (CBR) e modifica automaticamente:

- l'intervallo di trasmissione dei messaggi;
- la potenza di trasmissione;
- il bitrate radio.

Il programma può funzionare con una macchina a stati reattiva oppure con il
controllore adattativo selezionato da `--limeric`. Il generatore CAM C++ è
opzionale e si abilita con una sorgente statica oppure GPS.

Per una panoramica vedi [README.md](README.md); i dettagli del generatore e del
riscontro TX sono in [cam/README.md](cam/README.md).

> Il software è pensato per Linux e modifica parametri reali delle interfacce wireless tramite `nl80211`. Per questo motivo deve normalmente essere eseguito con privilegi di amministratore.

## Requisiti

- Linux con supporto `nl80211`;
- compilatore compatibile con C++17 (`g++`);
- GNU Make;
- librerie di sviluppo OpenSSL;
- librerie di sviluppo `libnl-3` e `libnl-genl-3`;
- nomi delle interfacce wireless per misura/controllo e trasmissione;
- Python 3 per i test Python e il generatore CBR;
- per trasmettere CAM: interfaccia monitor con iniezione radiotap e bridge del
  driver per il completamento TX, descritto nella [guida CAM](cam/README.md#riscontro-dal-driver-richiesto).

Il bridge del driver non è incluso. Senza un riscontro valido, il sender CAM si
ferma dopo il primo tentativo di invio ammesso e l'applicazione segnala un errore.
I test senza radio non richiedono questo bridge.

Su Ubuntu o Debian è possibile installare le dipendenze principali con:

```bash
sudo apt update
sudo apt install build-essential pkg-config libssl-dev libnl-3-dev libnl-genl-3-dev iw python3
```

## Compilazione

Dalla cartella principale del progetto eseguire:

```bash
make
```

Al termine viene creato l'eseguibile `dcc` nella stessa cartella.

Per eliminare l'eseguibile e i file oggetto:

```bash
make clean
```

Per ricompilare completamente:

```bash
make clean
make
```

## Sintassi e opzioni

```text
./dcc <interfaccia_rete> <interfaccia_radiotap> [opzioni]
```

Senza opzioni si avvia il DCC reattivo, con generazione CAM disabilitata.

| Opzione | Predefinito | Effetto |
| --- | --- | --- |
| `--limeric` | Disattivata | Controllore adattativo di δ |
| `--fake-cbr` | Disattivata | CBR dal file `fake_cbr_value`; solo DCC reattivo |
| `--cam-static` | Disattivata | CAM da posizione statica di esempio |
| `--cam-gps <device>` | Disattivata | CAM da GPRMC/GNRMC su seriale a 9600 baud |
| `--cam-station-id <id>` | `4316` | Identificativo CAM tra 0 e 4294967295 |
| `--cam-priority <0-7>` | `3` | User priority/TID del frame QoS |
| `--cam-feedback-port <porta>` | `5557` | Porta del bridge locale: 1–65535, escluse 5555 e 5556 |
| `--cam-force-100ms` | Disattivata | Generazione forzata ogni 100 ms nominali |

`--cam-static` e `--cam-gps` sono alternativi. Il flag `--cam-force-100ms` richiede
una delle due sorgenti CAM. ID, priorità e porta di feedback hanno effetto quando
una sorgente CAM è abilitata. `--fake-cbr` e `--limeric` sono incompatibili.

## Identificare le interfacce di rete

Il programma richiede due nomi di interfaccia:

1. `interfaccia_rete`: usata per leggere il CBR e impostare la potenza di trasmissione;
2. `interfaccia_radiotap`: usata per impostare il bitrate e per **tutti gli invii
   del generatore CAM integrato**.

Per visualizzare le interfacce disponibili:

```bash
ip link show
```

Per controllare le interfacce wireless e le relative modalità:

```bash
iw dev
```

I nomi `wlan0` e `wlan1` usati negli esempi sono indicativi: devono essere sostituiti
con quelli presenti sul proprio sistema. Misura CBR e controllo devono riferirsi
al canale e alla radio che trasmettono i CAM. Il programma non crea interfacce
monitor e non configura il canale: l'interfaccia di iniezione deve essere già
pronta e supportata dal driver.

## Avvio in modalità reattiva

La sintassi è:

```bash
sudo ./dcc <interfaccia_rete> <interfaccia_radiotap>
```

Esempio:

```bash
sudo ./dcc wlan0 wlan1
```

In questa modalità il CBR determina uno dei cinque stati seguenti:

| Stato | CBR di inizializzazione | Minimo gate DCC | Potenza TX richiesta | Bitrate richiesto |
| --- | ---: | ---: | ---: | ---: |
| `RELAXED` | inferiore al 30% | 50 ms | 23 dBm | 6 Mbps |
| `ACTIVE_1` | dal 30% a meno del 40% | 100 ms | 19 dBm | 12 Mbps |
| `ACTIVE_2` | dal 40% a meno del 50% | 200 ms | 16 dBm | 18 Mbps |
| `ACTIVE_3` | dal 50% a meno del 65% | 400 ms | 13 dBm | 24 Mbps |
| `RESTRICTIVE` | almeno il 65% | 1000 ms | 10 dBm | 24 Mbps |

Quando cambia stato, il programma richiede i nuovi parametri radio e invia il
nuovo stato come intero a 32 bit sulla porta UDP 5555 per eventuali processi
esterni. L'esito delle richieste hardware dipende dal driver.

Gli intervalli della tabella sono minimi del gate, non frequenze CAM fisse:
la generazione normale ha un minimo di 100 ms e da fermo lavora a 1 Hz.
L'airtime contabilizzato può richiedere attese più lunghe.

Le transizioni applicano una persistenza asimmetrica per evitare oscillazioni dovute a
picchi brevi del CBR:

- verso uno stato più restrittivo, il CBR deve rimanere almeno pari alla soglia
  superiore per 1 secondo continuo (`min(CBR negli ultimi 1 s) >= soglia`);
- verso uno stato meno restrittivo, il CBR deve rimanere strettamente inferiore alla
  soglia inferiore per 5 secondi continui (`max(CBR negli ultimi 5 s) < soglia`).

Dopo l’inizializzazione, ogni transizione attraversa un solo stato adiacente, anche
se il CBR cambia bruscamente. La persistenza riparte dopo ogni passaggio.

La prima lettura valida inizializza immediatamente lo stato; le regole di persistenza
si applicano ai cambiamenti successivi. Una lettura non valida interrompe la finestra
di persistenza in corso.

## Avvio in modalità LIMERIC

Per usare l'algoritmo adattativo LIMERIC aggiungere l'opzione `--limeric`:

```bash
sudo ./dcc wlan0 wlan1 --limeric
```

La modalità adattativa implementa il controllo di δ della ETSI TS 102 687
V1.2.1, §5.4 e tabella 3. Un thread dedicato acquisisce il CBR su tick UTC da 100 ms;
δ viene aggiornato ogni 200 ms, utilizzando due campioni consecutivi e il filtro
previsto. I valori CBR sono frazioni 0–1, non percentuali. I parametri sono
α=0.016, β=0.0012, target=0.68, δ_min=0.0006, δ_max=0.03,
G+=0.0005, G−=−0.00025. Inizializzazione: CBR filtrato=0 e δ=δ_min.
Campioni invalidi o tick mancanti interrompono la coppia di osservazioni.

Non viene più ipotizzato un airtime costante di 0.5 ms. Il controllo per pacchetto
usa il servizio di ammissione descritto sotto. L’intervallo inviato sulla porta
5555 è solo indicativo: prima del primo feedback vale 1000 ms, poi deriva
dall’airtime contabilizzato. Il primo valore viene notificato sempre; i successivi
quando differiscono di almeno 5 ms dall’ultimo notificato. Potenza e bitrate
seguono ancora una politica specifica del progetto, separata dal controllo ETSI di δ.

## Avvio e generazione CAM

I sorgenti del codec e gli schemi ASN.1 sono inclusi in `cam/`; non serve il
checkout separato di `ETSI_stack`. Il sender usa sempre la seconda interfaccia.
Per gli esempi seguenti serve il bridge del driver descritto nella
[guida CAM](cam/README.md#riscontro-dal-driver-richiesto).

```bash
# DCC reattivo e dati statici: generazione normale a 1 Hz da fermo
sudo ./dcc wlan0 wlan1 --cam-static

# DCC adattativo e dati GPS
sudo ./dcc wlan0 wlan1 --limeric --cam-gps /dev/ttyACM0

# Generazione forzata a 10 Hz nominali, anche da fermo
sudo ./dcc wlan0 wlan1 --cam-static --cam-force-100ms

# GPS, generazione forzata e priorità CAM predefinita esplicita
sudo ./dcc wlan0 wlan1 --cam-gps /dev/ttyACM0 --cam-force-100ms --cam-priority 3
```

### Politica normale

Il minimo di generazione è `T_GenCam_DCC = clamp(intervallo_DCC, 100 ms, 1000 ms)`;
se il parametro non è disponibile si usano 100 ms. La politica segue il §6.1.3 di
[ETSI EN 302 637-2 V1.4.1](https://www.etsi.org/deliver/etsi_en/302600_302699/30263702/01.04.01_60/en_30263702v010401p.pdf).

Il primo CAM è immediato appena la sorgente è valida. I successivi rispettano il
minimo DCC e sono generati quando posizione, direzione o velocità differiscono
rispetto all'ultimo CAM con TX confermata di **oltre 4 m, 4° o 0,5 m/s**, oppure
quando scade il timer T_GenCam. Il timer parte da 1000 ms; un trigger di movimento
lo imposta al tempo effettivo dall'ultima generazione. Dopo tre CAM temporizzati
consecutivi (N_GenCam=3) torna a 1000 ms. Un nuovo trigger di movimento azzera
il conteggio. Un veicolo fermo genera quindi a 1 Hz.

### Generazione forzata

`--cam-force-100ms` genera ogni 100 ms nominali, ignorando movimento, N_GenCam e
il minimo di generazione fornito dal DCC. Restano attivi la validità dei dati e
il controllo di accesso radio: **generare a 10 Hz non garantisce trasmettere a
10 Hz**. Senza flag vale la politica normale. I tick persi non vengono recuperati
con una raffica di messaggi.

### Dati, messaggi in attesa e priorità

La generazione viene controllata ogni 10 ms nominali e continua mentre il
trasmettitore attende il gate o il feedback. Rimane in attesa solo il CAM più
recente; dati non validi o scaduti svuotano lo slot. Il contenitore low frequency
è incluso nel primo CAM e nel primo successivo alla sua scadenza di 500 ms;
questa scadenza non provoca da sola un invio.

La sorgente statica usa 44.6292708 N, 10.9479523 E, velocità zero e direzione
indisponibile. Per GPS servono checksum e fix validi, data/ora UTC coerenti con
l'orologio del sistema e dati recenti. Sensori non forniti da RMC sono indicati
come indisponibili o omessi. Lo storico dei fix e i suoi limiti sono descritti
nella [guida CAM](cam/README.md).

Il predefinito è **GeoNetworking TC2, priorità/TID 3, coda AC_BE**, secondo
[ETSI TS 102 636-4-2 V1.4.1, tabella 4](https://www.etsi.org/deliver/etsi_ts/102600_102699/1026360402/01.04.01_60/ts_1026360402v010401p.pdf).
Il flag `--cam-priority` modifica il TID lasciando TC2 al messaggio CAM:

| Valori | Categoria EDCA |
| --- | --- |
| 0, 3 | AC_BE |
| 1, 2 | AC_BK |
| 4, 5 | AC_VI |
| 6, 7 | AC_VO |

Questa priorità dei pacchetti è distinta dalla priorità di scheduling del thread
DCC. L'effettiva selezione della coda richiede supporto del driver e della radio.

## Informazioni mostrate a schermo

Durante l'esecuzione, il monitor principale stampa ogni secondo:

- CBR corrente in percentuale (`-1` quando la misura è assente, non valida o vecchia);
- stato DCC, oppure `DYNAMIC (LIMERIC)`;
- intervallo del controller (`T_off` nel monitor): minimo di stato in modalità
  reattiva, valore indicativo derivato dal gate in modalità adattativa.

Con CAM attivi, una riga `[CAM MONITOR]` riporta contatori e frequenza delle TX
confermate (vedi [Telemetria CAM](#telemetria-cam)). All'avvio il sender
CAM stampa interfaccia, priorità/categoria EDCA, politica di generazione e porta
richiesta per il riscontro del driver.

La modalità reattiva segnala inoltre ogni cambio di stato. La modalità adattativa stampa CBR, CBR filtrato, δ e intervallo indicativo a ogni notifica significativa.

Se appare un errore relativo a `SCHED_FIFO`, il programma non è riuscito ad assegnare priorità real-time 70 al worker. L'elaborazione può comunque continuare con la normale politica di scheduling.

## Arresto

Premere `Ctrl+C` oppure inviare SIGTERM al processo. Il programma arresta i
worker e chiude il file di telemetria. I messaggi finali del percorso normale sono:

```text
[MAIN] Arresto del thread worker DCC...
[MAIN] Thread fermato. Programma terminato con successo.
```

Se un invio CAM è già in attesa del riscontro, la chiusura può attendere fino a
due secondi. Un esito TX incerto non viene dichiarato completato.

## Telemetria CSV

Il programma crea o aggiorna nella directory di esecuzione il file:

```text
dcc_telemetry_log.csv
```

Il file è aperto in modalità append, quindi i nuovi dati vengono aggiunti alle esecuzioni precedenti. Le colonne sono:

| Colonna | Significato |
| --- | --- |
| `Timestamp` | data e ora locale in formato ISO 8601 |
| `CBR_Percent` | CBR percentuale |
| `State_Mode` | stato reattivo oppure `LIMERIC` |
| `Interval_ms` | minimo di stato reattivo o intervallo indicativo adattativo, in ms |
| `Tx_Power_dBm` | potenza richiesta in dBm |
| `MCS_Netlink` | bitrate richiesto tramite Netlink |
| `HW_Config_Changed` | `YES` su cambio di stato reattivo o notifica significativa adattativa; non certifica l'esito hardware |

I campi di potenza e bitrate sono richieste del controller, non misure. Il CSV
non registra il numero di CAM effettivamente trasmessi. Per conservare log
distinti, rinominare o spostare il CSV tra un'esecuzione e l'altra.

## Telemetria CAM

Attivando `--cam-static` oppure `--cam-gps`, viene creato automaticamente
`cam_telemetry_log.csv` nella directory di esecuzione. Il monitor e il CSV sono
aggiornati ogni secondo nominale dal ciclo principale; viene salvato anche un
campione finale dopo l'arresto del sender, inclusi i completamenti o gli errori
arrivati durante la chiusura. Il CSV è in append: i contatori ripartono da zero
per ogni avvio, identificato da `Session_Unix_ms`. Non servono nuovi flag.

| Colonna | Significato |
| --- | --- |
| `Session_Unix_ms`, `Timestamp_Unix_ms` | Avvio della sessione e istante del campione, millisecondi Unix UTC |
| `Elapsed_s` | Secondi monotoni dall'apertura del log |
| `Generated` | CAM costruiti con successo e pubblicati nello slot di attesa |
| `Replaced` | CAM ancora validi sostituiti nello slot da un CAM più recente |
| `Expired` | CAM rimossi dallo slot per scadenza o lifetime residua inferiore a 50 ms |
| `Invalidated` | CAM rimossi dallo slot perché la sorgente non è più valida; non conta i tick senza fix |
| `Construction_Overruns` | Costruzioni scartate per raggiungimento del limite di 50 ms, escluse da `Generated` |
| `Submitted` | Frame accodati con successo dal socket radio, ancora senza garanzia di completamento PHY |
| `TX_Confirmed` | Invii con feedback PHY valido e REPORT accettato dal gate |
| `TX_Errors` | Operazioni di ammissione/invio/feedback fallite; un arresto durante l'attesa non è un errore |
| `TX_Hz` | Incremento di `TX_Confirmed` diviso per i secondi monotoni effettivi dall'ultimo campione |
| `Queue_Mean_ms` | Media cumulativa, sui frame accodati, dalla generazione all'inizio della chiamata di invio |
| `Gate_Mean_ms` | Media cumulativa, sui frame accodati, dalla prima selezione della stessa versione al suo invio; riparte quando cambia il CAM selezionato |
| `Feedback_Mean_ms` | Media cumulativa, sulle TX confermate, dall'ammissione al ritorno del callback che invia e attende il feedback; include preparazione, invio e attesa del bridge |
| `PHY_Airtime_us` | Somma degli airtime reali riportati dalle TX confermate, non del debito prudenziale addebitato dal gate |

I contatori sono cumulativi nella sessione; `TX_Hz` riguarda il singolo intervallo
di campionamento. Le medie senza osservazioni valgono zero. Una TX incerta può
incrementare `Submitted` e `TX_Errors` senza incrementare `TX_Confirmed`.
Le conferme attestano il completamento locale segnalato dal bridge, non la ricezione
del broadcast da parte di altri veicoli. Le attese non misurano separatamente le
code interne del driver. I campioni concorrenti possono cogliere un CAM tra due
passaggi del suo ciclo di vita; i contatori non costituiscono un bilancio istantaneo.

Il log CAM si apre solo con CAM attivi. Un errore di apertura o di scrittura viene
segnalato; durante la chiusura un errore di salvataggio non impedisce l'arresto dei
worker. Il CSV DCC esistente mantiene il proprio formato.

## Notifiche per processi CAM esterni

Le notifiche cross-layer sono inviate via UDP a:

```text
127.0.0.1:5555
```

Il payload è un `int32_t` nel byte order nativo della macchina:

- modalità reattiva: valore dello stato (`RELAXED=1`, `ACTIVE_1=2`, `ACTIVE_2=3`, `ACTIVE_3=4`, `RESTRICTIVE=5`);
- modalità LIMERIC: nuovo intervallo in millisecondi.

Un processo esterno deve essere in ascolto sulla porta 5555 per ricevere queste
notifiche. Il generatore CAM integrato legge direttamente il manager e il gate,
senza usare questa porta. Le notifiche sono indicative e non autorizzano un
invio. In assenza di destinatari, il DCC continua a funzionare.

## Simulare il CBR per i test

La simulazione deve essere abilitata esplicitamente con il parametro `--fake-cbr`. Senza questo parametro, il programma ignora l'eventuale file `fake_cbr_value` e legge sempre il CBR dall'interfaccia wireless.

Quando `--fake-cbr` è attivo, il campionatore della modalità reattiva controlla il file `fake_cbr_value` nella directory dalla quale viene avviato `dcc`. Se il file contiene un numero valido, questo valore sostituisce la misura ottenuta dall'interfaccia.

Il valore deve essere compreso tra `0.0` e `1.0`. Per esempio, `0.45` rappresenta un CBR del 45%:

```bash
printf '0.45\n' > fake_cbr_value
sudo ./dcc wlan0 wlan1 --fake-cbr
```

Per cambiare manualmente lo scenario durante l'esecuzione:

```bash
printf '0.70\n' > fake_cbr_value
```

Per tornare alla lettura reale, rimuovere il file da un altro terminale:

```bash
rm fake_cbr_value
```

È disponibile anche lo script `cbr_generator.py`. Il suo output predefinito è `cbr`, mentre il programma legge `fake_cbr_value`; occorre quindi specificare esplicitamente il nome corretto:

```bash
python3 cbr_generator.py --output fake_cbr_value
```

In un secondo terminale avviare la modalità reattiva con la simulazione abilitata:

```bash
sudo ./dcc wlan0 wlan1 --fake-cbr
```

Lo script produce anche `cbr_trace_log.csv`. Il suo intervallo di scrittura
predefinito è 150 ms; il DCC continua a campionare su tick da 100 ms.
La simulazione sostituisce solo il CBR: richieste di potenza/bitrate ed eventuali
invii CAM coinvolgono ancora le interfacce reali. Un file assente o non leggibile
porta alla lettura radio; un numero letto ma fuori da 0–1 produce un campione
non valido. Il cambio di sorgente interrompe la continuità delle misure.
Il parametro `--fake-cbr` è disponibile solo in modalità reattiva e non può essere usato insieme a `--limeric`.

## Problemi comuni

### Parametri mancanti

Se il programma mostra `Errore: Parametri mancanti!`, passare entrambi i nomi di interfaccia:

```bash
sudo ./dcc wlan0 wlan1
```

### Interfaccia non trovata

Controllare i nomi con `ip link show` e `iw dev`. Verificare inoltre che l'interfaccia sia attiva:

```bash
sudo ip link set wlan0 up
```

### Operazione Netlink non consentita

Eseguire il programma con `sudo`. Se l'errore persiste, il driver o l'hardware potrebbero non supportare potenza fissa o bitrate legacy sulla banda configurata.

### Impossibile compilare per librerie mancanti

Verificare che `pkg-config` trovi libnl:

```bash
pkg-config --cflags --libs libnl-3.0 libnl-genl-3.0
```

Se il comando fallisce, installare i pacchetti di sviluppo indicati nella sezione Requisiti.

### Il CBR simulato non viene letto

Assicurarsi che:

- il file si chiami esattamente `fake_cbr_value`;
- `dcc` sia stato avviato dalla directory che contiene quel file;
- il contenuto sia un numero in forma frazionaria, ad esempio `0.35` e non `35`;
- il programma sia stato avviato con `--fake-cbr`;
- non sia stata selezionata anche la modalità `--limeric`.

### Il CAM viene generato ma non trasmesso

Controllare che la seconda interfaccia supporti l'iniezione, che il CBR sia
valido e che il bridge TX sia attivo sulla porta configurata. Il flag
`--cam-force-100ms` forza la generazione, ma non apre il gate DCC.
`CAM TX completion feedback timed out` indica che manca il completamento reale;
non va sostituito con un riscontro costruito dal solo accodamento.

### Nessun CAM in modalità GPS

Verificare dispositivo, 9600 baud, frasi GPRMC/GNRMC con checksum e fix A,
aggiornamento dei fix e orologio UTC sincronizzato. Fix duplicati o fuori ordine
non rinnovano la validità. In modalità normale l'assenza di movimento comporta
un solo CAM al secondo.

### Porta del gate occupata

Un secondo DCC non può occupare la stessa porta UDP 5556. Verificare le istanze
in esecuzione. `--cam-feedback-port` cambia solo la porta del bridge TX,
non quella del gate. Per recuperare da una TX incerta occorre prima fermare e
svuotare il trasmettitore e le code del driver, poi riavviare il sistema.

## Esempio completo

```bash
# Compilazione
make

# Controllo dei nomi delle interfacce
iw dev

# Avvio in modalità reattiva
sudo ./dcc wlan0 wlan1

# Arresto: Ctrl+C
```

Al termine è possibile consultare `dcc_telemetry_log.csv` per analizzare l'andamento del CBR e le decisioni del controller.


## Controllo per pacchetto dell’occupazione radio

In entrambe le modalità viene avviato un servizio UDP locale su `127.0.0.1:5556`.
La porta 5555 mantiene le notifiche precedenti. Se la porta 5556 è occupata,
l’avvio termina con un errore, senza proseguire silenziosamente senza servizio.

Il servizio è un gate condiviso per il singolo canale controllato dal processo.
Il trasmettitore CAM/non-CAM deve chiedere autorizzazione per **ogni** pacchetto,
inviare una sola volta dopo GRANT e comunicare l’airtime PHY dopo il completamento
reale della trasmissione. Una semplice chiamata a `send()` non è un riscontro TX.
Il trasmettitore CAM integrato usa direttamente questo gate; i trasmettitori
esterni possono usare il protocollo UDP seguente. Il servizio non può bloccare
traffico che lo aggira né misurare autonomamente l'airtime dei pacchetti. Tutti i
produttori sullo stesso canale devono integrarlo.

Protocollo ASCII, un messaggio per datagramma:

| Richiesta | Risposta | Significato |
| --- | --- | --- |
| `ADMIT <id> <airtime_us>` | `GRANT <id>` | Riserva un singolo pacchetto |
| `ADMIT <id> <airtime_us>` | `WAIT <id> <microsecondi>` | Attendere e riprovare, senza trasmettere |
| `REPORT <id> <airtime_us_reale>` | `DONE <id>` | TX completata, addebito aggiornato |
| richiesta non valida | `ERROR <motivo>` | Non trasmettere |

Gli ID sono interi positivi a 64 bit, crescenti globalmente fra le ammissioni;
`time.time_ns()` è il criterio usato dal client di esempio. I client concorrenti
devono gestire `stale_id` senza inviare pacchetti. Gli airtime sono interi in
microsecondi fra 1 e 1 000 000, inclusi overhead PHY ed eventuali ritrasmissioni.
La stima iniziale deve essere un limite prudente della durata del pacchetto
completo, ottenuto dalla configurazione PHY e dalla lunghezza o dal driver.
Un REPORT più grande corregge la sottostima; uno più piccolo non restituisce credito.

Una sola riserva può essere aperta sul canale. Non scade automaticamente:
se manca il REPORT, il gate resta chiuso. ADMIT duplicati non ripetono GRANT;
il REPORT dell’ultima riserva completata può essere ripetuto senza doppi addebiti.
Dopo un timeout di ammissione non trasmettere: l’esito può essere incerto.
Un riavvio richiede prima di fermare/svuotare il trasmettitore e le code del driver.

La riapertura avviene dopo `max(intervallo_minimo, airtime_addebitato / δ)`,
conteggiato dalla ricezione del REPORT. Questo pacing è volutamente prudente:
include un margine per code del driver e ritardi nel feedback. Durante una riserva
viene mantenuto il δ più restrittivo osservato. Un aggiornamento non cancella
il debito già accumulato. Il minimo adattativo è 25 ms; il reattivo usa l’intervallo
del proprio stato e δ=0.03 come ulteriore tetto di occupazione. Per pacchetti grandi
il tempo può superare un secondo: non viene applicato un taglio che cancellerebbe
il debito. È una politica conservativa, non una riproduzione esatta del gate
informativo dell’Allegato B. Il gate blocca nuove ammissioni se il controllo non
fornisce dati validi per 500 ms, o segnala una lettura non valida.

Il modulo `airtime_client.py` fornisce un client riutilizzabile:

```python
from airtime_client import AirtimeClient

# send_and_wait_for_tx_completion deve essere implementata dal trasmettitore:
# invia un solo pacchetto, attende il completamento PHY, restituisce l’airtime in µs.
with AirtimeClient() as gate:
    actual_us = gate.transmit(estimated_airtime_us, send_and_wait_for_tx_completion)
```

Il callback del client Python dipende dal driver e dal trasmettitore esterno;
il trasmettitore C++ integrato usa invece il protocollo di feedback in `cam/README.md`.
Il client non ritenta automaticamente dopo errori di rete o esiti TX incerti.

Riferimento: [ETSI TS 102 687 V1.2.1, §5.4 e Allegato B](https://www.etsi.org/deliver/etsi_ts/102600_102699/102687/01.02.01_60/ts_102687v010201p.pdf).
Le modifiche non costituiscono una certificazione di conformità dell’intero stack ITS.


## Acquisizione CBR separata dal controllo

La misura CBR viene eseguita da `CbrSampler` in un thread dedicato, sia in modalità
reattiva sia in modalità adattativa. Soltanto questo thread accede al lettore radio
o al file di test; i controllori ricevono i campioni tramite un buffer sincronizzato.
Ogni campione contiene valore 0–1, timestamp monotono di acquisizione, identificativo
della finestra UTC da 100 ms, sequenza e indicatore di validità. Il monitor legge
l’ultima misura direttamente dal campionatore, indipendentemente dai ritardi del controllo.

Le letture sono pianificate su scadenze assolute, senza aggiungere il tempo di
elaborazione al periodo. Il reattivo valuta ogni nuovo campione; l’adattativo
aggiorna δ solo con due finestre consecutive che terminano sul confine UTC da 200 ms.
Una configurazione radio lenta non blocca la misura. Il buffer conserva al massimo
16 campioni; i controllori scartano quelli più vecchi di 150 ms e interrompono la
persistenza/finestra di filtraggio quando mancano osservazioni, senza riutilizzare
l’ultimo CBR come nuova misura. Il tempo di persistenza reattivo usa il timestamp
della misura, non l’istante in cui il controllo riesce a elaborarla.

Il primo campione inizializza la base di misura. Una lettura oltre la sua finestra,
un valore non valido o un errore producono un campione non valido; la ripresa
richiede una nuova base e una finestra completa. Anche il passaggio fra file di test
e radio interrompe la continuità, evitando medie su contatori radio lasciati fermi.
Le attese di invio/ricezione Netlink del lettore CBR hanno timeout di 80 ms;
un’operazione composta può richiedere più attese. Un ritardo viene rilevato e non
viene presentato come finestra valida da 100 ms. La precisione reale dipende comunque
dallo scheduler Linux e dal driver: il periodo è una pianificazione, non una garanzia hardware.

Se non arriva un nuovo campione entro 150 ms, il controllo disabilita il gate e
interrompe la continuità dell’algoritmo. La validità del budget del gate parte dal
timestamp della misura, così elaborare un campione in ritardo non lo ringiovanisce.
L’arresto interrompe le attese sui timer e sveglia i controllori in attesa.


## Test

```bash
make test
```

La suite esegue test C++ su stati DCC, controllore adattativo, gate UDP,
campionamento CBR e CAM, più test Python del client e del generatore CBR.
I test CAM coprono anche il flag forzato, i trigger di movimento e N_GenCam.
Non servono radio o bridge TX; i test UDP richiedono accesso ai socket locali.
La verifica con driver e radio Linux resta distinta dai test senza hardware.

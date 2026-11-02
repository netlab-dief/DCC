# Guida all'uso di DCC

## Scopo del programma

Questo programma implementa un sistema di **Distributed Congestion Control (DCC)** per reti veicolari wireless. Misura il Channel Busy Ratio (CBR) e modifica automaticamente:

- l'intervallo di trasmissione dei messaggi;
- la potenza di trasmissione;
- il bitrate radio.

Il programma può funzionare con una macchina a stati reattiva oppure con l'algoritmo adattativo LIMERIC.

> Il software è pensato per Linux e modifica parametri reali delle interfacce wireless tramite `nl80211`. Per questo motivo deve normalmente essere eseguito con privilegi di amministratore.

## Requisiti

- Linux con supporto `nl80211`;
- compilatore compatibile con C++17 (`g++`);
- GNU Make;
- librerie di sviluppo OpenSSL;
- librerie di sviluppo `libnl-3` e `libnl-genl-3`;
- due interfacce wireless compatibili con le operazioni richieste dal programma.

Su Ubuntu o Debian è possibile installare le dipendenze principali con:

```bash
sudo apt update
sudo apt install build-essential pkg-config libssl-dev libnl-3-dev libnl-genl-3-dev iw
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

## Identificare le interfacce di rete

Il programma richiede due nomi di interfaccia:

1. `interfaccia_rete`: usata per leggere il CBR e impostare la potenza di trasmissione;
2. `interfaccia_radiotap`: usata per impostare il bitrate.

Per visualizzare le interfacce disponibili:

```bash
ip link show
```

Per controllare le interfacce wireless e le relative modalità:

```bash
iw dev
```

I nomi `wlan0` e `wlan1` usati negli esempi sono indicativi: devono essere sostituiti con quelli presenti sul proprio sistema.

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

| Stato | CBR | Intervallo | Potenza TX | Bitrate |
| --- | ---: | ---: | ---: | ---: |
| `RELAXED` | inferiore al 30% | 50 ms | 23 dBm | 6 Mbps |
| `ACTIVE_1` | dal 30% a meno del 40% | 100 ms | 19 dBm | 12 Mbps |
| `ACTIVE_2` | dal 40% a meno del 50% | 200 ms | 16 dBm | 18 Mbps |
| `ACTIVE_3` | dal 50% a meno del 65% | 400 ms | 13 dBm | 24 Mbps |
| `RESTRICTIVE` | almeno il 65% | 1000 ms | 10 dBm | 24 Mbps |

Quando cambia stato, il programma aggiorna i parametri radio e invia al processo CAM il nuovo stato come intero a 32 bit.

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

## Informazioni mostrate a schermo

Durante l'esecuzione, il monitor principale stampa ogni secondo:

- CBR corrente in percentuale (`-1` quando la misura è assente, non valida o vecchia);
- stato DCC, oppure `DYNAMIC (LIMERIC)`;
- intervallo di trasmissione corrente (`T_off`).

La modalità reattiva segnala inoltre ogni cambio di stato. La modalità adattativa stampa CBR, CBR filtrato, δ e intervallo indicativo a ogni notifica significativa.

Se appare un errore relativo a `SCHED_FIFO`, il programma non è riuscito ad assegnare priorità real-time 70 al worker. L'elaborazione può comunque continuare con la normale politica di scheduling.

## Arresto

Premere `Ctrl+C` nel terminale in cui è in esecuzione il programma:

```text
[MAIN] Ricevuto Ctrl+C (SIGINT). Avvio chiusura pulita...
```

Il worker viene arrestato e il file di telemetria viene chiuso correttamente.

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
| `Interval_ms` | intervallo di trasmissione in millisecondi |
| `Tx_Power_dBm` | potenza richiesta in dBm |
| `MCS_Netlink` | bitrate richiesto tramite Netlink |
| `HW_Config_Changed` | `YES` quando la variazione è considerata significativa, altrimenti `NO` |

Per conservare log distinti, rinominare o spostare il CSV tra un'esecuzione e l'altra.

## Comunicazione con il processo CAM

Le notifiche cross-layer sono inviate via UDP a:

```text
127.0.0.1:5555
```

Il payload è un `int32_t` nel byte order nativo della macchina:

- modalità reattiva: valore dello stato (`RELAXED=1`, `ACTIVE_1=2`, `ACTIVE_2=3`, `ACTIVE_3=4`, `RESTRICTIVE=5`);
- modalità LIMERIC: nuovo intervallo in millisecondi.

Il processo CAM deve essere già in ascolto sulla porta 5555 se si vogliono ricevere le notifiche. In sua assenza, il controllo DCC continua comunque a funzionare.

## Simulare il CBR per i test

La simulazione deve essere abilitata esplicitamente con il parametro `--fake-cbr`. Senza questo parametro, il programma ignora l'eventuale file `fake_cbr_value` e legge sempre il CBR dall'interfaccia wireless.

Quando `--fake-cbr` è attivo, la modalità reattiva controlla a ogni ciclo il file `fake_cbr_value` nella directory dalla quale viene avviato `dcc`. Se il file contiene un numero valido, questo valore sostituisce la misura ottenuta dall'interfaccia.

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

Lo script produce anche `cbr_trace_log.csv`. Il parametro `--fake-cbr` è disponibile solo in modalità reattiva e non può essere usato insieme a `--limeric`.

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
Il processo CAM e il driver di trasmissione non sono inclusi in questo repository:
il servizio non può bloccare traffico che lo aggira, né misurare autonomamente
l’airtime dei pacchetti esterni. Tutti i produttori sul canale devono integrarlo.

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

Il callback non è incluso perché dipende dal driver e dallo stack CAM esterno.
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

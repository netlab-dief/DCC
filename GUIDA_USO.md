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

## Avvio in modalità LIMERIC

Per usare l'algoritmo adattativo LIMERIC aggiungere l'opzione `--limeric`:

```bash
sudo ./dcc wlan0 wlan1 --limeric
```

In questa modalità l'intervallo viene adattato dinamicamente in base al CBR, entro i limiti di 25–1000 ms. Anche potenza e bitrate vengono regolati dal programma. Quando l'intervallo cambia di almeno 5 ms, il nuovo valore viene inviato al processo CAM come intero a 32 bit.

## Informazioni mostrate a schermo

Durante l'esecuzione, il monitor principale stampa ogni secondo:

- CBR corrente in percentuale;
- stato DCC, oppure `DYNAMIC (LIMERIC)`;
- intervallo di trasmissione corrente (`T_off`).

La modalità reattiva segnala inoltre ogni cambio di stato. LIMERIC stampa i principali aggiornamenti dell'intervallo, insieme a CBR filtrato, rate, potenza e bitrate.

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

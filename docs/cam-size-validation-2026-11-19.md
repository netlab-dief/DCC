# Verifica dimensioni CAM — 19 novembre 2026

Questo rapporto descrive le catture precedenti all'introduzione del selettore
geometrico. Per il comportamento successivo e i limiti di conformità aggiornati,
vedere [PathHistory](path-history.md).

Eseguite 12 configurazioni in 17 sessioni radio: 430 record di TX status CAM, altrettante copie di iniezione. Nessun errore di decodifica tshark, discrepanza nelle lunghezze GeoNetworking, flag TX failure o kernel drop nelle catture. Questi risultati verificano costruzione e invio al driver; non dimostrano la ricezione su una radio indipendente.

## Ambiente e metodo

- wlan0: OCB, 5900 MHz, canale da 10 MHz; wlan2: monitor sullo stesso phy1. wlan1 non modificata.
- `make test`: superati nove eseguibili C++ e 13 test Python. `make -j2 all`: build aggiornata.
- Ogni sessione ha una finestra di prova nominale di 5 secondi, oltre ad avvio e arresto. Il worker può completare un ciclo durante lo shutdown.
- GPS simulato con frasi GPRMC checksummed su pseudo-terminale, circa 10 aggiornamenti/s. Nessuna verifica di un ricevitore GPS fisico.
- Catture con tcpdump, analisi tshark e controllo binario delle lunghezze GN/BTP. I byte CAM nella tabella escludono radiotap, MAC, LLC/SNAP, GN e BTP.
- Per le configurazioni ripetute la tabella usa la seconda misura. Gli intervalli sono mediane tra TX status, non garanzie di frequenza continua.

## Misure

| Configurazione | TX status CAM | Byte CAM | Intervallo mediano, ms | Punti di cronologia osservati |
| --- | ---: | --- | ---: | --- |
| reactive-static | 6 | 43 | 1008.27 | 0 |
| reactive-forced | 58 | 41, 43 | 100.68 | 0 |
| adaptive-static | 6 | 43 | 1008.15 | 0 |
| adaptive-forced | 25 | 41, 43 | 100.69 | 0 |
| reactive-net | 46 | 41, 43 | 100.74 | 0 |
| adaptive-net | 44 | 41, 43 | 100.73 | 0 |
| gps-stationary | 6 | 43 | 1006.46 | 0 |
| gps-jitter | 6 | 43, 129, 216, 241 | 1006.38 | 0, 10, 20, 23 |
| gps-moving | 33 | 41, 43, 95, 147, 190, 241 | 150.66 | 0, 6, 12, 17, 23 |
| gps-no-fix | 0 | — | — | — |
| gps-clock-offset | 0 | — | — | — |
| gps-dropout | 3 | 43 | 986.1 | 0 |

I casi `*-forced` e i due casi NET usano `--cam-force-100ms`. Gli altri casi GPS usano la generazione normale. `gps-jitter` varia la latitudine di meno di 15 cm, con velocità dichiarata zero: non attiva la soglia di movimento di 4 m, ma alimenta la cronologia. `gps-moving` simula circa 22 m/s; `gps-no-fix` fornisce stato V; `gps-clock-offset` fornisce UTC avanti di 10 s; `gps-dropout` interrompe i dati dopo circa 1 s.

## Interpretazione delle dimensioni

Il CAM base prodotto da questa implementazione occupa 41 byte. Con il contenitore Low Frequency e cronologia vuota occupa 43 byte. In modalità forzata, il contenitore appare al primo CAM e al primo CAM generato almeno 500 ms dopo la precedente inclusione: si alternano quindi CAM corti e lunghi. Alla frequenza ordinaria da fermo di circa 1 Hz, ogni CAM include LF.

Con coordinate GPS oscillanti, anche da fermo, la cronologia cresce fino al limite locale di 23 punti. Nel caso simulato sono stati osservati 43, 129, 216 e 241 byte. I valori dipendono dai dati e dai campi presenti; non sono dimensioni universali del protocollo. DCC_NET modifica quattro byte riservati dello SHB senza allungare il CAM.

La colonna Length di Wireshark comprende anche radiotap e gli altri protocolli. Nel percorso locale osservato la copia di iniezione ha radiotap da 9 byte e il TX status da 13 byte: lo stesso CAM da 43 byte appare rispettivamente come frame da 130 e 134 byte. Sono due record locali dello stesso invio, non due CAM diversi. In catture su altre radio possono cambiare metadati e presenza FCS.

## Confronto con lo standard e limiti

Riferimento del progetto: [ETSI EN 302 637-2 V1.4.1](https://www.etsi.org/deliver/etsi_en/302600_302699/30263702/01.04.01_60/en_30263702v010401p.pdf). Il §6.1.3 stabilisce la presenza periodica del contenitore LF; §7.6.2 e allegato A definiscono ASN.1 con codifica UPER e campi opzionali. La dimensione variabile osservata è coerente con questo formato.

Il §B.38 ammette fino a 23 punti PathHistory e richiama SAE J2735 per generarli. Il progetto conserva i fix precedenti ma non implementa quella selezione/compressione, come già dichiarato in cam/README.md. La corretta decodifica non dimostra quindi conformità completa della cronologia. Anche `--cam-force-100ms` è una modalità sperimentale, non una verifica della politica ordinaria di generazione ETSI. Non è stata eseguita una certificazione dell’intero stack.

Durante la prima sessione reattiva statica il CBR è diventato temporaneamente non valido (`-1%`): tre CAM sono scaduti e il divario massimo tra TX status è stato circa 4,33 s. La ripetizione ha prodotto sei CAM con mediana circa 1,008 s. Anche la seconda prova adattiva forzata ha inviato meno CAM per indisponibilità temporanea del CBR. È una limitazione osservata dell’ammissione con survey reali, indipendente dal GPS e dalle dimensioni CAM; la causa delle letture survey non valide non è stata diagnosticata da queste prove.

I test GPS confermano: un fix valido e fermo consente circa 1 Hz; stato V o orario fuori tolleranza impediscono la generazione; interrompendo gli aggiornamenti gli invii cessano dopo la scadenza della sorgente. La tolleranza UTC di ±2 s e la validità di 2 s sono politiche di questo progetto.

Nelle prime quattro catture veloci tcpdump ha lasciato da uno a due invii nel buffer alla chiusura, senza kernel drop. Sono state ripetute con `--immediate-mode` e un secondo di drenaggio: conteggi di iniezione, TX status e Submitted sono uguali in tutte le cinque ripetizioni.

## Evidenze locali

Directory ignorate da Git, conservate senza modificare il codice di produzione:

```text
build/cam-size-validation-20261119-160251/
build/cam-size-validation-20261119-160435/
```

Ogni scenario contiene capture.pcap, decoded.tsv, dcc.log, tcpdump.log e CSV di telemetria. Le directory contengono results.json e lo script di riproduzione; la prima include software-tests.log. Lo script richiede privilegi per socket/catture e modifica potenza e bitrate tramite DCC: va adattato prima di usarlo su un altro host. A fine prova la potenza originale di 23 dBm è stata ripristinata, insieme al multicast rate nominale 6. Nessun processo di prova resta attivo. I percorsi sotto build vengono rimossi da `make clean`.

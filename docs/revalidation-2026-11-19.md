# Riesame funzionale e degli standard — 19 novembre 2026

Il sistema supera le prove funzionali eseguite; **non è dimostrata la conformità
completa agli standard ETSI**. La firma dei CAM manca, alcune opzioni sono
sperimentali e le misure radio non sostituiscono un collaudo RF indipendente.

## Prove ripetute

- `make -j2 all test`: build aggiornata, 10 eseguibili C++ e 13 test Python
  superati. L'ultima esecuzione include i nuovi test casuali della geometria.
- Storico: aggiunti 16 percorsi deterministici casuali, 400 osservazioni ciascuno.
  A ogni aggiornamento si confrontano i segmenti emessi con tutti i campioni
  originali rappresentati, usando un calcolo planare indipendente e un margine
  per la quantizzazione delle coordinate. Superati anche i casi già presenti:
  rettilineo, cerchio, inversione, rumore da fermo, limiti, gap, dateline e polo.
- Lo stesso eseguibile geometrico è passato con AddressSanitizer e UBSan, senza
  segnalazioni. Questa strumentazione riguarda lo storico, non tutto lo stack.
- Hardware: superate 15 combinazioni di 10/15/20 dBm e bitrate nominali
  6/9/12/18/24, con ACK e readback della potenza. Bitrate 999 rifiutato.
- Eseguite 23 sessioni radio: 20 configurazioni della matrice, una sequenza
  congestione/recupero e due sessioni con il GPS u-blox reale su `/dev/ttyACM0`.
- Contati 765 invii: `Submitted`, `Estimated_Accounted`, copie di iniezione e
  TX status coincidono in ogni sessione. Nessun flag TX failure, errore tshark,
  discordanza delle lunghezze GN/BTP o kernel drop nelle catture.
- La radio è wlan0 in OCB a 5900 MHz / 10 MHz; wlan2 è il monitor sullo stesso
  phy1. wlan1 non è stata riconfigurata. Potenza originale 23 dBm e rate
  multicast nominale 6 ripristinati; impostazioni seriali GPS ripristinate.

## Misure per configurazione

Ogni sessione di matrice dura nominalmente 5 s, oltre ad avvio/arresto; le due
sessioni GPS reali durano nominalmente 10 s. I conteggi includono eventuali
ultimi invii durante l'arresto del worker. Le dimensioni sono del solo CAM.
Gli intervalli sono mediane tra TX status, non tempi PHY misurati.

| Configurazione | CAM / TX status | Byte CAM | Mediana intervallo, ms | Punti PathHistory |
| --- | ---: | --- | ---: | --- |

| reactive-static | 6 | 43 | 1007.77 | 0 |
| reactive-forced | 45 | 41, 43 | 100.66 | 0 |
| adaptive-static | 6 | 43 | 1008.65 | 0 |
| adaptive-forced | 55 | 41, 43 | 100.73 | 0 |
| reactive-net | 47 | 41, 43 | 100.67 | 0 |
| adaptive-net | 44 | 41, 43 | 100.76 | 0 |
| gps-stationary | 6 | 43 | 1005.91 | 0 |
| gps-jitter | 6 | 43 | 1006.11 | 0 |
| gps-moving | 27 | 41, 43, 52 | 201.34 | 0, 1 |
| gps-no-fix | 0 | — | — | — |
| gps-clock-offset | 0 | — | — | — |
| gps-dropout | 3 | 43 | 996.61 | 0 |
| adaptive-gps-stationary | 5 | 43 | 1002.3 | 0 |
| adaptive-net-gps-stationary | 5 | 43 | 1006.71 | 0 |
| reactive-net-gps-moving | 35 | 41, 43, 52 | 130.86 | 0, 1 |
| adaptive-net-gps-moving | 33 | 41, 43, 52 | 131.85 | 0, 1 |
| gps-forced-jitter | 58 | 41, 43 | 100.71 | 0 |
| gps-turning | 25 | 41, 43, 52, 60, 69, 78 | 201.3 | 0, 1, 2, 3, 4 |
| net-eirp-offset | 47 | 41, 43 | 100.7 | 0 |
| priority-7 | 58 | 41, 43 | 100.68 | 0 |
| reactive-congestion | 233 | 41, 43 | 100.76 | 0 |
| real-gps-reactive | 11 | 43, 52 | 1006.82 | 0, 1 |
| real-gps-adaptive-net | 10 | 43, 52 | 1006.94 | 0, 1 |

I casi `*-forced`, i casi statici `reactive-net`/`adaptive-net`,
`gps-forced-jitter`, `net-eirp-offset` e `priority-7` usano generazione forzata a
100 ms. Gli altri casi GPS usano la politica normale. Il caso `priority-7` è
volutamente sperimentale; non verifica la mappatura TC2/UP3 del profilo ordinario.

## Risultati specifici

Il GPS reale ha fornito GPRMC/GNRMC con stato A e checksum validi. È stato
utilizzato sequenzialmente, dopo verifica che nessun altro processo lo avesse
aperto. Le due sessioni hanno trasmesso 11 e 10 CAM, con mediane di 1006,82 e
1006,94 ms. È quindi confermato il funzionamento anche da fermo con il ricevitore
fisico, nelle condizioni di questa prova. La cronologia reale ha contenuto zero
o un punto, coerentemente con piccole variazioni delle coordinate.

I casi simulati senza fix o con UTC avanti di 10 s hanno trasmesso zero CAM,
come previsto dalla validazione della sorgente. Interrompendo i dati dopo circa
1 s, la trasmissione si è arrestata dopo la scadenza di freschezza. Il jitter
sub-metrico non ha riempito la cronologia. Nel test in curva i contenitori LF
hanno incluso fino a quattro punti; le proprietà di errore e i limiti sono
verificati separatamente dai test geometrici.

La sequenza con CBR sintetico è stata 0% per 3 s, 35% per 3 s, 45% per 3 s,
55% per 3 s, 80% per 6 s e 0% per 24 s. Radio e invii erano reali; non è stata
creata congestione RF. La telemetria ha mostrato:

```text
UNKNOWN → RELAXED → ACTIVE_1 → ACTIVE_2 → ACTIVE_3 → RESTRICTIVE
        → ACTIVE_3 → ACTIVE_2 → ACTIVE_1 → RELAXED
```

Il caso ha prodotto 233 CAM e mostrato tutti i rate nominali 6/12/18/24.
L'offset EIRP di +2,5 dB ha prodotto 25 nel campo pubblicizzato, come previsto
per potenza configurata 23 dBm e arrotondamento verso il basso. Il default ha
TID 3; l'override ha TID 7. Senza NET i quattro byte dell'estensione sono zero.

Un secondo analizzatore binario, distinto dai test del codec, ha controllato
header CAM v2/id2, station ID 4316, GN SHB/BTP-B, hop limit, traffic class,
lunghezze, lifetime, porta BTP, TID ed estensione DCC-MCO. I limiti stimati
osservati sono:

- Ton massimo stimato: **480 µs**.
- Massimo stimato su finestra mobile di un secondo: **4160 µs**.
- Minima distanza tra TX status meno Ton stimato precedente: **43725.15 µs**.

Sono diagnostiche basate sul modello OFDM a 10 MHz e su timestamp dei TX status.
Non dimostrano tempi effettivi sul canale o l'assenza di altre trasmissioni
esterne al gate dell'applicazione.

## Riesame degli standard

Il riesame riguarda il profilo e le versioni indicate in [standards.md](standards.md).
"Verificato nel software" non equivale a una certificazione di conformità.

| Area | Riferimento | Risultato ed evidenza |
| --- | --- | --- |
| CAM: contenitori, UPER e dati indisponibili | EN 302 637-2 V1.4.1 §7, allegati A/B | Codec, vincoli coperti dai test, unità, LF e decodifica delle catture verificati. Dimensione variabile prevista dal formato. |
| CAM: trigger, intervalli e LF | EN 302 637-2 §6.1.3 | Test su soglie e timer superati. La generazione ordinaria applica limiti 100–1000 ms; `--cam-force-100ms` è un esperimento, non la politica ordinaria. |
| CAM: sicurezza | EN 302 637-2 §6.2.2.2 | **Lacuna certa**: CAM non firmati; mancano Authorization Tickets, BitmapSsp e firma TS 103 097. |
| PathHistory | EN 302 637-2 §B.38 e SAE J2735 (2009) | Selettore geometrico VSC-A verificato su campioni; confronto completo con il testo SAE 2009 ancora aperto. Il valore 1 m è una calibrazione, non una soglia ETSI qui certificata. |
| DCC reattivo | TS 102 687 V1.2.1 §5.3 | Transizioni adiacenti e risposta agli ingressi verificati in test e nella prova radio. La tabella del progetto è una scelta dichiarata; allegato A informativo. |
| DCC adattivo | TS 102 687 §5.4 | Equazioni, filtro, saturazioni, coppie di campioni e fase UTC verificati nei test; esecuzione con e senza NET provata. Non è una misura di puntualità hard real-time. |
| Entità CROSS/ACC/NET/FAC | TS 103 175 V1.1.1 §§5–7 | Contratti e integrazione verificati nel software. NET disabilitato resta una modalità locale sperimentale, non il profilo GN/ITS-G5 completo richiesto da REQ002. |
| DCC_NET | TS 102 636-4-2 V1.4.1 §§5–7 | Parser, aggregazione, duplicati, wrap timestamp, expiry e integrazione testati nel software. Pubblicazione MCO verificata nelle catture. Scambio con nodo indipendente e interoperabilità restano da dimostrare. |
| TC/UP/AC | TS 102 636-4-2 §8, tabella 4 | Default TC2/UP3 verificato. `--cam-priority 7` mantiene TC2 e quindi devia intenzionalmente dalla mappatura standard. |
| Airtime, duty cycle e Toff | EN 302 571 V2.1.1 §4.2.10.2 | Limiti nel gate e modello coperti da test; valori stimati delle catture compatibili. **Conformità sul tempo in aria non dimostrata**. |
| Misura CBR | EN 302 571 §4.2.10.1 | Survey e trattamento delle misure non valide testati. Non calibrati la soglia RF −85 dBm né l'accuratezza ±3% di un metodo equivalente. |
| PHY, potenza ed EDCA | EN 302 663 V1.3.1; EN 302 571 | ACK/readback, OCB, TID e RATE verificati; potenza irradiata, modulazione effettiva, EDCA, sensibilità, emissioni e coesistenza DSRC non collaudati. |

Le mediane da fermo di circa 1006–1009 ms mostrano il jitter di esecuzione; i
limiti ideali dei timer non costituiscono una garanzia temporale rigorosa del
processo Linux. Il vincolo di costruzione di 50 ms è protetto dal codice con
scarto in caso di sforamento, ma non è stata eseguita una misura esaustiva del
worst case sotto carico. Nessuna cattura locale sostituisce una ricezione
indipendente o una misura RF con incertezza dichiarata.

Il TST GN viene costruito con il tempo della generazione, mentre generationDeltaTime
CAM usa il tempo RMC quando disponibile. L'allineamento completo delle semantiche
EPV/acquisizione, incluse le regole della stazione ferma, resta da verificare nel
profilo GN completo; il solo controllo delle unità ed epoch non chiude tale audit.

## Fonti primarie consultate

- [EN 302 637-2 V1.4.1](https://www.etsi.org/deliver/etsi_en/302600_302699/30263702/01.04.01_60/en_30263702v010401p.pdf).
- [TS 102 687 V1.2.1](https://www.etsi.org/deliver/etsi_ts/102600_102699/102687/01.02.01_60/ts_102687v010201p.pdf).
- [TS 103 175 V1.1.1](https://www.etsi.org/deliver/etsi_ts/103100_103199/103175/01.01.01_60/ts_103175v010101p.pdf), scaricato direttamente dalla fonte ufficiale quando il lettore web non riusciva ad aprirlo.
- [TS 102 636-4-2 V1.4.1](https://www.etsi.org/deliver/etsi_ts/102600_102699/1026360402/01.04.01_60/ts_1026360402v010401p.pdf).
- [EN 302 636-4-1 V1.4.1](https://www.etsi.org/deliver/etsi_en/302600_302699/3026360401/01.04.01_60/en_3026360401v010401p.pdf).
- [EN 302 663 V1.3.1](https://www.etsi.org/deliver/etsi_en/302600_302699/302663/01.03.01_60/en_302663v010301p.pdf).
- [EN 302 571 V2.1.1](https://www.etsi.org/deliver/etsi_en/302500_302599/302571/02.01.01_60/en_302571v020101p.pdf).

## Evidenze e riproduzione

Directory locali ignorate da Git:

```text
build/cam-size-validation-20261119-162853/
build/cam-size-validation-20261119-163203/
build/cam-size-validation-20261119-163441/

```

Contengono risultati JSON, audit binario indipendente, PCAP, TSV decodificati,
log e telemetria per sessione. La prima contiene log della suite e sanitizer;
la seconda contiene il controllo hardware e le fasi CBR; la terza contiene
l'ingresso NMEA reale e il relativo riepilogo. Gli script di riproduzione sono
salvati nelle tre directory. Richiedono privilegi e sono specifici di questo
host; controllano prima l'assenza di un DCC concorrente e di altri utilizzatori
della seriale. `make clean` elimina queste evidenze.

In questa revisione non è stato modificato il codice di produzione: sono stati
aggiunti i test casuali della geometria e rese esplicite le lacune di conformità
nella documentazione.

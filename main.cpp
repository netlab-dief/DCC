#include <iostream>
#include <chrono>
#include <thread>
#include <csignal> // Necessario per std::signal e SIGINT
#include <atomic>
#include "dcc_manager.h"

#define INTERFACE "wlan0" // Cambia con la tua interfaccia di rete
#define INTERFACE_RADIOTAP "wlan1" // Cambia con la tua interfaccia di rete per il radio tap

// Variabile atomica globale per controllare il ciclo del main
std::atomic<bool> keep_running{true};

// Funzione di gestione del segnale (Signal Handler)
void signal_handler(int signal_num) {
    if (signal_num == SIGINT) {
        std::cout << "\n[MAIN] Ricevuto Ctrl+C (SIGINT). Avvio chiusura pulita..." << std::endl;
        keep_running.store(false); // Rompe il ciclo del main
    }
}

// Funzione helper per stampare lo stato in formato testo
std::string to_string(DccState state) {
    switch (state) {
        case DccState::RELAXED:     return "RELAXED";
        case DccState::ACTIVE:      return "ACTIVE";
        case DccState::RESTRICTIVE: return "RESTRICTIVE";
        default:                    return "UNKNOWN";
    }
}

int main() {
    // Registriamo il signal handler per intercettare il Ctrl+C
    std::signal(SIGINT, signal_handler);

    std::cout << "[MAIN] Avvio del sistema di controllo DCC..." << std::endl;

    std::string interface_name = INTERFACE; // Cambia con la tua interfaccia
    std::string interface_radiotap = INTERFACE_RADIOTAP; // Cambia con la tua interfaccia radiotap
    int initial_interval_ms = 100;

    // Istanziamo il manager
    DccManager dcc_manager(interface_name, interface_radiotap, initial_interval_ms);

    // Avviamo il thread (ricorda di eseguire con sudo per la priorità RT)
    std::cout << "[MAIN] Avvio del thread DCC..." << std::endl;
    dcc_manager.start();

    std::cout << "[MAIN] Sistema in esecuzione. Premi Ctrl+C per terminare." << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;

    // Il ciclo principale continua finché keep_running è true
    while (keep_running.load()) {
        // Legge i dati dal thread DCC ogni secondo
        float current_cbr = dcc_manager.get_current_cbr();
        DccState current_state = dcc_manager.get_current_state();
        int current_interval = dcc_manager.get_current_interval_ms();

        std::cout << "[MAIN MONITOR] CBR: " << current_cbr 
                  << "% | Stato: " << to_string(current_state) 
                  << std::endl;

        // Aspetta 1 secondo prima del prossimo log, ma si sveglia se interrompiamo
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // --- FASE DI USCITA PULITA ---
    // Quando premi Ctrl+C, esce dal while ed esegue questo codice:
    std::cout << "[MAIN] Arresto del thread worker DCC..." << std::endl;
    dcc_manager.stop(); 

    std::cout << "[MAIN] Thread fermato. Programma terminato con successo." << std::endl;
    return 0;
}
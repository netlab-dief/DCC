#include <iostream>
#include <chrono>
#include <thread>
#include <csignal> // Necessario per std::signal e SIGINT
#include <atomic>
#include <string>
#include "dcc_manager.h"

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
std::string to_string(DccState state, bool limeric_enabled) {
    if (limeric_enabled) {
        return "DYNAMIC (LIMERIC)";
    }
    switch (state) {
        case DccState::RELAXED:     return "RELAXED";
        case DccState::ACTIVE_1:    return "ACTIVE_1";
        case DccState::ACTIVE_2:    return "ACTIVE_2";
        case DccState::ACTIVE_3:    return "ACTIVE_3";
        case DccState::RESTRICTIVE: return "RESTRICTIVE";
        default:                    return "UNKNOWN";
    }
}

int main(int argc, char* argv[]) {
    // Controllo parametri minimi obbligatori (nome_programma + interfaccia_standard + interfaccia_radiotap)
    if (argc < 3) {
        std::cerr << "Errore: Parametri mancanti!" << std::endl;
        std::cerr << "Uso: " << argv[0] << " <interfaccia_rete> <interfaccia_radiotap> [--limeric]" << std::endl;
        std::cerr << "Esempio Reattivo: sudo " << argv[0] << " wlan0 wlan1" << std::endl;
        std::cerr << "Esempio LIMERIC:  sudo " << argv[0] << " wlan0 wlan1 --limeric" << std::endl;
        return 1;
    }

    // Registriamo il signal handler per intercettare il Ctrl+C
    std::signal(SIGINT, signal_handler);

    std::cout << "[MAIN] Avvio del sistema di controllo DCC..." << std::endl;

    // Assegnazione dei parametri passati da riga di comando
    std::string interface_name = argv[1]; 
    std::string interface_radiotap = argv[2]; 
    int initial_interval_ms = 100;
    
    // Controllo del parametro opzionale LIMERIC
    bool use_limeric = false;
    if (argc >= 4 && std::string(argv[3]) == "--limeric") {
        use_limeric = true;
    }

    std::cout << "[MAIN] Interfaccia Rete: " << interface_name << std::endl;
    std::cout << "[MAIN] Interfaccia Radiotap: " << interface_radiotap << std::endl;
    std::cout << "[MAIN] Modalità Algoritmo: " << (use_limeric ? "LIMERIC (Adattativo)" : "Stati ETSI (Reattivo)") << std::endl;

    // Istanziamo il manager
    DccManager dcc_manager(interface_name, interface_radiotap, initial_interval_ms);

    // Avviamo il thread corretto in base alla scelta dell'utente
    std::cout << "[MAIN] Avvio del thread DCC Worker..." << std::endl;
    if (use_limeric) {
        dcc_manager.start_limeric();
    } else {
        dcc_manager.start();
    }

    std::cout << "[MAIN] Sistema in esecuzione. Premi Ctrl+C per terminare." << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;

    // Il ciclo principale continua finché keep_running è true
    while (keep_running.load()) {
        // Legge i dati dal thread DCC ogni secondo
        float current_cbr = dcc_manager.get_current_cbr();
        DccState current_state = dcc_manager.get_current_state();
        int current_interval = dcc_manager.get_current_interval_ms();

        std::cout << "[MAIN MONITOR] CBR: " << current_cbr 
                  << "% | Stato: " << to_string(current_state, use_limeric) 
                  << " | T_off corrente: " << current_interval << " ms"
                  << std::endl;

        // Aspetta 1 secondo prima del prossimo log, ma si sveglia se interrompiamo
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // --- FASE DI USCITA PULITA ---
    std::cout << "[MAIN] Arresto del thread worker DCC..." << std::endl;
    dcc_manager.stop(); 

    std::cout << "[MAIN] Thread fermato. Programma terminato con successo." << std::endl;
    return 0;
}
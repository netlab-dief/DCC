#include "console_log.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <csignal> // Necessario per std::signal e SIGINT
#include <atomic>
#include <string>
#include <stdexcept>
#include "dcc_manager.h"

// Variabile atomica globale per controllare il ciclo del main
std::atomic<bool> keep_running{true};

// Funzione di gestione del segnale (Signal Handler)
void signal_handler(int signal_num) {
    if (signal_num == SIGINT || signal_num == SIGTERM) {
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

int run(int argc, char* argv[]) {
    // Controllo parametri minimi obbligatori (nome_programma + interfaccia_standard + interfaccia_radiotap)
    if (argc < 3) {
        ConsoleLine(std::cerr) << "Errore: Parametri mancanti!" << std::endl;
        ConsoleLine(std::cerr) << "Uso: " << argv[0] << " <interfaccia_rete> <interfaccia_radiotap> [--limeric] [--fake-cbr] [--cam-static | --cam-gps <device>] [--cam-station-id <id>] [--cam-priority <0-7>] [--cam-force-100ms]" << std::endl;
        ConsoleLine(std::cerr) << "Esempio Reattivo: sudo " << argv[0] << " wlan0 wlan1" << std::endl;
        ConsoleLine(std::cerr) << "Esempio LIMERIC:  sudo " << argv[0] << " wlan0 wlan1 --limeric" << std::endl;
        ConsoleLine(std::cerr) << "Esempio Test CBR: sudo " << argv[0] << " wlan0 wlan1 --fake-cbr" << std::endl;
        return 1;
    }

    // Registriamo il signal handler per intercettare il Ctrl+C
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    ConsoleLine() << "[MAIN] Avvio del sistema di controllo DCC..." << std::endl;

    // Assegnazione dei parametri passati da riga di comando
    std::string interface_name = argv[1]; 
    std::string interface_radiotap = argv[2]; 
    int initial_interval_ms = 100;
    
    // Controllo dei parametri opzionali
    bool use_limeric = false;
    bool use_fake_cbr = false;
    cam::Options cam_options;
    auto numeric_option = [&](const std::string& option, int& index, std::uint64_t maximum) {
        if (++index >= argc) throw std::invalid_argument("Valore mancante per " + option);
        const std::string value = argv[index];
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("Valore non valido per " + option);
        const auto number = std::stoull(value);
        if (number > maximum) throw std::invalid_argument("Valore fuori intervallo per " + option);
        return number;
    };
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--limeric") {
            use_limeric = true;
        } else if (option == "--fake-cbr") {
            use_fake_cbr = true;
        } else if (option == "--cam-force-100ms") {
            cam_options.force_100ms = true;
        } else if (option == "--cam-static" || option == "--cam-gps") {
            if (cam_options.enabled) throw std::invalid_argument("Scegli una sola modalità CAM");
            cam_options.enabled = true;
            if (option == "--cam-gps") {
                if (++i >= argc || std::string(argv[i]).rfind("--", 0) == 0)
                    throw std::invalid_argument("Dispositivo mancante per --cam-gps");
                cam_options.gps_device = argv[i];
            }
        } else if (option == "--cam-station-id") {
            cam_options.station_id = static_cast<std::uint32_t>(numeric_option(option, i, UINT32_MAX));
        } else if (option == "--cam-priority") {
            cam_options.priority = static_cast<std::uint8_t>(numeric_option(option, i, 7));
        } else {
            ConsoleLine(std::cerr) << "Errore: opzione sconosciuta '" << option << "'." << std::endl;
            return 1;
        }
    }

    if (cam_options.force_100ms && !cam_options.enabled)
        throw std::invalid_argument("--cam-force-100ms richiede --cam-static o --cam-gps <device>");

    if (use_limeric && use_fake_cbr) {
        ConsoleLine(std::cerr) << "Errore: --fake-cbr è disponibile solo in modalità reattiva, "
                  << "non insieme a --limeric." << std::endl;
        return 1;
    }

    ConsoleLine() << "[MAIN] Interfaccia Rete: " << interface_name << std::endl;
    ConsoleLine() << "[MAIN] Interfaccia Radiotap: " << interface_radiotap << std::endl;
    ConsoleLine() << "[MAIN] Modalità Algoritmo: " << (use_limeric ? "LIMERIC (Adattativo)" : "Stati ETSI (Reattivo)") << std::endl;
    ConsoleLine() << "[MAIN] Sorgente CBR: " << (use_fake_cbr ? "file fake_cbr_value (TEST)" : "interfaccia wireless") << std::endl;

    // Istanziamo il manager
    DccManager dcc_manager(interface_name, interface_radiotap, initial_interval_ms, use_fake_cbr);

    // Avviamo il thread corretto in base alla scelta dell'utente
    ConsoleLine() << "[MAIN] Avvio del thread DCC Worker..." << std::endl;
    if (use_limeric) {
        dcc_manager.start_limeric();
    } else {
        dcc_manager.start();
    }
    dcc_manager.start_cam(cam_options);

    ConsoleLine() << "[MAIN] Sistema in esecuzione. Premi Ctrl+C per terminare." << std::endl;
    ConsoleLine() << "--------------------------------------------------------" << std::endl;

    // Il ciclo principale continua finché keep_running è true
    while (keep_running.load()) {
        dcc_manager.check_cam();
        // Legge i dati dal thread DCC ogni secondo
        float current_cbr = dcc_manager.get_current_cbr();
        DccState current_state = dcc_manager.get_current_state();
        int current_interval = dcc_manager.get_current_interval_ms();

        ConsoleLine() << "[MAIN MONITOR] CBR: " << current_cbr
                  << "% | Stato: " << to_string(current_state, use_limeric) 
                  << " | T_off corrente: " << current_interval << " ms"
                  << std::endl;

        // Aspetta 1 secondo prima del prossimo log, ma si sveglia se interrompiamo
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // --- FASE DI USCITA PULITA ---
    ConsoleLine() << "[MAIN] Arresto del thread worker DCC..." << std::endl;
    dcc_manager.stop(); 

    ConsoleLine() << "[MAIN] Thread fermato. Programma terminato con successo." << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    try { return run(argc, argv); }
    catch (const std::exception& error) {
        ConsoleLine(std::cerr) << "[DCC ERROR] " << error.what() << std::endl;
        return 1;
    }
}

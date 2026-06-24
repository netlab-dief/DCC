#include "rate_manager.h"
#include <iostream>
#include <cstdlib> // Necessario per std::system
#include <sstream>

RateManager::RateManager() : is_initialized_(false) {}

RateManager::~RateManager() {
    cleanup();
}

bool RateManager::setup_rate_manager(const std::string& interface) {
    interface_ = interface;
    is_initialized_ = true;
    //std::cout << interface_ << " [RateManager] Inizializzazione completata." << std::endl;
    return true;
}

int RateManager::map_rate_index(int rate_idx) const {
    // Mappa l'indice passato dal DCC sul numero Mbps effettivo da passare a "iw"
    switch (rate_idx) {
        case 24:  return 24; 
        case 12:  return 12; 
        case 6: return 6; 
        default: return rate_idx; // Fallback: usa il numero grezzo passato
    }
}

void RateManager::set_fixed_rate(int rate_idx) {
    if (!is_initialized_) {
        std::cerr << "\033[1;31m[RateManager] Errore: Istanza non inizializzata!\033[0m" << std::endl;
        return;
    }

    // Otteniamo il numero Mbps (6, 12 o 24)
    int bitrate_num = map_rate_index(rate_idx);

    // Componiamo la stringa del comando esatto
    std::stringstream ss;
    ss << "sudo iw dev " << interface_ << " set bitrates legacy-5 " << bitrate_num << " > /dev/null 2>&1";
    std::string command = ss.str();

    // Eseguiamo il comando a livello di sistema operativo
    int ret = std::system(command.c_str());

    if (ret != 0) {
        std::cerr << "\033[1;31m[RateManager] Errore nell'esecuzione del comando: " << command << "\033[0m" << std::endl;
    } else {
        std::cout << "\033[1;32m[RateManager] Comando eseguito: iw dev " << interface_ 
                  << " set bitrates legacy-5 " << bitrate_num << "\033[0m" << std::endl;
    }
}

void RateManager::cleanup() {
    is_initialized_ = false;
}
#include "dcc_state_machine.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void test_threshold_mapping() {
    require(DccStateMachine::state_for_cbr(29.99f) == DccState::RELAXED, "below 30 is RELAXED");
    require(DccStateMachine::state_for_cbr(30.0f) == DccState::ACTIVE_1, "30 is ACTIVE_1");
    require(DccStateMachine::state_for_cbr(40.0f) == DccState::ACTIVE_2, "40 is ACTIVE_2");
    require(DccStateMachine::state_for_cbr(50.0f) == DccState::ACTIVE_3, "50 is ACTIVE_3");
    require(DccStateMachine::state_for_cbr(65.0f) == DccState::RESTRICTIVE, "65 is RESTRICTIVE");
}

void test_initial_state_and_parameters() {
    DccStateMachine machine;
    const auto t0 = DccStateMachine::Clock::time_point{};
    require(machine.update(45.0f, t0) == DccState::ACTIVE_2, "first valid sample is immediate");
    const DccParameters expected[] = {
        {50, 23, 6}, {100, 19, 12}, {200, 16, 18}, {400, 13, 24}, {1000, 10, 24}
    };
    const DccState states[] = {
        DccState::RELAXED, DccState::ACTIVE_1, DccState::ACTIVE_2,
        DccState::ACTIVE_3, DccState::RESTRICTIVE
    };
    for (int i = 0; i < 5; ++i) {
        const auto actual = DccStateMachine::parameters_for(states[i]);
        require(actual.interval_ms == expected[i].interval_ms &&
                    actual.tx_power_dbm == expected[i].tx_power_dbm &&
                    actual.mcs_mbps == expected[i].mcs_mbps,
                "state radio parameters");
    }
}

void test_upward_persistence_and_reset() {
    DccStateMachine machine;
    const auto t0 = DccStateMachine::Clock::time_point{};
    machine.update(20.0f, t0);
    require(machine.update(35.0f, t0 + 100ms) == DccState::RELAXED, "upward transition waits");
    require(machine.update(35.0f, t0 + 1099ms) == DccState::RELAXED, "less than one second waits");
    require(machine.update(35.0f, t0 + 1100ms) == DccState::ACTIVE_1, "one continuous second transitions");

    DccStateMachine reset_machine;
    reset_machine.update(20.0f, t0);
    reset_machine.update(35.0f, t0 + 100ms);
    reset_machine.update(20.0f, t0 + 900ms);
    require(reset_machine.update(35.0f, t0 + 2s) == DccState::RELAXED, "return to band resets timer");
}

void test_downward_persistence_and_invalid_sample() {
    DccStateMachine machine;
    const auto t0 = DccStateMachine::Clock::time_point{};
    machine.update(70.0f, t0);
    require(machine.update(20.0f, t0 + 1s) == DccState::RESTRICTIVE, "downward transition waits");
    require(machine.update(20.0f, t0 + 5999ms) == DccState::RESTRICTIVE, "less than five seconds waits");
    require(machine.update(20.0f, t0 + 6s) == DccState::ACTIVE_3, "five continuous seconds moves down one state");

    DccStateMachine invalid_machine;
    invalid_machine.update(20.0f, t0);
    invalid_machine.update(35.0f, t0 + 100ms);
    invalid_machine.invalidate_observation();
    require(invalid_machine.update(35.0f, t0 + 2s) == DccState::RELAXED, "invalid sample resets timer");
}

void test_adjacent_transitions() {
    const auto t0 = DccStateMachine::Clock::time_point{};
    DccStateMachine machine;
    machine.update(10.0f, t0);
    for (int level = 2; level <= 5; ++level) {
        const auto start = t0 + std::chrono::seconds((level - 2) * 2 + 1);
        machine.update(90.0f, start);
        require(static_cast<int>(machine.update(90.0f, start + 1s)) == level,
                "sharp rise progresses through every adjacent state");
    }
    for (int level = 4; level >= 1; --level) {
        const auto start = t0 + std::chrono::seconds(10 + (4 - level) * 6);
        machine.update(0.0f, start);
        require(static_cast<int>(machine.update(0.0f, start + 5s)) == level,
                "sharp fall progresses through every adjacent state");
    }
    machine.update(80.0f, t0 + 40s);
    machine.update(std::numeric_limits<float>::quiet_NaN(), t0 + 41s);
    require(machine.update(80.0f, t0 + 42s) == DccState::RELAXED, "NaN interrupts persistence");
}

void test_unknown_parameters_are_rejected() {
    bool thrown = false;
    try {
        (void)DccStateMachine::parameters_for(DccState::UNKNOWN);
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    require(thrown, "UNKNOWN parameters throw");
}
}

int main() {
    test_threshold_mapping();
    test_initial_state_and_parameters();
    test_upward_persistence_and_reset();
    test_downward_persistence_and_invalid_sample();
    test_adjacent_transitions();
    test_unknown_parameters_are_rejected();
    std::cout << "All DCC state-machine tests passed\n";
}

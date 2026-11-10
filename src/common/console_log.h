#pragma once

#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

// Assemble each message privately; commit complete lines under one shared lock.
// Formatting flags cannot leak from a controller into another thread's monitor.
class ConsoleLine {
public:
    explicit ConsoleLine(std::ostream& output = std::cout) : output_(output) {}
    ConsoleLine(const ConsoleLine&) = delete;
    ~ConsoleLine() noexcept {
        try {
            auto text = buffer_.str();
            if (text.empty()) return;
            if (text.back() != '\n') text += '\n';
            std::lock_guard<std::mutex> lock(mutex());
            output_.write(text.data(), static_cast<std::streamsize>(text.size()));
            output_.flush();
        } catch (...) { /* Logging must not terminate a controller worker. */ }
    }
    template<class T> ConsoleLine& operator<<(T&& value) {
        buffer_ << std::forward<T>(value);
        return *this;
    }
    ConsoleLine& operator<<(std::ostream& (*manipulator)(std::ostream&)) {
        manipulator(buffer_);
        return *this;
    }
private:
    static std::mutex& mutex() { static std::mutex value; return value; }
    std::ostream& output_;
    std::ostringstream buffer_;
};

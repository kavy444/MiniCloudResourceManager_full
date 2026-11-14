#include "server.h"

#include <stdexcept>
#include <utility>

Server::Server(int id, std::string name, Resources total)
    : id_(id), name_(std::move(name)), total_(total), available_(total) {
    if (total.cpu <= 0 || total.ramGb <= 0 || total.storageGb <= 0) {
        throw std::invalid_argument("Server capacities (CPU, RAM, storage) must all be positive.");
    }
}

Resources Server::used() const {
    return Resources{total_.cpu - available_.cpu,
                     total_.ramGb - available_.ramGb,
                     total_.storageGb - available_.storageGb};
}

bool Server::canFit(const Resources& r) const {
    return r.cpu <= available_.cpu && r.ramGb <= available_.ramGb && r.storageGb <= available_.storageGb;
}

std::string Server::shortfall(const Resources& r) const {
    std::string text;
    auto add = [&text](const std::string& part) {
        if (!text.empty()) text += ", ";
        text += part;
    };
    if (r.cpu > available_.cpu) {
        add("CPU (needs " + std::to_string(r.cpu) + ", free " + std::to_string(available_.cpu) + ")");
    }
    if (r.ramGb > available_.ramGb) {
        add("RAM (needs " + std::to_string(r.ramGb) + " GB, free " + std::to_string(available_.ramGb) + " GB)");
    }
    if (r.storageGb > available_.storageGb) {
        add("storage (needs " + std::to_string(r.storageGb) + " GB, free " + std::to_string(available_.storageGb) + " GB)");
    }
    return text;
}

bool Server::allocate(const Resources& r) {
    if (r.cpu < 0 || r.ramGb < 0 || r.storageGb < 0) return false;
    if (!canFit(r)) return false;
    available_.cpu -= r.cpu;
    available_.ramGb -= r.ramGb;
    available_.storageGb -= r.storageGb;
    return true;
}

bool Server::release(const Resources& r) {
    if (r.cpu < 0 || r.ramGb < 0 || r.storageGb < 0) return false;
    if (available_.cpu + r.cpu > total_.cpu || available_.ramGb + r.ramGb > total_.ramGb ||
        available_.storageGb + r.storageGb > total_.storageGb) {
        return false;  // would exceed capacity: refuse, state unchanged
    }
    available_.cpu += r.cpu;
    available_.ramGb += r.ramGb;
    available_.storageGb += r.storageGb;
    return true;
}

double Server::cpuUtilization() const {
    return static_cast<double>(total_.cpu - available_.cpu) / total_.cpu;
}
double Server::ramUtilization() const {
    return static_cast<double>(total_.ramGb - available_.ramGb) / total_.ramGb;
}
double Server::storageUtilization() const {
    return static_cast<double>(total_.storageGb - available_.storageGb) / total_.storageGb;
}
double Server::overallUtilization() const {
    return (cpuUtilization() + ramUtilization() + storageUtilization()) / 3.0;
}

double Server::loadScoreAfter(const Resources& r) const {
    const Resources u = used();
    const double cpu = static_cast<double>(u.cpu + r.cpu) / total_.cpu;
    const double ram = static_cast<double>(u.ramGb + r.ramGb) / total_.ramGb;
    const double storage = static_cast<double>(u.storageGb + r.storageGb) / total_.storageGb;
    return (cpu + ram + storage) / 3.0;
}

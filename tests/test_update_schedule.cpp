#include "update_schedule.hpp"
#include <iostream>
#include <stdexcept>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        using glint::update::Schedule;
        Schedule schedule;
        require(schedule.due(100), "first automatic check must be due");
        schedule.success(100);
        require(!schedule.due(100 + 86399), "successful checks wait one day");
        require(schedule.due(100 + 86400), "daily check becomes due");
        schedule.failure(200, 7200);
        require(!schedule.due(200 + 3600), "server retry limit overrides local backoff");
        require(!schedule.manual_allowed(200 + 7199), "manual check respects server limit");
        require(schedule.manual_allowed(200 + 7200), "manual retry allowed after server limit");
        require(schedule.due(200 + 7200), "failed checks retry before daily interval");
        schedule.failure(8000);
        require(!schedule.due(11599) && schedule.due(11600), "second failure waits an hour");
        schedule.failure(12000);
        require(!schedule.due(33599) && schedule.due(33600), "later failures wait six hours");
        schedule.success(40000);
        require(schedule.failures == 0 && schedule.retry_at == 0 && schedule.server_limit == 0,
            "success clears retry state");
        require(schedule.due(30000), "clock moved backwards must not suppress updates indefinitely");
        std::cout << "Update scheduling checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

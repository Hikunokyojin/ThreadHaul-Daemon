#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace daemon {

// Dashboard session: one random access key at a time, with an idle timeout,
// an absolute lifetime, explicit sign-out and a lockout after repeated bad
// keys. Thread-safe. The clock and key generator are injectable for tests.
class SessionGuard {
public:
    using Clock = std::chrono::steady_clock;
    enum class Result { Ok, Invalid, Expired, Locked };
    static Clock::time_point SteadyNow() { return Clock::now(); }

    struct Limits {
        std::chrono::seconds idle{30 * 60};
        std::chrono::seconds absolute{12 * 60 * 60};
        int maxFailures = 10;                 // within failureWindow
        std::chrono::seconds failureWindow{60};
        std::chrono::seconds lockout{60};
    };

    SessionGuard(std::function<std::string()> newKey, Limits limits = {},
                 std::function<Clock::time_point()> now = &SteadyNow)
        : newKey_(std::move(newKey)), limits_(limits), now_(std::move(now)) {}

    // Returns the current key, issuing a fresh one if none is live.
    std::string CurrentOrNewKey() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto t = now_();
        if (key_.empty() || ExpiredLocked(t)) {
            key_ = newKey_();
            issued_ = lastUse_ = t;
        }
        return key_;
    }

    Result Check(const std::string& presented) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto t = now_();
        if (t < lockedUntil_) return Result::Locked;
        if (key_.empty() || !Equal(presented, key_)) {
            failures_.push_back(t);
            while (!failures_.empty() && t - failures_.front() > limits_.failureWindow) {
                failures_.pop_front();
            }
            if (static_cast<int>(failures_.size()) >= limits_.maxFailures) {
                lockedUntil_ = t + limits_.lockout;
                failures_.clear();
                return Result::Locked;
            }
            return Result::Invalid;
        }
        if (ExpiredLocked(t)) {
            key_.clear();
            return Result::Expired;
        }
        lastUse_ = t;
        return Result::Ok;
    }

    // Sign-out: the current key stops working immediately.
    void Revoke() {
        std::lock_guard<std::mutex> lock(mutex_);
        key_.clear();
    }

private:
    bool ExpiredLocked(Clock::time_point t) const {
        return t - lastUse_ > limits_.idle || t - issued_ > limits_.absolute;
    }
    static bool Equal(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        return diff == 0;
    }

    std::function<std::string()> newKey_;
    Limits limits_;
    std::function<Clock::time_point()> now_;
    std::mutex mutex_;
    std::string key_;
    Clock::time_point issued_{}, lastUse_{}, lockedUntil_{};
    std::deque<Clock::time_point> failures_;
};

}

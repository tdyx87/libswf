#ifndef LIBSWF_COMMON_PROFILER_H
#define LIBSWF_COMMON_PROFILER_H

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <iostream>

namespace libswf {

// High-resolution timer for performance profiling
class ScopedTimer {
public:
    explicit ScopedTimer(const std::string& name, bool printOnDestruct = true)
        : name_(name), printOnDestruct_(printOnDestruct) {
        start_ = std::chrono::high_resolution_clock::now();
    }
    
    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        
        if (printOnDestruct_) {
            std::cout << "[Timer] " << name_ << ": " << duration << " us (" 
                      << (duration / 1000.0) << " ms)" << std::endl;
        }
    }
    
    [[nodiscard]] double elapsedMs() const {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        return duration / 1000.0;
    }
    
    [[nodiscard]] double elapsedUs() const {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
    }

private:
    std::string name_;
    bool printOnDestruct_;
    std::chrono::high_resolution_clock::time_point start_;
};

// Performance counter for tracking metrics
class PerformanceCounter {
public:
    struct Metric {
        std::string name;
        uint64_t count = 0;
        uint64_t totalTimeUs = 0;
        uint64_t minTimeUs = UINT64_MAX;
        uint64_t maxTimeUs = 0;
        
        void addSample(uint64_t timeUs) {
            count++;
            totalTimeUs += timeUs;
            if (timeUs < minTimeUs) minTimeUs = timeUs;
            if (timeUs > maxTimeUs) maxTimeUs = timeUs;
        }
        
        [[nodiscard]] double averageTimeUs() const {
            return count > 0 ? static_cast<double>(totalTimeUs) / count : 0.0;
        }
    };
    
    static PerformanceCounter& instance() {
        static PerformanceCounter instance;
        return instance;
    }
    
    void record(const std::string& name, uint64_t timeUs) {
        metrics_[name].addSample(timeUs);
    }
    
    void printReport() const {
        std::cout << "\n========== Performance Report ==========\n";
        for (const auto& [name, metric] : metrics_) {
            std::cout << name << ":\n"
                      << "  Count: " << metric.count << "\n"
                      << "  Avg: " << metric.averageTimeUs() << " us\n"
                      << "  Min: " << metric.minTimeUs << " us\n"
                      << "  Max: " << metric.maxTimeUs << " us\n"
                      << "  Total: " << (metric.totalTimeUs / 1000.0) << " ms\n\n";
        }
        std::cout << "========================================\n";
    }
    
    void reset() {
        metrics_.clear();
    }

private:
    std::unordered_map<std::string, Metric> metrics_;
};

// RAII timer that records to PerformanceCounter
class ProfileScope {
public:
    explicit ProfileScope(const std::string& name) : name_(name) {
        start_ = std::chrono::high_resolution_clock::now();
    }
    
    ~ProfileScope() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        PerformanceCounter::instance().record(name_, duration);
    }

private:
    std::string name_;
    std::chrono::high_resolution_clock::time_point start_;
};

// Memory usage tracker
class MemoryTracker {
public:
    struct Allocation {
        size_t size;
        std::string tag;
        std::chrono::steady_clock::time_point time;
    };
    
    static MemoryTracker& instance() {
        static MemoryTracker instance;
        return instance;
    }
    
    void allocate(size_t size, const std::string& tag = "unknown") {
        totalAllocated_ += size;
        currentAllocated_ += size;
        peakAllocated_ = std::max(peakAllocated_, currentAllocated_);
        allocations_++;
    }
    
    void deallocate(size_t size) {
        currentAllocated_ -= size;
        deallocations_++;
    }
    
    [[nodiscard]] size_t getCurrentAllocated() const { return currentAllocated_; }
    [[nodiscard]] size_t getPeakAllocated() const { return peakAllocated_; }
    [[nodiscard]] size_t getTotalAllocated() const { return totalAllocated_; }
    [[nodiscard]] size_t getAllocationCount() const { return allocations_; }
    
    void printReport() const {
        std::cout << "\n========== Memory Report ==========\n"
                  << "Current: " << (currentAllocated_ / 1024.0 / 1024.0) << " MB\n"
                  << "Peak: " << (peakAllocated_ / 1024.0 / 1024.0) << " MB\n"
                  << "Total: " << (totalAllocated_ / 1024.0 / 1024.0) << " MB\n"
                  << "Allocations: " << allocations_ << "\n"
                  << "Deallocations: " << deallocations_ << "\n"
                  << "====================================\n";
    }
    
    void reset() {
        currentAllocated_ = 0;
        peakAllocated_ = 0;
        totalAllocated_ = 0;
        allocations_ = 0;
        deallocations_ = 0;
    }

private:
    size_t currentAllocated_ = 0;
    size_t peakAllocated_ = 0;
    size_t totalAllocated_ = 0;
    size_t allocations_ = 0;
    size_t deallocations_ = 0;
};

// Convenience macros
#ifdef ENABLE_PROFILING
    #define PROFILE_SCOPE(name) ProfileScope _profileScope_##__LINE__(name)
    #define PROFILE_FUNCTION() ProfileScope _profileFunc_##__LINE__(__FUNCTION__)
    #define TIME_SCOPE(name) ScopedTimer _timer_##__LINE__(name)
#else
    #define PROFILE_SCOPE(name)
    #define PROFILE_FUNCTION()
    #define TIME_SCOPE(name)
#endif

} // namespace libswf

#endif // LIBSWF_COMMON_PROFILER_H

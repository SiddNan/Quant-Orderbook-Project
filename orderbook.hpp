#pragma once
#include <cstdint>
#include <vector>
#include <map>
#include <deque>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <mutex>


static constexpr int64_t TICK_PRECISION = 100;

enum class Side       { Buy, Sell };
enum class OrderType  { Limit, Market };
enum class TimeInForce{ GTC, IOC, FOK, GFD };

struct Fill {
    uint64_t makerOrderId;
    uint64_t takerOrderId;
    uint32_t quantity;
    int64_t  priceTick;
    uint64_t timestamp;
};

struct Order {
    uint64_t    id;
    Side        side;
    int64_t     priceTick;
    uint32_t    quantity;
    OrderType   type;
    TimeInForce tif;
    uint32_t    ownerId;
    uint64_t    timestamp;
};

struct LevelInfo {
    int64_t    priceTick;
    uint64_t   totalQuantity;
    uint32_t   count;
    uint32_t   padding;
};

class OrderBook {
public:
    OrderBook(size_t maxOrders = 1000000);
    ~OrderBook() = default;

    // Core operations
    bool submitOrder(const Order& order, std::vector<Fill>* fills = nullptr);
    bool cancelOrder(uint64_t orderId);
    std::vector<Fill> modifyOrder(uint64_t orderId, int64_t newPrice, uint32_t newQty);
    void cancelAll(Side side);

    // Market data access
    double bestBid() const;
    double bestAsk() const;
    std::vector<LevelInfo> getTopLevels(Side side, size_t depth) const;
    
    // Advanced features
    uint64_t getTotalVolume(Side side) const;
    double getWeightedMidPrice() const;
    uint64_t getOrderCount() const { return orderCount_.load(); }

    using FillHandler = std::function<void(const Fill&)>;
    void setFillHandler(FillHandler handler);
    
    // Performance monitoring
    struct Stats {
        std::atomic<uint64_t> ordersProcessed{0};
        std::atomic<uint64_t> fillsGenerated{0};
        std::atomic<uint64_t> avgProcessingTimeNs{0};
        std::atomic<uint64_t> peakOrdersPerSecond{0};
        
        // Copy constructor and assignment deleted for atomics
        Stats() = default;
        Stats(const Stats&) = delete;
        Stats& operator=(const Stats&) = delete;
        
        // Helper to get values
        uint64_t getOrdersProcessed() const { return ordersProcessed.load(); }
        uint64_t getFillsGenerated() const { return fillsGenerated.load(); }
        uint64_t getAvgProcessingTimeNs() const { return avgProcessingTimeNs.load(); }
        uint64_t getPeakOrdersPerSecond() const { return peakOrdersPerSecond.load(); }
    };
    
    const Stats& getStats() const { return stats_; }
    void resetStats() { 
        stats_.ordersProcessed = 0;
        stats_.fillsGenerated = 0;
        stats_.avgProcessingTimeNs = 0;
        stats_.peakOrdersPerSecond = 0;
    }

private:
    // Core matching logic
    bool canFullyFill(const Order& order) const;
    void matchLoop(const Order& order, uint32_t& remaining, std::vector<Fill>* fills);
    void restOrder(const Order& order, uint32_t remaining);
    
    // Thread safe data structures using standard containers + mutex
    mutable std::mutex mutex_;
    std::map<int64_t, std::deque<Order*>> bids_;
    std::map<int64_t, std::deque<Order*>> asks_;
    std::unordered_map<uint64_t, Order> orders_;
    
    // Atomic counters for performance
    std::atomic<uint64_t> orderCount_{0};
    std::atomic<int64_t> bestBidTick_{0};
    std::atomic<int64_t> bestAskTick_{INT64_MAX};
    
    mutable Stats stats_;
    FillHandler fillCb_;
    
    // Utility functions
    uint64_t getCurrentTimeNs() const;
};

namespace HFTUtils {
    // Branch prediction hints
#if defined(__GNUC__) || defined(__clang__)
    #define LIKELY(x)   __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)
#endif
    
    // Memory barriers
    inline void memoryBarrier() {
        std::atomic_thread_fence(std::memory_order_acq_rel);
    }
}

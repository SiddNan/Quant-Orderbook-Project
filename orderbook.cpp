#include "order_book.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>

using namespace HFTUtils;

OrderBook::OrderBook(size_t maxOrders) {
    orders_.reserve(maxOrders);
}

uint64_t OrderBook::getCurrentTimeNs() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

bool OrderBook::submitOrder(const Order& o, std::vector<Fill>* fills) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint64_t startTime = getCurrentTimeNs();
    
    // FOK pre-check
    if (UNLIKELY(o.tif == TimeInForce::FOK && !canFullyFill(o))) {
        return false;
    }

    uint32_t remaining = o.quantity;
    matchLoop(o, remaining, fills);

    // Handle remaining quantity
    if (remaining > 0) {
        if (UNLIKELY(o.tif == TimeInForce::IOC || o.tif == TimeInForce::FOK)) {
            return true;
        }
        restOrder(o, remaining);
    }
    
    // Update performance statistics
    uint64_t processingTime = getCurrentTimeNs() - startTime;
    stats_.ordersProcessed.fetch_add(1, std::memory_order_relaxed);
    stats_.avgProcessingTimeNs.store(processingTime, std::memory_order_relaxed);
    
    return true;
}

void OrderBook::matchLoop(const Order& incomingOrder, uint32_t& remaining, std::vector<Fill>* fills) {
    auto& contraLevels = (incomingOrder.side == Side::Buy) ? asks_ : bids_;
    
    if (incomingOrder.side == Side::Buy) {
        // Buy order: match against asks
        auto it = contraLevels.begin();
        while (remaining > 0 && it != contraLevels.end() && it->first <= incomingOrder.priceTick) {
            auto& queue = it->second;
            
            while (remaining > 0 && !queue.empty()) {
                Order* restingOrder = queue.front();
                
                // Prevent self-matching
                if (UNLIKELY(restingOrder->ownerId == incomingOrder.ownerId)) {
                    break;
                }
                
                uint32_t fillQty = std::min(remaining, restingOrder->quantity);
                
                Fill fill{
                    restingOrder->id,
                    incomingOrder.id,
                    fillQty,
                    it->first,
                    getCurrentTimeNs()
                };
                
                if (fills) fills->push_back(fill);
                if (fillCb_) fillCb_(fill);
                
                restingOrder->quantity -= fillQty;
                remaining -= fillQty;
                
                if (restingOrder->quantity == 0) {
                    orders_.erase(restingOrder->id);
                    queue.pop_front();
                }
                
                stats_.fillsGenerated.fetch_add(1, std::memory_order_relaxed);
            }
            
            if (queue.empty()) {
                it = contraLevels.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        // Sell order: match against bids (highest price first)
        auto it = contraLevels.rbegin();
        while (remaining > 0 && it != contraLevels.rend() && it->first >= incomingOrder.priceTick) {
            auto& queue = it->second;
            
            while (remaining > 0 && !queue.empty()) {
                Order* restingOrder = queue.front();
                
                if (UNLIKELY(restingOrder->ownerId == incomingOrder.ownerId)) {
                    break;
                }
                
                uint32_t fillQty = std::min(remaining, restingOrder->quantity);
                
                Fill fill{
                    restingOrder->id,
                    incomingOrder.id,
                    fillQty,
                    it->first,
                    getCurrentTimeNs()
                };
                
                if (fills) fills->push_back(fill);
                if (fillCb_) fillCb_(fill);
                
                restingOrder->quantity -= fillQty;
                remaining -= fillQty;
                
                if (restingOrder->quantity == 0) {
                    orders_.erase(restingOrder->id);
                    queue.pop_front();
                }
                
                stats_.fillsGenerated.fetch_add(1, std::memory_order_relaxed);
            }
            
            if (queue.empty()) {
                // Convert reverse iterator to forward iterator for erase
                auto forward_it = std::next(it).base();
                contraLevels.erase(forward_it);
                // Restart iteration from the beginning
                it = contraLevels.rbegin();
            } else {
                ++it;
            }
        }
    }
}

void OrderBook::restOrder(const Order& order, uint32_t remaining) {
    Order newOrder = order;
    newOrder.quantity = remaining;
    newOrder.timestamp = getCurrentTimeNs();
    
    orders_[order.id] = newOrder;
    Order* orderPtr = &orders_[order.id];
    
    auto& levels = (order.side == Side::Buy) ? bids_ : asks_;
    levels[order.priceTick].push_back(orderPtr);
    
    orderCount_.fetch_add(1, std::memory_order_relaxed);
    
    // Update best price tracking
    if (order.side == Side::Buy) {
        int64_t currentBest = bestBidTick_.load(std::memory_order_relaxed);
        while (order.priceTick > currentBest) {
            if (bestBidTick_.compare_exchange_weak(currentBest, order.priceTick)) break;
        }
    } else {
        int64_t currentBest = bestAskTick_.load(std::memory_order_relaxed);
        while (order.priceTick < currentBest) {
            if (bestAskTick_.compare_exchange_weak(currentBest, order.priceTick)) break;
        }
    }
}

bool OrderBook::canFullyFill(const Order& order) const {
    uint32_t needed = order.quantity;
    const auto& contraLevels = (order.side == Side::Buy) ? asks_ : bids_;
    
    if (order.side == Side::Buy) {
        for (const auto& [price, queue] : contraLevels) {
            if (price > order.priceTick) break;
            
            for (const Order* restingOrder : queue) {
                if (restingOrder->ownerId == order.ownerId) continue;
                
                if (restingOrder->quantity >= needed) return true;
                needed -= restingOrder->quantity;
                if (needed == 0) return true;
            }
        }
    } else {
        for (auto it = contraLevels.rbegin(); it != contraLevels.rend(); ++it) {
            if (it->first < order.priceTick) break;
            
            for (const Order* restingOrder : it->second) {
                if (restingOrder->ownerId == order.ownerId) continue;
                
                if (restingOrder->quantity >= needed) return true;
                needed -= restingOrder->quantity;
                if (needed == 0) return true;
            }
        }
    }
    
    return needed == 0;
}

double OrderBook::bestBid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bids_.empty()) return -1.0;
    return bids_.rbegin()->first / double(TICK_PRECISION);
}

double OrderBook::bestAsk() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (asks_.empty()) return -1.0;
    return asks_.begin()->first / double(TICK_PRECISION);
}

std::vector<LevelInfo> OrderBook::getTopLevels(Side side, size_t depth) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LevelInfo> result;
    result.reserve(depth);
    
    const auto& levels = (side == Side::Buy) ? bids_ : asks_;
    
    if (side == Side::Buy) {
        // Bids: highest price first
        for (auto it = levels.rbegin(); it != levels.rend() && result.size() < depth; ++it) {
            uint64_t totalQty = 0;
            for (const Order* order : it->second) {
                totalQty += order->quantity;
            }
            result.push_back({
                it->first,
                totalQty,
                static_cast<uint32_t>(it->second.size()),
                0
            });
        }
    } else {
        // Asks: lowest price first
        for (auto it = levels.begin(); it != levels.end() && result.size() < depth; ++it) {
            uint64_t totalQty = 0;
            for (const Order* order : it->second) {
                totalQty += order->quantity;
            }
            result.push_back({
                it->first,
                totalQty,
                static_cast<uint32_t>(it->second.size()),
                0
            });
        }
    }
    
    return result;
}

uint64_t OrderBook::getTotalVolume(Side side) const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = 0;
    const auto& levels = (side == Side::Buy) ? bids_ : asks_;
    
    for (const auto& [price, queue] : levels) {
        for (const Order* order : queue) {
            total += order->quantity;
        }
    }
    
    return total;
}

double OrderBook::getWeightedMidPrice() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bids_.empty() || asks_.empty()) return -1.0;
    
    double bid = bids_.rbegin()->first / double(TICK_PRECISION);
    double ask = asks_.begin()->first / double(TICK_PRECISION);
    
    // Get volumes at best levels
    uint64_t bidVol = 0, askVol = 0;
    for (const Order* order : bids_.rbegin()->second) {
        bidVol += order->quantity;
    }
    for (const Order* order : asks_.begin()->second) {
        askVol += order->quantity;
    }
    
    if (bidVol + askVol == 0) return (bid + ask) / 2.0;
    return (bid * askVol + ask * bidVol) / (bidVol + askVol);
}

bool OrderBook::cancelOrder(uint64_t orderId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = orders_.find(orderId);
    if (it == orders_.end()) return false;
    
    const Order& order = it->second;
    auto& levels = (order.side == Side::Buy) ? bids_ : asks_;
    
    auto levelIt = levels.find(order.priceTick);
    if (levelIt != levels.end()) {
        auto& queue = levelIt->second;
        queue.erase(std::remove_if(queue.begin(), queue.end(),
                       [orderId](const Order* o) { return o->id == orderId; }),
                   queue.end());
        
        if (queue.empty()) {
            levels.erase(levelIt);
        }
    }
    
    orders_.erase(it);
    orderCount_.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

std::vector<Fill> OrderBook::modifyOrder(uint64_t orderId, int64_t newPrice, uint32_t newQty) {
    std::vector<Fill> fills;
    
    // Find the order first
    Order originalOrder;
    bool found = false;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = orders_.find(orderId);
        if (it != orders_.end()) {
            originalOrder = it->second;
            found = true;
        }
    }
    
    if (found) {
        // Cancel and resubmit without nested locking
        cancelOrder(orderId);
        
        Order modifiedOrder = originalOrder;
        modifiedOrder.priceTick = newPrice;
        modifiedOrder.quantity = newQty;
        
        submitOrder(modifiedOrder, &fills);
    }
    
    return fills;
}

void OrderBook::cancelAll(Side side) {
    // Collect order IDs to cancel first
    std::vector<uint64_t> toCancel;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, order] : orders_) {
            if (order.side == side) {
                toCancel.push_back(id);
            }
        }
    }
    
    // Cancel each order
    for (uint64_t id : toCancel) {
        cancelOrder(id);
    }
}

void OrderBook::setFillHandler(FillHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    fillCb_ = std::move(handler);
}

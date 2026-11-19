#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // integer ticks, never floating point
using Qty = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class Tif : std::uint8_t { GTC, IOC };
enum class Status : std::uint8_t { Ok, DuplicateId, UnknownId, ZeroQty };

struct Trade {
  OrderId taker;
  OrderId maker;
  Price price;
  Qty qty;
  bool operator==(const Trade&) const = default;
};

// Every share an order brings in ends up in exactly one of these buckets.
struct Result {
  Status status = Status::Ok;
  Qty filled = 0;     // matched against resting orders
  Qty rested = 0;     // left on the book
  Qty cancelled = 0;  // IOC/market remainder, or shares removed by cancel/reduce/replace
  bool operator==(const Result&) const = default;
};

// Outcome of a single-price auction.
struct Auction {
  Price price;
  std::uint64_t volume;     // shares that trade
  std::uint64_t imbalance;  // eligible shares left unmatched at that price
  bool operator==(const Auction&) const = default;
};

struct Level;

struct Order {
  OrderId id;
  Price price;
  Qty qty;
  Side side;
  Order* prev;
  Order* next;
  Level* level;
};

// One price level: a FIFO queue of orders as an intrusive doubly linked list.
struct Level {
  Price price;
  std::uint64_t total = 0;
  Order* head = nullptr;
  Order* tail = nullptr;
};

// Price-time priority limit order book for one instrument.
// Single-threaded by design: an exchange matching engine runs one book per core.
class OrderBook {
 public:
  explicit OrderBook(std::size_t expected_orders = 0);
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  Result limit(OrderId id, Side side, Price price, Qty qty, Tif tif = Tif::GTC);
  Result market(OrderId id, Side side, Qty qty);
  Result cancel(OrderId id);
  Result reduce(OrderId id, Qty qty);  // partial cancel or execution report; keeps priority
  Result replace(OrderId old_id, OrderId new_id, Price price, Qty qty);  // loses priority

  // While halted, orders rest without matching (IOC and market remainders are cancelled), so the
  // book may cross. Resuming does not uncross it: the venue's reopening cross must clear it first.
  void set_halted(bool halted) { halted_ = halted; }
  bool halted() const { return halted_; }

  // Single-price auction over the crossed part of the book. Candidate prices are the resting
  // level prices; pick the one that (1) trades the most shares, then (2) leaves the smallest
  // imbalance, then (3) is closest to `reference`, then (4) is lower. nullopt if nothing crosses.
  std::optional<Auction> indicative(Price reference) const;
  // Runs that auction: bids best price then oldest first against asks the same way, all at one
  // price. Trades report the buy order as taker and the sell order as maker. Leaves the book uncrossed.
  std::optional<Auction> uncross(Price reference);

  // Trades produced by the most recent call.
  const std::vector<Trade>& trades() const { return trades_; }

  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  const Order* front(Side side) const;  // oldest order at the best price
  const Order* find(OrderId id) const;
  std::size_t order_count() const { return index_.size(); }
  std::size_t level_count(Side side) const;
  std::uint64_t resting_qty(Side side) const;

  // Visits levels best price first.
  template <class F>
  void for_each_level(Side side, F&& f) const {
    if (side == Side::Buy) {
      for (const auto& kv : bids_) f(kv.second);
    } else {
      for (const auto& kv : asks_) f(kv.second);
    }
  }

  // Walks the whole structure. The book must be uncrossed unless halted. For tests, not the hot path.
  bool check_invariants() const;

 private:
  using Bids = std::map<Price, Level, std::greater<Price>>;
  using Asks = std::map<Price, Level, std::less<Price>>;

  Result submit(OrderId id, Side side, Price limit, Qty qty, Tif tif);
  template <class Book>
  Qty match(Book& book, Side taker_side, Price limit, Qty qty, OrderId taker);
  template <class Book>
  void rest(Book& book, OrderId id, Side side, Price price, Qty qty);
  static void detach(Level& level, Order* o);
  void fill(Order* o, Qty qty);
  void remove(Order* o);
  Order* alloc();

  Bids bids_;
  Asks asks_;
  std::unordered_map<OrderId, Order*> index_;
  std::deque<Order> pool_;  // stable addresses; freed slots are reused
  std::vector<Order*> free_;
  std::vector<Trade> trades_;
  bool halted_ = false;
};

}  // namespace lob

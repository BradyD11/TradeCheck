#include "lob/order_book.hpp"

#include <algorithm>
#include <limits>

// Mutants are deliberate bugs compiled into separate test binaries. The test suite
// must fail on each one, which proves the tests can actually catch these bugs.
#ifndef LOB_MUTANT
#define LOB_MUTANT 0
#endif

namespace lob {

OrderBook::OrderBook(std::size_t expected_orders) {
  index_.reserve(expected_orders);
  trades_.reserve(64);
}

Order* OrderBook::alloc() {
  if (!free_.empty()) {
    Order* o = free_.back();
    free_.pop_back();
    return o;
  }
  return &pool_.emplace_back();
}

void OrderBook::detach(Level& l, Order* o) {
  (o->prev ? o->prev->next : l.head) = o->next;
  (o->next ? o->next->prev : l.tail) = o->prev;
  l.total -= o->qty;
}

void OrderBook::remove(Order* o) {
  Level& l = *o->level;
  detach(l, o);
  if (!l.head) {
    if (o->side == Side::Buy) {
      bids_.erase(o->price);
    } else {
      asks_.erase(o->price);
    }
  }
  index_.erase(o->id);
  free_.push_back(o);
}

template <class Book>
Qty OrderBook::match(Book& book, Side taker_side, Price limit, Qty qty, OrderId taker) {
  Qty filled = 0;
  while (qty > 0 && !book.empty()) {
    auto it = book.begin();
    Level& l = it->second;
#if LOB_MUTANT == 3
    bool crosses = taker_side == Side::Buy ? l.price < limit : l.price > limit;  // misses equal price
#else
    bool crosses = taker_side == Side::Buy ? l.price <= limit : l.price >= limit;
#endif
    if (!crosses) break;
    while (qty > 0 && l.head) {
#if LOB_MUTANT == 1
      Order* m = l.tail;  // LIFO instead of FIFO
#else
      Order* m = l.head;
#endif
      Qty f = std::min(qty, m->qty);
      trades_.push_back({taker, m->id, l.price, f});
      qty -= f;
      filled += f;
      if (f == m->qty) {
        detach(l, m);
        index_.erase(m->id);
        free_.push_back(m);
      } else {
        m->qty -= f;
        l.total -= f;
#if LOB_MUTANT == 2
        m->qty -= 1;  // a share disappears on partial fill
#endif
      }
    }
    if (!l.head) book.erase(it);
  }
  return filled;
}

template <class Book>
void OrderBook::rest(Book& book, OrderId id, Side side, Price price, Qty qty) {
  auto [it, inserted] = book.try_emplace(price);
  Level& l = it->second;
  if (inserted) l.price = price;
  Order* o = alloc();
  *o = Order{id, price, qty, side, l.tail, nullptr, &l};
  (l.tail ? l.tail->next : l.head) = o;
  l.tail = o;
  l.total += qty;
  index_.emplace(id, o);
}

Result OrderBook::submit(OrderId id, Side side, Price limit, Qty qty, Tif tif) {
  trades_.clear();
  if (qty == 0) return {Status::ZeroQty};
  if (index_.contains(id)) return {Status::DuplicateId};
  Result r;
  if (!halted_)
    r.filled = side == Side::Buy ? match(asks_, side, limit, qty, id) : match(bids_, side, limit, qty, id);
  Qty left = qty - r.filled;
  if (left == 0) return r;
  if (tif == Tif::IOC) {
    r.cancelled = left;
    return r;
  }
  if (side == Side::Buy) {
    rest(bids_, id, side, limit, left);
  } else {
    rest(asks_, id, side, limit, left);
  }
  r.rested = left;
  return r;
}

Result OrderBook::limit(OrderId id, Side side, Price price, Qty qty, Tif tif) {
  return submit(id, side, price, qty, tif);
}

Result OrderBook::market(OrderId id, Side side, Qty qty) {
  Price p = side == Side::Buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();
  return submit(id, side, p, qty, Tif::IOC);
}

Result OrderBook::cancel(OrderId id) {
  trades_.clear();
  auto it = index_.find(id);
  if (it == index_.end()) return {Status::UnknownId};
  Result r;
  r.cancelled = it->second->qty;
  remove(it->second);
  return r;
}

Result OrderBook::reduce(OrderId id, Qty qty) {
  trades_.clear();
  auto it = index_.find(id);
  if (it == index_.end()) return {Status::UnknownId};
  if (qty == 0) return {Status::ZeroQty};
  Order* o = it->second;
  Result r;
  r.cancelled = std::min(qty, o->qty);
  if (r.cancelled == o->qty) {
    remove(o);
  } else {
    o->qty -= r.cancelled;
    o->level->total -= r.cancelled;
  }
  return r;
}

Result OrderBook::replace(OrderId old_id, OrderId new_id, Price price, Qty qty) {
  trades_.clear();
  auto it = index_.find(old_id);
  if (it == index_.end()) return {Status::UnknownId};
  if (qty == 0) return {Status::ZeroQty};
  if (new_id != old_id && index_.contains(new_id)) return {Status::DuplicateId};
  Side side = it->second->side;
  Qty removed = it->second->qty;
  remove(it->second);
  Result r = submit(new_id, side, price, qty, Tif::GTC);
  r.cancelled += removed;
  return r;
}

std::optional<Price> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

const Order* OrderBook::front(Side side) const {
  if (side == Side::Buy) return bids_.empty() ? nullptr : bids_.begin()->second.head;
  return asks_.empty() ? nullptr : asks_.begin()->second.head;
}

const Order* OrderBook::find(OrderId id) const {
  auto it = index_.find(id);
  return it == index_.end() ? nullptr : it->second;
}

std::size_t OrderBook::level_count(Side side) const {
  return side == Side::Buy ? bids_.size() : asks_.size();
}

std::uint64_t OrderBook::resting_qty(Side side) const {
  std::uint64_t sum = 0;
  for_each_level(side, [&](const Level& l) { sum += l.total; });
  return sum;
}

bool OrderBook::check_invariants() const {
  std::size_t n = 0;
  auto check_side = [&](const auto& book, Side side) {
    for (const auto& [price, l] : book) {
      if (l.price != price || !l.head || l.head->prev || !l.tail || l.tail->next) return false;
      std::uint64_t sum = 0;
      for (const Order* o = l.head; o; o = o->next) {
        if (o->qty == 0 || o->side != side || o->price != price || o->level != &l) return false;
        if (o->next ? o->next->prev != o : l.tail != o) return false;
        auto it = index_.find(o->id);
        if (it == index_.end() || it->second != o) return false;
        sum += o->qty;
        ++n;
      }
      if (sum != l.total) return false;
    }
    return true;
  };
  if (!check_side(bids_, Side::Buy) || !check_side(asks_, Side::Sell)) return false;
  if (n != index_.size()) return false;
  // Never crossed or locked while trading.
  if (!halted_ && !bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) return false;
  return true;
}

}  // namespace lob

#pragma once
// Feeds ITCH order flow into one OrderBook per instrument and checks it against Nasdaq's own book.
#include <memory>
#include <string>
#include <vector>

#include "lob/itch.hpp"
#include "lob/latency.hpp"
#include "lob/order_book.hpp"

namespace lob {

struct ReplayStats {
  std::uint64_t adds = 0, executions = 0, cancels = 0, deletes = 0, replaces = 0;
  std::uint64_t rejected = 0;           // duplicate ids or zero shares
  std::uint64_t unknown_refs = 0;       // ITCH named an order our book doesn't have
  std::uint64_t unexpected_trades = 0;  // our engine matched an add; Nasdaq's displayed book never crosses
  std::uint64_t front_checks = 0;       // 'E' executions
  std::uint64_t front_agree = 0;        // ...that hit the order our price-time priority says is next
};

class Replayer : public itch::NullHandler {
 public:
  // hist may be null to skip timing.
  explicit Replayer(LatencyHistogram* hist = nullptr, std::string symbol = {})
      : hist_(hist), symbol_(std::move(symbol)), books_(65536) {}

  using itch::NullHandler::on;

  void on(const itch::StockDirectory& m) {
    std::string s(m.stock, 8);
    s.erase(s.find_last_not_of(' ') + 1);
    if (s == symbol_) filter_ = m.h.locate;
  }

  void on(const itch::AddOrder& m) {
    if (skip(m.h)) return;
    ++stats.adds;
    OrderBook& b = book(m.h.locate);
    Result r = timed([&] { return b.limit(m.ref, side(m.side), m.price, m.shares); });
    if (r.status != Status::Ok) ++stats.rejected;
    if (!b.trades().empty()) ++stats.unexpected_trades;
  }

  void on(const itch::OrderExecuted& m) {
    if (skip(m.h)) return;
    ++stats.executions;
    OrderBook& b = book(m.h.locate);
    const Order* o = b.find(m.ref);
    if (!o) {
      ++stats.unknown_refs;
      return;
    }
    if (!m.has_price) {
      ++stats.front_checks;
      stats.front_agree += b.front(o->side) == o;
    }
    timed([&] { return b.reduce(m.ref, m.shares); });
  }

  void on(const itch::OrderCancel& m) {
    if (skip(m.h)) return;
    ++stats.cancels;
    count(timed([&] { return book(m.h.locate).reduce(m.ref, m.shares); }));
  }

  void on(const itch::OrderDelete& m) {
    if (skip(m.h)) return;
    ++stats.deletes;
    count(timed([&] { return book(m.h.locate).cancel(m.ref); }));
  }

  void on(const itch::OrderReplace& m) {
    if (skip(m.h)) return;
    ++stats.replaces;
    OrderBook& b = book(m.h.locate);
    count(timed([&] { return b.replace(m.orig_ref, m.new_ref, m.price, m.shares); }));
    if (!b.trades().empty()) ++stats.unexpected_trades;
  }

  const OrderBook* book_for(std::uint16_t locate) const { return books_[locate].get(); }
  std::uint16_t filter_locate() const { return filter_; }

  bool check_all_invariants() const {
    for (const auto& b : books_)
      if (b && !b->check_invariants()) return false;
    return true;
  }

  ReplayStats stats;

 private:
  static Side side(char c) { return c == 'B' ? Side::Buy : Side::Sell; }

  bool skip(const itch::Header& h) const { return !symbol_.empty() && h.locate != filter_; }

  OrderBook& book(std::uint16_t locate) {
    auto& b = books_[locate];
    if (!b) b = std::make_unique<OrderBook>();
    return *b;
  }

  void count(const Result& r) {
    if (r.status == Status::UnknownId) {
      ++stats.unknown_refs;
    } else if (r.status != Status::Ok) {
      ++stats.rejected;
    }
  }

  template <class F>
  Result timed(F&& f) {
    if (!hist_) return f();
    std::uint64_t t0 = now_ns();
    Result r = f();
    hist_->record(now_ns() - t0);
    return r;
  }

  LatencyHistogram* hist_;
  std::string symbol_;
  std::uint16_t filter_ = 0;
  std::vector<std::unique_ptr<OrderBook>> books_;
};

}  // namespace lob

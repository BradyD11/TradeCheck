#pragma once
// Feeds ITCH order flow into one OrderBook per instrument and checks it against Nasdaq's own book.
#include <memory>
#include <optional>
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

// Context for an add/replace that traded in our engine although Nasdaq's book didn't cross.
struct CrossEvent {
  std::uint16_t locate;
  std::uint64_t timestamp;
  char kind;   // 'A' add or 'U' replace
  char state;  // the symbol's last 'H' trading state ('?' if none seen)
  char phase;  // the last 'S' system event code
  char side;   // 'B' or 'S'
  std::uint32_t price;
  Price touch;  // best opposite price it hit
  Qty traded;
};

// Our auction on our book at the moment Nasdaq printed a cross ('Q' arrives before the cross's executions).
struct CrossCheck {
  std::uint16_t locate;
  std::uint64_t timestamp;
  char type;  // O opening, C closing, H halt/IPO reopening
  std::uint32_t nasdaq_price;
  std::uint64_t nasdaq_shares;
  std::optional<Auction> ours;
  Price reference;
};

// Our indicative auction vs each 'I' imbalance message during a halt.
struct NoiiStats {
  std::uint64_t checks = 0;
  std::uint64_t price_agree = 0;   // our clearing price == Nasdaq's near price
  std::uint64_t paired_agree = 0;  // our volume == Nasdaq's paired shares
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
    names_[m.h.locate] = std::move(s);
  }

  void on(const itch::SystemEvent& m) { phase_ = m.code; }

  void on(const itch::CrossTrade& m) {
    if (skip(m.h)) return;
    OrderBook& b = book(m.h.locate);
    Price ref = reference(b, m.h.locate);
    crosses_checked.push_back({m.h.locate, m.h.timestamp, m.cross_type, m.price, m.shares, b.indicative(ref), ref});
    if (m.price) last_price_[m.h.locate] = m.price;
  }

  void on(const itch::Imbalance& m) {
    if (skip(m.h) || m.cross_type != 'H') return;
    OrderBook& b = book(m.h.locate);
    if (!b.halted()) return;
    auto a = b.indicative(reference(b, m.h.locate));
    ++noii.checks;
    noii.price_agree += a ? a->price == Price(m.near_price) : m.near_price == 0;
    noii.paired_agree += (a ? a->volume : 0) == m.paired;
  }
  // Nasdaq doesn't match a paused, halted or quotation-only symbol, so neither do we.
  void on(const itch::TradingAction& m) {
    state_[m.h.locate] = m.state;
    if (!skip(m.h)) book(m.h.locate).set_halted(m.state != 'T');
  }

  void on(const itch::AddOrder& m) {
    if (skip(m.h)) return;
    ++stats.adds;
    OrderBook& b = book(m.h.locate);
    Result r = timed([&] { return b.limit(m.ref, side(m.side), m.price, m.shares); });
    if (r.status != Status::Ok) ++stats.rejected;
    if (!b.trades().empty()) crossed(b, m.h, 'A', m.side, m.price, r.filled);
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
    last_price_[m.h.locate] = m.has_price ? Price(m.price) : o->price;
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
    const Order* old = b.find(m.orig_ref);
    char s = old && old->side == Side::Buy ? 'B' : 'S';
    Result r = timed([&] { return b.replace(m.orig_ref, m.new_ref, m.price, m.shares); });
    count(r);
    if (!b.trades().empty()) crossed(b, m.h, 'U', s, m.price, r.filled);
  }

  const OrderBook* book_for(std::uint16_t locate) const { return books_[locate].get(); }
  std::uint16_t filter_locate() const { return filter_; }

  bool check_all_invariants() const {
    for (const auto& b : books_)
      if (b && !b->check_invariants()) return false;
    return true;
  }

  const std::string& name(std::uint16_t locate) const { return names_[locate]; }

  ReplayStats stats;
  std::vector<CrossEvent> crosses;
  std::vector<CrossCheck> crosses_checked;
  NoiiStats noii;

 private:
  static Side side(char c) { return c == 'B' ? Side::Buy : Side::Sell; }

  // Auction tie-break: the last execution before the halt, else the middle of the crossed range.
  Price reference(const OrderBook& b, std::uint16_t locate) const {
    if (last_price_[locate]) return last_price_[locate];
    if (b.best_bid() && b.best_ask()) return (*b.best_bid() + *b.best_ask()) / 2;
    return 0;
  }

  bool skip(const itch::Header& h) const { return !symbol_.empty() && h.locate != filter_; }

  OrderBook& book(std::uint16_t locate) {
    auto& b = books_[locate];
    if (!b) b = std::make_unique<OrderBook>();
    return *b;
  }

  void crossed(const OrderBook& b, const itch::Header& h, char kind, char side, std::uint32_t price, Qty filled) {
    ++stats.unexpected_trades;
    crosses.push_back({h.locate, h.timestamp, kind, state_[h.locate], phase_, side, price, b.trades().front().price,
                       filled});
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
  char phase_ = '?';
  std::vector<std::unique_ptr<OrderBook>> books_;
  std::vector<std::string> names_ = std::vector<std::string>(65536);
  std::vector<char> state_ = std::vector<char>(65536, '?');
  std::vector<Price> last_price_ = std::vector<Price>(65536, 0);
};

}  // namespace lob

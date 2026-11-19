// Random order flow, checked step by step against a brute-force model.
// After every operation:
//   - results, trades and the full depth of book match the model exactly
//   - the book is never crossed and its internal links are consistent
//   - no shares are created or lost: submitted == resting + 2*traded + cancelled
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <tuple>
#include <utility>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;
using Levels = std::vector<std::pair<Price, std::uint64_t>>;

// Linear scans, no clever data structures: slow and obviously correct.
struct Model {
  struct O {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
    std::uint64_t seq;
  };
  std::vector<O> orders;
  std::vector<Trade> trades;
  std::uint64_t seq = 0;
  bool halted = false;

  O* find(OrderId id) {
    for (auto& o : orders)
      if (o.id == id) return &o;
    return nullptr;
  }
  void erase(OrderId id) {
    orders.erase(std::remove_if(orders.begin(), orders.end(), [&](const O& o) { return o.id == id; }), orders.end());
  }

  Result submit(OrderId id, Side side, Price limit, Qty qty, Tif tif) {
    trades.clear();
    if (qty == 0) return {Status::ZeroQty};
    if (find(id)) return {Status::DuplicateId};
    Result r;
    while (qty > 0 && !halted) {
      O* best = nullptr;
      for (auto& o : orders) {
        if (o.side == side) continue;
        if (side == Side::Buy ? o.price > limit : o.price < limit) continue;
        if (!best || (side == Side::Buy ? o.price < best->price : o.price > best->price) ||
            (o.price == best->price && o.seq < best->seq))
          best = &o;
      }
      if (!best) break;
      Qty f = std::min(qty, best->qty);
      trades.push_back({id, best->id, best->price, f});
      qty -= f;
      r.filled += f;
      best->qty -= f;
      if (best->qty == 0) erase(best->id);
    }
    if (qty > 0) {
      if (tif == Tif::IOC) {
        r.cancelled = qty;
      } else {
        orders.push_back({id, side, limit, qty, seq++});
        r.rested = qty;
      }
    }
    return r;
  }
  Result market(OrderId id, Side side, Qty qty) {
    return submit(id, side, side == Side::Buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min(),
                  qty, Tif::IOC);
  }
  Result cancel(OrderId id) {
    trades.clear();
    O* o = find(id);
    if (!o) return {Status::UnknownId};
    Result r;
    r.cancelled = o->qty;
    erase(id);
    return r;
  }
  Result reduce(OrderId id, Qty qty) {
    trades.clear();
    O* o = find(id);
    if (!o) return {Status::UnknownId};
    if (qty == 0) return {Status::ZeroQty};
    Result r;
    r.cancelled = std::min(qty, o->qty);
    o->qty -= r.cancelled;
    if (o->qty == 0) erase(id);
    return r;
  }
  Result replace(OrderId old_id, OrderId new_id, Price price, Qty qty) {
    trades.clear();
    O* o = find(old_id);
    if (!o) return {Status::UnknownId};
    if (qty == 0) return {Status::ZeroQty};
    if (new_id != old_id && find(new_id)) return {Status::DuplicateId};
    Side side = o->side;
    Qty removed = o->qty;
    erase(old_id);
    Result r = submit(new_id, side, price, qty, Tif::GTC);
    r.cancelled += removed;
    return r;
  }
  // Every resting price is a candidate; scan all orders for each one.
  std::optional<Auction> indicative(Price ref) const {
    std::optional<Auction> best;
    for (const auto& c : orders) {
      std::uint64_t buy = 0, sell = 0;
      for (const auto& o : orders) {
        if (o.side == Side::Buy && o.price >= c.price) buy += o.qty;
        if (o.side == Side::Sell && o.price <= c.price) sell += o.qty;
      }
      Auction a{c.price, std::min(buy, sell), buy > sell ? buy - sell : sell - buy};
      if (a.volume == 0) continue;
      auto key = [&](const Auction& x) {
        Price d = x.price > ref ? x.price - ref : ref - x.price;
        return std::make_tuple(~x.volume, x.imbalance, d, x.price);  // smaller is better
      };
      if (!best || key(a) < key(*best)) best = a;
    }
    return best;
  }
  std::optional<Auction> uncross(Price ref) {
    trades.clear();
    std::optional<Auction> a = indicative(ref);
    if (!a) return a;
    auto by_priority = [](const O& x, const O& y) {
      if (x.price != y.price) return x.side == Side::Buy ? x.price > y.price : x.price < y.price;
      return x.seq < y.seq;
    };
    std::uint64_t left = a->volume;
    while (left > 0) {
      O *b = nullptr, *s = nullptr;
      for (auto& o : orders) {
        O*& slot = o.side == Side::Buy ? b : s;
        if (!slot || by_priority(o, *slot)) slot = &o;
      }
      Qty f = static_cast<Qty>(std::min<std::uint64_t>({b->qty, s->qty, left}));
      trades.push_back({b->id, s->id, a->price, f});
      left -= f;
      b->qty -= f;
      s->qty -= f;
      OrderId bid = b->id, sid = s->id;
      if (find(bid)->qty == 0) erase(bid);
      if (find(sid)->qty == 0) erase(sid);
    }
    return a;
  }
  Levels levels(Side side) const {
    std::map<Price, std::uint64_t> m;
    for (const auto& o : orders)
      if (o.side == side) m[o.price] += o.qty;
    Levels v(m.begin(), m.end());
    if (side == Side::Buy) std::reverse(v.begin(), v.end());
    return v;
  }
};

static Levels levels(const OrderBook& b, Side side) {
  Levels v;
  b.for_each_level(side, [&](const Level& l) { v.emplace_back(l.price, l.total); });
  return v;
}

static bool run(std::uint64_t seed, int steps) {
  OrderBook book;
  Model model;
  std::mt19937_64 rng(seed);
  OrderId next = 1;
  std::uint64_t submitted = 0, cancelled = 0, traded = 0;

  auto fail = [&](int step, const char* what) {
    std::fprintf(stderr, "seed %llu step %d: %s\n", (unsigned long long)seed, step, what);
    return false;
  };
  auto some_id = [&]() -> OrderId {  // usually a live order, sometimes dead or never seen
    if (!model.orders.empty() && rng() % 5) return model.orders[rng() % model.orders.size()].id;
    return 1 + rng() % (next + 5);
  };
  auto new_id = [&]() -> OrderId { return rng() % 20 == 0 ? some_id() : next++; };

  for (int step = 0; step < steps; ++step) {
    // Occasionally halt; resume through a reopening auction that must match the model exactly.
    if (rng() % 40 == 0) {
      if (model.halted) {
        Price ref = 95 + static_cast<Price>(rng() % 11);
        Levels bids = model.levels(Side::Buy), asks = model.levels(Side::Sell);
        bool crossed = !bids.empty() && !asks.empty() && bids[0].first >= asks[0].first;
        if (book.indicative(ref) != model.indicative(ref)) return fail(step, "indicative auction differs from model");
        if (book.indicative(ref).has_value() != crossed) return fail(step, "indicative disagrees with crossed book");
        if (book.uncross(ref) != model.uncross(ref)) return fail(step, "auction result differs from model");
        if (book.trades() != model.trades) return fail(step, "auction trades differ from model");
        for (const Trade& t : book.trades()) traded += t.qty;
      }
      model.halted = !model.halted;
      book.set_halted(model.halted);
      if (!book.check_invariants()) return fail(step, "invariant violated after auction or halt change");
    }
    unsigned roll = rng() % 100;
    Side side = rng() & 1 ? Side::Buy : Side::Sell;
    Price price = 95 + static_cast<Price>(rng() % 11);  // narrow band so orders cross often
    Qty qty = static_cast<Qty>(rng() % 51);              // 0 is a valid reject case
    Result a, b;
    bool adds_shares = false;
    if (roll < 50) {
      Tif tif = rng() % 7 == 0 ? Tif::IOC : Tif::GTC;
      OrderId id = new_id();
      a = book.limit(id, side, price, qty, tif);
      b = model.submit(id, side, price, qty, tif);
      adds_shares = true;
    } else if (roll < 58) {
      OrderId id = new_id();
      a = book.market(id, side, qty);
      b = model.market(id, side, qty);
      adds_shares = true;
    } else if (roll < 78) {
      OrderId id = some_id();
      a = book.cancel(id);
      b = model.cancel(id);
    } else if (roll < 88) {
      OrderId id = some_id();
      a = book.reduce(id, qty);
      b = model.reduce(id, qty);
    } else {
      OrderId old_id = some_id(), id = new_id();
      a = book.replace(old_id, id, price, qty);
      b = model.replace(old_id, id, price, qty);
      adds_shares = true;
    }

    if (!(a == b)) return fail(step, "result differs from model");
    if (book.trades() != model.trades) return fail(step, "trades differ from model");
    if (levels(book, Side::Buy) != model.levels(Side::Buy)) return fail(step, "bids differ from model");
    if (levels(book, Side::Sell) != model.levels(Side::Sell)) return fail(step, "asks differ from model");
    if (book.order_count() != model.orders.size()) return fail(step, "order count differs");
    if (!book.check_invariants()) return fail(step, "invariant violated (crossed book or broken links)");

    if (a.status == Status::Ok) {
      if (adds_shares) submitted += qty;
      cancelled += a.cancelled;
      for (const Trade& t : book.trades()) traded += t.qty;
    }
    std::uint64_t resting = book.resting_qty(Side::Buy) + book.resting_qty(Side::Sell);
    if (submitted != resting + 2 * traded + cancelled) return fail(step, "shares created or lost");
  }
  return true;
}

int main(int argc, char** argv) {
  int seeds = argc > 1 ? std::atoi(argv[1]) : 300;
  int steps = argc > 2 ? std::atoi(argv[2]) : 2000;
  for (int s = 1; s <= seeds; ++s)
    if (!run(static_cast<std::uint64_t>(s), steps)) return 1;
  std::printf("property: %d seeds x %d ops OK\n", seeds, steps);
}

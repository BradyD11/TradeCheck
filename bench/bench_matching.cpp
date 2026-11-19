// Matching-engine benchmark: a recorded stream of limit/IOC/market/cancel/reduce orders,
// replayed into a fresh book. Throughput run first (no timers), then a per-op latency run.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "lob/latency.hpp"
#include "lob/order_book.hpp"

using namespace lob;

enum Kind : std::uint8_t { kPassive, kAggressive, kIoc, kMarket, kCancel, kReduce, kKinds };
static const char* kNames[] = {"limit passive", "limit cross", "ioc", "market", "cancel", "reduce"};

struct Op {
  Kind kind;
  Side side;
  OrderId id;
  Price price;
  Qty qty;
};

static Result apply(OrderBook& b, const Op& op) {
  switch (op.kind) {
    case kPassive:
    case kAggressive: return b.limit(op.id, op.side, op.price, op.qty);
    case kIoc: return b.limit(op.id, op.side, op.price, op.qty, Tif::IOC);
    case kMarket: return b.market(op.id, op.side, op.qty);
    case kCancel: return b.cancel(op.id);
    default: return b.reduce(op.id, op.qty);
  }
}

// Generated against a live book so cancels name orders that are actually resting.
static std::vector<Op> make_ops(std::size_t n, std::uint64_t seed) {
  OrderBook book(n);
  std::mt19937_64 rng(seed);
  std::vector<Op> ops;
  ops.reserve(n);
  std::vector<OrderId> live;
  OrderId next = 1;
  Price mid = 100'000;
  while (ops.size() < n) {
    if (ops.size() % 1000 == 0) mid += static_cast<Price>(rng() % 3) - 1;  // slow drift
    unsigned roll = rng() % 100;
    Side side = rng() & 1 ? Side::Buy : Side::Sell;
    int dir = side == Side::Buy ? 1 : -1;
    Op op{};
    if (roll < 45 && !live.empty()) {
      std::size_t i = rng() % live.size();
      OrderId id = live[i];
      const Order* o = book.find(id);
      if (!o) {
        live[i] = live.back();
        live.pop_back();
        continue;
      }
      op = roll < 40 ? Op{kCancel, o->side, id, 0, 0} : Op{kReduce, o->side, id, 0, 1 + Qty(rng() % o->qty)};
    } else if (roll < 90) {
      Price off = static_cast<Price>(rng() % 50);  // passive, 0..49 ticks behind the touch
      op = {kPassive, side, next++, mid - dir * (1 + off), 1 + Qty(rng() % 500)};
    } else if (roll < 95) {
      op = {kAggressive, side, next++, mid + dir * static_cast<Price>(rng() % 5), 1 + Qty(rng() % 500)};
    } else if (roll < 99) {
      op = {kIoc, side, next++, mid + dir * static_cast<Price>(rng() % 5), 1 + Qty(rng() % 500)};
    } else {
      op = {kMarket, side, next++, 0, 1 + Qty(rng() % 200)};
    }
    Result r = apply(book, op);
    if (op.kind == kAggressive || op.kind == kPassive) {
      if (op.kind == kPassive && !book.trades().empty()) op.kind = kAggressive;  // mid drifted into it
      if (r.rested) live.push_back(op.id);
    }
    ops.push_back(op);
  }
  return ops;
}

int main(int argc, char** argv) {
  std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
  std::printf("generating %zu ops...\n", n);
  auto ops = make_ops(n, 42);

  std::uint64_t trades = 0, sink = 0;
  {
    OrderBook book(n);
    std::uint64_t t0 = now_ns();
    for (const Op& op : ops) {
      sink += apply(book, op).filled;
      trades += book.trades().size();
    }
    double secs = (now_ns() - t0) / 1e9;
    std::printf("throughput  %.2f M ops/s  (%zu ops in %.3f s, %llu trades, %zu resting at end)\n", n / secs / 1e6, n,
                secs, (unsigned long long)trades, book.order_count());
    if (!book.check_invariants()) {
      std::puts("INVARIANT VIOLATED");
      return 1;
    }
  }

  LatencyHistogram timer, all, by_kind[kKinds];
  for (int i = 0; i < 1'000'000; ++i) {
    std::uint64_t t0 = now_ns();
    timer.record(now_ns() - t0);
  }
  {
    OrderBook book(n);
    for (const Op& op : ops) {
      std::uint64_t t0 = now_ns();
      sink += apply(book, op).filled;
      std::uint64_t dt = now_ns() - t0;
      all.record(dt);
      by_kind[op.kind].record(dt);
    }
  }
  std::printf("\nper-op latency (wall clock around each call):\n");
  timer.print("timer only");
  all.print("all ops");
  for (int k = 0; k < kKinds; ++k) by_kind[k].print(kNames[k]);
  return sink == 0;  // keeps the work observable
}

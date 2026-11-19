// Hand-checked scenarios. The property test covers the rest.
#include <vector>

#include "check.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using T = std::vector<Trade>;

static void price_then_time_priority() {
  OrderBook b;
  b.limit(1, Side::Sell, 100, 100);
  b.limit(2, Side::Sell, 100, 100);
  b.limit(3, Side::Sell, 99, 100);
  Result r = b.limit(10, Side::Buy, 100, 250);
  CHECK((r == Result{Status::Ok, 250, 0, 0}));
  CHECK((b.trades() == T{{10, 3, 99, 100}, {10, 1, 100, 100}, {10, 2, 100, 50}}));
  CHECK(b.find(2)->qty == 50);
  CHECK(b.front(Side::Sell)->id == 2);
  CHECK(b.check_invariants());
}

static void remainder_rests_at_limit() {
  OrderBook b;
  b.limit(1, Side::Sell, 100, 30);
  Result r = b.limit(2, Side::Buy, 101, 50);
  CHECK((r == Result{Status::Ok, 30, 20, 0}));
  CHECK(b.best_bid() == 101);
  CHECK(!b.best_ask());
  CHECK(b.check_invariants());
}

static void ioc_and_market_never_rest() {
  OrderBook b;
  b.limit(1, Side::Buy, 100, 40);
  CHECK((b.limit(2, Side::Sell, 100, 100, Tif::IOC) == Result{Status::Ok, 40, 0, 60}));
  CHECK(b.order_count() == 0);
  CHECK((b.market(3, Side::Buy, 10) == Result{Status::Ok, 0, 0, 10}));  // empty book
  CHECK(b.order_count() == 0);
}

static void market_sweeps_levels() {
  OrderBook b;
  b.limit(1, Side::Buy, 100, 10);
  b.limit(2, Side::Buy, 98, 10);
  b.limit(3, Side::Buy, 99, 10);
  CHECK((b.market(4, Side::Sell, 25) == Result{Status::Ok, 25, 0, 0}));
  CHECK((b.trades() == T{{4, 1, 100, 10}, {4, 3, 99, 10}, {4, 2, 98, 5}}));
  CHECK(b.best_bid() == 98);
}

static void rejects() {
  OrderBook b;
  b.limit(1, Side::Buy, 100, 10);
  CHECK(b.limit(1, Side::Sell, 200, 10).status == Status::DuplicateId);
  CHECK(b.limit(2, Side::Sell, 200, 0).status == Status::ZeroQty);
  CHECK(b.cancel(99).status == Status::UnknownId);
  CHECK(b.reduce(99, 1).status == Status::UnknownId);
  CHECK(b.replace(99, 5, 100, 1).status == Status::UnknownId);
  CHECK(b.order_count() == 1);
}

static void reduce_keeps_priority_replace_loses_it() {
  OrderBook b;
  b.limit(1, Side::Sell, 100, 50);
  b.limit(2, Side::Sell, 100, 50);
  CHECK((b.reduce(1, 20) == Result{Status::Ok, 0, 0, 20}));
  CHECK(b.front(Side::Sell)->id == 1);
  CHECK((b.reduce(1, 1000) == Result{Status::Ok, 0, 0, 30}));  // clamps, then removes
  CHECK(!b.find(1));
  b.limit(3, Side::Sell, 100, 50);
  CHECK((b.replace(2, 4, 100, 50) == Result{Status::Ok, 0, 50, 50}));
  CHECK(b.front(Side::Sell)->id == 3);
  CHECK(b.check_invariants());
}

static void freed_slots_are_reused() {
  OrderBook b;
  for (int round = 0; round < 3; ++round) {
    for (OrderId i = 1; i <= 1000; ++i) b.limit(round * 1000 + i, Side::Buy, 100 - i % 10, 5);
    CHECK(b.check_invariants());
    b.market(999'999 - round, Side::Sell, 5000);
    CHECK(b.order_count() == 0);
  }
}

static void halted_book_rests_without_matching() {
  OrderBook b;
  b.limit(1, Side::Sell, 100, 50);
  b.set_halted(true);
  CHECK((b.limit(2, Side::Buy, 105, 30) == Result{Status::Ok, 0, 30, 0}));  // crosses, rests
  CHECK((b.limit(3, Side::Buy, 105, 10, Tif::IOC) == Result{Status::Ok, 0, 0, 10}));
  CHECK((b.market(4, Side::Buy, 10) == Result{Status::Ok, 0, 0, 10}));
  CHECK(b.trades().empty() && b.best_bid() == 105 && b.best_ask() == 100);
  CHECK(b.check_invariants());  // crossed is allowed while halted
  b.set_halted(false);
  CHECK(!b.check_invariants());  // ...but not once trading resumes
  b.cancel(2);
  CHECK(b.check_invariants());
  CHECK((b.limit(5, Side::Buy, 100, 20) == Result{Status::Ok, 20, 0, 0}));
}

int main() {
  price_then_time_priority();
  remainder_rests_at_limit();
  ioc_and_market_never_rest();
  market_sweeps_levels();
  rejects();
  reduce_keeps_priority_replace_loses_it();
  freed_slots_are_reused();
  halted_book_rests_without_matching();
  std::puts("unit: OK");
}

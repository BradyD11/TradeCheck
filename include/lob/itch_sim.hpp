#pragma once
// Synthetic but valid ITCH 5.0 flow, so everything runs without the 4 GB Nasdaq download.
// Like Nasdaq's displayed book, it never crosses, and executions always hit the front of the best level.
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "lob/itch.hpp"
#include "lob/order_book.hpp"

namespace lob::itch {

inline std::vector<std::uint8_t> simulate(std::size_t messages, std::size_t symbols, std::uint64_t seed) {
  Writer w;
  std::mt19937_64 rng(seed);
  std::vector<std::unique_ptr<OrderBook>> books;
  std::vector<std::uint32_t> mids;
  std::vector<std::string> names;
  std::uint64_t ts = 34'200'000'000'000;  // 09:30:00
  for (std::size_t s = 0; s < symbols; ++s) {
    char name[24];  // GCC can't see that s <= 65535 keeps it at 8 chars
    std::snprintf(name, sizeof name, "SYM%04zu", s);
    names.emplace_back(name);
    w.stock_directory(static_cast<std::uint16_t>(s + 1), ts, name);
    books.push_back(std::make_unique<OrderBook>());
    mids.push_back(static_cast<std::uint32_t>(1'000'000 + s * 10'000));  // $100.00 and up
  }

  struct Live {
    std::uint64_t ref;
    std::uint16_t locate;
  };
  std::vector<Live> live;
  std::uint64_t next_ref = 1, match = 1;
  std::size_t written = symbols;
  auto price_for = [&](std::size_t s, Side side) {
    std::uint32_t off = static_cast<std::uint32_t>(1 + rng() % 30) * 100;  // 1..30 cents from mid
    return side == Side::Buy ? mids[s] - off : mids[s] + off;
  };

  while (written < messages) {
    ts += 1 + rng() % 1000;
    unsigned roll = rng() % 100;
    if (live.empty() || roll < 40) {
      std::size_t s = rng() % symbols;
      auto locate = static_cast<std::uint16_t>(s + 1);
      Side side = rng() & 1 ? Side::Buy : Side::Sell;
      std::uint32_t price = price_for(s, side);
      auto shares = static_cast<std::uint32_t>(100 * (1 + rng() % 10));
      std::uint64_t ref = next_ref++;
      books[s]->limit(ref, side, price, shares);
      std::optional<std::uint32_t> attribution;
      if (roll < 5) attribution = 0x4E534454;  // "NSDT"
      w.add(locate, ts, ref, side == Side::Buy ? 'B' : 'S', shares, names[s], price, attribution);
      live.push_back({ref, locate});
    } else if (roll < 50) {
      std::size_t s = rng() % symbols;
      const Order* o = books[s]->front(rng() & 1 ? Side::Buy : Side::Sell);
      if (!o) continue;
      auto shares = static_cast<std::uint32_t>(1 + rng() % o->qty);
      auto locate = static_cast<std::uint16_t>(s + 1);
      if (rng() % 4 == 0) {
        w.executed_with_price(locate, ts, o->id, shares, match++, static_cast<std::uint32_t>(o->price));
      } else {
        w.executed(locate, ts, o->id, shares, match++);
      }
      books[s]->reduce(o->id, shares);
    } else {
      std::size_t i = rng() % live.size();
      Live l = live[i];
      OrderBook& b = *books[l.locate - 1];
      const Order* o = b.find(l.ref);
      if (!o) {  // fully executed earlier
        live[i] = live.back();
        live.pop_back();
        continue;
      }
      if (roll < 85) {
        w.del(l.locate, ts, l.ref);
        b.cancel(l.ref);
        live[i] = live.back();
        live.pop_back();
      } else if (roll < 92) {
        if (o->qty < 2) continue;
        auto shares = static_cast<std::uint32_t>(1 + rng() % (o->qty - 1));
        w.cancel(l.locate, ts, l.ref, shares);
        b.reduce(l.ref, shares);
      } else {
        std::uint64_t new_ref = next_ref++;
        std::uint32_t price = price_for(l.locate - 1, o->side);
        auto shares = static_cast<std::uint32_t>(100 * (1 + rng() % 10));
        w.replace(l.locate, ts, l.ref, new_ref, shares, price);
        b.replace(l.ref, new_ref, price, shares);
        live[i].ref = new_ref;
      }
    }
    ++written;
  }
  return w.bytes;
}

}  // namespace lob::itch

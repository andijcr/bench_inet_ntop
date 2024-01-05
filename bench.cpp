#include <arpa/inet.h>
#include <array>
#include <benchmark/benchmark.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <cstring>
#include <bitset>

// glibc adapter: assume out has enough space
auto glibc_method(std::string &out, ::in6_addr const &in) {
  inet_ntop(AF_INET6, &in, out.data(), out.size());
  auto end = out.find('\0');
  if (end != out.npos) {
    out.resize(end);
  }
}

// manual conversion from glibc source code, substitute snprintf with
// fmt::format_to
auto manual_method(::in6_addr const &in) -> std::string {
  /*
   * Note that int32_t and int16_t need only be "at least" large enough
   * to contain a value of the specified size.  On some systems, like
   * Crays, there is no such thing as an integer variable with 16 bits.
   * Keep this in mind if you think this function should have been coded
   * to use pointer overlays.  All the world's not a VAX.
   */
  auto *src = reinterpret_cast<const uint8_t *>(&in);
  auto out = std::string(INET6_ADDRSTRLEN, '\0');
  auto *tp = out.data();
  struct {
    int base, len;
  } best, cur;
  u_int words[16 / 2];
  int i;

  /*
   * Preprocess:
   *	Copy the input (bytewise) array into a wordwise array.
   *	Find the longest run of 0x00's in src[] for :: shorthanding.
   */
  memset(words, '\0', sizeof words);
  for (i = 0; i < 16; i += 2)
    words[i / 2] = (src[i] << 8) | src[i + 1];
  best.base = -1;
  cur.base = -1;
  best.len = 0;
  cur.len = 0;
  for (i = 0; i < (16 / 2); i++) {
    if (words[i] == 0) {
      if (cur.base == -1)
        cur.base = i, cur.len = 1;
      else
        cur.len++;
    } else {
      if (cur.base != -1) {
        if (best.base == -1 || cur.len > best.len)
          best = cur;
        cur.base = -1;
      }
    }
  }
  if (cur.base != -1) {
    if (best.base == -1 || cur.len > best.len)
      best = cur;
  }
  if (best.base != -1 && best.len < 2)
    best.base = -1;

  /*
   * Format the result.
   */
  for (i = 0; i < (16 / 2); i++) {
    /* Are we inside the best run of 0x00's? */
    if (best.base != -1 && i >= best.base && i < (best.base + best.len)) {
      if (i == best.base)
        *tp++ = ':';
      continue;
    }
    /* Are we following an initial run of 0x00s or any real hex? */
    if (i != 0)
      *tp++ = ':';
    /* Is this address an encapsulated IPv4? */
    if (i == 6 && best.base == 0 &&
        (best.len == 6 || (best.len == 5 && words[5] == 0xffff))) {
      tp =
          fmt::format_to(tp, "{}.{}.{}.{}", src[12], src[13], src[14], src[15]);
      break;
    }
    tp = fmt::format_to(tp, "{:x}", words[i]);
  }
  /* Was it a trailing run of 0x00's? */
  if (best.base != -1 && (best.base + best.len) == (16 / 8))
    *tp++ = ':';
  *tp++ = '\0';

  auto end = out.find('\0');
  if (end != out.npos) {
    out.resize(end);
  }
  return out;
}

// deeper fmt conversion of the above
auto fmt_method_v2(::in6_addr const &in) -> std::string {
  auto *src = reinterpret_cast<const uint8_t *>(&in);

  /*
   * Preprocess:
   *	Copy the input (bytewise) array into a wordwise array.
   *	Find the longest run of 0x00's in src[] for :: shorthanding.
   */
  std::array<uint16_t, 8> buf;
  auto words = std::span{buf.begin(), buf.end()};
  for (auto i = 0; i < 16; i += 2)
    words[i / 2] = (src[i] << 8) | src[i + 1];

  constexpr static auto next = [](auto it, auto end) {
    auto start = std::find(it, end, '\0');
    auto finish =
        std::find_if_not(start, end, [](auto word) { return word == '\0'; });
    return std::span{start, finish};
  };

  auto best = std::span{words.end(), words.end()};
  for (auto cur = next(words.begin(), words.end()); !cur.empty();
       cur = next(cur.end(), words.end())) {
    if (cur.size() > 1 && cur.size() > best.size()) {
      best = cur;
    }
  }

  /*
   * Format the result.
   */
  // encapsulated ipv4
  if (best.begin() == words.begin() &&
      (best.size() == 6 || (best.size() == 5 && words[5] == 0xffff))) {
    return fmt::format(":{}:{}.{}.{}.{}", best.size() == 5 ? ":ffff" : "",
                       src[12], src[13], src[14], src[15]);
  }
  // ipv6
  // no zeros run
  if (best.empty()) {
    return fmt::format("{:x}", fmt::join(words, ":"));
  }

  return fmt::format("{:x}::{:x}",
                     fmt::join(std::span{words.begin(), best.begin()}, ":"),
                     fmt::join(std::span{best.end(), words.end()}, ":"));
}

consteval auto hex_lookup() {
    std::array<char, 256*2> buf;
    auto hex = "0123456789abcdef";

    for (auto i = 0; i < 256; i++) {
        buf[i*2  ] = hex[i >> 4];
        buf[i*2+1] = hex[i & 15];
    }
    return buf;
}


struct run {
    int8_t start : 4 = -1;
    uint8_t len  : 4 =  0;

    constexpr auto operator<=>(const run& other) const {
        if (len != other.len)
            return len <=> other.len;
        return other.start <=> start;
    }
    constexpr bool operator==(const run& other) const = default;

    constexpr operator bool() const { return *this != run{}; }
};
static_assert(run{} == run{});      // invalid == invalid
static_assert(run{} < run{0,1});    // invalid < any valid
static_assert(run{0,1} < run{0,2}); // short run < long run
static_assert(run{0,1} > run{2,1}); // left run > right run
static_assert(sizeof(run) == 1);

consteval auto precompute_runs() {
    std::array<run, 256> runs {};

    for (auto i = 0; i < 256; i++) {
        run current;
        run best;
        for (auto j = 7; j >= 0; j--) {
            if ((i & (1<<j)) == 0) {
                if (!current)
                    current.start = 7-j;
                current.len++;
                if (best < current)
                    best = current;
            }
            else
                current = {};
        }
        if (best.len == 1)
            best = {};

        runs[i] = best;
    }
    return runs;
}
constexpr auto precomputed_runs = precompute_runs();
static_assert(precomputed_runs[0b11111111] == run{});
static_assert(precomputed_runs[0b00000000] == run{0,8});
static_assert(precomputed_runs[0b00000001] == run{0,7});
static_assert(precomputed_runs[0b01010101] == run{});
static_assert(precomputed_runs[0b01001101] == run{2,2});
static_assert(precomputed_runs[0b00101101] == run{0,2});
static_assert(precomputed_runs[0b10001000] == run{1,3});
static_assert(precomputed_runs[0b11001000] == run{5,3});
static_assert(precomputed_runs[0b01000100] == run{2,3});
static_assert(precomputed_runs[0b01000100] == run{2,3});
static_assert(sizeof(precomputed_runs) == 256);

auto izas_method(::in6_addr const &in) -> std::string {
    const auto *src16 = reinterpret_cast<const uint16_t *>(&in);
    const auto *src8  = reinterpret_cast<const uint8_t  *>(&in);

    std::array<uint16_t, 8> buf16;
    std::bitset<8> zeros;
    for (auto i = 0; i < buf16.size(); i++) {
        buf16[i] = __builtin_bswap16(src16[i]);
        zeros[7-i] = buf16[i] != 0;
    }

    auto best_run = precomputed_runs[zeros.to_ulong()];

    char out[46] {};
    char *ptr = out;

    auto to_hex = [](auto val, char *ptr) {
        constexpr auto hex = hex_lookup();

        auto write = [&](auto val) {
            memcpy(ptr, hex.data() + val*2, 2);
            ptr += 2;
        };
        auto hi = val >> 8;
        auto lo = val & 255;

        if (val < 0xf)
            *ptr++ = "0123456789abcdef"[lo];
        else if (val < 0xff)
            write(lo);
        else if (val < 0xfff) {
            *ptr++ = "0123456789abcdef"[hi];
            write(lo);
        }
        else {
            write(hi);
            write(lo);
        }
        return ptr;
    };

    // annoying special cases
    if (best_run == run{0,8})
        return "::";
    if (best_run == run{0,6})
        return fmt::format("::{}.{}.{}.{}", src8[12], src8[13], src8[14], src8[15]);
    if (best_run == run{0,5} && buf16[5] == 0xffff)
        return fmt::format("::ffff:{}.{}.{}.{}", src8[12], src8[13], src8[14], src8[15]);

    for (auto i = 0; i < 8; i++) {
        if (i == best_run.start) {
            if (i == 0) // no previous run added a :
                *ptr++ = ':';
            i += best_run.len - 1;
        }
        else
            ptr = to_hex(buf16[i], ptr);

        if (i < 7)
            *ptr++ = ':';
    }
    return out;
}

constexpr static auto string_addresses = std::array{
    "::ffff:123.123.123.123",
    "2001:0db8:85a3:0000:0000:8a2e:0370:7334",
    "2001:db8::1:0",
    "2001:db8:0:1:1:1:1:1",
    "2001:db8:1234:ffff:ffff:ffff:ffff:ffff",
    "2001:db8:85a3:8d3:1319:8a2e:370:7348",
    "fe80::1ff:fe23:4567:890a",
    "64:ff9b::255.255.255.255",
    "2001:db8:3333:4444:5555:6666:7777:8888",
    "2001:db8::123.123.123.123",
    "2001:db8::1234:5678:5.6.7.8",
    "::1",
    "::",
    "::123.123.123.123",
    ":ffff:123.123.123.123",
    "1:0:1:0:1:0:1:0",
    "0:1:0:1:0:1:0:1",
    "1:1:1:1:1:0:1:1",
    "1:1:1:1:1:0:0:1",
};
auto addresses() {
  auto data = string_addresses;

  auto out = std::array<::in6_addr, data.size()>{};
  std::ranges::copy(data | std::views::transform([](const char *in) {
                      ::in6_addr addr;
                      if (-1 == inet_pton(AF_INET6, in, &addr)) {
                        throw std::invalid_argument{in};
                      }
                      return addr;
                    }),
                    out.data());
  return out;
}

template <size_t V>
static void BM_runner(benchmark::State &state,
                      std::integral_constant<size_t, V> v) {
  auto data = addresses();

  [[maybe_unused]] auto out = std::string(INET6_ADDRSTRLEN, '\0');
  for (auto _ : state)
    for (auto &addr : data) {
      if constexpr (v == 0) {
        glibc_method(out, addr);
      } else if constexpr (v == 1) {
        benchmark::DoNotOptimize(manual_method(addr));
      } else if constexpr (v == 2) {
        benchmark::DoNotOptimize(fmt_method_v2(addr));
      } else {
        benchmark::DoNotOptimize(izas_method(addr));
      }
    }
}

BENCHMARK_CAPTURE(BM_runner, glibc, std::integral_constant<size_t, 0>{});
BENCHMARK_CAPTURE(BM_runner, manual, std::integral_constant<size_t, 1>{});
BENCHMARK_CAPTURE(BM_runner, fmt_v2, std::integral_constant<size_t, 2>{});
BENCHMARK_CAPTURE(BM_runner, izas, std::integral_constant<size_t, 3>{});

// BENCHMARK(BM_runner<1>);

int main(int argc, char **argv) {
    auto data = addresses();

    fmt::print("{:39} {:39} {:39} {:39}\n", "string", "libc", "andrea", "iza");
    for (auto i = 0; i < string_addresses.size(); i++) {
        auto &addr = data[i];
        auto libc = std::string(INET6_ADDRSTRLEN, '\0');
        glibc_method(libc, addr);
        auto andrea = fmt_method_v2(addr);
        auto iza = izas_method(addr);
        if (iza != andrea)
            fmt::print("\x1b[41m");
        else
            fmt::print("\x1b[42m");
        fmt::print("{:39} {:39} {:39} {:39}", string_addresses[i], libc, andrea, iza);
        fmt::print("\x1b[m\n");

    }

  fmt::print("----\n");
  ::benchmark::Initialize(&argc, argv);
  if (::benchmark::ReportUnrecognizedArguments(argc, argv))
    return 1;
  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}

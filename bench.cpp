#include <arpa/inet.h>
#include <array>
#include <benchmark/benchmark.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>

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

auto addresses() {
  auto data = std::array{
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
  };

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
      } else {
        benchmark::DoNotOptimize(fmt_method_v2(addr));
      }
    }
}

BENCHMARK_CAPTURE(BM_runner, glibc, std::integral_constant<size_t, 0>{});
BENCHMARK_CAPTURE(BM_runner, fmt_v1, std::integral_constant<size_t, 1>{});
BENCHMARK_CAPTURE(BM_runner, fmt_v2, std::integral_constant<size_t, 2>{});

// BENCHMARK(BM_runner<1>);

int main(int argc, char **argv) {
  auto data = addresses();

  fmt::print("libc, fmt\n");
  for (auto &addr : data) {
    auto libc = std::string(INET6_ADDRSTRLEN, '\0');
    glibc_method(libc, addr);
    auto mine = fmt_method_v2(addr);
    if (libc != mine) {
      fmt::print("{}, {}\n", libc, mine);
    }
  }
  fmt::print("----\n");
  ::benchmark::Initialize(&argc, argv);
  if (::benchmark::ReportUnrecognizedArguments(argc, argv))
    return 1;
  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}
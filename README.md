# bechmark libc inet_ntop vs fmtlib implementation

basic reimplementation of inet_ntop (ipv6 only) and benchmark against libc.

dataset:

```cpp
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
```

on my machine the result is:

<pre>libc, fmt
----
2023-12-28T17:27:53+01:00
Running ./bench_inet_ntop
Run on (12 X 5000 MHz CPU s)
CPU Caches:
  L1 Data 32 KiB (x6)
  L1 Instruction 32 KiB (x6)
  L2 Unified 256 KiB (x6)
  L3 Unified 12288 KiB (x1)
Load Average: 1.65, 1.12, 1.00
***WARNING*** CPU scaling is enabled, the benchmark real time measurements may be noisy and will incur extra overhead.
-----------------------------------------------------------
Benchmark                 Time             CPU   Iterations
-----------------------------------------------------------
<span style="color:#4E9A06">BM_runner/glibc  </span><span style="color:#C4A000">      2769 ns         2769 ns   </span><span style="color:#06989A">    251399</span>
<span style="color:#4E9A06">BM_runner/fmt_v1 </span><span style="color:#C4A000">      2775 ns         2774 ns   </span><span style="color:#06989A">    241027</span>
<span style="color:#4E9A06">BM_runner/fmt_v2 </span><span style="color:#C4A000">      1950 ns         1950 ns   </span><span style="color:#06989A">    398019</span>
</pre>


## build and run:

tested only on linux. Needs cmake and a c++ compiler:

```sh
mkdir build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/bench_inet_ntop
```


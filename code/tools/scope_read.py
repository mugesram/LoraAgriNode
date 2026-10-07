#!/usr/bin/env python3
"""Read a Siglent SDS2000X Plus binary waveform (.bin, format version 4) of
the node's supply current: print its settings and levels, list the steps in
it, and write a reduced CSV (time, volts, amps).

The current probe in use passes only the edges of a current step: each edge
shows as a spike that dies away in about 0.2 ms. The size of the step is
estimated as the area of the spike divided by its decay time.

The steady level of the trace is the probe's zero error, not current: it is
reported and taken out. Power is then worked out from the steps alone, so it
covers the large steps (LoRa transmit) and nothing else.

Usage: scope_read.py capture.bin [--v-per-a 0.2] [--supply-v 3.3] [--step-ms 0.1] [--csv out.csv]
"""
import argparse
import struct
import sys


def edges(raw, volt, rate, v_per_a, clipped, supply_v):
    """List the spikes at current steps and the step each one stands for,
    then the charge and power of the bursts. Returns the zero level in volts."""
    blk = max(1, int(rate * 50e-6))
    dt = blk / rate
    pts = [volt(sum(raw[k:k + blk]) / blk) for k in range(0, len(raw) - blk + 1, blk)]
    base = sorted(pts)[len(pts) // 2]
    dev = [v - base for v in pts]
    print(f"zero error taken out: {base * 1e3:.2f} mV = {base / v_per_a * 1e3:.1f} mA")
    thr = max(abs(x) for x in dev) * 0.25
    if thr < 4 * abs(volt(1) - volt(0)):
        print("edges: no spike stands clear of the noise")
        return base
    found, k = [], 0
    while k < len(dev):
        if abs(dev[k]) < thr:
            k += 1
            continue
        sign = 1 if dev[k] > 0 else -1
        s0 = k
        while s0 > 0 and dev[s0 - 1] * sign > 0 and k - s0 < 20:
            s0 -= 1
        s1 = k
        while s1 < len(dev) and dev[s1] * sign > 0 and s1 - k < int(0.005 / dt):
            s1 += 1
        seg = [x * sign for x in dev[s0:s1]]
        pk = max(seg)
        tail = [(j, x) for j, x in enumerate(seg) if j > seg.index(pk) and 0.08 * pk < x < 0.6 * pk]
        tau = None
        if len(tail) >= 4:      # straight-line fit of log(level) against time
            import math
            n = len(tail)
            sx, sy = sum(j for j, _ in tail), sum(math.log(x) for _, x in tail)
            sxx, sxy = sum(j * j for j, _ in tail), sum(j * math.log(x) for j, x in tail)
            slope = (n * sxy - sx * sy) / (n * sxx - sx * sx)
            tau = -dt / slope if slope < 0 else None
        found.append((s0 * dt, sign, pk, sum(seg) * dt, tau))
        k = s1 + int(0.002 / dt)
    taus = sorted(t for *_, t in found if t)
    if not taus:
        print("edges: spikes found but their decay could not be measured")
        return base
    tau = taus[len(taus) // 2]
    print(f"edges: {len(found)} spikes, decay time {tau * 1e3:.3f} ms (middle value; {taus[0] * 1e3:.3f} to {taus[-1] * 1e3:.3f})"
          + ("  (CLIPPED: steps read low)" if clipped else ""))
    print("  (time ms, direction, spike peak mA, estimated step mA, time since the edge before ms)")
    prev = None
    for t, sign, pk, area, _ in found:
        gap = f"{(t - prev) * 1e3:9.2f}" if prev is not None else "         "
        print(f"  {t * 1e3:9.2f} {'up  ' if sign > 0 else 'down'} {pk / v_per_a * 1e3:8.1f} {area / tau / v_per_a * 1e3:8.1f} {gap}")
        prev = t
    for name, sg in (("up", 1), ("down", -1)):
        st = [area / tau / v_per_a * 1e3 for _, s, _, area, _ in found if s == sg]
        if st:
            print(f"  {name} steps: {len(st)}, mean {sum(st) / len(st):.1f} mA, {min(st):.1f} to {max(st):.1f} mA")

    # bursts: an up edge followed by a down edge; rounds: bursts less than 0.5 s apart
    step = lambda e: e[3] / tau / v_per_a
    bursts = [(u[0], d[0] - u[0], (step(u) + step(d)) / 2) for u, d in zip(found, found[1:]) if u[1] > 0 and d[1] < 0]
    rounds = []
    for b in bursts:
        if rounds and b[0] - rounds[-1][-1][0] < 0.5:
            rounds[-1].append(b)
        else:
            rounds.append([b])
    if len(rounds) < 2:
        return base
    period = (rounds[-1][0][0] - rounds[0][0][0]) / (len(rounds) - 1)
    print(f"power from the steps, {len(rounds)} rounds, period {period * 1e3:.1f} ms, supply {supply_v:g} V:")
    print("  (burst in the round, length ms, step mA, charge mC, energy mJ)")
    total = 0.0
    for j in range(max(len(r) for r in rounds)):
        bs = [r[j] for r in rounds if len(r) > j]
        ln, amp = sum(b[1] for b in bs) / len(bs), sum(b[2] for b in bs) / len(bs)
        q = sum(b[1] * b[2] for b in bs) / len(bs)
        total += q
        print(f"  {j + 1:3d} {ln * 1e3:8.2f} {amp * 1e3:8.1f} {q * 1e3:8.3f} {q * supply_v * 1e3:8.2f}")
    peak = max(b[2] for b in bursts)
    print(f"  per round: {total * 1e3:.3f} mC, {total * supply_v * 1e3:.2f} mJ")
    print(f"  average over the period: {total / period * 1e3:.2f} mA, {total / period * supply_v * 1e3:.2f} mW")
    print(f"  highest step: {peak * 1e3:.1f} mA, {peak * supply_v * 1e3:.0f} mW")
    return base


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("bin")
    ap.add_argument("--v-per-a", type=float, default=0.2, help="current probe output, volts per amp (probe is 5 A per volt)")
    ap.add_argument("--supply-v", type=float, default=3.3, help="supply voltage at the measuring point, for the power figures")
    ap.add_argument("--step-ms", type=float, default=0.1, help="averaging step of the reduced trace")
    ap.add_argument("--csv", help="write the reduced trace here")
    a = ap.parse_args()

    data = open(a.bin, "rb").read()
    d = lambda o: struct.unpack_from("<d", data, o)[0]
    i = lambda o: struct.unpack_from("<i", data, o)[0]
    if i(0) != 4:
        sys.exit(f"format version {i(0)} is not handled (only 4)")
    start, vdiv, offset, tdiv, delay = i(4), d(0x18), d(0xB8), d(0x19C), d(0x1C4)
    n, rate, probe, width, cpd = struct.unpack_from("<I", data, 0x1EC)[0], d(0x1F0), d(0x244), data[0x264], i(0x270)
    if width != 0:
        sys.exit("16-bit data is not handled")
    raw = data[start:start + n]
    volt = lambda code: (code - 128) * vdiv / cpd - offset
    print(f"{n} samples at {rate:g} Sa/s = {n / rate:g} s, {vdiv * 1e3:g} mV/div, offset {offset * 1e3:.2f} mV, "
          f"{tdiv * 1e3:g} ms/div, probe {probe:g}x")
    lo, hi = min(raw), max(raw)
    print(f"range {volt(lo) * 1e3:.2f} to {volt(hi) * 1e3:.2f} mV" + ("  (CLIPPED at the screen edge)" if lo == 0 or hi == 255 else ""))

    blk = max(1, int(rate * a.step_ms / 1000))
    pts = [volt(sum(raw[k:k + blk]) / len(raw[k:k + blk])) for k in range(0, n, blk)]
    dt = blk / rate
    amps = [v / a.v_per_a for v in pts]
    mean = sum(amps) / len(amps)
    print(f"current at {a.v_per_a:g} V/A: min {min(amps) * 1e3:.2f} mA, max {max(amps) * 1e3:.2f} mA, mean over the capture {mean * 1e3:.3f} mA")

    # steps: stretches where the level stays on one side of a change
    span = max(amps) - min(amps)
    if span > 0:
        thr = span * 0.08
        segs, s0, acc = [], 0, [amps[0]]
        for k in range(1, len(amps)):
            ref = sum(acc[-20:]) / len(acc[-20:])
            if abs(amps[k] - ref) > thr and all(abs(x - ref) > thr for x in amps[k:k + 5]):
                segs.append((s0, k, acc))
                s0, acc = k, []
            acc.append(amps[k])
        segs.append((s0, len(amps), acc))
        print("steps (start ms, length ms, mean mA, peak mA):")
        for s0, s1, acc in segs:
            if (s1 - s0) * dt >= 0.0005:
                print(f"  {s0 * dt * 1e3:8.1f} {(s1 - s0) * dt * 1e3:8.1f} {sum(acc) / len(acc) * 1e3:9.2f} {max(acc) * 1e3:9.2f}")
    zero = edges(raw, volt, rate, a.v_per_a, lo == 0 or hi == 255, a.supply_v)
    if a.csv:
        with open(a.csv, "w") as f:
            f.write("time_ms,volts,amps,amps_zero_removed\n")
            for k, v in enumerate(pts):
                f.write(f"{k * dt * 1e3:.3f},{v:.6f},{v / a.v_per_a:.6f},{(v - zero) / a.v_per_a:.6f}\n")
        print(f"reduced trace: {len(pts)} points -> {a.csv}")


if __name__ == "__main__":
    main()

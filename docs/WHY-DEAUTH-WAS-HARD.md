# Why the deauth attack took seven releases

It shipped in 1.2.2 and did not transmit a single frame until 1.6. Four releases were spent actively
debugging it. The fix was four bytes. This is what made those four bytes so expensive to find.

None of the difficulty was in the fix. All of it was in the measurement.

## 1. Every instrument said it was working

The device reported success at every level:

- `deauthSent` / `da` climbed steadily — it is incremented at the end of `sendInternalKick()`
  unconditionally.
- `ic_tx_pkt()` returns `void`. There is no failure to report even in principle.
- On the raw path, `esp_wifi_80211_tx()` returning `ESP_OK` means "accepted for queueing", not "sent".
- The frame-builder calls all succeeded: `ieee80211_alloc_deauth()` returned non-null, the ebuf pointer
  `P` was non-null, `hstate` looked sane.

So the natural reading — counter rising, no errors — was "we are transmitting, something downstream is
ignoring us". That framing was wrong, and it survived four releases because nothing available contradicted
it. Every hypothesis it generated (PMF, DFS, promiscuous mode, frame subtype, sequence numbers, TX rate,
BSS context) was an explanation for *why a transmitted frame might be ignored*. The truth was that no
frame was ever transmitted.

## 2. Two faults, each hiding the other

There were two transmit paths, and both were broken — differently:

| path | how it failed |
| --- | --- |
| driver-internal slot | silently, while reporting success |
| raw `esp_wifi_80211_tx()` | loudly, with `ESP_ERR_INVALID_ARG` |

The dispatch preferred the internal slot whenever a slot existed, which was always. So the raw path —
the one that fails with a specific, diagnosable, greppable error code — **never executed even once**
before 1.6. Its loud failure was shadowed by the silent one.

This is the part worth internalising. A silent failure in front of a loud one doesn't just hide itself;
it hides the diagnostic you would have gotten for free. The first thing 1.6 did that earlier attempts
did not was simply *run the other path* (`kickpath 1`), and it immediately printed the answer:
`ESP_ERR_INVALID_ARG`, 304 times out of 304.

## 3. There was no oracle, and the obvious one was blind

"Did the frame reach the air?" cannot be answered from the transmitting device. It needs a second radio.
Without one, "transmitted but ignored" and "never transmitted" are indistinguishable — and they lead to
completely disjoint investigations.

The witness that *was* tried, a macOS Wi-Fi scan, cannot do the job: macOS redacts every SSID in
`system_profiler SPAirPortDataType`, so the beacon-injection self-test (`txtest`) could not be read even
in principle. The result was recorded as inconclusive, which is correct and was also a dead end.

Rule 10 had said since 1.2.5 that an external witness was required before claiming anything. It was
right. It took until 1.6 for someone to actually go and get one — an ESP32-S3 in monitor mode, about an
hour of work — and the answer arrived within minutes of it being plugged in.

## 4. A real bug on the way made the wrong theory feel right

1.2.5 found something genuine: the code was overwriting the frame control with `0xC8 0x02`, a QoS-Null
data frame, which every station ignores. That is a real bug and fixing it was correct.

But it was *confirming evidence for the wrong model*. It said "the problem is the frame we send", which
is exactly what the counters already implied. Fixing it changed nothing on air, because the frame's
content was never the issue. A genuine bug found inside a false theory is worse than no bug at all: it
buys the theory another release.

## 5. The actual blocker is invisible from the source

The thing stopping the attack is one instruction in a closed-source binary:

```
jal  ieee80211_raw_frame_sanity_check
bnez a0, ...        # nonzero -> return, nothing transmitted
```

No amount of reading Bandwatch's own code reveals it. It is not in a header, not in the 4277-line
sdkconfig, and not in any error the API returns — `ESP_ERR_INVALID_ARG` is as much as the driver will
say. Finding it meant disassembling `libnet80211.a`, which is not where anyone looks on day one. (The
restriction *is* documented, in a one-line `@attention` on `esp_wifi_80211_tx` listing the supported
subtypes. Deauthentication is absent from that list by omission, which is easy to read past.)

## 6. The standard escape hatch silently does nothing

Patching this check with `-Wl,--wrap=ieee80211_raw_frame_sanity_check` is the well-known technique, used
by every ESP32 deauther project. On this core it does not work: the check and its only caller are both
inside `ieee80211_output.o`, so the call is bound within the object file and is never an undefined
reference for the linker to redirect.

It does not error. The build succeeds, the wrapper is silently dropped as unreferenced, and the image
comes out byte-identical. Tried without the witness, this looks exactly like "the patch worked but the
attack still fails" — another release lost. The byte-identical size was the only tell.

## The lesson

The bug was in the measurement, not the mechanism.

Four releases produced careful, plausible, well-documented work aimed at a question that could not be
answered with the instruments in use. The counters were not merely unhelpful, they were actively
misleading, and the architecture guaranteed that the one component that would have told the truth never
ran.

Buy the oracle first. An hour spent making the system observable would have been cheaper than any of the
four releases spent reasoning about it, and `tools/witness/verify.py` now turns the question into a
seconds-long yes/no.

And the corollary, for what comes next: this is why `docs/DEVELOPER.md` §11 is emphatic that `da` still
proves nothing, and why "a real station actually disconnects" is listed as **untested** rather than
assumed. Frames reaching the air is a measured fact. A client honouring them is a different claim, and
it needs its own oracle.

# M0 Runbook — Measurements on Two Machines

> **AI-generated document.** Written autonomously by an AI agent. Review and adapt before relying on the results.

This runbook turns the M0 deliverables in [`milestones.md`](milestones.md) into
repeatable steps. Do not start M1 until the gate at the end has been evaluated.

## 1. Get the same build on both machines

Both machines must run the same commit. The Linux peer is the local build:

```shell
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target mixxx --parallel "$(nproc)"
./build/mixxx --settings-path /tmp/mixxx-collab
```

For two instances on one machine, use separate settings directories and
distinct probe ports:

```shell
./build/mixxx --settings-path /tmp/mixxx-collab-a
./build/mixxx --settings-path /tmp/mixxx-collab-b
```

Use the QML skin (`LateNightQML`, or launch with `--new-ui`). The diagnostics
page exists only in QML skins: **Options → Settings → Online B2B**.

### Windows peer

Mixxx does not support cross-compiling from Linux. Use one of these:

**GitHub Actions artifact (recommended).** The fork builds a Windows MSI for
every pushed branch:

1. Enable Actions once on the fork: **Settings → Actions → General → Allow all
   actions** (the Actions tab may also offer *"I understand my workflows, go
   ahead and enable them"*).
2. Push the branch, or run **Actions → Pull request or branch build → Run
   workflow** and select the branch.
3. When the run finishes, open it and download the **Windows x64 Installer**
   artifact. It contains an unsigned `*.msi`; SmartScreen will warn.
4. Install it, e.g. to `C:\Program Files\Mixxx`.

**Native build on the Windows machine.** Install Visual Studio Build Tools with
the C++ workload, then from a *x64 Native Tools Command Prompt*:

```bat
tools\windows_buildenv.bat setup
tools\windows_buildenv.bat build
```

The setup downloads a prebuilt vcpkg environment (several GB, once). The build
takes tens of minutes on the first run. Binaries end up in `build\`.

Launch the Windows peer with a fresh profile so the clock probe settings and
the CSV do not mix with a normal DJ profile:

```bat
"C:\Program Files\Mixxx\mixxx.exe" --new-ui --settings-path "C:\mixxx-collab"
```

Windows asks for a firewall exception on first start. Allow Mixxx for
**private networks** and for **UDP**; otherwise the probe only ever shows zero
samples. If the dialog was dismissed, add an inbound rule for
`mixxx.exe` in Windows Defender Firewall.

With `--settings-path "C:\mixxx-collab"` the diagnostics file is
`C:\mixxx-collab\collab-diagnostics.csv`.

## 2. Clock probe

1. On machine A set:
   - peer address: IP of B
   - peer port: `40411`
   - local port: `40410`
   - probe interval: `200` ms
2. On machine B set:
   - peer address: IP of A
   - peer port: `40410`
   - local port: `40411`
3. Start the probe on both machines. The ports are swapped on purpose.
4. Let it run for 10 minutes.

The CSV defaults to `<settings-path>/collab-diagnostics.csv`. Start both peers
before the run and stop the probe before copying the file.

Repeat over:

- loopback (both instances on one machine, `127.0.0.1`)
- wired LAN
- Wi-Fi
- Tailscale

For loopback with an artificial offset and skew, add the diagnostics clock
options once available; a loopback run alone cannot validate offset or skew
because both instances share one clock.

## 3. Network impairment

On Linux, apply delay and loss on one peer's interface:

```shell
sudo tc qdisc add dev <iface> root netem delay 10ms 2ms loss 1%
# ... run the probe ...
sudo tc qdisc del dev <iface> root
```

Record the measured RTT and offset for:

- 20 ms RTT
- 50 ms RTT
- 1 % loss
- 0 % loss

## 4. Scheduling and audio alignment

The probe measures the clock only. The scheduling gate needs audio:

1. Load the same track on a deck on both machines and set the same position.
2. Use the scheduled-play test (M0 harness or a temporary mapping) to ask both
   decks to play at the same target session time.
3. Record both outputs into one stereo file (loopback interface or a small
   mixer) so the two channels share a sample clock.
4. Measure the onset difference per attempt and repeat at 128, 256, and
   512-frame buffers.

Report p50, p95, p99, and the worst case of the onset difference.

## 5. Drift

Run the same track on both machines for 30–60 minutes without correction and
log the playhead position once per second. Repeat with an anchor every 1–2 s.

Report the ppm difference between the two audio devices and the residual error
with anchors.

## 6. Hashing and analysis spike

- Hash a typical 8 MB MP3, a FLAC, and a 100 MB WAV with SHA-256 and record the
  time on each machine.
- Save and reload `Waveform`, `Beats`, and cue data for an imported track.
- Confirm analysis survives a restart and can be transferred by the planned
  TCP channel.

## 7. Report

```shell
tools/collab_diagnostics_report.py collab-diagnostics.csv
```

Record the output in the M0 decision memo:

- Go/no-go for buffer-boundary scheduling
- Clock offset p95 per transport
- Drift ppm and residual anchor error
- Hash timings
- Any analysis fallback

## Gate

If scheduled play alignment p95 is worse than ~15 ms on a wired LAN, stop and
design the engine-side scheduled-command queue before M1. Do not hide the
problem with a larger lead.

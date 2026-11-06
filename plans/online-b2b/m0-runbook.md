# M0 Runbook — Measurements on Two Machines

> **AI-generated document.** Written autonomously by an AI agent. Review and adapt before relying on the results.

This runbook turns the M0 deliverables in [`milestones.md`](milestones.md) into
repeatable steps. Do not start M1 until the gate at the end has been evaluated.

## 1. Get the same build on both machines

Both machines must run the same commit. The Linux peer is the local build:

```shell
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target mixxx --parallel "$(nproc)"
./build/mixxx --developer --settings-path /tmp/mixxx-collab
```

For two instances on one machine, use separate settings directories and
distinct probe ports:

```shell
./build/mixxx --developer --settings-path /tmp/mixxx-collab-a
./build/mixxx --developer --settings-path /tmp/mixxx-collab-b
```

### Diagnostics entry point

Use the default **LateNight** skin (the QWidget skin Mixxx starts with). Open the
diagnostics from **Developer → B2B Diagnostics**:

```shell
./build/mixxx --developer --settings-path /tmp/mixxx-collab
```

`--developer` is required; it adds the **Developer** menu. The diagnostics open
in a separate window, so you can keep mixing while it is open. No skin change is
needed.

QML skins expose the same page from their Developer menu (NewUi additionally
under **Options → Settings → Online B2B**). Whichever entry point you use, it is
the same page and writes the same CSV.

### Windows peer

Mixxx cannot be cross-compiled from Linux. Build it on the Windows machine.
CI is not used for this branch.

1. Install **Visual Studio Build Tools** with the *Desktop development with
   C++* workload (MSVC x64, Windows SDK, CMake and Ninja). No separate Qt
   install is needed, the dependencies come from the prebuilt vcpkg
   environment.
2. Clone the branch and check out the test commit:

   ```bat
   git clone --branch online-multi https://github.com/rdhwan/mixxx.git
   cd mixxx
   git log -1 --oneline
   ```

3. Open an **x64 Native Tools Command Prompt**, `cd` into the checkout, and
   download the dependency environment:

   ```bat
   tools\windows_buildenv.bat setup
   ```

   It downloads several GB once into `buildenv\` and prints the exact `cmake`
   configure command, including the vcpkg toolchain path.

4. Configure and build in the same console (QML must be enabled, otherwise the
   diagnostics page does not exist):

   ```bat
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
     -DCMAKE_TOOLCHAIN_FILE="%MIXXX_VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
     -DQT6=ON -DQML=ON
   cmake --build build --parallel
   ```

5. Run the binary from the build tree:

   ```bat
   build\mixxx.exe --developer --settings-path "C:\mixxx-collab"
   ```

Launch the Windows peer with a fresh profile so the clock probe settings and
the CSV do not mix with a normal DJ profile.

Select nothing special: the default LateNight skin is used. The diagnostics live
under **Developer → B2B Diagnostics**.

Windows asks for a firewall exception on first start. Allow Mixxx for
**private networks** and for **UDP**; otherwise the probe only ever shows zero
samples. If the dialog was dismissed, add an inbound rule for
`mixxx.exe` in Windows Defender Firewall.

With `--settings-path "C:\mixxx-collab"` the diagnostics file is
`C:\mixxx-collab\collab-diagnostics.csv`.

## 2. Clock probe

The UDP ports are derived from the role, so there is nothing to swap by hand:

- `host`: local `40410`, peer `40411`
- `guest`: local `40411`, peer `40410`

1. On machine A: peer address = IP of B, role `host`, click **Connect**.
2. On machine B: peer address = IP of A, role `guest`, click **Connect**.
3. Both status lines must turn green and read **Connected to …**. A yellow
   **Waiting for …** means the peer is not answering: check the address, the
   firewall, and that the opposite role is selected on the other machine.
4. Let the probe run for 10 minutes.

The CSV defaults to `<settings-path>/collab-diagnostics.csv`. Stop the probe
before copying the file.

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
   Both decks must be paused before the test.
2. Connect both peers and wait until both show **Connected**.
3. In the **Scheduling test** panel pick the deck and a lead of `3000` ms.
4. Click **Schedule play once on either peer**. The command is sent to the
   other peer, so both decks start at the same session-time boundary. Both
   status lines then report the deck and how late it started.
5. Record both outputs into one stereo file (loopback interface or a small
   mixer) so the two channels share a sample clock.
6. Measure the onset difference per attempt and repeat at 128, 256, and
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

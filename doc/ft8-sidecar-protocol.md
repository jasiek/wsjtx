# External FT8 decoder ("sidecar") protocol, v1

WSJT-X can send FT8 decoding to an external, long-running process
instead of the built-in `jt9` decoder. Every other mode keeps using
`jt9`.

## Enabling it

Set `WSJTX_FT8_DECODER` to a command line before starting WSJT-X:

```sh
WSJTX_FT8_DECODER='/path/to/mydec --some-option' /Applications/wsjtx.app/Contents/MacOS/wsjtx
```

The command is split into words like a shell would split it, with
quoting honoured, but no shell runs it. Put any variables or pipes in
a wrapper script. WSJT-X starts the process **once**, at startup, and
keeps it running for the whole session, so the decoder can keep state
between periods.

> macOS: apps started from Finder or the Dock don't see shell
> environment variables. Start WSJT-X from a terminal, or run
> `launchctl setenv WSJTX_FT8_DECODER '...'` and then restart the app.

The process's environment is the one WSJT-X has, plus:

| Variable          | Meaning                                         |
|-------------------|-------------------------------------------------|
| `WSJTX_TEMP_DIR`  | WSJT-X temp directory                           |
| `WSJTX_DATA_DIR`  | writeable data directory (ALL.TXT lives here)   |
| `WSJTX_SHARE_DIR` | read-only shipped data (cty.dat, ALLCALL7.TXT…) |

## When the sidecar is used

For each FT8 decode request, as long as the process is running,
**except**:

- Fox mode (Fox caller tracking is built into `jt9`), and
- SuperFox, both Fox and Hound (it uses a different decoder).

In those cases, and permanently after the process exits or crashes,
FT8 goes to `jt9`. A status-bar message and a log line tell you
when that happens.

## Transport

- **stdin** (WSJT-X → decoder): UTF-8 JSON, one compact object per
  line, ending in `\n`. A `decode` line is immediately followed by
  binary audio (see below).
- **stdout** (decoder → WSJT-X): UTF-8 JSON, one object per line.
  WSJT-X logs and ignores any line that isn't JSON. **Flush after
  every line.**
- **stderr**: free-form text, copied line by line into the WSJT-X
  log.

Every message WSJT-X sends includes `"v": 1`.

## Messages from WSJT-X

### `hello`: sent once, right after the process starts

```json
{"type":"hello","v":1,"app":"WSJT-X","version":"3.0.0",
 "temp_dir":"...","data_dir":"...","share_dir":"..."}
```

No reply is expected.

### `decode`: one per decode pass

```json
{"type":"decode","v":1,"seq":17,
 "mode":"FT8","utc":123015,"pass":50,"final":true,"disk":false,
 "period_start":"2026-11-08T12:30:15.000Z","dial_freq":14074000,
 "special_op":0,
 "sample_rate":12000,"samples":172800,
 "params":{ ... every field of dec_data.params ... }}
```

The JSON line is followed **immediately** by `samples × 2` bytes:
mono, signed 16-bit **little-endian** PCM at 12000 Hz. Sample 0 is
the start of the T/R period. `samples` is how much audio has been
captured so far: fewer than 180000 on early passes, and about 172800
(14.4 s) on the normal final pass.

**Re-decodes:** when `params.newdat` is false (the user clicked
Decode, or changed a setting and asked to decode again), `samples` is
**0**. Decode the audio from the most recent request that carried
audio again. WSJT-X's own buffer may already contain the next period
by then.

| Field          | Meaning |
|----------------|---------|
| `seq`          | request id. Echo it in every reply line |
| `utc`          | period start as an `hhmmss` integer |
| `pass`         | `nzhsym`: how many half-symbol blocks have been captured. Early passes are about 41/46/47; the final pass is `m_hsymStop` (usually 50) |
| `final`        | true for the last pass of this period, and always for disk data |
| `disk`         | true when the audio comes from a `.wav` file the user opened, not live input |
| `period_start` | ISO-8601 UTC for live data; empty for disk data (use `params.datetime` / `params.yymmdd` instead) |
| `dial_freq`    | rig dial frequency in Hz |
| `special_op`   | 0 NONE, 1 NA_VHF, 2 EU_VHF, 3 FIELD_DAY, 4 RTTY, 5 WW_DIGI, 6 FOX, 7 HOUND, 8 ARRL_DIGI, 9 Q65_PILEUP |
| `params`       | **all** of `dec_data.params` (`commons.h`) under their C/Fortran names, with char arrays as trimmed strings |

The `params` fields that matter most for FT8:

| Field | Meaning |
|-------|---------|
| `nfa`, `nfb` | lowest and highest audio frequency to decode, in Hz |
| `nfqso` | Rx frequency (Hz), where the QSO partner is expected |
| `nftx` | Tx frequency (Hz) |
| `ntol` | frequency tolerance around `nfqso` |
| `ndepth` | decoding depth (bit flags; `& 7` gives 1–3) |
| `lft8apon`, `lapcqonly`, `napwid`, `nQSOProgress` | a priori (AP) decoding: enabled, CQ only, bandwidth, QSO state 0–5 |
| `mycall`, `mygrid`, `hiscall`, `hisgrid`, `mybcall`, `hisbcall` | own and DX station calls/grids (`*bcall` = base call with prefix/suffix stripped) |
| `b_even_seq` | true for an even (:00/:30) period |
| `newdat`, `nagain` | `newdat=false` with `nagain=true` means re-decode the same audio (the user clicked Decode) |
| `lbandchanged`, `nsecbandchanged` | the band changed; reset per-band state |
| `ltxing`, `nlasttx` | we transmitted recently / what was last sent |
| `lhound` | Hound mode |
| `lmultift8` and `nmt`, `nft8cycles`, `ndecoderstart`, `nft8rxfsens`, `lft8lowth`, `lft8subpass`, `ncandthin`, `lhideft8dupes`, `lwidedxcsearch`, `lenabledxcsearch`, `lskiptx1`, `lapmyc` | multithreaded decoder ("ft8md") options |

### `quit`: sent when WSJT-X closes

```json
{"type":"quit","v":1}
```

stdin is then closed. Exit within 1 s or the process is killed.

## Replies from the decoder

For each `decode` request, write zero or more decode lines followed
by exactly one `done` line:

```json
{"seq":17,"snr":-12,"dt":0.3,"freq":1234,"msg":"CQ K1ABC FN42"}
{"seq":17,"snr":-20,"dt":-0.1,"freq":2210,"msg":"K1ABC W9XYZ -15","ap":2,"lowconf":true}
{"seq":17,"done":true}
```

| Field     | Type   | Meaning |
|-----------|--------|---------|
| `seq`     | int    | the request's `seq`. Lines with a different `seq` are dropped. If omitted, the line counts toward the current request |
| `snr`     | int    | SNR in dB |
| `dt`      | number | time offset in seconds, using the usual FT8 convention (0 = 0.5 s after period start) |
| `freq`    | number | audio frequency in Hz, rounded |
| `msg`     | string | decoded message text; at most 37 characters are used |
| `ap`      | int    | optional. AP decoding type 1–9; shown as `a1`…`a9` |
| `lowconf` | bool   | optional. Puts a `?` at column 37, like `jt9` does for low-quality AP decodes |
| `done`    | bool   | this request is finished |

Optional `{"log":"text"}` lines can be written at any time; they go
to the WSJT-X log.

WSJT-X formats each decode as a `jt9` FT8 output line
(`hhmmss snr dt freq ~  message ap`) and runs it through the same
processing as `jt9` output: false-decode filtering, ALL.TXT, the Band
Activity and Rx Frequency windows, UDP, PSK Reporter and auto-sequencing.

### Timing

WSJT-X won't send another request until you send `done`. Meanwhile
the decoder counts as busy, so later early passes of the same period
are skipped. If `done` hasn't arrived within `max(5 s, T/R period)`,
the request is given up, and later lines for that `seq` are dropped.
Reply to early passes quickly, or ignore their audio and answer with
just `done`, so the final pass isn't delayed.

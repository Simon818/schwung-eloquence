# Schwung-Eloquence

A hack for replacing Flite with Eloquence (OpenEVV) in Schwung's screen reader on the Ableton Move.

## Why Does This Exist?

The [Schwung](https://schwung.dev) screen reader offers two engines, eSpeak and Flite. Many people dislike both of these synthesizers or find them difficult to understand. Right now, adding alternative speech synthesis directly to Schwung would require maintaining a separate branch with separate updates, which is not ideal. Adding external TTS is on the roadmap, but is not possible at this time.

This project is a stopgap solution until Schwung supports external TTS. The install script replaces the Flite engine with a small proxy library that loads in its place and speaks through [OpenEVV](https://github.com/mudb0y/openevv), a C port of IBM's ETI-Eloquence. In practice, this means choosing **Flite** gets you Eloquence instead. ESpeak and Schwung are left unmodified.

## Install

First, make sure you're running Schwung 1.5 or later. 1.5 was released on September 27th, only a few days before Schwung-Eloquence, and earlier Schwung releases had a bug that caused Move to crash when trying to change Flite's speed or pitch.

You need SSH access to your Move if you don't have it already. Schwung's installer should have set this up. From a terminal, `ssh ableton@move.local`, then run:

```
wget -qO- https://simonj.me/schwung-eloquence/install.sh | sh
```

Move restarts by itself when the install is finished. To restart it manually instead, end the command with `| sh -s -- --no-restart`.

After the restart, go to Global Settings (shift+hold step 2), choose **Screen Reader**, and set Engine to **Flite**. Your Flite should sound suspiciously Eloquence-like.

## Security Notice

The Move's `wget` does not check HTTPS certificates, so the one-liner trusts your network. If you'd rather perform a local install or your Move is not online, download `install.sh` and `schwung-eloquence.tar.gz` from <https://simonj.me/schwung-eloquence/> on a computer, copy both to the Move, and run `sh install.sh schwung-eloquence.tar.gz`.

## Files Added or Changed on your Move

| Path | Description |
|---|---|
| `/data/UserData/eloquence/` | the engine, your settings, the text fixes and any dictionaries in `dict/`, logs, licences, and a copy of the installer |
| `/data/UserData/boot-targets/eloquence/` | the "Schwung + Eloquence" boot target |
| `/data/UserData/schwung/lib/libflite*.so.1` | replaced; Schwung's own are kept in `eloquence/orig/` |
| `/data/UserData/boot-targets/default` | set to `eloquence` |

## Updating Schwung

Updating Schwung core would normally bring back Flite. To solve this, the installer makes "Schwung + Eloquence" the default boot target, which automatically puts Eloquence back after Schwung updates. You can change this back from the web manager or by uninstalling Schwung-Eloquence.

## Uninstalling

SSH into your Move and run:

```
sh /data/UserData/eloquence/install.sh --uninstall
```

This puts Schwung's own Flite back, removes the boot target, deletes everything in `/data/UserData/eloquence`, and restarts Move. That includes your settings and any dictionaries you added, so keep copies of those elsewhere.

## Settings

`/data/UserData/eloquence/eloquence.json` is read when the screen reader first speaks after Move starts, so restart Move after changing it: over SSH, `/data/UserData/schwung/restart-move.sh`. The installer puts `eloquence.example.json` next to it, with every setting at its default; copy it to `eloquence.json` and change what you like. The example is replaced with the latest each time you install, but your `eloquence.json` is not. Every key is optional:

```json
{
  "voice": 1,
  "wpm": 0,
  "pitch": 0,
  "inflection": -1,
  "phrase_prediction": 0,
  "log_text": 0
}
```

- `voice`: Eloquence's preset voice, 1 to 8: Adult Male 1, Adult Female 1, Child 1, Adult Male 2, Adult Male 3, Adult Female 2, Elderly Female 1, Elderly Male 1.
- `wpm`: words a minute at Schwung's normal speed. 0 keeps the default.
- `pitch`: hertz at Schwung's normal pitch of 110. 0 keeps the default.
- `inflection`: how far the pitch rises and falls, from 0 (monotone) to 100. -1 keeps the voice's own, which is 30 to 44 depending on the voice.
- `phrase_prediction` (0-1): This Eloquence-specific feature inserts pauses in sentences to make them sound more natural. Schwung-ELoquence disables it by default.
- `log_text`: 1 writes every text spoken to `eloquence.log`, before and after the text fixes.

## Text fixes

Eloquence mispronounces some things, and screen reader users have long fixed them with regular expressions. Rules go in `/data/UserData/eloquence/dict/regexp.dic`. The installer puts one there with the fixes from the Eloquence add-ons for NVDA (dates like "03 Marble", "Mc" names, numbers like "1,000,500"), and replaces it with the latest each time you install, so keep your own changes elsewhere too. Changes are picked up the next time something is spoken, with no restart, and with no file there are simply no fixes.

The file has the shape of OpenEVV's own dictionaries: one line per rule, consisting of a regular expression, a tab, and the replacement. Patterns use the same syntax as the Eloquence add-ons for NVDA, and `(?i)` at the start of one makes it ignore case:

```
\b(Mc)\s+([A-Z][a-z]|[A-Z][A-Z]+)	\1\2
(?i)\bmidi\b	MIDI
```

In a replacement, `\1` or `\g<1>` is what the first group matched. A replacement can also contain Eloquence's own annotations, such as `` `p300 `` for a 300 ms pause; backticks in what Schwung says are read as spaces, so only your rules can add them. Lines starting with `#` are comments. A rule that cannot be read is reported in `eloquence.log` with its line number.

## Pronunciation dictionaries

The community's [IBMTTS dictionaries](https://github.com/eigencrow/IBMTTSDictionaries) correct how Eloquence says thousands of words. They are not included, but any or all of the three US English ones can be added: copy `ENUmain.dic`, `ENURoot.dic` and `ENUabbr.dic` into `/data/UserData/eloquence/dict/` with those exact names, and restart Move. They are read when the screen reader first speaks, and `eloquence.log` says how long each took.

`ENURoot.dic` has 69,000 entries, and during testing it took minutes to load, during which the screen reader is silent, and made every phrase several times slower afterwards. This appears to be a bug in OpenEVV. `ENUmain.dic` (whole words) and `ENUabbr.dic` (abbreviations) are small and cost nothing noticeable.

## Repository Layout

| File | Description |
|---|---|
| `flite_eci.c` | the proxy library, `libflite.so.1` |
| `install.sh` | the installer; the one on the website is this file |
| `entry.sh` | the boot target, runs on the Move and replaces Flite with Eloquence |
| `build.sh`, `Dockerfile` | the build |
| `regexp.dic` | the default text fixes |
| `eloquence.example.json` | the example settings, with every setting at its default |
| `VERSION` | the version, shown by the installer |
| `LICENSE` | the MIT licence for this code |

## How it works

Schwung links Flite dynamically and calls five of its functions. `flite_eci.c` provides exactly those five, as a `libflite.so.1` that takes the place of Flite's:

- **Loading:** it loads OpenEVV (`libeci.so.1`) the first time something is spoken, after moving that thread off Move's real-time audio priority and audio core.
- **Sample rate:** it synthesizes at Eloquence's native 11,025 Hz and upsamples to 44.1 kHz itself, which takes about 55 ms for a typical phrase on the Move.
- **Long text:** it cuts utterances at 11.9 seconds, because Schwung drops anything longer than 12 seconds outright.

The other three Flite libraries Schwung links are replaced by empty ones.

## Building

You need git and Docker. Everything is built for the Move in Debian bookworm, as Schwung is, because newer Debian versions use a newer glibc than the Move's.

```
git clone --recursive https://github.com/simon818/schwung-eloquence
cd schwung-eloquence
./build.sh
```

That makes `out/install.sh` and `out/schwung-eloquence.tar.gz`, the two files to publish. To try a build on a Move first:

```
scp out/install.sh out/schwung-eloquence.tar.gz ableton@move.local:.
ssh ableton@move.local sh install.sh schwung-eloquence.tar.gz
```

The library's log is `ssh ableton@move.local cat /data/UserData/eloquence/eloquence.log`.

OpenEVV is a git submodule pinned to a tested commit. To try a newer one, run `git submodule update --remote openevv` before building.

## Licensing

The code in this repository is MIT licensed; see `LICENSE`. The archive carries that too, in `licenses/`, and also:

- **OpenEVV's** `libeci.so.1`. OpenEVV's code is MIT licensed; its language data is transcribed from IBM's ETI-Eloquence and is not covered by that licence. See `licenses/openevv-LICENSE` and `licenses/openevv-NOTICE`.
- **PCRE2**, linked into `libflite.so.1` for the text fixes, under its BSD licence (`licenses/pcre2-LICENCE`).

## LLM Transparency

Some of this code was written or refined by Claude Opus 5.5, especially the proxy library. All code is thoroughly human-tested.

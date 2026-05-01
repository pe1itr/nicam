# NICAM direct QPSK experiments

Dit project is opgezet voor experimenten waarbij het NICAM DQPSK-signaal direct
op RF wordt gezet, dus zonder analoge TV-draaggolf/subcarrierketen.

De eerste focus is een RTL-SDR ontvanger die een direct gecentreerd NICAM-signaal
kan demoduleren en frames kan vinden. De PlutoSDR-zenderkant staat als tijdelijke
wrapper in `src/nicam/pluto_tx.py`, zodat bestaande Pluto-initialisatiecode daar
later in kan worden gezet.

## Signaalparameters

- NICAM bitrate: `728 kbit/s`
- DQPSK symbolrate: `364 ksym/s`
- Frame: `728 bits`, dus `1 ms`
- FAW: `01001110`
- Scrambler: PN9, polynomial `x^9 + x^4 + 1`, seed `111111111`
- Direct-QPSK mode: stem de RTL-SDR af op de NICAM RF-carrier zelf

Voor de eerste receiver is `1.456 MS/s` gekozen, precies 4 samples per DQPSK
symbool. Dat houdt de timing in deze experimentele versie eenvoudig.

## Installatie

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
pip install -e .
```

Daarnaast moet de `rtl_sdr` command line tool beschikbaar zijn, bijvoorbeeld uit
het pakket `rtl-sdr`. Voor `--stream-url` en `--audio-file` is ook `ffmpeg`
nodig.

## Ontvanger starten

```sh
python -m nicam.rtlsdr_rx --freq 100000000 --gain 30
```

Belangrijke opties:

- `--freq`: frequentie van de directe NICAM QPSK carrier in Hz
- `--ppm`: RTL-SDR correctie
- `--gain`: tuner gain, of `auto`
- `--sample-rate`: standaard `1456000`
- `--freq-offset`: kleine baseband-correctie in Hz als de carrier niet precies in
  het midden zit
- `--iq-file`: lees IQ uit een bestand in plaats van live van `rtl_sdr`

De huidige ontvanger decodeert nog geen audio. Hij demoduleert DQPSK naar bits,
zoekt NICAM frame alignment, descrambelt de frame-body en toont frame metadata.
Dat is de juiste tussenstap voordat de 704-bit payload naar audio wordt omgezet.

## Audio stream moduleren

Voor end-to-end testen is er een experimentele audiopayload ingebouwd. Die gebruikt
wel de NICAM frame-, scrambler-, interleaver- en DQPSK-keten, maar nog niet de
officiele NICAM near-instantaneous companding. Audio wordt tijdelijk als eenvoudige
10-bit PCM plus parity in de 704-bit payload gezet.

Een stream-URL naar direct-QPSK IQ moduleren:

```sh
python -m nicam.stream_tx \
  --stream-url "https://icecast.omroep.nl/radio2-bb-aac" \
  --out /tmp/radio2-nicam.iq
```

`stream_tx` gebruikt `ffmpeg` om de URL of audiobestanden naar `s16le`, stereo,
32 kHz te decoderen.

## PlutoSDR uitzenden

De Pluto-zender leest dezelfde `rtl_sdr`-achtige unsigned 8-bit IQ-stream als de
softwaretest gebruikt. Daardoor kun je `stream_tx` direct naar de Pluto pipe'en:

```sh
python -m nicam.stream_tx --tone \
  | python -m nicam.pluto_tx --lo 100000000 --tx-gain -30
```

Met een audiostream:

```sh
python -m nicam.stream_tx \
  --stream-url "https://icecast.omroep.nl/radio2-bb-aac" \
  | python -m nicam.pluto_tx --lo 100000000 --tx-gain -30
```

Belangrijke Pluto-opties:

- `--lo`: RF-carrier in Hz
- `--uri`: standaard `ip:192.168.2.1`
- `--sample-rate`: standaard `1456000`
- `--tx-gain`: Pluto TX hardware gain/attenuation in dB, begin laag, bijvoorbeeld `-40` tot `-30`
- `--rf-bandwidth`: standaard `750000`
- `--iq-in`: IQ-bestand of `-` voor stdin

Voor een korte herhalende test kun je eerst IQ maken en die in de cyclic buffer
van de Pluto zetten:

```sh
python -m nicam.stream_tx --tone --seconds 1 --out /tmp/nicam-tone.iq
python -m nicam.pluto_tx --iq-in /tmp/nicam-tone.iq --lo 100000000 --tx-gain -40 --cyclic
```

Let op: zend alleen op frequenties, vermogens en aansluitingen die toegestaan
zijn. Voor labtests is een coaxverbinding met verzwakker tussen Pluto en RTL-SDR
het veiligst.

## RTL-SDR audio terugontvangen

`stream_rx` kan nu direct `rtl_sdr` starten en audio naar WAV of stdout schrijven:

```sh
python -m nicam.stream_rx \
  --freq 100000000 \
  --gain 30 \
  --audio-out /tmp/nicam-rx.wav \
  --verbose --stats
```

Live luisteren kan met `ffplay`:

```sh
python -m nicam.stream_rx \
  --freq 100000000 \
  --gain 30 \
  --audio-out - \
  | ffplay -hide_banner -loglevel error -nodisp \
      -f s16le -sample_rate 32000 -ch_layout stereo -i -
```

Als de Pluto en RTL-SDR niet exact op frequentie staan, probeer dan een kleine
baseband-correctie:

```sh
python -m nicam.stream_rx --freq 100000000 --freq-offset 2000 --audio-out /tmp/nicam-rx.wav
```

## Testen zonder SDR's

Directe pipe van modulator naar demodulator naar een WAV-bestand:

```sh
python -m nicam.stream_tx --tone --seconds 5 \
  | python -m nicam.stream_rx --audio-out /tmp/nicam-loop.wav --verbose
```

Live luisteren met `ffplay`:

```sh
python -m nicam.stream_tx \
  --stream-url "https://icecast.omroep.nl/radio2-bb-aac" \
  | python -m nicam.stream_rx --audio-out - --timing-phase 0 \
  | ffplay -hide_banner -loglevel error -nodisp -f s16le -sample_rate 32000 -ch_layout stereo -i -
```

Dezelfde keten met een interne testtoon:

```sh
python -m nicam.stream_tx --tone \
  | python -m nicam.stream_rx --audio-out - --timing-phase 0 \
  | ffplay -hide_banner -loglevel error -nodisp -f s16le -sample_rate 32000 -ch_layout stereo -i -
```

Als er geen geluid uit `ffplay` komt, schrijf dan eerst een WAV-bestand om te
controleren of de modulator-demodulator-keten audio produceert:

```sh
python -m nicam.stream_tx --tone --seconds 3 \
  | python -m nicam.stream_rx --audio-out /tmp/nicam-tone.wav --verbose
ffplay -hide_banner -loglevel error -autoexit /tmp/nicam-tone.wav
```

## Frame-lock test zonder audio

Maak een synthetisch direct-DQPSK IQ-bestand:

```sh
python tools/generate_test_iq.py --frames 500 --out /tmp/nicam-test.iq
python -m nicam.rtlsdr_rx --iq-file /tmp/nicam-test.iq
```

## Pluto-zender

`src/nicam/pluto_tx.py` gebruikt `pyadi-iio` en verwacht dat de Pluto via USB of
netwerk bereikbaar is. De CLI kan IQ uit stdin, een bestand of een cyclic buffer
uitzenden.

## Opus over QPSK/DQPSK experiment

Naast de NICAM-keten staat er in `src/opus/` een eerste experimentele set voor
een enkel stereo audiokanaal via Opus over een directe differentiele
QPSK-byte-stream.
Dit is bedoeld als pragmatisch startpunt: packet sync, sequence numbers, CRC en
Opus werken eerst; FEC/interleaving en betere carrier/timing recovery kunnen
daarna worden toegevoegd.

Extra Python dependency:

```sh
pip install --user opuslib
```

`opuslib` gebruikt de systeemlibrary `libopus`. Op Debian/Ubuntu is dat meestal:

```sh
sudo apt install libopus0 ffmpeg rtl-sdr
```

Offline pipe-test naar WAV:

```sh
python -m opus.qpsk_tx --tone --seconds 5 \
  | python -m opus.qpsk_rx --audio-out /tmp/opus-qpsk.wav --verbose
ffplay -hide_banner -loglevel error -autoexit /tmp/opus-qpsk.wav
```

Live luisteren uit de softwareketen:

```sh
python -m opus.qpsk_tx --tone \
  | python -m opus.qpsk_rx --audio-out - \
  | ffplay -hide_banner -loglevel error -nodisp \
      -f s16le -sample_rate 48000 -ch_layout stereo -i -
```

Met een audiostream of bestand:

```sh
python -m opus.qpsk_tx \
  --stream-url "https://icecast.omroep.nl/radio2-bb-aac" \
  --bitrate 128000 \
  | python -m opus.qpsk_rx --audio-out - \
  | ffplay -hide_banner -loglevel error -nodisp \
      -f s16le -sample_rate 48000 -ch_layout stereo -i -
```

Via PlutoSDR zenden kan met dezelfde `pluto_tx` wrapper als de NICAM-test:

```sh
python -m opus.qpsk_tx --tone \
  | python -m nicam.pluto_tx --lo 100000000 --tx-gain -40 --rf-bandwidth 500000
```

RTL-SDR ontvangen:

```sh
python -m opus.qpsk_rx \
  --freq 100000000 \
  --gain 30 \
  --audio-out - \
  --verbose \
  | ffplay -hide_banner -loglevel error -nodisp \
      -f s16le -sample_rate 48000 -ch_layout stereo -i -
```

De huidige defaults gebruiken `1.456 MS/s` en `364 ksym/s`, dus 4 samples per
symbool. Dat is breder dan strikt nodig voor 128 kbit/s Opus, maar sluit aan op
de bestaande SDR-pijplijn en houdt de eerste timing eenvoudig.

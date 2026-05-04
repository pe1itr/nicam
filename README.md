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
- Praktische RF-bandbreedte (deze implementatie): ongeveer `500-750 kHz`
- Frame: `728 bits`, dus `1 ms`
- FAW: `01001110`
- Scrambler: PN9, polynomial `x^9 + x^4 + 1`, seed `111111111`
- Direct-QPSK mode: stem de RTL-SDR af op de NICAM RF-carrier zelf

Voor de eerste receiver is `1.456 MS/s` gekozen, precies 4 samples per DQPSK
symbool. Dat houdt de timing in deze experimentele versie eenvoudig.

## Installatie

Kies de methode die past bij de target machine.

Optie A: user-installatie (zonder venv):

```sh
python3 -m pip install --user -r requirements.txt
python3 -m pip install --user -e .
```

Optie B: virtual environment (`venv`):

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
pip install -e .
```

Daarnaast moet de `rtl_sdr` command line tool beschikbaar zijn, bijvoorbeeld uit
het pakket `rtl-sdr`. Voor `--stream-url` en `--audio-file` is ook `ffmpeg`
nodig.

Voor systemd user-services is het aan te raden overal `python3` te gebruiken,
zodat je niet afhankelijk bent van een eventuele andere `python` default op het
systeem.

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
  --ffmpeg-reconnect \
  --out /tmp/radio2-nicam.iq
```

`stream_tx` gebruikt `ffmpeg` om de URL of audiobestanden naar `s16le`, stereo,
32 kHz te decoderen.

Digital Baseband V1.4-compatible station-ID kan in de NICAM additional-data
bits worden meegestuurd. Alleen de eerste 8 ASCII-tekens worden gebruikt:

```sh
python -m nicam.stream_tx --tone --station-id PE1MUD --out /tmp/nicam.iq
NICAM_TX_STATION_ID=PE1MUD tools/tim-nicam-tx
```

De mapping is `AD0..AD2 = tekenpositie 0..7` en `AD3..AD10 = ASCII-teken`.
De C-decoder print een gestabiliseerde ontvangen ID op stderr als
`station_id=...`.

Praktisch commando om een internetstream direct door de hele keten te starten:

```sh
python -m nicam.stream_tx \
  --stream-url "https://icecast.omroep.nl/radio2-bb-aac" \
  --ffmpeg-reconnect \
  | python -m nicam.stream_rx --audio-out - --timing-phase 0 \
  | ffplay -hide_banner -loglevel error -nodisp \
      -f s16le -sample_rate 32000 -ch_layout stereo -i -
```

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

## WBFM-zender

Naast de directe NICAM/QPSK-keten bevat het project ook een experimentele stereo
WBFM-zender voor PlutoSDR. Deze maakt een FM-MPX-signaal met L+R, 19 kHz pilot
en L-R DSB-subcarrier, moduleert dat naar complex baseband IQ en kan dit direct
naar de Pluto sturen.

De standaardinstellingen staan in `config/config.yaml`. Test eerst offline of de
audio-naar-IQ-keten werkt:

```sh
PYTHONPATH=src python3 -m wbfm.main \
  --config config/config.yaml \
  --source wav \
  --input test_audio/voorbeeld.wav \
  --iq-out /tmp/wbfm.complex64 \
  --seconds 2
```

Uitzenden via PlutoSDR:

```sh
PYTHONPATH=src python3 -m wbfm.main \
  --config config/config.yaml \
  --freq 2323.7 \
  --gain -30 \
  --source wav \
  --input test_audio/voorbeeld.wav \
  --cyclic
```

Na een editable install kun je ook `wbfm-tx` gebruiken in plaats van
`python3 -m wbfm.main`.

Belangrijke WBFM-opties:

- `--freq`: Pluto TX LO in MHz
- `--gain`: Pluto TX hardware gain in dB; begin laag
- `--source`: `wav`, `stream`, `device` of `silence`
- `--iq-out`: schrijf complex64 baseband IQ naar een bestand in plaats van TX
- `--deviation`: FM-deviatie in Hz, standaard `75000`
- `--preemphasis-us`: pre-emphasis, standaard `50`
- `--pilot-level`: 19 kHz pilotniveau

## WBFM broadcast ontvangen

De RTL-SDR WBFM-ontvanger decodeert standaard stereo broadcast-FM: FM
discriminator, 19 kHz pilot, L-R subcarrier, 50 us de-emphasis en stereo PCM.
De stereo-decoder gebruikt standaard gedeeltelijke stereo-blend om L-R-ruis te
beperken.

Naar WAV opnemen:

```sh
PYTHONPATH=src python3 -m wbfm.rtl_rx \
  --device-index 1 \
  --freq 100700000 \
  --sample-rate 960000 \
  --gain 29.7 \
  --audio-out /tmp/wbfm-rx.wav
```

Live luisteren met `ffplay`:

```sh
PYTHONPATH=src python3 -m wbfm.rtl_rx \
  --device-index 1 \
  --freq 100700000 \
  --sample-rate 960000 \
  --gain 29.7 \
  --audio-out - \
  | ffplay -hide_banner -loglevel error -nodisp \
      -f s16le -sample_rate 48000 -ch_layout stereo -i -
```

Een offline WBFM IQ-bestand uit de zender terugdecoderen kan met:

```sh
PYTHONPATH=src python3 -m wbfm.rtl_rx \
  --iq-in /tmp/wbfm.complex64 \
  --iq-format complex64 \
  --audio-out /tmp/wbfm-loop.wav
```

Na een editable install kun je ook `wbfm-rx` gebruiken in plaats van
`python3 -m wbfm.rtl_rx`.

Bij ruis eerst mono vergelijken:

```sh
tools/websdr-wbfm-rx --mono
```

Daarna stereo-blend instellen. Lager is rustiger, hoger is breder stereo:

```sh
tools/websdr-wbfm-rx --stereo-blend 0.35
tools/websdr-wbfm-rx --stereo-blend 1.0
```

De ontvanger heeft standaard pilot-squelch. Als de zender wegvalt en de 19 kHz
pilot onvoldoende is, fade't de audio dicht:

```sh
tools/websdr-wbfm-rx --verbose
tools/websdr-wbfm-rx --squelch-pilot 0.12
tools/websdr-wbfm-rx --no-squelch
```

## Machineprofielen en uniforme start

Voor machines met verschillende Python-installaties en audio-uitgangen staat er
een launcher in `tools/nicam-run`. Die laadt eerst een profiel uit
`config/environments/` en start daarna de juiste module met dezelfde commando's
op elke host.

Profielkeuze:

```sh
tools/nicam-run wbfm-rx ...
NICAM_ENV=websdr tools/nicam-run nicam-rx ...
NICAM_ENV_FILE=/opt/nicam/local.env tools/nicam-run wbfm-tx ...
```

Zonder override zoekt de launcher automatisch:

```sh
config/environments/$(hostname -s).env
```

Voorbeeldprofielen staan in:

- `config/environments/websdr.env.example`
- `config/environments/odroid.env.example`
- `config/environments/desktop.env.example`
- `config/environments/user-install.env.example`

De belangrijkste profielvelden:

```sh
PYTHON_MODE=src        # src, user of venv
PYTHON_BIN=python3
VENV_PATH=${REPO_DIR}/.venv
AUDIO_BACKEND=aplay   # stdout, ffplay, aplay of none
AUDIO_DEVICE=plughw:0,0
NICAM_AUDIO_RATE=32000
WBFM_AUDIO_RATE=48000
```

Voorbeelden:

```sh
tools/nicam-run nicam-rx --device-index 1 --freq 435970000 --gain 29.7
tools/nicam-run wbfm-rx --device-index 1 --freq 100700000 --gain 29.7
tools/nicam-run wbfm-tx --config config/config.yaml --source stream --stream-url "https://icecast.omroep.nl/radio2-bb-aac"
```

WebSDR/Odroid WBFM shortcut, met `config/environments/websdr.env` en standaard
`436000000` Hz:

```sh
tools/websdr-wbfm-rx
```

Tijdelijk overschrijven:

```sh
GAIN=29.7 FREQ_HZ=436000000 tools/websdr-wbfm-rx
tools/websdr-wbfm-rx --mono
```

Tim WBFM-zender shortcut, met `.venv` en standaard `2324 MHz`:

```sh
cp config/environments/tim.env.example config/environments/tim.env
tools/tim-wbfm-tx
```

Tijdelijk overschrijven:

```sh
TX_GAIN_DB=-15 tools/tim-wbfm-tx
TX_FREQ_MHZ=2324 TX_STREAM_URL="https://icecast.omroep.nl/radio2-bb-aac" tools/tim-wbfm-tx
```

Bij korte internetstream-haperingen blijft de WBFM-zender standaard doorlopen:
ffmpeg reconnect wordt gebruikt, de decoder wordt opnieuw gestart bij EOF, en
er wordt tijdelijk stilte uitgezonden. Uitzetten kan met:

```sh
tools/tim-wbfm-tx --no-stream-silence
tools/tim-wbfm-tx --no-stream-reconnect
```

NICAM shortcuts:

```sh
tools/tim-nicam-tx
tools/websdr-nicam-rx
```

De NICAM zender op `tim` gebruikt standaard `2324 MHz` en de NICAM ontvanger op
`websdr` gebruikt standaard `436 MHz` IF. Tijdelijk overschrijven:

```sh
NICAM_TX_GAIN_DB=-8 tools/tim-nicam-tx
NICAM_TX_SOURCE=tone tools/tim-nicam-tx
NICAM_RX_GAIN=29.7 tools/websdr-nicam-rx
NICAM_RX_FREQ_OFFSET=-2900 tools/websdr-nicam-rx
```

De lokale configuratie bevat een operatorprofiel voor een volledige
amateurvergunning op de amateurbanden:

```yaml
operator:
  amateur_radio_license: "full"
  amateur_bands_authorized: true
  suppress_tx_warnings: true
```

Met `suppress_tx_warnings: true` onderdrukt de WBFM-zender de generieke
TX-gain-waarschuwing. Voor labtests blijft een coaxverbinding met verzwakker
tussen Pluto en RTL-SDR het meest reproduceerbaar.

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

## Systemd user-service (RTL-SDR ontvanger)

Voor een installatie waar de repo in `~/nicam-transmitter` staat:

```sh
mkdir -p ~/.config/systemd/user
cp ~/nicam-transmitter/systemd/user/nicam-rx.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now nicam-rx.service
```

Frequentie-instelling met converter:

- RF doelfrequentie: `2324 MHz`
- Externe converter LO: `1888 MHz`
- RTL-SDR tuningfrequentie (IF): `436 MHz` (`2324 - 1888 = 436`)

Status en logs:

```sh
systemctl --user status nicam-rx.service
journalctl --user -u nicam-rx.service -f
```

Standaard draait deze service met `rtl_sdr -d 2` op `436000000` Hz.
Dit is bedoeld voor een externe converter met LO `1888 MHz` voor een doelfrequentie
van `2324 MHz` (`2324 - 1888 = 436 MHz` IF).

RF SNR-indicatie aanzetten (periodieke ontvangstmeting):

```sh
systemctl --user edit nicam-rx.service
```

Voeg toe:

```ini
[Service]
Environment=EXTRA_RX_ARGS=--rf-snr --rf-snr-interval 1.0
```

Daarna:

```sh
systemctl --user daemon-reload
systemctl --user restart nicam-rx.service
```

Uitzetten: verwijder deze `Environment=EXTRA_RX_ARGS=...` regel weer (of maak hem leeg) en herstart de service.

Waar zie je het:

- In foreground/terminal: op `stderr` van `python -m nicam.stream_rx`
- Als systemd user-service: via `journalctl --user -u nicam-rx.service -f`

Voorbeeldregel:

```text
rf_stats: level=-32.4 dBFS snr_est=18.7 dB
```

## Referenties

- ETSI ETS 300 163 (NICAM 728, Nov 1994): https://www.etsi.org/deliver/etsi_i_ets/300100_300199/300163/01_60/ets_300163e01p.pdf
- ETSI EN 300 163 V1.2.1 (Mar 1998, catalogus): https://standards.iteh.ai/catalog/standards/etsi/60810122-7c6b-44ed-9196-311b7673a79c/etsi-ets-300-163-ed-1-1994-11

De huidige defaults gebruiken `1.456 MS/s` en `364 ksym/s`, dus 4 samples per
symbool. Dat is breder dan strikt nodig voor 128 kbit/s Opus, maar sluit aan op
de bestaande SDR-pijplijn en houdt de eerste timing eenvoudig.

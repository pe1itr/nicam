# NICAM decoderstappen

Bronnen:

- `docs/en_300163v010201o.pdf`: ETSI EN 300 163 V1.2.1, de officiële
  NICAM-728 standaard voor systemen B, G, H, I, K1 en L.
- `docs/The NICAM system (Elektor Electronics, May 1992).pdf`: praktisch
  achtergrondartikel met bruikbare decoderblokdiagrammen.

Dit document vat de punten samen die voor een softwaredecoder relevant zijn. De
ETSI-standaard is leidend voor exacte waarden en bitindelingen; het
Elektor-artikel helpt vooral bij de praktische decoderketen.

## Signaal en front-end

Een klassieke TV-ontvanger haalt de NICAM-draaggolf uit het TV-basebandspectrum:

- PAL system I: NICAM op `6.552 MHz` boven de vision carrier.
- PAL systemen B/G/H/K1/L: NICAM op `5.850 MHz`.
- De NICAM-band is ongeveer `600 kHz` breed.
- De officiële bitrate is `728 kbit/s +/- 1 ppm`.
- De digitale carrierfrequentie is ook gespecificeerd met `+/- 1 ppm`.
- Het modulated digital signal ligt voor B/G/H/I ongeveer `100:1` in vermogen
  onder de peak vision carrier, dus ongeveer `-20 dB`. Voor K1/L is dat
  ongeveer `500:1`. Voor PAL-I kabeldistributie noemt de norm ongeveer `300:1`.

Voor deze repo is de TV-beeldketen meestal niet aanwezig. De praktische stap is
daarom: stem de SDR direct af op de NICAM/QPSK-carrier, meng naar complex
baseband, filter de band en lever genormaliseerde IQ-samples aan de DQPSK
demodulator.

## 1. DQPSK demoduleren

NICAM-728 gebruikt differentieel gecodeerde QPSK:

- bit rate: `728 kbit/s +/- 1 ppm`
- symbol rate: `364 ksym/s`
- 2 bits per symbool
- vier fase-rusttoestanden, 90 graden uit elkaar

De faseverandering per bitpaar is:

| Bitpaar | Faseverandering |
| --- | --- |
| `00` | `0 deg` |
| `01` | `-90 deg` |
| `11` | `-180 deg` |
| `10` | `-270 deg` / `+90 deg` |

Voor een softwaredecoder betekent dit:

1. Corrigeer grove carrier-offset.
2. Pas eventueel een matched filter toe als de zender pulse shaping gebruikt.
3. Herstel symbol timing op `364 ksym/s`.
4. Neem per symbool een complex sample of beslissing.
5. Decodeer bitparen uit de faseverandering tussen opeenvolgende symbolen.

Omdat het differentieel is, hoeft de absolute fase niet bekend te zijn. De
decoder moet wel de vorige symboolfase onthouden over chunkgrenzen.

## Spectrum shaping

De norm beschrijft een data-shaping filter voor de symbol-rate impulsen voordat
ze quadrature-gemoduleerd worden. Het ideale receiverfilter is hetzelfde filter;
de gecombineerde TX+RX-respons is dus het kwadraat van de enkelvoudige
filterrespons.

- Voor systemen B/G/H/K1/L is `k = 0.4`; TX+RX geeft `40 %` cosine roll-off.
- Voor systeem I is de gespecificeerde respons anders; TX+RX geeft `100 %`
  cosine roll-off.
- Voor B/G/H/K1/L noemt de norm ook een praktische spectrumtolerantie van
  ongeveer `+/- 2 dB` binnen `5.85 MHz +/- 250 kHz` en een differentiele
  group-delay binnen ongeveer `+/- 100 ns` in dat gebied.

De huidige repo gebruikt voor de directe SDR-opzet standaard een RRC-pad met
`0.4` roll-off. Dat sluit dus goed aan bij B/G/H/K1/L en is praktisch voor onze
direct-QPSK experimenten. Voor strikt system-I gedrag is een aparte
`100 %`-roll-off profielkeuze nodig.

## 2. Frame-lock zoeken

De gedemoduleerde bitstream bestaat uit frames van `728 bits`. Elk frame duurt
`1 ms` en begint met het frame alignment word:

```text
FAW = 01001110
```

De FAW is niet gescrambled. Gebruik hem daarom voor de eerste lock:

1. Zoek `01001110` in de bitstream.
2. Controleer dat dezelfde offset iedere `728 bits` terugkomt.
3. Bevestig lock over meerdere frames, niet op een enkel FAW-hit.
4. Gebruik daarna de verwachte volgende framepositie om snel locked te blijven.

Het artikel noemt ook de frame flag `C0`: die is acht frames `1` en daarna acht
frames `0`. Dat geeft een 16-frame patroon. In software is dat nuttig als extra
sanity check bij lock-acquisitie en relock.

## 3. Body descramblen

Na de FAW volgen `720 bits`:

```text
5 control bits + 11 additional-data bits + 704 sound/data bits
```

Deze 720 bits zijn gescrambled voor energy dispersal. De FAW zelf niet.

Scramblerdetails uit het artikel:

- polynomial: `x^9 + x^4 + 1`
- seed: `111111111`
- de PRBS-reeks start volgens EN 300 163 met `0000 0111 1011 1110 0010`
- de eerste bit na de FAW wordt met de eerste PRBS-bit ge-xord
- de bit vlak voor de volgende FAW is de laatste gescramblede bit
- TX scramblet direct na interleaving
- RX descramblet dus voor de-interleaving

De decoderstap is:

```text
body_descrambled = body_received XOR PN9[0:720]
```

Daarna zijn de eerste 16 body-bits de control/additional-data velden en de
laatste 704 bits de interleaved sound/data payload.

## 4. Control bits interpreteren

De eerste 5 descramblede body-bits zijn:

```text
C0 C1 C2 C3 C4
```

Betekenis:

- `C0`: frame flag voor de 16-frame cyclus.
- `C1 C2 C3`: toepassing van de 704-bit payload.
- `C4`: reserve sound switching flag, bedoeld voor fallback naar analoge FM als
  dezelfde audio daar beschikbaar is.

De basisapplicaties voor `C1 C2 C3`:

| `C1 C2 C3` | Payload |
| --- | --- |
| `000` | stereo, afwisselend A- en B-kanaalsamples |
| `010` | twee onafhankelijke mono-kanalen, M1/M2 in afwisselende frames |
| `100` | een mono-kanaal plus een transparant datakanaal in afwisselende frames |
| `110` | een transparant `704 kbit/s` datakanaal |

Als `C3=1`, gaat het om extra opties. Een decoder die die opties niet
ondersteunt hoort naar FM/AM fallback te gaan of geen digitale audio uit te
geven.

De officiële tabel koppelt `C4` aan fallback:

- `C4=1`: de analoge FM/AM-drager voert hetzelfde programma als de digitale
  stereo- of mono-audio. Fallback naar analoog is toegestaan.
- `C4=0`: de analoge FM/AM-drager voert niet hetzelfde programma, of fallback
  moet worden verhinderd.
- Bij datatransmissie heeft `C4` geen betekenis.

Voor deze repo is stereo `000` de hoofdroute.

## 5. Payload de-interleaven

De laatste `704 bits` van het frame zijn interleaved. Het artikel tekent dit als
`44` rijen van `16` bits. In de verzonden volgorde staan bits die oorspronkelijk
naast elkaar lagen minimaal 16 klokperioden uit elkaar.

Decoderstap:

1. Neem de 704 payloadbits na descrambling.
2. Gebruik de ETSI-transmissievolgorde `25,69,113,...,685`, daarna
   `26,70,114,...,686`, enzovoort.
3. Voor oorspronkelijk payloadbit `n` geldt:
   `tx_index = (n % 44) * 16 + (n / 44)`.
4. Interpreteer de output als `64` woorden van `11 bits`.

In de huidige Python-code is dit dezelfde operatie als
`deinterleave_payload()` in `src/nicam/frame.py`.

## 6. 64 sound words maken

Een audioframe bevat `64` sound samples, elk als een `11-bit` woord:

```text
x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 p
```

- `x0..x9`: 10-bit two's-complement companded sample.
- `p`: parity/signalling bit.
- De parity controleert de zes meest significante samplebits.

Bij stereo:

- oneven sampleposities zijn kanaal A, in TV-context links.
- even sampleposities zijn kanaal B, in TV-context rechts.
- Per frame zijn er dus `32` samples links en `32` samples rechts.
- De audiosamplerate is `32 kHz`, dus een NICAM-frame bevat exact `1 ms`
  stereo-audio.

Let op: het artikel nummert samples vanaf 1. Code nummert normaal vanaf 0. De
praktische mapping in code is daarom `words[0::2]` voor links/A en `words[1::2]`
voor rechts/B.

## 7. Scale factors uit parity-syndromes halen

NICAM gebruikt near-instantaneous companding. Elk kanaalblok van 32 samples heeft
een 3-bit scale-factor/range word. Dat range word wordt niet als apart veld
verstuurd, maar in de parity bits gesignaleerd.

Decoderstappen:

1. Bereken per 11-bit woord de parity syndrome over de zes MSB's plus de
   paritybit.
2. Verzamel de syndromes in vaste groepen.
3. Gebruik majority decision om per groep een range-bit te bepalen.
4. Herstel daarmee het 3-bit range word voor links en rechts.
5. Tel afwijkingen binnen de groepen als parity/quality errors.

Voor stereo gebruikt het artikel zes groepen:

| Groep | Betekenis |
| --- | --- |
| samples `1,7,13,...,49` | `R2` voor kanaal A |
| samples `2,8,14,...,50` | `R2` voor kanaal B |
| samples `3,9,15,...,51` | `R1` voor kanaal A |
| samples `4,10,16,...,52` | `R1` voor kanaal B |
| samples `5,11,17,...,53` | `R0` voor kanaal A |
| samples `6,12,18,...,54` | `R0` voor kanaal B |

Samples `55..64` kunnen aanvullende informatie dragen. De huidige
softwaredecoder gebruikt ze vooral als extra foutindicatie.

Voor mono-sound modes is de grouping anders dan bij stereo. Dan bevat een
sound-frame 64 opeenvolgende mono-samples, dus twee compandingblokken van 32
samples. De scale-factor bits voor het tweede mono-blok beginnen al in samples
`28..32` van het eerste blok. De stereo-groepering hierboven mag daarom niet
blind op mono-mode worden toegepast.

## 8. Decompanding naar PCM

De originele audiosamples zijn 14-bit. Voor verzending worden ze per kanaalblok
teruggebracht naar 10-bit two's-complement waarden. Het gevonden range word
bepaalt hoeveel de 10-bit waarde weer omhoog geschaald moet worden.

Praktische decode:

1. Zet de 10 samplebits om naar signed 10-bit.
2. Bepaal de shift uit het 3-bit range word.
3. Schuif terug naar 14-bit samplewaarde.
4. Schaal naar de gewenste outputrepresentatie, in deze repo `s16le`.

De huidige mapping staat in `RANGE_TO_SHIFT` in `src/nicam/nicam728.py`:

| Range word | Shift |
| --- | --- |
| `111` | 4 |
| `110` | 3 |
| `101` | 2 |
| `011` | 1 |
| `100`, `010`, `001`, `000` | 0 |

Het artikel noemt ook J.17 pre-emphasis aan de zenderkant. Een volledige
broadcast-compatibele ontvanger zou daarom na decompanding nog de bijbehorende
de-emphasis kunnen toepassen. De huidige directe SDR-experimenten gebruiken deze
stap niet als kernvoorwaarde voor frame/audio-lock.

EN 300 163 specificeert daarnaast nominale referentieniveaus. Een `400 Hz`
sinus op alignment level ligt `22 dB` onder maximum digital coding range voor
B/G/H/K1/L en `24.3 dB` onder maximum voor systeem I. Voor onze directe
experimenten is dat vooral relevant als we echte broadcast-level calibratie
willen doen; voor decoder-lock is het niet kritisch zolang clipping wordt
vermeden.

## 9. Foutdetectie, muting en concealment

De parity-syndromes dienen twee doelen:

- range signalling terugwinnen;
- fouten detecteren in de belangrijkste samplebits.

Voor een robuuste live decoder:

1. Accepteer een frame alleen als de FAW/lock en parityscore binnen grenzen
   blijven.
2. Gebruik majority voting voor range bits, want enkele bitfouten hoeven het
   range word niet direct te verpesten.
3. Houd een bad-frame streak bij.
4. Mute of herhaal/maskeer audio bij slechte frames.
5. Drop lock pas na meerdere slechte frames, zodat korte RF-storingen niet
   meteen relock veroorzaken.

## Aanbevolen implementatievolgorde

Voor verdere decoderontwikkeling in deze repo is dit de meest nuttige volgorde:

1. IQ front-end betrouwbaar maken: frequentiecorrectie, filtering, AGC/level en
   symbol timing.
2. Profiel kiezen voor shaping: repo-default `0.4` RRC voor B/G/H/K1/L-achtige
   directe QPSK, of een aparte system-I respons als dat doel expliciet is.
3. DQPSK-bitstream valideren met FAW-herhaling iedere `728 bits`.
4. Descrambler controleren met bekende PRBS-start en `C0` 8-aan/8-uit patroon.
5. Payload de-interleaver valideren met parityscores.
6. Range majority voting en parity-error reporting stabiel maken.
7. Pas daarna audio-uitvoer optimaliseren: decompanding, concealment, eventuele
   de-emphasis en mute/fallback-gedrag.

## Koppeling met huidige code

Relevante bestaande onderdelen:

- `src/nicam/constants.py`: bitrates, framegrootte en FAW.
- `src/nicam/dqpsk.py`: DQPSK mapping, symboolsampling en pulse-shaping helpers.
- `src/nicam/frame.py`: framebouw, descrambling, FAW-lock helpers en
  payload-interleaving.
- `src/nicam/nicam728.py`: NICAM audio payload encode/decode,
  scale-factor/parity handling en decompanding.
- `src/nicam_cli.c`: live C-decoder met lock, matched filter,
  demodulatiehypotheses, parityscores en audio-uitvoer.

De belangrijkste les uit het artikel is dat een NICAM-decoder pas echt stabiel
wordt wanneer de lagen in deze volgorde kloppen: DQPSK decisions, FAW-lock,
descrambling, de-interleaving, range voting, daarna audio.

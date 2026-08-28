# SimpleSerialProtocol

A lightweight master-slave serial communication framework for remote GPIO control of Arduino boards from a Python host. Implements a **request-response** paradigm where the Master delegates commands to Slave microcontrollers over serial.

*TODO: add polling or multiplexing to use a single serial port for multiple Slaves.*

## Protocol

### Request packet (7 bytes)

| Command | PinNumber | ValueHigh | ValueLow | FreqHigh | FreqLow | CheckSum |
|--|--|--|--|--|--|--|
| R/W/r/w | id of pin to use | high byte of the value | low byte of the value | high byte of the frequency | low byte of the frequency | XOR of the six preceding bytes |

### Response packet (6 bytes)

| PinNumber | ValueHigh | ValueLow | FreqHigh | FreqLow | CheckSum |
|--|--|--|--|--|--|
| id of pin used | high byte of the value | low byte of the value | high byte of the frequency | low byte of the frequency | XOR of the five preceding bytes |

Value and frequency are both **16 bit, big endian**. The frequency only means anything to `w`; every other command ignores it on the way in, and every response reports the frequency the pin ended up on.

The Slave answers with the state the pin is **actually in**, not with an echo of the request. A value that gets clamped, or a frequency that could not be honoured, is visible from the response alone — there is no separate error packet, and the one case this does not cover is under [PWM](#pwm). A malformed frame (bad checksum, unknown command, pin out of range) gets no answer at all, and the Master says so on stderr and exits non-zero.

### Commands

| Command | Description | Value field | Frequency field |
|---|---|---|---|
| `R` | Digital READ | ignored; response carries 0 or 1 | ignored |
| `W` | Digital WRITE | 0 = LOW, anything else = HIGH | ignored |
| `r` | Analog READ | ignored; response carries the raw ADC reading (0-1023) | ignored |
| `w` | PWM WRITE | duty cycle, 0-255 | see below |

## PWM

`w` drives both PWM generators on the board, and the frequency field is what picks between them.

| Frequency | What `w` does |
|---|---|
| *omitted* (`0xFFFF` on the wire) | sets the duty, leaves the pin on whatever generator it is already using |
| `0` | hands the pin back to `analogWrite()` — hardware PWM at the core's own frequency |
| `1`-`2000` | software PWM at that frequency |

**Hardware** (`analogWrite`) costs no CPU and has no jitter, but only works on the pins wired to a timer — 3, 5, 6, 9, 10, 11 on an Uno — and runs at whatever frequency the core picked (~490 Hz, ~980 Hz on the Timer0 pins).

⚠️ Asking for frequency `0` on a pin that has **no** timer is neither refused nor visible in the response: `analogWrite()` quietly degrades to a `digitalWrite()` with its threshold at 128, so the pin sits at a fixed level — LOW below 128, HIGH from 128 up. `w 14 125 0` on A0 therefore leaves the pin at 0 V while answering `14,125,analogWrite`, which is the truth about the generator but says nothing about the timer. On those pins a software frequency is the only thing that produces PWM.

**Software** PWM is generated from `micros()` and `digitalWrite()`. No timer register is touched, so it is portable across cores and works on **any digital pin**, at the frequency you ask for.

```
w  9 128         # analogWrite on pin 9, core frequency
w  9 128  100    # same pin, now software PWM at 100 Hz
w  9  64         # duty only: still 100 Hz, frequency untouched
w  9 200    0    # back to analogWrite
w 13 128  100    # pin 13 has no timer, so this only works in software
w 14 125  100    # A0, same story
```

Each line is one run of the Master, so the first of them reads `python SimpleSerialMaster.py -p COM3 w 9 128` in full.

Leaving the frequency out is what makes a duty sweep cheap: you name the frequency once, then pass the duty alone. The frequency lives on the **Slave**, so it survives across separate runs of the Master — as long as the board does not reset in between, which is what the Master goes out of its way to avoid (see [Usage](#usage)).

### Limits

| | |
|---|---|
| Frequency range | 1-2000 Hz (`PWM_MIN_FREQ_HZ` / `PWM_MAX_FREQ_HZ`) |
| Simultaneous software channels | 6 (`PWM_MAX_CHANNELS`) |
| Duty resolution | loop period ÷ PWM period |

The edges land on `loop()` iterations, so the duty resolution degrades as the frequency rises: comfortable below 500 Hz, roughly 2% at the top of the range. Each active channel adds work to every iteration, so six of them are coarser than one.

A frequency above the cap is **clamped**, not rejected. A 7th software channel is refused, and the pin falls back to `analogWrite` — in both cases the response says which frequency the pin actually ended up on, so the Master can tell.

Since the software PWM depends on `loop()` spinning freely, the Slave never blocks: the frame is assembled byte by byte across iterations, and a half-received frame is dropped after 50 ms of silence, so a lost byte costs one frame instead of desynchronising the stream.

`R`, `W` and `r` all take the pin back from the software PWM before using it — otherwise the two would fight over the same output.

## Project structure

```
SimpleSerialProtocol/
├── SimpleSerialMaster.py        # Python master, one request per invocation
├── requirements.txt
└── SimpleSerialSlave/
    └── SimpleSerialSlave.ino    # Arduino slave firmware
```

## Usage

Upload `SimpleSerialSlave.ino` to an Arduino board, then call the Master once per action — one request, one answer, one process:

```bash
python SimpleSerialMaster.py -p COM3 w 14 125 100
```

```
SimpleSerialMaster.py [-p PORT] [--boot-wait SEC] [-v] COMMAND PIN [VALUE] [FREQ]
```

| Argument | |
|---|---|
| `COMMAND` | `R`, `W`, `r` or `w` — the case is what tells digital from analog |
| `PIN` | 0 to 19; A0 to A5 are 14 to 19 on an Uno/Nano |
| `VALUE` | 0/1 for `W`, duty 0-255 for `w`; `R` and `r` take none |
| `FREQ` | `w` only, and optional — see [PWM](#pwm) |
| `-p`, `--port` | serial port of the Slave, `/dev/ttyACM0` if omitted |
| `--boot-wait` | seconds to wait before the single retry, default 2, `0` disables it |
| `--dtr` | assert DTR and RTS on open — **required on a Nano Every**, see below |
| `-v`, `--verbose` | also print both frames in hex |

The answer is one line on stdout, `pin,value,frequency` — for the call above:

```
14,125,100Hz
```

Exit status is what a script should read: **0** the Slave answered, **1** it did not, or the answer carried a bad checksum, **2** the arguments do not make sense. Everything that is not the answer goes to stderr — a port that cannot be opened (together with the list of ports actually present), a silent Slave, a usage error. Communication runs at 9600 baud.

### The port and the reset

On most Arduino boards DTR and RTS are wired to RESET, so opening the serial port reboots the Slave. That was harmless while the Master was interactive and held the port open for a whole session. With one process per command it would reset the board on **every** call, wiping the software PWM channels each time — and a `w 14 200` with the frequency omitted would silently land back on `analogWrite`.

So the Master holds both lines low from *before* the open, which avoids the reset wherever the driver honours it. Where it does not, the first frame falls into the bootloader and gets no answer: the Master then waits `--boot-wait` seconds and sends the same frame once more. The retry is safe because every command of this protocol sets an absolute state — none of them is a toggle or an increment.

Which of the two your board does is worth knowing, and two calls are enough to tell: name a frequency, then send a bare duty.

```bash
python SimpleSerialMaster.py -p COM3 w 14 125 100
```

```bash
python SimpleSerialMaster.py -p COM3 w 14 200
```

`14,200,100Hz` means the channel survived the second open. `14,200,analogWrite` means the board reset, the Slave no longer has a channel on that pin, and any sequence that spans several calls has to name the frequency every time.

### The board that needs the opposite

Holding those lines low is right for a classic Nano and **wrong for a Nano Every**, whose USB side is a SAMD11 bridge that passes no data at all until the host asserts DTR. The Slave then looks mute: no answer to any command, on a board that was programmed over that very cable seconds earlier. `--dtr` is the whole fix.

| Board | |
|---|---|
| Nano (classic) | default — lines held low, so opening the port does not reset the board |
| Nano Every | `--dtr`, or the bridge forwards nothing |

Two things make this one hard to see, and both are specific to the Every. Its upload goes over **UPDI** (`upload.protocol=jtag2updi` in the core's `boards.txt`), not over the serial port, so an upload that works proves the cable and the bridge but says **nothing** about whether the UART path does. And a sketch that spams `Serial.print` only exercises **TX**: the direction this protocol needs first is RX, and that is the one a mute bridge blocks.

Worth ruling out early on that board, since it looks identical from outside: the 4809 runs on an internal oscillator whose 16-or-20 MHz choice lives in a fuse, and a mismatch against the core's `F_CPU` skews every baud rate by 25%. It is not usually the culprit — the Arduino core rewrites `fuse2` to 16 MHz on every upload and corrects the oscillator tolerance from `SIGROW.OSC16ERR5V` — and it fails differently anyway: garbled characters rather than silence.

## Dependencies

- **Master**: Python 3, plus the package in [requirements.txt](requirements.txt) (`pyserial`; the CLI itself is `argparse` from the standard library)
- **Slave**: Arduino IDE

```bash
pip install -r requirements.txt
```

## License

Apache License 2.0 - see [LICENSE](LICENSE) for details.

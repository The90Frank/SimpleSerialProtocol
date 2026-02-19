# SimpleSerialProtocol

A lightweight master-slave serial communication framework for remote GPIO control of Arduino boards from a Python host. Implements a **request-response** paradigm where the Master delegates commands to Slave microcontrollers over serial.

*TODO: add polling or multiplexing to use a single serial port for multiple Slaves.*

## Protocol

### Request packet (4 bytes)

| Command | PinNumber | PinValue | CheckSum |
|--|--|--|--|
| R/W/r/w | id of pin to use | value to set (ignored on read) | (Command XOR PinNumber) XOR PinValue |

### Response packet (3 bytes)

| PinNumber | PinValue | CheckSum |
|--|--|--|
| id of pin used | value set/read | PinNumber XOR PinValue |

### Commands

| Command | Description |
|---|---|
| `R` | Digital READ (returns 0 or 1) |
| `W` | Digital WRITE (HIGH/LOW) |
| `r` | Analog READ (returns 0-255, scaled from 10-bit ADC) |
| `w` | Analog WRITE (PWM, 0-255) |

## Project structure

```
SimpleSerialProtocol/
├── SimpleSerialMaster.py        # Python master with interactive CLI
└── SimpleSerialSlave/
    └── SimpleSerialSlave.ino    # Arduino slave firmware
```

## Usage

Upload `SimpleSerialSlave.ino` to an Arduino board, then run the master:

```bash
python SimpleSerialMaster.py
```

The interactive CLI prompts for command, pin number, and value. Communication runs at 9600 baud on `/dev/ttyACM0`.

## Dependencies

- **Master**: Python 3, `pyserial`, `pyinputplus`
- **Slave**: Arduino IDE

## License

Apache License 2.0 - see [LICENSE](LICENSE) for details.

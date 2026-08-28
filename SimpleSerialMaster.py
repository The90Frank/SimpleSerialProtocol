#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""SimpleSerialProtocol master - one request per invocation.

    SimpleSerialMaster.py [-p PORT] COMMAND PIN [VALUE] [FREQ]

The answer is printed as "pin,value,frequency", the frequency being the one the
pin actually ended up on. Exit status is 0 when the slave answered, 1 when it
did not (or answered with a broken checksum), 2 when the arguments do not make
sense.
"""

import argparse
import sys
import time
import serial
import serial.tools.list_ports

REQ_SIZE = 7
RES_SIZE = 6

DEFAULT_PORT = '/dev/ttyACM0'   # overridden by -p
BAUD_RATE = 9600

MAX_PIN = 20                # Uno/Nano ceiling; the slave uses NUM_DIGITAL_PINS

PWM_MAX_FREQ_HZ = 2000      # must match PWM_MAX_FREQ_HZ in the slave sketch
FREQ_UNCHANGED = 0xFFFF     # must match PWM_FREQ_UNCHANGED in the slave sketch

# The six bytes of an answer take 6 ms on the wire, so this is all slack.
RESPONSE_TIMEOUT_S = 0.5

# Long enough for the Uno/Nano bootloader to get out of the way: see transact().
DEFAULT_BOOT_WAIT_S = 2.0

# (min, max) of the value field per command.
# None means the command ignores the value field.
VALUE_FIELD = {
    'R': None,
    'r': None,
    'W': (0, 1),
    'w': (0, 255),
}

EPILOG = """examples:
  %(prog)s -p COM22 w 14 125 100    software PWM on A0: duty 125 at 100 Hz
  %(prog)s -p COM22 w 14 200        same channel, duty only, frequency untouched
  %(prog)s -p COM22 w 9 128 0       hand pin 9 back to analogWrite
  %(prog)s -p COM22 W 13 1          drive pin 13 high
  %(prog)s -p COM22 R 13            read pin 13
  %(prog)s -p COM22 r 14            read the ADC on A0

A pin with no timer behind it (anything outside 3, 5, 6, 9, 10, 11 on an Uno)
cannot do hardware PWM: there, FREQ 0 leaves analogWrite() falling back on a
plain digitalWrite with its threshold at 128. Ask for a software frequency
instead.
"""

def buildParser():
    parser = argparse.ArgumentParser(
        description='Send one SimpleSerialProtocol request and print the answer.',
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter)

    parser.add_argument('command', metavar='COMMAND', choices=sorted(VALUE_FIELD),
                        help='R digital read, W digital write, r analog read, w PWM write')
    parser.add_argument('pin', metavar='PIN', type=int,
                        help='pin to act on, 0 to ' + str(MAX_PIN - 1)
                             + ' (A0 to A5 are 14 to 19 on an Uno/Nano)')
    parser.add_argument('value', metavar='VALUE', type=int, nargs='?',
                        help='0 or 1 for W, duty 0-255 for w; R and r take none')
    parser.add_argument('freq', metavar='FREQ', type=int, nargs='?',
                        help='w only: 0 hands the pin to analogWrite, 1-'
                             + str(PWM_MAX_FREQ_HZ) + ' is software PWM,'
                             + ' omitted leaves the current frequency alone')

    parser.add_argument('-p', '--port', default=DEFAULT_PORT,
                        help='serial port of the slave (default: %(default)s)')
    parser.add_argument('--boot-wait', type=float, default=DEFAULT_BOOT_WAIT_S,
                        metavar='SEC',
                        help='seconds to wait before the single retry, for a board'
                             ' that reset on open (default: %(default)s, 0 disables it)')
    parser.add_argument('--dtr', action='store_true',
                        help='assert DTR and RTS on open, for a USB bridge that will'
                             ' not pass data without them; this resets any board that'
                             ' wires those lines to RESET')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='also print both frames in hex')

    return parser

def readArguments(parser, args):
    """Whatever the type/choices machinery of argparse cannot express.

    Returns the (value, freq) pair to put on the wire, so the two optional
    positionals turn into the two fields the protocol always carries.
    """
    if not 0 <= args.pin < MAX_PIN:
        parser.error('PIN out of range: ' + str(args.pin)
                     + ' (0 to ' + str(MAX_PIN - 1) + ')')

    field = VALUE_FIELD[args.command]
    if field is None and args.value is not None:
        parser.error("'" + args.command + "' ignores VALUE, so do not pass one")
    if field is not None and args.value is None:
        parser.error("'" + args.command + "' needs a VALUE, "
                     + str(field[0]) + ' to ' + str(field[1]))
    if field is not None and not field[0] <= args.value <= field[1]:
        parser.error('VALUE out of range: ' + str(args.value)
                     + ' (' + str(field[0]) + ' to ' + str(field[1]) + ')')

    if args.freq is not None and args.command != 'w':
        parser.error("only 'w' takes a FREQ")
    if args.freq is not None and not 0 <= args.freq <= PWM_MAX_FREQ_HZ:
        parser.error('FREQ out of range: ' + str(args.freq)
                     + ' (0 to ' + str(PWM_MAX_FREQ_HZ) + ')')

    value = 0 if field is None else args.value
    freq = FREQ_UNCHANGED if args.freq is None else args.freq

    return value, freq

def calcCheckSum(b):
    checksum = b[0]
    for i in range(1,len(b)):
        checksum ^= b[i]
    return checksum

def convertToMessage(command, number, value, freq=FREQ_UNCHANGED):
    message = bytearray(command, "ascii")
    message.append(number & 0xFF)
    message.append((value >> 8) & 0xFF)
    message.append(value & 0xFF)
    message.append((freq >> 8) & 0xFF)
    message.append(freq & 0xFF)
    message.append(calcCheckSum(message))

    return message

def formatResponse(reply):
    value = (reply[1] << 8) | reply[2]
    freq = (reply[3] << 8) | reply[4]
    return str(reply[0]) + "," + str(value) + "," + (str(freq)+"Hz" if freq else "analogWrite")

def attempt(ser, message):
    """Returns RES_SIZE bytes, or None if fewer than that arrived in time."""
    # A partial answer left over from an earlier attempt would shift this one.
    ser.reset_input_buffer()
    ser.write(message)
    reply = ser.read(RES_SIZE)      # comes back as soon as RES_SIZE bytes are in
    return reply if len(reply) == RES_SIZE else None

def transact(ser, message, bootWait):
    """One request, one response, or None if the slave never answered.

    An attempt that gets nothing is retried once after bootWait seconds:
    opening the port can still reset the board despite openPort() holding
    DTR/RTS low, and the bootloader swallows whatever arrives while it runs.
    Retrying is safe because every command of the protocol sets an absolute
    state - none of them is a toggle or an increment.
    """
    reply = attempt(ser, message)
    if reply is None and bootWait > 0:
        time.sleep(bootWait)
        reply = attempt(ser, message)
    return reply

def openPort(port, assertLines=False):
    """Opens the port, by default without ever asserting DTR or RTS.

    On boards where those lines are wired to RESET - the classic Nano among
    them - a plain open() reboots the slave. With one process per command that
    would wipe the software PWM channels between two invocations, and a 'w'
    with the frequency omitted would silently land on analogWrite. Holding the
    lines low from before the open avoids the reset wherever the driver
    honours it; the retry in transact() covers the boards where it does not.

    The other kind of board exists too: a USB bridge that passes no data until
    the host asserts the same lines, which makes the default here look like a
    mute Slave. That is what assertLines is for.
    """
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD_RATE
    ser.timeout = RESPONSE_TIMEOUT_S
    ser.dtr = assertLines
    ser.rts = assertLines

    try:
        ser.open()
    except serial.SerialException as e:
        found = [p.device for p in serial.tools.list_ports.comports()]
        print("Cannot open " + port + ": " + str(e), file=sys.stderr)
        print("Serial ports detected: " + (", ".join(found) if found else "none"),
              file=sys.stderr)
        sys.exit(1)

    return ser

def main():
    parser = buildParser()
    args = parser.parse_args()
    value, freq = readArguments(parser, args)

    message = convertToMessage(args.command, args.pin, value, freq)

    ser = openPort(args.port, args.dtr)
    if args.verbose:
        print("sent:     " + message.hex(" "))
    try:
        reply = transact(ser, message, args.boot_wait)
    finally:
        ser.close()

    if reply is None:
        print("no answer from the slave on " + args.port, file=sys.stderr)
        return 1

    if args.verbose:
        print("received: " + reply.hex(" "))

    # Printed before the checksum is judged: a corrupt frame is still worth
    # seeing, and it is the exit status that says not to trust it.
    print(formatResponse(reply))

    if reply[RES_SIZE-1] != calcCheckSum(reply[:-1]):
        print("bad checksum in the answer", file=sys.stderr)
        return 1

    return 0

if __name__ == "__main__":
    sys.exit(main())

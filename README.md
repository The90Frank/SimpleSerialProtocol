
# SimpleSerialProtocol
A simple way to delegate the microcontroller logic to Master. It's an implementation of **request–response** paradigm. In this scenario the Master has a serial comunicaton for each Slave. 
*TODO: add the polling or similar to use a single serial port for more Slaves.*

## Request packet
The request packet size is 4 bytes.
|Command  |PinNumber  |PinValue|CheckSum|
|--|--|--|--|
|read/write|id of pin to use|vale to set/ignore in read|(Command XOR PinNumber) XOR PinValue



## Response packet
The request packet size is 3 bytes.
|PinNumber  |PinValue|CheckSum|
|--|--|--|
|id of pin used |value set/read|PinNumber XOR PinValue|

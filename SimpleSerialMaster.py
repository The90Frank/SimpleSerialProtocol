#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import re
import time
import serial
import pyinputplus as pyip

def checkCommand(c):  
    if bool(re.match(r'(R|W|r|w)', c)) == False:
        raise Exception('Invalid command [R,r,W,w]')
    return c

def calcCheckSum(b):
    checksum = b[0]
    for i in range(1,len(b)):
        checksum ^= b[i]
    return checksum

def convertToMessage(command, number, value):
    byteCommand = bytes(command, "ascii")
    byteNumber = number.to_bytes(1, 'big')
    byteValue = value.to_bytes(1, 'big')
    
    message = bytearray(byteCommand)
    message.append(byteNumber[0])
    message.append(byteValue[0])
    message.append(calcCheckSum(message))

    return message

def sendAndReceve(ser, message):
    ser.write(message)
    time.sleep(0.2)
    byteread = ser.read(3)
    if(len(byteread)!=3):
        return "None"
    calcCheck = calcCheckSum(byteread[:-1])
    byteCheckSum = byteread[2]
    retmes = str(byteread[0]) + "," + str(byteread[1])
    if(calcCheck != byteCheckSum):
        retmes += "(!)"
    return retmes

def main():
    ser = serial.Serial('/dev/ttyACM0', 9600, timeout=5)
    while True:
        
        command = pyip.inputCustom(checkCommand, prompt="Command: ")
        number = int(pyip.inputNum(prompt="PinNumber: ", min=0, lessThan=20))
        value = int(pyip.inputNum(prompt="PinValue: ", min=0, lessThan=256))

        print('invio: "'+command+','+str(number)+','+str(value)+'"')
        m = convertToMessage(command,number,value)
        r = sendAndReceve(ser,m)
        print('ricevo: "'+r+'"') 

if __name__ == "__main__":
    main()
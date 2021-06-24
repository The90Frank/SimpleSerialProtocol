size_t sizeOut = 3;
size_t sizeIn = 4;

byte * buffOut;
byte * buffIn;

void setup() {
  Serial.begin(9600);
  Serial.setTimeout(10000);
  buffOut = (byte *) calloc(sizeOut, sizeof(byte));
  buffIn = (byte *) calloc(sizeIn, sizeof(byte));
}

void loop() {
    
  size_t nbytes = Serial.readBytes((char *)buffIn, sizeIn);
  if(nbytes == sizeIn && buffIn[sizeIn-1]==calcCheckSum(buffIn,sizeIn-1)) {
    byte command = buffIn[0];
    byte pinNumber = buffIn[1];
    byte pinValue = buffIn[2];
    bool error = false;
    
    if (pinNumber < 20) {
      switch (command) {
        case 'R':
            pinMode(pinNumber, INPUT);
            pinValue = digitalRead(pinNumber);
            break;
        case 'W':
            pinMode(pinNumber, OUTPUT);
            if (pinValue==0) digitalWrite(pinNumber, LOW);
            else             digitalWrite(pinNumber, HIGH);
            break;
        case 'r':
            pinMode(pinNumber, INPUT);
            pinValue = analogRead(pinNumber)/4;
            break;
        case 'w':
            pinMode(pinNumber, OUTPUT);
            analogWrite(pinNumber, pinValue);
            break;
        default :
            error = true;
            break;
      } 
      
      if(!error){
        buffOut[0] = pinNumber;
        buffOut[1] = pinValue;
        buffOut[2] = calcCheckSum(buffOut,sizeOut-1);
        delay(100);
        Serial.write(buffOut, sizeOut); 
      }
    } 
  }
}

byte calcCheckSum(byte *buff, size_t sz){
  if (sz<=0 || buff==NULL) return 0;
  byte check = buff[0];
  for(int i=1; i<sz; i++){
    check = check ^ buff[i];
  } 
  return check;
}

/*
 * SimpleSerialProtocol - Arduino slave.
 *
 *   request  (7 bytes): [Command][PinNumber][ValueHigh][ValueLow][FreqHigh][FreqLow][CheckSum]
 *   response (6 bytes): [PinNumber][ValueHigh][ValueLow][FreqHigh][FreqLow][CheckSum]
 *
 * Both fields are 16 bit: a PWM frequency in Hz does not fit in a byte, and
 * the analog read needs 10. The frequency only means anything to command 'w',
 * which uses it to pick between hardware and software PWM; every response
 * reports the frequency the pin ended up on, 0 meaning plain analogWrite.
 *
 * PWM is generated in software (micros() + digitalWrite) instead of through
 * the timer registers, so the sketch stays portable across cores and works on
 * any digital pin, not only the ones wired to a timer. The price is that
 * loop() must never block: see serialService().
 */

#define REQ_SIZE 7
#define RES_SIZE 6

/* Frequency value that leaves the pin's current frequency alone - this is what
   the Master sends when the user omits it. Deliberately outside the valid
   range, so it cannot collide with a real request. */
#define PWM_FREQ_UNCHANGED 0xFFFF

/* A partially received frame is dropped after this idle time, so a lost byte
   costs one frame instead of desynchronising the stream forever. */
#define RX_FRAME_TIMEOUT_MS 50

/* Software PWM range. The edges land on loop() iterations, so the duty
   resolution is (loop period / PWM period): fine below 500 Hz, around 2% at
   the top of the range, useless above it. */
#define PWM_MIN_FREQ_HZ 1
#define PWM_MAX_FREQ_HZ 2000

/* Every active channel is serviced on every loop() iteration, so more
   channels means coarser edges for all of them. */
#define PWM_MAX_CHANNELS 6

#ifndef NUM_DIGITAL_PINS
#define NUM_DIGITAL_PINS 20
#endif

struct PwmChannel {
  uint8_t  pin;
  uint16_t freqHz;        /* 0 marks the slot as free */
  uint8_t  duty;
  uint32_t periodUs;
  uint32_t highUs;
  uint32_t periodStartUs;
  bool     level;
};

PwmChannel pwmChannels[PWM_MAX_CHANNELS];

byte buffOut[RES_SIZE];
byte buffIn[REQ_SIZE];
size_t buffInLen = 0;
uint32_t lastByteMs = 0;

void setup() {
  Serial.begin(9600);
  for (uint8_t i = 0; i < PWM_MAX_CHANNELS; i++) {
    pwmChannels[i].freqHz = 0;
  }
}

void loop() {
  pwmService();
  serialService();
}

/* ------------------------------------------------------------- software PWM */

/* Channel index for a pin, or -1 if the pin has no active channel.
   Indexes rather than PwmChannel* on purpose: the Arduino prototype generator
   inserts the auto-generated prototypes above the struct definition, so a
   custom type in a signature would not compile. */
int8_t pwmFind(byte pin) {
  for (uint8_t i = 0; i < PWM_MAX_CHANNELS; i++) {
    if (pwmChannels[i].freqHz != 0 && pwmChannels[i].pin == pin) return (int8_t) i;
  }
  return -1;
}

void pwmRecompute(int8_t idx) {
  /* periodUs is at most 1000000 (1 Hz) and duty at most 255, so the product
     stays inside 32 bit. */
  pwmChannels[idx].periodUs = 1000000UL / pwmChannels[idx].freqHz;
  pwmChannels[idx].highUs = (pwmChannels[idx].periodUs * (uint32_t) pwmChannels[idx].duty) / 255UL;
}

void pwmRelease(byte pin) {
  int8_t idx = pwmFind(pin);
  if (idx < 0) return;
  pwmChannels[idx].freqHz = 0;
  digitalWrite(pin, LOW);
}

/* Returns the frequency actually applied, which the caller echoes back to the
   master. 0 means either "released" or "refused because every channel is
   busy" - in both cases the pin is not running a software PWM. */
uint16_t pwmSetFrequency(byte pin, uint16_t freqHz) {
  if (freqHz == 0) {
    pwmRelease(pin);
    return 0;
  }

  if (freqHz > PWM_MAX_FREQ_HZ) freqHz = PWM_MAX_FREQ_HZ;
  if (freqHz < PWM_MIN_FREQ_HZ) freqHz = PWM_MIN_FREQ_HZ;

  int8_t idx = pwmFind(pin);
  if (idx < 0) {
    for (uint8_t i = 0; i < PWM_MAX_CHANNELS; i++) {
      if (pwmChannels[i].freqHz == 0) { idx = (int8_t) i; break; }
    }
    if (idx < 0) return 0;

    pwmChannels[idx].pin = pin;
    pwmChannels[idx].duty = 0;
    pwmChannels[idx].level = false;
    pwmChannels[idx].periodStartUs = micros();
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);   /* also detaches any timer left on by analogWrite */
  }

  pwmChannels[idx].freqHz = freqHz;
  pwmRecompute(idx);
  return freqHz;
}

void pwmSetDuty(int8_t idx, byte duty) {
  pwmChannels[idx].duty = duty;
  pwmRecompute(idx);
}

void pwmService() {
  uint32_t now = micros();

  for (uint8_t i = 0; i < PWM_MAX_CHANNELS; i++) {
    if (pwmChannels[i].freqHz == 0) continue;

    /* Unsigned subtraction, so the micros() rollover needs no special case. */
    uint32_t elapsed = now - pwmChannels[i].periodStartUs;

    if (elapsed >= pwmChannels[i].periodUs) {
      /* Restart from now instead of advancing by periodUs: if loop() was held
         up, advancing would queue a burst of catch-up periods. Stretching one
         period is the lesser evil. */
      pwmChannels[i].periodStartUs = now;
      elapsed = 0;
    }

    /* Covers both ends: duty 0 gives highUs 0 (never high), duty 255 gives
       highUs == periodUs (always high). */
    bool wantHigh = (elapsed < pwmChannels[i].highUs);
    if (wantHigh != pwmChannels[i].level) {
      digitalWrite(pwmChannels[i].pin, wantHigh ? HIGH : LOW);
      pwmChannels[i].level = wantHigh;
    }
  }
}

/* ------------------------------------------------------------------ serial */

void serialService() {
  if (buffInLen > 0 && (millis() - lastByteMs) > RX_FRAME_TIMEOUT_MS) buffInLen = 0;

  while (Serial.available() > 0) {
    buffIn[buffInLen++] = (byte) Serial.read();
    lastByteMs = millis();

    if (buffInLen == REQ_SIZE) {
      buffInLen = 0;
      if (buffIn[REQ_SIZE-1] == calcCheckSum(buffIn, REQ_SIZE-1)) handleRequest();
    }

    pwmService();   /* a long burst must not starve the PWM */
  }
}

void handleRequest() {
  byte command = buffIn[0];
  byte pinNumber = buffIn[1];
  uint16_t value = ((uint16_t) buffIn[2] << 8) | (uint16_t) buffIn[3];
  uint16_t freqHz = ((uint16_t) buffIn[4] << 8) | (uint16_t) buffIn[5];
  bool error = false;

  if (pinNumber >= NUM_DIGITAL_PINS) return;

  switch (command) {
    case 'R':
        pwmRelease(pinNumber);
        pinMode(pinNumber, INPUT);
        value = digitalRead(pinNumber);
        break;
    case 'W':
        pwmRelease(pinNumber);
        pinMode(pinNumber, OUTPUT);
        if (value==0) digitalWrite(pinNumber, LOW);
        else          digitalWrite(pinNumber, HIGH);
        break;
    case 'r':
        pwmRelease(pinNumber);
        pinMode(pinNumber, INPUT);
        value = analogRead(pinNumber);
        break;
    case 'w': {
        if (value > 255) value = 255;
        /* Settle which generator owns the pin first, so the duty below lands
           on the right one whatever was asked for. */
        if (freqHz != PWM_FREQ_UNCHANGED) pwmSetFrequency(pinNumber, freqHz);
        int8_t idx = pwmFind(pinNumber);
        if (idx >= 0) {
          pwmSetDuty(idx, (byte) value);
        } else {
          pinMode(pinNumber, OUTPUT);
          analogWrite(pinNumber, (byte) value);
        }
        break;
    }
    default :
        error = true;
        break;
  }

  if(!error){
    /* Read the frequency back instead of echoing it: the answer is the state
       the pin is really in, after any clamping or refusal. */
    int8_t idx = pwmFind(pinNumber);
    uint16_t appliedFreq = (idx >= 0) ? pwmChannels[idx].freqHz : 0;

    buffOut[0] = pinNumber;
    buffOut[1] = (byte)(value >> 8);
    buffOut[2] = (byte)(value & 0xFF);
    buffOut[3] = (byte)(appliedFreq >> 8);
    buffOut[4] = (byte)(appliedFreq & 0xFF);
    buffOut[5] = calcCheckSum(buffOut,RES_SIZE-1);
    Serial.write(buffOut, RES_SIZE);
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

#include <Arduino.h>

#define DOOR_PIN 0
extern bool button_pressed = false;

void Door_btn_Init() {
    pinMode(DOOR_PIN, INPUT_PULLUP);
}

void Door_btn_Update() {
    button_pressed = (digitalRead(DOOR_PIN) == LOW);
}



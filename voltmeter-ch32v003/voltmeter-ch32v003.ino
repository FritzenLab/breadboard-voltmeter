#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>


U8G2_SSD1306_64X32_1F_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE); // Configure our 64x32 pixels display

int distancemm; // varible for the distance from the VL53L0x sensor
long oldtime; // variable for keeping track of time

void setup() {
  u8g2.begin();
  
  Serial.begin(115200);

}

// fonts https://github.com/olikraus/u8g2/wiki/fntlistall#4-pixel-height (I have picked two of them for this article)

void loop()
{
  
  if(millis() - oldtime > 100){ // Only does anything every 100ms
    oldtime= millis();

    u8g2.clearBuffer();          // clear the internal memory  
    u8g2.setCursor(0, 24);
    u8g2.setFont(u8g2_font_9x18_mf);
    u8g2.print("test 1"); 
    u8g2.sendBuffer();          // transfer internal memory to the display 
  }
     
}

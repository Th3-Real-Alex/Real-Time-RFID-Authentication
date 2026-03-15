
#include <SPI.h>
#include <MFRC522.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecureBearSSL.h> // Required for secure (HTTPS) connection to Google
#include <Servo.h> 
#include <WiFiUdp.h>
#include <NTPClient.h>

// --- Hardware Pin Definitions ---
#define RST_PIN D2    // Reset pin for RFID
#define SS_PIN D4     // Slave Select pin for SPI communication
#define BUZZER D8     // Active Buzzer for audio feedback
#define SERVO_PIN D1  // Servo motor control pin

// --- Network Credentials ---
#define WIFI_SSID "Barlog"  
#define WIFI_PASSWORD "Barlogu101" 

// --- Time Settings ---
// Offset for Eastern European Time (EET): UTC + 2 hours = 7200 seconds
const long utcOffsetInSeconds = 7200; 

// Initialize NTP Client to get time from the internet
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

// Initialize RFID and Servo objects
MFRC522 mfrc522(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key; 
MFRC522::StatusCode status;      
Servo myservo; 

// RFID Memory Block Configuration
// We are reading the User Name stored in Block 2 of the card
int blockNum = 2; 
byte bufferLen = 18;
byte readBlockData[18]; // Buffer to store data read from the card

// Google Apps Script Web App URL (for logging data)
const String sheet_url = "https://script.google.com/macros/s/AKfycbx5AoK0nUO4Hl1oC-72DRaM7IjwO7O1MuxojJZpNuLdddfSmVT6geBGqRU62YBATaLCkA/exec?name="; 

void setup()
{
  // 1. Initialize Servo and lock the door initially
  myservo.attach(SERVO_PIN); 
  myservo.write(0); // 0 degrees = Locked

  // 2. Connect to WiFi
  // The system must hang here until connected because it relies on the internet
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED){
    delay(500); // Wait 500ms before checking again
  }
  
  // 3. Start the Time Client (NTP)
  timeClient.begin();

  // 4. Initialize Peripherals
  pinMode(BUZZER, OUTPUT);
  SPI.begin();        // Init SPI bus
  mfrc522.PCD_Init(); // Init RFID reader
}

void loop()
{
  // Updates the time from the server every loop iteration
  timeClient.update();

  // --- RFID Detection ---
  // If no new card is present, exit the loop function and try again
  if ( ! mfrc522.PICC_IsNewCardPresent()) { delay(500); return; }
  // If we can't read the serial data, exit
  if ( ! mfrc522.PICC_ReadCardSerial()) {return;}
  
  // Read the custom name stored in Block 2
  ReadDataFromBlock(blockNum, readBlockData);
  
  // --- Data Processing ---
  // Convert the byte array from the card into a readable String
  String rfid_name = "";
  for (int j = 0; j < 16; j++) {
      if (readBlockData[j] != 0x00) { // Ignore empty bytes
        rfid_name += (char)readBlockData[j];
      }
  }
  rfid_name.trim(); // Remove any extra whitespace
  
  // Audio feedback: Short beep to acknowledge scan
  digitalWrite(BUZZER, HIGH); delay(100); digitalWrite(BUZZER, LOW);
  
  // --- Access Control Logic ---
  bool openDoor = false;
  int currentHour = timeClient.getHours(); // Get current hour (0-23)
  
  // Case 1: Administrator
  if (rfid_name == "Boss") {
      openDoor = true; // Unlimited access
  }
  // Case 2: Restricted Worker
  else if (rfid_name == "Worker1") {
      // Time Logic: Access allowed only between 17:00 and 19:00 (5 PM - 7 PM)
      if (currentHour >= 17 && currentHour < 19) { 
          openDoor = true;
      } else {
          openDoor = false; // Outside of shift hours
      }
  }
  // Case 3: Unknown or unauthorized card
  else {
      openDoor = false;
  }

  // --- Actuation ---
  if (openDoor) {
      // Unlock sequence
      myservo.write(180);  // Open door
      digitalWrite(BUZZER, HIGH); delay(100); digitalWrite(BUZZER, LOW); // Success beep
      delay(3000);         // Keep door open for 3 seconds
      myservo.write(0);    // Lock door again
  } else {
      // Access Denied sequence: Long beep
      digitalWrite(BUZZER, HIGH); delay(800); digitalWrite(BUZZER, LOW);
  }

  // --- Cloud Logging ---
  // Prepare status string based on the decision
  String statusToSend = openDoor ? "Granted" : "Denied";
  
  // Send the data (Name + Status) to Google Sheets
  SendToGoogleSheets(rfid_name, statusToSend);
  
  // Stop encryption on the card and wait before next scan
  delay(2000);
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

// Helper Function: Reads data from a specific block on the RFID card
void ReadDataFromBlock(int blockNum, byte readBlockData[]) 
{ 
  // Prepare the default key (0xFFFFFFFFFFFF)
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;
  
  // Authenticate using Key A
  status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockNum, &key, &(mfrc522.uid));
  if (status != MFRC522::STATUS_OK) return; // Auth failed
  
  // Read the data
  status = mfrc522.MIFARE_Read(blockNum, readBlockData, &bufferLen);
}

// Helper Function: Sends an HTTPS GET request to Google Scripts
void SendToGoogleSheets(String name, String status) {
  if (WiFi.status() == WL_CONNECTED) {
    // Use WiFiClientSecure for HTTPS
    std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);
    
    // IMPORTANT: Skip SSL certificate validation. 
    // This is less secure but necessary for simple ESP8266 projects 
    // to avoid managing complex certificates.
    client->setInsecure(); 
    
    // Construct the full URL with parameters
    String url = sheet_url + name + "&sts=" + status;
    
    HTTPClient https;
    if (https.begin(*client, url)){
      // Send the GET request to trigger the script
      https.GET();
      https.end(); // Close connection
    }
  }
}
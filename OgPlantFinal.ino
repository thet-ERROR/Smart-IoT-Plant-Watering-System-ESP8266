#include "arduino_secrets.h"
/*
 * Smart Plant Care System IoT
 * Κώδικας για NodeMCU (ESP8266) + Arduino IoT Cloud
 * Σκοπός: Έλεγχος υγρασίας και αυτόματο πότισμα με προστασία
 */

#include "thingProperties.h"  // Βιβλιοθήκη για τη σύνδεση με το Arduino Cloud
#include <DHT.h>              // Βιβλιοθήκη για τον αισθητήρα θερμοκρασίας/υγρασίας

// --- ΡΥΘΜΙΣΕΙΣ ΧΡΗΣΤΗ (Configuration) ---
const int DRY_LIMIT = 40;       // Όριο Υγρασίας: Αν πέσει κάτω από 40%, χρειάζεται πότισμα
const long PUMP_TIME = 1000;    // Διάρκεια Ποτίσματος: Η αντλία ανοίγει για 1 δευτερόλεπτο
const long SOAK_TIME = 20000;   // Χρόνος Απορρόφησης: Περιμένουμε 20 δευτ. πριν ξαναμετρήσουμε
const int MAX_ATTEMPTS = 4;     // Μέγιστες προσπάθειες: Αν ποτίσει 4 φορές χωρίς αποτέλεσμα, σταματάει (Error)

// --- ΟΡΙΑ ΘΕΡΜΟΚΡΑΣΙΑΣ (Προστασία Φυτού) ---
const float MIN_TEMP = 5.0;     // Κάτω όριο: Αν έχει κάτω από 5°C, ΔΕΝ ποτίζουμε (κίνδυνος παγετού)
const float MAX_TEMP = 35.0;    // Άνω όριο: Αν έχει πάνω από 35°C, ΔΕΝ ποτίζουμε (θερμικό σοκ)

// --- ΟΡΙΣΜΟΣ PINS (Συνδεσμολογία) ---
const int soilPin = A0;         // Ο αισθητήρας υγρασίας εδάφους στο Αναλογικό Pin A0
const int pumpPin = 5;          // Η αντλία (μέσω Ρελέ) στο Pin D1 (GPIO 5)
const int dhtPin = 4;           // Ο αισθητήρας αέρα DHT11 στο Pin D2 (GPIO 4)

#define DHTTYPE DHT11           // Ορίζουμε τον τύπο του αισθητήρα ως DHT11
DHT dht(dhtPin, DHTTYPE);       // Δημιουργία αντικειμένου για τον αισθητήρα

// --- ΜΕΤΑΒΛΗΤΕΣ ΧΡΟΝΟΥ ---
unsigned long lastMeasureTime = 0;   // Πότε μετρήσαμε τους αισθητήρες τελευταία φορά
const long measureInterval = 2000;   // Κάθε πότε μετράμε (2000ms = 2 δευτερόλεπτα)

// --- ΜΕΤΑΒΛΗΤΕΣ ΛΕΙΤΟΥΡΓΙΑΣ ---
unsigned long pumpStartTime = 0;         // Πότε άνοιξε η αντλία (για να την κλείσουμε στην ώρα της)
unsigned long lastWateringFinishTime = 0;// Πότε τελείωσε το προηγούμενο πότισμα (για το Soak Time)
int wateringAttempts = 0;                // Μετρητής: Πόσες φορές ποτίσαμε σερί
bool tankEmptyError = false;             // "Σημαία" σφάλματος: True αν άδειασε το νερό

// --- ΡΥΘΜΙΣΕΙΣ ΕΚΚΙΝΗΣΗΣ (Setup) ---
void setup() {
  Serial.begin(9600);           // Έναρξη σειριακής επικοινωνίας για debugging
  delay(1500);                  // Μικρή καθυστέρηση για σταθεροποίηση

  pinMode(pumpPin, OUTPUT);     // Ορίζουμε το Pin της αντλίας ως ΕΞΟΔΟ
  digitalWrite(pumpPin, LOW);   // Σιγουρεύουμε ότι η αντλία είναι κλειστή στην αρχή
  
  pinMode(dhtPin, INPUT);       // Ορίζουμε το Pin του DHT ως ΕΙΣΟΔΟ
  dht.begin();                  // Εκκίνηση του αισθητήρα DHT

  initProperties();             // Αρχικοποίηση μεταβλητών Arduino Cloud
  ArduinoCloud.begin(ArduinoIoTPreferredConnection); // Σύνδεση στο Cloud
  setDebugMessageLevel(2);      // Εμφάνιση μηνυμάτων λάθους (για έλεγχο)
  ArduinoCloud.printDebugInfo();
}

// --- ΚΥΡΙΟΣ ΒΡΟΧΟΣ (Loop) ---
void loop() {
  ArduinoCloud.update();        // Συγχρονισμός δεδομένων με το Cloud (στείλε/πάρε εντολές)

  // Έλεγχος αν πέρασαν 2 δευτερόλεπτα για να μετρήσουμε
  if (millis() - lastMeasureTime > measureInterval) {
    lastMeasureTime = millis(); // Ανανέωση χρόνου τελευταίας μέτρησης
    readSensors();              // Κάλεσε τη συνάρτηση που διαβάζει αισθητήρες
    logicEngine();              // Κάλεσε τη συνάρτηση που παίρνει αποφάσεις ("Ο Εγκέφαλος")
  }
}

// --- ΣΥΝΑΡΤΗΣΗ 1: ΑΝΑΓΝΩΣΗ ΑΙΣΘΗΤΗΡΩΝ ---
void readSensors() {
  // 1. Διάβασμα Υγρασίας Εδάφους
  int rawValue = analogRead(soilPin); // Παίρνουμε την "ωμή" τιμή (0-1024)
  // Μετατροπή της τιμής σε ποσοστό % (800=Στεγνό, 280=Βρεγμένο)
  int percent = map(rawValue, 800, 280, 0, 100); 
  percent = constrain(percent, 0, 100); // Περιορισμός ώστε να μην βγάζει αρνητικά ή >100
  soil_moisture = percent;              // Ενημέρωση της Cloud μεταβλητής

  // 2. Διάβασμα Θερμοκρασίας & Υγρασίας Αέρα
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  
  // Έλεγχος: Αν οι τιμές είναι έγκυρες (όχι "NaN" - Not a Number)
  if (!isnan(h) && !isnan(t)) {
    temperature = t;  // Ενημέρωση Cloud μεταβλητής
    humidity = h;     // Ενημέρωση Cloud μεταβλητής
  }
}

// --- ΣΥΝΑΡΤΗΣΗ 2: Η ΛΟΓΙΚΗ ΤΟΥ ΑΥΤΟΜΑΤΙΣΜΟΥ (Logic Engine) ---
void logicEngine() {
  
  // 1. Έλεγχος Σφαλμάτων (Safety First)
  // Αν έχουμε ανιχνεύσει άδεια δεξαμενή, σταματάμε τα πάντα
  if (tankEmptyError == true) {
    plant_status = "SOS: Άδειασε το νερό! Γέμισέ με και κάνε Restart.";
    digitalWrite(pumpPin, LOW); // Κλείσιμο αντλίας για ασφάλεια
    pump_switch = false;        // Ενημέρωση διακόπτη στο Cloud
    return;                     // Βγες από τη συνάρτηση, μην κάνεις τίποτα άλλο
  }

  // 2. Επαναφορά Μετρητή (Reset Logic)
  // Αν η υγρασία ανέβηκε (άρα ποτίστηκε), μηδένισε τις αποτυχημένες προσπάθειες
  if (soil_moisture > DRY_LIMIT + 5) {
    wateringAttempts = 0;
    // Ενημέρωση μηνύματος "Όλα καλά" (μόνο αν δεν ποτίζει τώρα)
    if (pump_switch == false) plant_status = "OK. Υγρασία & Θερμοκρασία καλές.";
  }

  // 3. AUTO MODE (Αυτόματη Λειτουργία)
  if (auto_mode == true) {
    
    // --- ΠΡΟΣΤΑΣΙΑ ΘΕΡΜΟΚΡΑΣΙΑΣ ---
    // Έλεγχος για πολύ κρύο
    if (temperature < MIN_TEMP) {
       plant_status = "ΑΠΑΓΟΡΕΥΤΙΚΟ: Πολύ κρύο για πότισμα (<" + String(MIN_TEMP, 1) + "°C)";
       digitalWrite(pumpPin, LOW); 
       return; // Ακύρωση ποτίσματος
    }
    
    // Έλεγχος για καύσωνα
    if (temperature > MAX_TEMP) {
       plant_status = "ΑΠΑΓΟΡΕΥΤΙΚΟ: Καύσωνας! Περιμένω να πέσει η ζέστη.";
       digitalWrite(pumpPin, LOW);
       return; // Ακύρωση ποτίσματος
    }

    // --- ΑΠΟΦΑΣΗ ΠΟΤΙΣΜΑΤΟΣ ---
    // Αν η αντλία είναι κλειστή ΚΑΙ το χώμα είναι στεγνό
    if (pump_switch == false && soil_moisture < DRY_LIMIT) {
       
       // Έλεγχος αν πέρασε ο χρόνος αναμονής (Soak Time) από το προηγούμενο πότισμα
       if (millis() - lastWateringFinishTime > SOAK_TIME) {
          
          // Έλεγχος: Μήπως προσπαθούμε πολλές φορές χωρίς αποτέλεσμα;
          if (wateringAttempts >= MAX_ATTEMPTS) {
            tankEmptyError = true;           // Ενεργοποίηση Συναγερμού
            Serial.println("ALARM: Tank Empty");
          } else {
            // ΕΝΑΡΞΗ ΠΟΤΙΣΜΑΤΟΣ
            pump_switch = true;              // Ενημέρωση Cloud
            digitalWrite(pumpPin, HIGH);     // Άνοιγμα Ρελέ/Αντλίας
            pumpStartTime = millis();        // Καταγραφή ώρας έναρξης
            wateringAttempts++;              // Αύξηση μετρητή προσπαθειών
            
            // Στείλε μήνυμα στο Dashboard
            plant_status = "Ποτίζω... (" + String(wateringAttempts) + "/" + String(MAX_ATTEMPTS) + ")";
          }

       } else {
          // Αν είμαστε σε χρόνο αναμονής
          plant_status = "Περιμένω να απορροφηθεί το νερό...";
       }
    }

    // --- ΤΕΡΜΑΤΙΣΜΟΣ ΠΟΤΙΣΜΑΤΟΣ (Timer) ---
    // Αν η αντλία είναι ανοιχτή ΚΑΙ πέρασε ο χρόνος (1 δευτερόλεπτο)
    if (pump_switch == true && (millis() - pumpStartTime > PUMP_TIME)) {
      pump_switch = false;             // Ενημέρωση Cloud
      digitalWrite(pumpPin, LOW);      // Κλείσιμο Αντλίας
      lastWateringFinishTime = millis(); // Καταγραφή ώρας λήξης
      Serial.println("Watering cycle finished.");
    }
  } else {
    // Αν το Auto Mode είναι κλειστό (Manual)
    if (tankEmptyError == false) plant_status = "Manual Mode / Αναμονή";
  }
}

// --- CALLBACK: ΟΤΑΝ ΑΛΛΑΖΕΙ Ο ΔΙΑΚΟΠΤΗΣ ΣΤΟ CLOUD ---
void onPumpSwitchChange()  {
  // Επιτρέπουμε χειροκίνητο έλεγχο ΜΟΝΟ αν δεν είμαστε σε Auto Mode και δεν υπάρχει λάθος
  if (auto_mode == false && tankEmptyError == false) { 
    if (pump_switch == true) {
      digitalWrite(pumpPin, HIGH);      // Άναψε αντλία
      plant_status = "Χειροκίνητο Πότισμα ON";
    } else {
      digitalWrite(pumpPin, LOW);       // Σβήσε αντλία
      plant_status = "Χειροκίνητο Πότισμα OFF";
    }
  } else {
    // Αν πατήθηκε κατά λάθος ενώ είναι Auto, το αγνοούμε (ή το διορθώνουμε)
    if (pump_switch != (digitalRead(pumpPin) == HIGH)) {
       // Εδώ δεν κάνουμε τίποτα για να μην μπερδέψουμε τον αυτοματισμό
    }
  }
}

// --- CALLBACK: ΟΤΑΝ ΑΛΛΑΖΕΙ ΤΟ AUTO MODE ---
void onAutoModeChange()  {
  if (auto_mode == true) {
    tankEmptyError = false; // Reset τυχόν σφαλμάτων κατά την ενεργοποίηση
    wateringAttempts = 0;   // Μηδενισμός μετρητή
    plant_status = "Auto Mode: ΕΝΕΡΓΟ";
  } else {
    digitalWrite(pumpPin, LOW); // Σβήσε την αντλία αν το κλείσουμε
    pump_switch = false;
    plant_status = "Auto Mode: ΚΛΕΙΣΤΟ";
  }
}

// Συναρτήσεις που δημιουργεί αυτόματα το Cloud (δεν χρειάζονται κώδικα εδώ)
void onSoilMoistureChange() {}
void onTemperatureChange() {}
void onHumidityChange() {}
void onPlantStatusChange() {}
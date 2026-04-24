/*
 * UWB_MulticastTag.ino
 *
 * Platform : Arduino Stella (Truesense DCU040 / nRF52840)
 * Library  : StellaUWB
 *            https://github.com/Truesense-it/StellaUWB
 *
 * Role     : Multicast Responder / Controlee — this tag joins four
 *            multicast sessions (one per anchor) to get TWR distance
 *            measurements from all anchors simultaneously.
 *
 * ── CONFIGURATION ──────────────────────────────────────────────────
 *   Change ONLY the TAG_ID below (1 – 12).
 *   Everything else — MAC address, group assignment, session IDs,
 *   anchor addresses — is derived automatically.
 *
 * ── ADDRESSING SCHEME ──────────────────────────────────────────────
 *   Tag    MAC      : { 0x00, TAG_ID }
 *   Anchor MAC      : { 0xA0, ANCHOR_ID }    ANCHOR_ID ∈ [0..3]
 *   Group assignment : group = (TAG_ID - 1) / TAGS_PER_GROUP
 *     Tags  1– 6 → Group 0
 *     Tags  7–12 → Group 1
 *   Session ID      : ANCHOR_ID * 100 + GROUP_INDEX + 1
 *
 *   Example for TAG_ID = 9 (Group 1):
 *     Tag MAC   = {0x00, 0x09}
 *     Joins session 2   with Anchor 0  (MAC {0xA0, 0x00})
 *     Joins session 102 with Anchor 1  (MAC {0xA0, 0x01})
 *     Joins session 202 with Anchor 2  (MAC {0xA0, 0x02})
 *     Joins session 302 with Anchor 3  (MAC {0xA0, 0x03})
 *
 * ── SESSION BUDGET ─────────────────────────────────────────────────
 *   4 sessions per tag     (limit: 5 on DCU040)  ✔
 *   2 sessions per anchor  (limit: 5 on SR150)   ✔
 * ───────────────────────────────────────────────────────────────────
 */

#include "StellaUWB.h"

// ===================== CHANGE THIS ================================
#define TAG_ID          2        // 1 through 12
// ==================================================================

#define NUM_ANCHORS     4
#define TAGS_PER_GROUP  6

// Derived constants
#define GROUP_INDEX     (((TAG_ID) - 1) / TAGS_PER_GROUP)   // 0 or 1

// File-static, heap-allocated session objects — survive beyond setup()
static MulticastResponder* sessions[NUM_ANCHORS] = { nullptr };

// Per-anchor range storage for serial output
struct AnchorRange {
  float    distance_cm;
  bool     valid;
  uint32_t timestamp;
};

AnchorRange ranges[NUM_ANCHORS];

// ---------- Ranging callback -------------------------------------
//
// Fired by the UWB stack when a multicast ranging round produces a
// result.  We use the session handle to figure out which anchor
// the measurement came from.
//
void rangingHandler(UWBRangingData &rangingData) {
  if (rangingData.measureType() != (uint8_t)uwb::MeasurementType::TWO_WAY)
    return;

  uint32_t sessionHandle = rangingData.sessionHandle();

  // Reverse the session-ID formula to get anchor index
  // sessionId = anchorId * 100 + groupIndex + 1
  int anchorId = (int)(sessionHandle / 100);

  // Bounds check
  if (anchorId < 0 || anchorId >= NUM_ANCHORS)
    return;

  RangingMeasures twr = rangingData.twoWayRangingMeasure();

  for (int j = 0; j < rangingData.available(); j++) {
    if (twr[j].status == 0 && twr[j].distance != 0xFFFF) {
      ranges[anchorId].distance_cm = (float)twr[j].distance;
      ranges[anchorId].valid       = true;
      ranges[anchorId].timestamp   = millis();
    }
  }
}

// ---------- Setup ------------------------------------------------
void setup() {
  Serial.begin(115200);

  // ── Tag MAC address ──
  uint8_t tagAddrBytes[] = { 0x00, (uint8_t)TAG_ID };
  UWBMacAddress tagMac(UWBMacAddress::Size::SHORT, tagAddrBytes);

  // Register callback before starting the stack
  UWB.registerRangingCallback(rangingHandler);

  UWB.begin();
  delay(1000);
  Serial.println("=========================================");
  Serial.print("  Multicast Tag T");
  Serial.print(TAG_ID);
  Serial.print("  MAC {0x00, 0x");
  if (TAG_ID < 0x10) Serial.print("0");
  Serial.print(TAG_ID, HEX);
  Serial.print("}  Group ");
  Serial.println(GROUP_INDEX);
  Serial.println("=========================================");
  Serial.println("Waiting for UWB stack ...");

  while (UWB.state() != 0)
    delay(10);

  Serial.println("UWB stack ready.");

  // Initialize range storage
  for (int a = 0; a < NUM_ANCHORS; a++) {
    ranges[a].distance_cm = 0;
    ranges[a].valid       = false;
    ranges[a].timestamp   = 0;
  }

  // ── Join one multicast session per anchor ──
  for (int a = 0; a < NUM_ANCHORS; a++) {

    // Anchor MAC for this session
    uint8_t anchorAddrBytes[] = { 0xA0, (uint8_t)a };
    UWBMacAddress anchorMac(UWBMacAddress::Size::SHORT, anchorAddrBytes);

    // Session ID must match the anchor's formula exactly
    uint32_t sessionId = (uint32_t)(a * 100 + GROUP_INDEX + 1);

    Serial.print("  Joining session ");
    Serial.print(sessionId);
    Serial.print("  →  Anchor A");
    Serial.print(a);
    Serial.print("  (MAC {0xA0, 0x0");
    Serial.print(a);
    Serial.println("})");

    // Heap-allocate so the object outlives setup()
    sessions[a] = new MulticastResponder(sessionId, tagMac, anchorMac);

    UWBSessionManager.addSession(*sessions[a]);
    delay(100);
    sessions[a]->init();
    delay(100);
    sessions[a]->start();
    delay(100);
  }

  Serial.println("-----------------------------------------");
  Serial.println("All responder sessions active.");
  Serial.println("-----------------------------------------");
}

// ---------- Loop: periodic range report --------------------------
//
// Prints a compact line showing distances to all 4 anchors.
// Format: T1 | A0=123cm  A1=089cm  A2=---  A3=210cm  [3/4]
//
void loop() {
  // Invalidate stale readings (>500 ms old)
  for (int a = 0; a < NUM_ANCHORS; a++) {
    if (ranges[a].valid && (millis() - ranges[a].timestamp > 2500)) {
      ranges[a].valid = false;
    }
  }

  // Print range report
  Serial.print("T");
  Serial.print(TAG_ID);
  Serial.print(" | ");

  int validCount = 0;
  for (int a = 0; a < NUM_ANCHORS; a++) {
    Serial.print("A");
    Serial.print(a);
    Serial.print("=");
    if (ranges[a].valid) {
      Serial.print((int)ranges[a].distance_cm);
      Serial.print("cm");
      validCount++;
    } else {
      Serial.print("---");
    }
    if (a < NUM_ANCHORS - 1) Serial.print("  ");
  }

  Serial.print("  [");
  Serial.print(validCount);
  Serial.println("/4]");

  delay(10);
}

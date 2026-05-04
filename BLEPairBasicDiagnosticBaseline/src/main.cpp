// ─────────────────────────────────────────────────────────────────────────────
//  BLE HID Sniffer — NimBLE-Arduino
//  Scans for "Kobo Remote", connects, pairs/bonds, subscribes to all
//  notifiable characteristics and dumps everything to Serial.
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <algorithm>
#include <cctype>

// ── Configuration ─────────────────────────────────────────────────────────────
static const char*    TARGET_NAME         = "Kobo Remote";
static const uint32_t SERIAL_STABILIZE_MS = 12000;  // wait before first scan
static const uint32_t SCAN_DURATION_S     = 10;      // each scan window length
static const uint32_t SCAN_PAUSE_MS       = 3000;    // gap between scan cycles
static const uint32_t AUTH_TIMEOUT_MS     = 12000;   // wait for bond after connect
static const uint32_t RECONNECT_DELAY_MS  = 2000;    // pause before next scan after failure

static bool nameMatchesTarget(const std::string& rawName) {
    if (rawName.empty()) return false;

    // Strip control chars that can appear in advertisement payload strings.
    std::string cleaned;
    cleaned.reserve(rawName.size());
    for (unsigned char c : rawName) {
        if (c >= 32 && c <= 126) cleaned.push_back((char)c);
    }

    while (!cleaned.empty() && cleaned.front() == ' ') cleaned.erase(cleaned.begin());
    while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();

    std::string lhs = cleaned;
    std::string rhs = TARGET_NAME;
    std::transform(lhs.begin(), lhs.end(), lhs.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    std::transform(rhs.begin(), rhs.end(), rhs.begin(), [](unsigned char c){ return (char)std::tolower(c); });

    return lhs == rhs || lhs.find(rhs) != std::string::npos;
}

// ── State ─────────────────────────────────────────────────────────────────────
static NimBLEAddress  targetAddress;
static NimBLEClient*  pClient    = nullptr;
static bool           doConnect  = false;
static bool           connected  = false;

// ── Notification / Indication callback ───────────────────────────────────────
static void notifyCallback(NimBLERemoteCharacteristic* pChar,
                           uint8_t* pData, size_t length, bool isNotify)
{
    Serial.printf("[%s] handle=0x%04X  UUID=%-40s  len=%d  bytes:",
                  isNotify ? "NOTIFY " : "INDICATE",
                  pChar->getHandle(),
                  pChar->getUUID().toString().c_str(),
                  (int)length);
    for (size_t i = 0; i < length; i++) Serial.printf(" %02X", pData[i]);
    Serial.println();
}

// ── Client Callbacks ──────────────────────────────────────────────────────────
class ClientCB : public NimBLEClientCallbacks
{
    void onConnect(NimBLEClient* c) override {
        Serial.printf("[CLIENT]  Connected     addr=%s  connId=%d\n",
                      c->getPeerAddress().toString().c_str(),
                      c->getConnId());
    }

    void onDisconnect(NimBLEClient* c) override {
        connected = false;
        Serial.printf("[CLIENT]  Disconnected  addr=%s  — will re-scan\n",
                      c->getPeerAddress().toString().c_str());
    }

    bool onConnParamsUpdateRequest(NimBLEClient* c,
                                   const ble_gap_upd_params* p) override {
        Serial.printf("[CLIENT]  Conn param update request: "
                      "itvl_min=%u  itvl_max=%u  latency=%u  supervision=%u  — accepted\n",
                      p->itvl_min, p->itvl_max, p->latency, p->supervision_timeout);
        return true; // accept whatever the peripheral wants
    }

    // ── Security events ────────────────────────────────────────────────────────
    uint32_t onPassKeyRequest() override {
        Serial.println("[SECURITY]  PassKey requested by peer — returning 0");
        return 0;
    }

    bool onConfirmPIN(uint32_t pin) override {
        Serial.printf("[SECURITY]  Numeric comparison PIN: %06u — confirming YES\n", pin);
        return true;
    }

    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        Serial.println ("[SECURITY] ═══════════════ Authentication Complete ═══════════════");
        Serial.printf  ("  peer addr      : %s\n",
                        NimBLEAddress(desc->peer_id_addr).toString().c_str());
        Serial.printf  ("  bonded         : %s\n",
                        desc->sec_state.bonded        ? "YES" : "NO");
        Serial.printf  ("  authenticated  : %s\n",
                        desc->sec_state.authenticated ? "YES" : "NO");
        Serial.printf  ("  encrypted      : %s\n",
                        desc->sec_state.encrypted     ? "YES" : "NO");
        Serial.printf  ("  key size       : %u bytes\n",
                        desc->sec_state.key_size);
        Serial.println ("[SECURITY] ════════════════════════════════════════════════════════");

        if (!desc->sec_state.encrypted) {
            Serial.println("[SECURITY]  Encryption NOT established — aborting, will retry");
            NimBLEDevice::deleteClient(pClient);
            pClient = nullptr;
            return;
        }
        connected = true;
        Serial.println("[SECURITY]  Bond established — listening for HID events");
    }
};

static ClientCB clientCB;

// ── Scan Callbacks ─────────────────────────────────────────────────────────────
class ScanCB : public NimBLEAdvertisedDeviceCallbacks
{
    void onResult(NimBLEAdvertisedDevice* dev) override {
        // print every discovered device for full visibility
        std::string svcInfo = "";
        if (dev->haveServiceUUID()) {
            svcInfo = "  svc=" + dev->getServiceUUID().toString();
        }
        Serial.printf("[SCAN]  %s  RSSI=%-4d  addrType=%d  name=\"%s\"%s\n",
                      dev->getAddress().toString().c_str(),
                      dev->getRSSI(),
                      (int)dev->getAddress().getType(),
                      dev->haveName() ? dev->getName().c_str() : "",
                      svcInfo.c_str());

        bool isTarget = dev->haveName() && nameMatchesTarget(dev->getName());
        if (isTarget) {
            Serial.printf("[SCAN]  ─── TARGET FOUND: \"%s\" at %s ───\n",
                          TARGET_NAME,
                          dev->getAddress().toString().c_str());
            targetAddress = dev->getAddress();
            doConnect     = true;
            NimBLEDevice::getScan()->stop();
        }
    }
};

static ScanCB scanCB;

// ── Explore and subscribe all characteristics in a service ────────────────────
static void exploreService(NimBLERemoteService* svc)
{
    Serial.printf("  [SVC]  %s\n", svc->getUUID().toString().c_str());

    std::vector<NimBLERemoteCharacteristic*>* chars = svc->getCharacteristics(true);
    if (!chars) { Serial.println("    (no characteristics)"); return; }

    for (auto* ch : *chars) {
        Serial.printf("    [CHAR]  0x%04X  %-40s  caps=[%s%s%s%s]\n",
                      ch->getHandle(),
                      ch->getUUID().toString().c_str(),
                      ch->canRead() ? "READ " : "",
                      ch->canWrite() ? "WRITE " : "",
                      ch->canNotify() ? "NOTIFY " : "",
                      ch->canIndicate() ? "INDICATE " : "");

        // read current value if possible
        if (ch->canRead()) {
            NimBLEAttValue v = ch->readValue();
            if (v.size()) {
                Serial.printf("      [READ]  len=%d :", (int)v.size());
                for (auto b : v) Serial.printf(" %02X", b);
                Serial.println();
            }
        }

        // subscribe to notifications / indications
        if (ch->canNotify() || ch->canIndicate()) {
            bool ok = ch->subscribe(true, notifyCallback);
            Serial.printf("      [SUBSCRIBE]  %s\n", ok ? "OK" : "FAILED");
        }

        // dump descriptors
        std::vector<NimBLERemoteDescriptor*>* descs = ch->getDescriptors(true);
        if (descs) {
            for (auto* d : *descs) {
                NimBLEAttValue dv = d->readValue();
                Serial.printf("      [DESC]  0x%04X  %-36s  raw:",
                              d->getHandle(),
                              d->getUUID().toString().c_str());
                for (auto b : dv) Serial.printf(" %02X", b);
                Serial.println();
            }
        }
    }
}

// ── Connect, pair, and explore all services ───────────────────────────────────
static bool connectAndExplore()
{
    Serial.printf("\n[CONN]  Connecting to %s  (addr type %d) ...\n",
                  targetAddress.toString().c_str(),
                  (int)targetAddress.getType());

    if (!pClient) {
        pClient = NimBLEDevice::createClient(targetAddress);
        pClient->setClientCallbacks(&clientCB, /*deleteOnDisconnect=*/false);
        pClient->setConnectionParams(12, 12, 0, 200); // 15ms itvl, 2s timeout
        pClient->setConnectTimeout(10);               // 10 s connect timeout
    }

    if (!pClient->connect()) {
        Serial.println("[CONN]  connect() returned false — will rescan");
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
        return false;
    }

    Serial.printf("[CONN]  Link layer up  MTU=%d\n", pClient->getMTU());

    // ── service discovery ──────────────────────────────────────────────────────
    Serial.println("[CONN]  Discovering all services...");
    std::vector<NimBLERemoteService*>* services = pClient->getServices(true);

    if (!services || services->empty()) {
        Serial.println("[CONN]  Service discovery returned nothing");
        return false;
    }

    Serial.printf("[CONN]  %d service(s) found — exploring:\n", (int)services->size());
    for (auto* svc : *services) {
        exploreService(svc);
    }

    Serial.println("\n[CONN]  Exploration complete — waiting for security callbacks...");
    return true;
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    // ── wait for serial to stabilise before printing anything ──────────────────
    delay(SERIAL_STABILIZE_MS);

    Serial.println("\n\n╔══════════════════════════════════════════╗");
    Serial.println("║   BLE HID Sniffer  (NimBLE-Arduino)     ║");
    Serial.println("╚══════════════════════════════════════════╝");
    Serial.printf("  Target  : \"%s\"\n", TARGET_NAME);
    Serial.printf("  SDK ver : %s\n\n", ESP.getSdkVersion());

    NimBLEDevice::init(""); // empty name = no advertisement

    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // maximum TX power

    // Request larger local ATT MTU before connecting (client.setMTU is not in 1.4.x).
    bool mtuOk = NimBLEDevice::setMTU(247);
    Serial.printf("[INIT]  Local MTU request 247: %s\n", mtuOk ? "OK" : "FAILED");

    // ── security: bond with Secure Connections, no MITM (just-works) ──────────
    // Most BLE HID peripherals (remotes, keyboards) use just-works pairing.
    NimBLEDevice::setSecurityAuth(/*bonding*/true, /*MITM*/false, /*SC*/true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT); // just-works

    Serial.printf("[INIT]  NimBLE initialised\n");
    Serial.printf("[INIT]  Stored bonds : %d\n", NimBLEDevice::getNumBonds());
    for (int i = 0; i < NimBLEDevice::getNumBonds(); i++) {
        NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
        Serial.printf("[INIT]    bond[%d] = %s\n", i, a.toString().c_str());
    }
    Serial.println();
}

// ── loop ───────────────────────────────────────────────────────────────────────
void loop()
{
    // ── actively connected: idle, everything is callback-driven ───────────────
    if (connected) {
        delay(200);
        return;
    }

    // ── scan found the target: connect ────────────────────────────────────────
    if (doConnect) {
        doConnect = false;
        if (connectAndExplore()) {
            // wait for onAuthenticationComplete to set connected=true
            uint32_t t = millis();
            while (!connected && (millis() - t) < AUTH_TIMEOUT_MS) {
                delay(100);
            }
            if (!connected) {
                Serial.println("[CONN]  Auth timeout — restarting scan cycle");
            }
        }
        delay(RECONNECT_DELAY_MS);
        return;
    }

    // ── start a scan cycle ────────────────────────────────────────────────────
    Serial.printf("[SCAN]  ─── scan cycle  duration=%ds ───\n", SCAN_DURATION_S);
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(&scanCB, /*wantDuplicates=*/false);
    pScan->setActiveScan(true);   // send scan requests for complete adv data
    pScan->setInterval(45);
    pScan->setWindow(15);
    pScan->clearResults();
    pScan->start(SCAN_DURATION_S, /*blocking=*/false); // non-blocking start

    // poll until scan ends or target found
    while (pScan->isScanning() && !doConnect) {
        delay(100);
    }
    if (pScan->isScanning()) pScan->stop();

    if (!doConnect) {
        Serial.printf("[SCAN]  \"%s\" not found — pausing %ums before next cycle\n",
                      TARGET_NAME, SCAN_PAUSE_MS);
        delay(SCAN_PAUSE_MS);
    }
}

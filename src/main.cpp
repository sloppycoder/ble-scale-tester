// ble_scale_tester
//
// Drives esp-arduino-ble-scales the same way GaggiMate's BLEScalePlugin does
// (see ../gaggimate/src/display/plugins/BLEScalePlugin.cpp): register every
// scale plugin, scan, connect via RemoteScalesFactory, and wire the weight/log
// callbacks. Goal is to verify a scale driver (in particular a modified one,
// e.g. Timemore Basic 3.0 Link support) behaves correctly, using the exact
// same driver entry points GaggiMate calls, without needing GaggiMate's full
// display/controller firmware.
//
// Serial commands (type into the monitor, newline-terminated) reproduce the
// other ways GaggiMate exercises a connected scale:
//   tare   - double-tare, same as BLEScalePlugin::onProcessStart() (brew/grind start)
//   start  - scale->startTimer()  (BLEScalePlugin calls this on controller:grind:start etc.)
//   stop   - scale->stopTimer()   (BLEScalePlugin calls this on controller:brew:end)
//   reset  - scale->resetTimer()
//   status - snapshot of connection + optional fields, same fields BLEScalePlugin
//            polls in pollScaleMetadata() (battery, weight unit) plus flow/timer/RSSI
//   scan   - disconnect and restart scanning (BLEScalePlugin::scan())
//   help   - list commands
//
// If no weight has been received (no scale connected yet, or connected but
// silent), prints "waiting for scale..." every 3 seconds.

#include <Arduino.h>
#include <memory>

#include <remote_scales.h>
#include <remote_scales_plugin_registry.h>

#include <scales/acaia.h>
#include <scales/bookoo.h>
#include <scales/decent.h>
#include <scales/difluid.h>
#include <scales/dot.h>
#include <scales/eclair.h>
#include <scales/eureka.h>
#include <scales/felicitaScale.h>
#include <scales/myscale.h>
#include <scales/timemore.h>
#include <scales/varia.h>
#include <scales/weighmybru.h>

namespace {

constexpr unsigned long WAIT_MESSAGE_INTERVAL_MS = 3000;

RemoteScalesScanner *scanner = nullptr;
std::unique_ptr<RemoteScales> scale;

unsigned long lastWeightMs = 0;
unsigned long lastWaitMessageMs = 0;

void onScaleLog(std::string message) {
    if (!message.empty()) {
        Serial.print(message.c_str());
    }
}

void onWeightMeasurement(float value) {
    lastWeightMs = millis();
    Serial.printf("[weight] %.2f g\n", value);
}

void registerScalePlugins() {
    // Same set BLEScalePlugin::setup() registers.
    AcaiaScalesPlugin::apply();
    BookooScalesPlugin::apply();
    DecentScalesPlugin::apply();
    DifluidScalesPlugin::apply();
    EclairScalesPlugin::apply();
    EurekaScalesPlugin::apply();
    FelicitaScalePlugin::apply();
    TimemoreScalesPlugin::apply();
    TimemoreDotScalesPlugin::apply();
    VariaScalesPlugin::apply();
    WeighMyBrewScalePlugin::apply();
    myscalePlugin::apply();
}

void disconnectScale() {
    if (!scale) return;
    scale->disconnect();
    scale.reset();
    lastWeightMs = 0;
}

void tryConnectToDiscovered() {
    if (scanner == nullptr) return;
    auto discovered = scanner->getDiscoveredScales();
    if (discovered.empty()) return;

    const DiscoveredDevice &d = discovered.front();
    Serial.printf("[main] Found '%s' [%s], connecting...\n", d.getName().c_str(), d.getAddress().toString().c_str());
    scanner->stopAsyncScan();

    auto factory = RemoteScalesFactory::getInstance();
    scale = factory->create(d);
    if (!scale) {
        Serial.println("[main] No driver matched this device, resuming scan");
        scanner->initializeAsyncScan();
        return;
    }

    scale->setLogCallback(onScaleLog);
    scale->setWeightUpdatedCallback(onWeightMeasurement);

    if (!scale->connect()) {
        Serial.println("[main] connect() failed, resuming scan");
        scale.reset();
        scanner->initializeAsyncScan();
        return;
    }

    Serial.println("[main] Connected.");
}

// Mirrors BLEScalePlugin::onProcessStart() exactly: tare twice with a short
// delay, re-checking connectivity in between, since some drivers only zero
// reliably on the second command.
void doTare() {
    if (!scale || !scale->isConnected()) {
        Serial.println("[main] No connected scale to tare");
        return;
    }
    Serial.println("[main] tare() x2 (mirrors BLEScalePlugin::onProcessStart)");
    scale->tare();
    delay(50);
    if (scale && scale->isConnected()) {
        scale->tare();
    }
}

void doStartTimer() {
    if (!scale || !scale->isConnected()) {
        Serial.println("[main] No connected scale");
        return;
    }
    if (!scale->hasTimerControl()) {
        Serial.println("[main] Driver reports hasTimerControl()==false; sending anyway (no-op if unsupported)");
    }
    scale->startTimer();
    Serial.println("[main] startTimer() sent");
}

void doStopTimer() {
    if (!scale || !scale->isConnected()) {
        Serial.println("[main] No connected scale");
        return;
    }
    scale->stopTimer();
    Serial.println("[main] stopTimer() sent");
}

void doResetTimer() {
    if (!scale || !scale->isConnected()) {
        Serial.println("[main] No connected scale");
        return;
    }
    scale->resetTimer();
    Serial.println("[main] resetTimer() sent");
}

// Mirrors the fields BLEScalePlugin::pollScaleMetadata() reads, plus a few
// more (RSSI, flow rate, scale timer) useful while bench-testing a driver.
void printStatus() {
    if (!scale) {
        Serial.println("[status] no scale connected");
        return;
    }
    Serial.printf("[status] name=%s addr=%s connected=%d rssi=%d\n", scale->getDeviceName().c_str(),
                   scale->getDeviceAddress().c_str(), scale->isConnected(), scale->getRSSI());
    Serial.printf("[status] weight=%.2f g\n", scale->getWeight());

    if (scale->hasBatteryLevel()) {
        const uint8_t pct = scale->getBatteryLevel();
        if (pct == REMOTE_SCALES_BATTERY_UNKNOWN) {
            Serial.println("[status] battery=unknown");
        } else {
            Serial.printf("[status] battery=%u%%\n", pct);
        }
    } else {
        Serial.println("[status] battery: not supported by this driver");
    }

    if (scale->hasFlowRate()) {
        Serial.printf("[status] flow=%.2f g/s\n", scale->getFlowRate());
    }
    if (scale->hasWeightUnit()) {
        const char *unit = "unknown";
        switch (scale->getWeightUnit()) {
        case ScaleWeightUnit::GRAM: unit = "gram"; break;
        case ScaleWeightUnit::OUNCE: unit = "ounce"; break;
        default: break;
        }
        Serial.printf("[status] unit=%s\n", unit);
    }
    if (scale->hasScaleTimer()) {
        Serial.printf("[status] scaleTimerMs=%lu\n", static_cast<unsigned long>(scale->getScaleTimerMs()));
    }
    Serial.printf("[status] hasTimerControl=%d\n", scale->hasTimerControl());
}

void doRescan() {
    if (scale) {
        Serial.println("[main] Disconnecting current scale...");
        disconnectScale();
    }
    Serial.println("[main] Restarting scan...");
    if (scanner != nullptr) {
        scanner->initializeAsyncScan();
    }
}

void printHelp() {
    Serial.println("Commands: tare | start | stop | reset | status | scan | help");
}

void handleSerialCommands() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) return;

    if (line == "help") {
        printHelp();
    } else if (line == "tare") {
        doTare();
    } else if (line == "start") {
        doStartTimer();
    } else if (line == "stop") {
        doStopTimer();
    } else if (line == "reset") {
        doResetTimer();
    } else if (line == "status") {
        printStatus();
    } else if (line == "scan") {
        doRescan();
    } else {
        Serial.printf("[main] Unknown command '%s'. Type 'help'.\n", line.c_str());
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    Serial.println("=== ble_scale_tester ===");

    registerScalePlugins();
    scanner = new RemoteScalesScanner();
    scanner->initializeAsyncScan();

    printHelp();
    Serial.println("Scanning for scales...");
}

void loop() {
    handleSerialCommands();

    if (scale) {
        if (!scale->isConnected()) {
            Serial.println("[main] Scale disconnected, resuming scan");
            scale.reset();
            if (scanner != nullptr) {
                scanner->initializeAsyncScan();
            }
        } else {
            scale->update();
        }
    } else {
        tryConnectToDiscovered();
    }

    const unsigned long now = millis();
    if (now - lastWaitMessageMs >= WAIT_MESSAGE_INTERVAL_MS) {
        lastWaitMessageMs = now;
        if (lastWeightMs == 0) {
            Serial.println("waiting for scale...");
        }
    }

    delay(20);
}

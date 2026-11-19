#ifndef NET_H
#define NET_H

#include <Arduino.h>
#include <WiFi.h>

enum NetMode { NET_NONE, NET_STA, NET_AP };

// Unconfigured board: starts the setup AP "espcam-setup-XXXX" and stays there.
// Configured board: joins the Pi's AP with its static IP; if that fails within STA_TIMEOUT_MS it
// starts the fallback AP "espcam-<ID>-setup" instead. Blocks for up to STA_TIMEOUT_MS.
void netBegin();

// Call from loop(): drops to the fallback AP if the Pi's AP is lost for too long,
// and retries the Pi's AP while in fallback mode with no clients connected.
void netLoop();

NetMode netMode();
bool netIsSetupMode();  // true while running the factory setup AP (board not configured yet)
const char *netModeName();
IPAddress netIp();

// Low power = WiFi modem sleep (STA mode only). Used while the camera is disarmed.
void netSetLowPower(bool lowPower);

#endif  // NET_H

// config_test.cpp — host test for the Config seam (no board, plain g++).
//
// The seam's whole reason to exist is the distinction the F2 tickets depend on:
//   * a field that was NEVER SET must fall back to the Factory default
//   * a field explicitly SET TO EMPTY must NOT fall back
// Collapsing those two is the bug the provisioning feature has to avoid, so it is
// the first thing this test pins down.
//
// Build/run: pwsh -File tools/configtest/build.ps1
#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"

static int g_fail = 0;

static void check(bool ok, const char* what) {
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) g_fail++;
}

static void check_str(const char* got, const char* want, const char* what) {
  bool ok = got && want && strcmp(got, want) == 0;
  printf("  [%s] %s (got '%s', want '%s')\n", ok ? "PASS" : "FAIL", what,
         got ? got : "(null)", want ? want : "(null)");
  if (!ok) g_fail++;
}

// The Factory defaults the seam is built against in this test build.
// They come from the test's own config.cpp compile-time defines.
int main() {
  printf("\nConfig seam — unset vs empty\n");

  // ── 1. Before any runtime write, every field reports the Factory default. ──
  cfgReset();
  check_str(cfgWifiSsid(), "factory-ssid", "unset SSID falls back to the Factory default");
  check_str(cfgWifiPass(), "factory-pass", "unset password falls back to the Factory default");
  check_str(cfgServerHost(), "factory-host", "unset Server host falls back to the Factory default");
  check(cfgServerIpIsSet(), "unset Server IP reports the Factory default as set");
  check(cfgServerIpState() == CFG_IP_UNSET, "and reports it as UNSET, not SET");
  check_str(cfgWeatherLocation(), "factory-place", "unset weather location falls back");
  check(cfgServerPort() == 8266, "Server port comes from the Factory default");

  // ── 1b. A save must never FREEZE a Factory default into the store. ──
  // cfgSaveWifi() has always guarded this ("persist ONLY what was actually set"), but
  // cfgSaveServer() and cfgSaveWeather() did not, so calling either while a field was
  // still unset wrote the resolved Factory default into the store as if it were a
  // deliberate choice. The damage is invisible until the next boot, because setup()
  // calls cfgLoad(): the field then reads STORED and a later secrets.h edit is ignored.
  // Found by a code review of the provisioning form (ticket #7).
  printf("\nConfig seam — a save must not pin a Factory default\n");
  cfgReset(); cfgTestWipe();
  cfgSaveServer();
  cfgSaveWeather();
  cfgReset(); cfgLoad();
  check(!cfgServerHostIsStored(),
        "cfgSaveServer does not pin the hostname's Factory default");
  check(!cfgWeatherIsStored(),
        "cfgSaveWeather does not pin the weather Factory default");
  check_str(cfgServerHost(), "factory-host",
            "and the hostname still resolves to the Factory default after a reboot");

  // ── 2. THE DISTINCTION: an explicitly empty value must NOT fall back, and the
  //       seam must be able to TELL unset from empty (not just behave differently). ──
  printf("\nConfig seam — the distinction the feature depends on\n");
  cfgReset();
  cfgSetWifiSsid("my-net");
  cfgSetWifiPass("");                      // deliberately emptied
  check_str(cfgWifiSsid(), "my-net", "a set SSID is used");
  check_str(cfgWifiPass(), "", "an explicitly EMPTY password stays empty (no fallback)");

  // The Server IP is the field where the three states are all reachable, so the seam
  // must report them separately — ticket #2 needs to know whether a stored value
  // exists at all, which "is there a fallback?" cannot answer.
  cfgReset();
  check(cfgServerIpState() == CFG_IP_UNSET, "never-set IP reports UNSET");
  check(cfgServerIpIsSet(), "an unset IP still yields a fallback (the Factory default)");
  cfgSetServerIp("");
  check(cfgServerIpState() == CFG_IP_EMPTY, "explicitly blank IP reports EMPTY");
  check(!cfgServerIpIsSet(), "an EMPTY IP reports no fallback");
  cfgSetServerIp("10.0.0.5");
  check(cfgServerIpState() == CFG_IP_SET, "a set IP reports SET");
  check(cfgServerIpIsSet(), "a SET IP reports a fallback");
  cfgClearServerIp();
  check(cfgServerIpState() == CFG_IP_UNSET, "clearing returns the IP to UNSET");

  // ── 3. Runtime values win over the Factory default, per field. ──
  printf("\nConfig seam — per-field precedence\n");
  cfgReset();
  cfgSetWifiSsid("only-wifi-changed");
  check_str(cfgWifiSsid(), "only-wifi-changed", "the changed field is used");
  check_str(cfgServerHost(), "factory-host", "an untouched field keeps the Factory default");
  check_str(cfgWeatherLocation(), "factory-place", "another untouched field keeps its default");

  // ── 4. Clearing returns a field to the Factory default. ──
  printf("\nConfig seam — clearing\n");
  cfgReset();
  cfgSetWeatherLocation("Somewhere");
  check_str(cfgWeatherLocation(), "Somewhere", "set value reads back");
  cfgClearWeatherLocation();
  check_str(cfgWeatherLocation(), "factory-place", "cleared value falls back again");

  // ── 5. Reset wipes everything back to the defaults. ──
  printf("\nConfig seam — reset\n");
  cfgSetWifiSsid("x"); cfgSetServerHost("y"); cfgSetWeatherLocation("z");
  cfgReset();
  check_str(cfgWifiSsid(), "factory-ssid", "reset restores the SSID default");
  check_str(cfgServerHost(), "factory-host", "reset restores the Server host default");
  check_str(cfgWeatherLocation(), "factory-place", "reset restores the weather default");

  // ── 6. Octet parsing. The Factory default arrives via CFG_STR() of a
  //       comma-separated macro, which leaves SPACES in the text ("192, 168, 1, 99"),
  //       so the parser must tolerate them as well as dots. ──
  printf("\nConfig seam — Server IP octets\n");
  static const unsigned char wantDefault[4] = {192, 168, 1, 99};   // from -DCFG_DEF_SERVER_IP
  unsigned char oct[4];

  cfgReset();
  cfgServerIpOctets(oct);
  check(oct[0] == wantDefault[0] && oct[1] == wantDefault[1] &&
        oct[2] == wantDefault[2] && oct[3] == wantDefault[3],
        "the Factory default parses, spaces and all");

  cfgSetServerIp("10.20.30.40");
  cfgServerIpOctets(oct);
  check(oct[0] == 10 && oct[1] == 20 && oct[2] == 30 && oct[3] == 40, "dotted-quad parses");

  cfgSetServerIp("  10 , 20 ,30,  40 ");
  cfgServerIpOctets(oct);
  check(oct[0] == 10 && oct[1] == 20 && oct[2] == 30 && oct[3] == 40, "spaced comma form parses");

  cfgSetServerIp("1.2.3");
  cfgServerIpOctets(oct);
  check(oct[0] == 1 && oct[1] == 2 && oct[2] == 3 && oct[3] == 0, "a short address pads with 0");

  cfgSetServerIp("999.1.1.1");
  cfgServerIpOctets(oct);
  check(oct[0] == 255, "an out-of-range octet clamps rather than wrapping");

  // ── 7. Server port. `0` is treated as unset because port 0 is not a valid HTTP
  //       port, so an accidental 0 must not produce "http://host:0/usage". ──
  printf("\nConfig seam — Server port\n");
  cfgReset();
  check(cfgServerPort() == 8266, "an unset port yields the Factory default");
  cfgSetServerPort(9000);
  check(cfgServerPort() == 9000, "a set port is used");
  cfgSetServerPort(0);
  check(cfgServerPort() == 8266, "port 0 is invalid, so it reads back as the Factory default");

  // ── 8. Over-long values are truncated, not overflowed. ──
  printf("\nConfig seam — bounds\n");
  cfgReset();
  static char big[300];
  for (int i = 0; i < 299; i++) big[i] = 'a' + (i % 26);
  big[299] = 0;
  cfgSetWifiSsid(big);
  check(strlen(cfgWifiSsid()) == 63, "an over-long value truncates to the buffer limit");
  check(strncmp(cfgWifiSsid(), big, 63) == 0, "and truncation keeps the leading bytes");

  // ── 9. PERSISTENCE (ticket #2). The rule that matters: a value that was stored
  //       comes back after a reload, and one that was NEVER stored does not become
  //       stored merely because something else was saved (per-field persistence). ──
  printf("\nConfig seam — persistence\n");

  // Start from a clean Board and store only WiFi.
  cfgReset(); cfgTestWipe();
  cfgSetWifiSsid("stored-net");
  cfgSetWifiPass("stored-pass");
  cfgSaveWifi();

  // Simulate a reboot: RAM is cleared, then cfgLoad() repopulates from the store.
  cfgReset();
  check_str(cfgWifiSsid(), "factory-ssid", "before cfgLoad the RAM state is the default");
  cfgLoad();
  check_str(cfgWifiSsid(), "stored-net", "a stored SSID survives a reload");
  check_str(cfgWifiPass(), "stored-pass", "a stored password survives a reload");
  check_str(cfgServerHost(), "factory-host", "an UNSTORED field still falls back (per-field)");
  check_str(cfgWeatherLocation(), "factory-place", "another unstored field still falls back");

  // Storing WiFi alone must not silently persist the Server's current effective
  // value: that would freeze today's Factory default into the store.
  cfgReset(); cfgTestWipe();
  cfgSetWifiSsid("only-wifi"); cfgSaveWifi();
  cfgReset(); cfgLoad();
  check(cfgServerIpState() == CFG_IP_UNSET,
        "saving WiFi alone leaves the Server IP UNSET (not frozen to the Factory default)");
  check(!cfgServerHostIsStored(),
        "saving WiFi alone does not mark the Server host as stored");
  check(!cfgServerPortIsStored(),
        "saving WiFi alone does not mark the Server port as stored");

  // The same trap for the PASSWORD: "SETWIFI NewNet" must not freeze the Factory
  // default password into the store, or a later secrets.h change would be ignored
  // and the Board would keep trying a password nobody chose.
  cfgReset(); cfgTestWipe();
  cfgSetWifiSsid("net-without-pass"); cfgSaveWifi();   // password never set
  cfgReset(); cfgLoad();
  check(cfgWifiIsStored(), "the SSID is stored");
  check(cfgWifiPass()[0] != 0, "the password still resolves to something (the default)");
  // Reset the Factory default to a NEW value and reload: a frozen copy would keep the
  // OLD default, which is exactly the failure this guards against.
  // (The test build's default is compile-time, so instead assert the store has no
  //  pass key by checking that saving an unrelated field leaves the pass unset.)
  cfgReset(); cfgTestWipe();
  cfgSetServerHost("h"); cfgSaveServer();
  cfgReset(); cfgLoad();
  check(strcmp(cfgWifiPass(), "factory-pass") == 0,
        "an unstored password follows the Factory default, not a frozen copy");

  // An explicitly EMPTY password DOES persist (it is a deliberate choice).
  cfgReset(); cfgTestWipe();
  cfgSetWifiSsid("open-net"); cfgSetWifiPass(""); cfgSaveWifi();
  cfgReset(); cfgLoad();
  check_str(cfgWifiPass(), "", "an explicitly empty password persists as empty");

  // ── 9b. THE FREEZE TRAP, tested directly. Saving a group must persist only the
  //        fields that were SET. Storing the RESOLVED value instead writes the Factory
  //        default into the store as if it had been chosen, which (a) makes a later
  //        secrets.h change invisible and (b) flips an UNSET field to SET, breaking
  //        the per-field rule.
  //
  //        The trick: change what "unset" RESOLVES TO between the save and the reload.
  //        A frozen copy keeps the old value; a correct implementation follows the new
  //        default. Earlier tests missed this bug because they only inspected the field
  //        that HAD been set — both code reviews caught that gap, so this closes it.
  printf("\nConfig seam — a save must not freeze the Factory default\n");
  cfgReset(); cfgTestWipe();
  cfgSetWifiSsid("net");            // SSID set; password NEVER set
  cfgSaveWifi();
  cfgReset();                       // "reboot"
  cfgTestSetFactoryPass("ROTATED-DEFAULT");
  cfgLoad();
  check_str(cfgWifiPass(), "ROTATED-DEFAULT",
            "an unstored password follows the CURRENT default, not a frozen copy");

  cfgReset(); cfgTestWipe();
  cfgSetServerHost("host-only");    // host set; IP NEVER set
  cfgSaveServer();
  cfgReset();
  cfgTestSetFactoryIp("10.9.9.9");
  cfgLoad();
  check(cfgServerIpState() == CFG_IP_UNSET,
        "an unstored Server IP stays UNSET after a save (did not freeze to a default)");
  unsigned char octFrozen[4];
  cfgServerIpOctets(octFrozen);
  check(octFrozen[0] == 10 && octFrozen[1] == 9 && octFrozen[2] == 9 && octFrozen[3] == 9,
        "and it follows the CURRENT Factory default IP");

  // Storing the Server, including an explicitly-empty IP.
  cfgReset(); cfgTestWipe();
  cfgSetServerHost("stored-host");
  cfgSetServerIp("");              // explicit "no fallback"
  cfgSaveServer();
  cfgReset(); cfgLoad();
  check_str(cfgServerHost(), "stored-host", "a stored Server host survives a reload");
  check(cfgServerIpState() == CFG_IP_EMPTY,
        "an explicitly EMPTY Server IP survives as EMPTY (not resurrected as unset)");

  // A stored EMPTY password must come back as EMPTY, not as unset. This is the
  // distinction the feature exists to protect, so it is tested through a reload.
  cfgReset(); cfgTestWipe();
  cfgSetWifiSsid("net"); cfgSetWifiPass(""); cfgSaveWifi();
  cfgReset(); cfgLoad();
  check_str(cfgWifiPass(), "", "a stored EMPTY password reloads as empty, not as the default");

  // Weather.
  cfgReset(); cfgTestWipe();
  cfgSetWeatherLocation("Stored Place"); cfgSaveWeather();
  cfgReset(); cfgLoad();
  check_str(cfgWeatherLocation(), "Stored Place", "a stored weather location survives a reload");

  // ── 10. FACTORY: wipes the store AND reboots, so the wipe outlives the reboot. ──
  printf("\nConfig seam — recovery (FACTORY)\n");
  cfgReset(); cfgTestWipe();
  cfgSetWifiSsid("bad-net"); cfgSetServerHost("bad-host"); cfgSetWeatherLocation("bad-place");
  cfgSaveWifi(); cfgSaveServer(); cfgSaveWeather();
  cfgReset(); cfgLoad();
  check_str(cfgWifiSsid(), "bad-net", "the bad values are live before the wipe");

  cfgTestClearReboot();
  cfgFactoryReset();
  check(cfgTestRebootRequested(), "FACTORY requests a reboot");

  // The reboot is a fresh boot: RAM starts empty and cfgLoad() reads the store.
  cfgReset();
  cfgLoad();
  check_str(cfgWifiSsid(), "factory-ssid", "after FACTORY the SSID is the Factory default");
  check_str(cfgServerHost(), "factory-host", "after FACTORY the Server host is the Factory default");
  check_str(cfgWeatherLocation(), "factory-place", "after FACTORY the weather location is the default");
  check(cfgServerIpState() == CFG_IP_UNSET, "after FACTORY the Server IP is UNSET again");

  printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
  return g_fail == 0 ? 0 : 1;
}

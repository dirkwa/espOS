# Tutorial: OTA from a manifest

**Advanced.** Two builds of your firmware, a static web server on your laptop
and a `manifest.json`: the device finds the newer build, installs it, verifies
the signature, reboots, confirms itself — and rolls back when a build is
broken. This is [ota.md](../ota.md) in practice. Start from any earlier
tutorial's project (say [first-sensor](first-sensor.md)) on a device that is
on the network and approved.

## 1. Versions and the key

The version a device reports is `PROJECT_VER`: `git describe` when the
checkout has a tag, otherwise `version.txt` in the project root
([releasing.md](../releasing.md)). This project has no tags, so the file
decides. The signing key is `secure_boot_signing_key.pem` in the project
root, generated on the first configure; a device accepts only images signed
with the key whose public half it runs, so every build below must use that
same file. Keep it out of git (the `.gitignore` the prologue expects already
does) and do not lose it. Build 0.1.0 and flash it over USB:

```sh
echo 0.1.0 > version.txt
espos/scripts/build.sh -DIDF_TARGET=esp32c6 build
espos/scripts/build.sh -p /dev/ttyUSB0 flash monitor
curl -s http://espos-xxxx.local/api/v1/ota/status | python3 -m json.tool   # running.version "0.1.0", slot ota_0
```

## 2. Publish 0.1.1

Change something you can see in the log, bump, build, and put the signed image
next to a manifest on any static web server — your laptop will do:

```sh
echo 0.1.1 > version.txt && espos/scripts/build.sh build
mkdir -p ~/fw && cp build/first_sensor.bin ~/fw/first_sensor-esp32c6-0.1.1.bin
cat > ~/fw/manifest.json <<'EOF'
{"schema": 1, "app": "first_sensor",
 "builds": [{"version": "0.1.1", "target": "esp32c6", "channel": "stable",
             "url": "first_sensor-esp32c6-0.1.1.bin", "notes": "tutorial"}]}
EOF
python3 -m http.server 8000 --directory ~/fw
```

`app` must equal the project name (`project(first_sensor)`) and `target` the
chip; `url` is relative to the manifest. `build/first_sensor.bin` is the
signed image; the build also leaves `build/first_sensor-unsigned.bin`, which
step 4 uses. The device takes the highest version matching its target,
channel and app, and calls it available only if it is newer than what runs.

## 3. Check and install

```sh
H='Content-Type: application/json'; D=http://espos-xxxx.local/api/v1
curl -s -X PUT -H "$H" -d '{"ota":{"manifest_url":"http://<laptop-ip>:8000/manifest.json"}}' $D/config
curl -s -X POST -H "$H" $D/ota/check                 # 202
curl -s $D/ota/status | python3 -m json.tool         # "available": {"version": "0.1.1", "newer": true, …}
curl -s -X POST -H "$H" -d '{}' $D/ota               # install the available build
curl -N $D/events                                    # ota events: downloading + progress, verifying, ready
```

The device reboots into `ota_1` about 1.5 s after `ready`. The new image
boots as `pending_verify` and confirms itself as soon as WiFi is connected —
the monitor says so — so a minute later `GET /ota/status` shows
`running.version 0.1.1`, `slot ota_1`, `other_version 0.1.0`, `confirmed
true`. The OTA page in the UI drives the same endpoints with buttons;
`ota.auto_check` (every `ota.check_h` hours) and `ota.auto_install` make it
unattended. A fleet needs nothing more than this file on a boat-side host.

## 4. What is refused

Serve `build/first_sensor-unsigned.bin` as version `0.1.2` in the manifest,
check, install: `state failed`, `last_error` `image rejected: bad signature or
corrupt`, and nothing changed on the device. An image of another project is
refused the same way (its `project_name` differs), as is a manifest whose
`app` is not yours. Plain `http://` is acceptable for the source because the
signature, not the transport, is what protects the image; `https://` uses the
certificate bundle, `ota.allow_insecure` skips the check for self-signed hosts.

## 5. Rollback

An image that panics before it confirms itself never becomes the running one.
The reference app in `main/` has a hook for exactly this test; add it to yours.
In `main/CMakeLists.txt`:

```cmake
if(ESPOS_BROKEN_BUILD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_BROKEN_BUILD=1)
endif()
```

and first thing in `app_main()`:

```c
#ifdef ESPOS_BROKEN_BUILD
    ESP_ERROR_CHECK(ESP_FAIL); /* rollback test: die before the image can confirm itself */
#endif
```

```sh
echo 0.1.2 > version.txt && espos/scripts/build.sh -DESPOS_BROKEN_BUILD=1 build
cp build/first_sensor.bin ~/fw/first_sensor-esp32c6-0.1.2.bin        # and list it in manifest.json as 0.1.2
```

Check and install as before, watching the monitor: 0.1.2 boots into `ota_0`,
aborts in `app_main`, and the bootloader — seeing an unconfirmed image that
failed — boots `ota_1` again. `GET /ota/status` now says `running.version
0.1.1`, `rolled_back true`, `other_version 0.1.2`. The slower failure is an
image that runs but never reaches the network: after `ota.confirm_tmo_s`
(600 s) it marks itself invalid and reboots into the previous slot. `POST
/ota/confirm` confirms by hand, `POST /ota/rollback` goes back on purpose.
`-D` values stick in the CMake cache: pass `-DESPOS_BROKEN_BUILD=0` on your
next build, or delete `build/`.

For anything you ship, the key comes from a password manager or a CI secret
and the manifest lives on the boat's server — "Releasing, and forking" in
[ota.md](../ota.md).

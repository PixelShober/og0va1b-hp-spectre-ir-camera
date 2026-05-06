# OG0VA1B HP Spectre IR Camera

Experimental Linux support files for the OG0VA1B / OVTI00AB IR camera path on
an HP Spectre laptop, including a DKMS driver snapshot, an IPU6/libcamera bridge
to `v4l2loopback`, and Howdy integration helpers.

## Status

This is a machine-local working snapshot, not a polished upstream driver.

Validated locally:

- `og0va1b` and `ipu-bridge` build through DKMS on tested CachyOS kernels.
- The IR camera is exposed to userspace through libcamera.
- `ir-camera-bridge.service` publishes the camera as `/dev/video100`.
- Howdy can use `/dev/video100` and toggle the TXNW3643/LM3643 IR illuminator.

Known caveats:

- The image path depends on Intel IPU6/libcamera support on the host.
- The bridge uses `v4l2loopback`, not direct Howdy access to the raw IPU6 node.
- Register tables and tuning are specific to the tested hardware.
- This repository has not been generalized for other laptops.

## Contents

- `driver/`: out-of-tree `og0va1b` and `ipu-bridge` DKMS source files.
- `bridge/`: libcamera-to-v4l2loopback bridge plus live image tuning tool.
- `systemd/`: bridge service.
- `modprobe/`: v4l2loopback, i2c-dev, and Howdy IR I2C access config.
- `howdy/`: Howdy config plus patched recorder that toggles the IR illuminator.
- `notes/`: local manifest and checksums from the snapshot.

## Architecture

Current validated architecture:

1. Kernel modules expose the OG0VA1B sensor through IPU6/libcamera.
2. `ir-camera-bridge.service` publishes the IR camera as `/dev/video100`.
3. Howdy uses `/dev/video100`.
4. The TXNW3643/LM3643 IR illuminator is toggled over I2C bus 2, address `0x63`.

## Install Sketch

This is intentionally only a sketch. Review the files before installing them on
another system.

```bash
sudo cp -r driver /usr/src/og0va1b-0.1
sudo dkms add -m og0va1b -v 0.1
sudo dkms build -m og0va1b -v 0.1
sudo dkms install -m og0va1b -v 0.1

sudo install -m 0755 bridge/ir-camera-bridge.py /usr/local/bin/ir-camera-bridge.py
sudo install -m 0644 bridge/ir-camera-bridge.ini /etc/ir-camera-bridge.ini
sudo install -m 0644 systemd/ir-camera-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ir-camera-bridge.service
```

Additional integration files under `modprobe/` and `howdy/` are provided for
reference. Do not blindly overwrite your PAM or Howdy setup without checking
your distribution paths.

## Provenance And Licensing

The driver source files carry SPDX `GPL-2.0` identifiers inherited from the
Linux-kernel-derived source they are based on. Helper scripts in this repository
are provided under the same license for simplicity.

Some sensor register values and tuning choices came from local hardware testing
and Windows-driver analysis. Treat them as experimental hardware bring-up notes,
not as an upstream-quality specification.

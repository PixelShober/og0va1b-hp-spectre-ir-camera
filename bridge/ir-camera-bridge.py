#!/usr/bin/env python3
"""Bridge IR camera (og0va1b / LNK1) to /dev/video100 via GStreamer libcamerasrc."""
import configparser
import os
import sys
import gi
gi.require_version('Gst', '1.0')
gi.require_version('GLib', '2.0')
from gi.repository import Gst, GLib

CAMERA_NAME = "\\_SB_.PC00.LNK1"
LOOPBACK_DEV = "/dev/video100"
WIDTH, HEIGHT = 640, 480
CONFIG_PATH = "/etc/ir-camera-bridge.ini"

# Keep the IR face image usable for dlib/Howdy. The sensor tends to wash out
# skin in torch mode, so bias auto exposure toward highlights and add a small
# post-ISP correction before writing to v4l2loopback.
DEFAULTS = {
    "ae_enable": "true",
    "ae_constraint_mode": "1",
    "exposure_value": "-1.0",
    "source_brightness": "-0.15",
    "source_contrast": "1.15",
    "bridge_brightness": "-0.18",
    "bridge_contrast": "1.25",
    "ir_torch_current": "0",
}


def read_settings():
    parser = configparser.ConfigParser()
    parser["image"] = DEFAULTS.copy()
    parser.read(CONFIG_PATH)
    image = parser["image"]
    return {
        "ae-enable": image.getboolean("ae_enable", fallback=True),
        "ae-constraint-mode": image.getint("ae_constraint_mode", fallback=1),
        "exposure-value": image.getfloat("exposure_value", fallback=-1.0),
        "brightness": image.getfloat("source_brightness", fallback=-0.15),
        "contrast": image.getfloat("source_contrast", fallback=1.15),
        "bridge_brightness": image.getfloat("bridge_brightness", fallback=-0.18),
        "bridge_contrast": image.getfloat("bridge_contrast", fallback=1.25),
    }


def set_if_supported(element, name, value):
    if element.find_property(name):
        element.set_property(name, value)


def apply_settings(src, balance, settings):
    for name in [
        "ae-enable",
        "ae-constraint-mode",
        "exposure-value",
        "brightness",
        "contrast",
    ]:
        set_if_supported(src, name, settings[name])
    balance.set_property("brightness", settings["bridge_brightness"])
    balance.set_property("contrast", settings["bridge_contrast"])

def main():
    Gst.init(None)
    settings = read_settings()

    src = Gst.ElementFactory.make("libcamerasrc", "src")
    if not src:
        sys.exit("libcamerasrc not found; install gst-plugin-libcamera")
    src.set_property("camera-name", CAMERA_NAME)

    src_caps_filter = Gst.ElementFactory.make("capsfilter", "src_caps")
    src_caps = Gst.Caps.from_string(
        f"video/x-raw,format=BGRx,width={WIDTH},height={HEIGHT}"
    )
    src_caps_filter.set_property("caps", src_caps)

    convert = Gst.ElementFactory.make("videoconvert", "convert")
    if not convert:
        sys.exit("videoconvert not found; install gst-plugins-base")

    balance_caps_filter = Gst.ElementFactory.make("capsfilter", "balance_caps")
    balance_caps = Gst.Caps.from_string(
        f"video/x-raw,format=BGR,width={WIDTH},height={HEIGHT}"
    )
    balance_caps_filter.set_property("caps", balance_caps)

    balance = Gst.ElementFactory.make("videobalance", "balance")
    if not balance:
        sys.exit("videobalance not found; install gst-plugins-good")
    apply_settings(src, balance, settings)

    gray_convert = Gst.ElementFactory.make("videoconvert", "gray_convert")
    if not gray_convert:
        sys.exit("videoconvert not found; install gst-plugins-base")

    out_caps_filter = Gst.ElementFactory.make("capsfilter", "out_caps")
    out_caps = Gst.Caps.from_string(
        f"video/x-raw,format=GRAY8,width={WIDTH},height={HEIGHT}"
    )
    out_caps_filter.set_property("caps", out_caps)

    sink = Gst.ElementFactory.make("v4l2sink", "sink")
    if not sink:
        sys.exit("v4l2sink not found; install gst-plugins-good")
    sink.set_property("device", LOOPBACK_DEV)

    pipeline = Gst.Pipeline.new("ir-bridge")
    for elem in [
        src,
        src_caps_filter,
        convert,
        balance_caps_filter,
        balance,
        gray_convert,
        out_caps_filter,
        sink,
    ]:
        pipeline.add(elem)
    src.link(src_caps_filter)
    src_caps_filter.link(convert)
    convert.link(balance_caps_filter)
    balance_caps_filter.link(balance)
    balance.link(gray_convert)
    gray_convert.link(out_caps_filter)
    out_caps_filter.link(sink)

    bus = pipeline.get_bus()
    bus.add_signal_watch()
    loop = GLib.MainLoop()

    def on_message(bus, msg):
        t = msg.type
        if t == Gst.MessageType.ERROR:
            err, dbg = msg.parse_error()
            print(f"ERROR: {err.message}", file=sys.stderr)
            if dbg:
                print(f"DEBUG: {dbg}", file=sys.stderr)
            loop.quit()
        elif t == Gst.MessageType.EOS:
            loop.quit()

    bus.connect("message", on_message)
    config_mtime = None

    def reload_settings():
        nonlocal config_mtime
        try:
            next_mtime = os.path.getmtime(CONFIG_PATH)
        except FileNotFoundError:
            next_mtime = None
        if next_mtime != config_mtime:
            config_mtime = next_mtime
            try:
                apply_settings(src, balance, read_settings())
            except Exception as exc:
                print(f"Failed to apply {CONFIG_PATH}: {exc}", file=sys.stderr)
        return True

    GLib.timeout_add(500, reload_settings)

    ret = pipeline.set_state(Gst.State.PLAYING)
    if ret == Gst.StateChangeReturn.FAILURE:
        sys.exit("Failed to start pipeline")

    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        pipeline.set_state(Gst.State.NULL)

if __name__ == "__main__":
    main()

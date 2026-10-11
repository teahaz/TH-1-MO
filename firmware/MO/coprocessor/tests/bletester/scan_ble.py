#!/usr/bin/env python3

import argparse
import sys

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

BLUEZ_SERVICE = "org.bluez"
ADAPTER_PATH = "/org/bluez/hci0"
ADAPTER_IFACE = "org.bluez.Adapter1"
DEVICE_IFACE = "org.bluez.Device1"
OBJECT_MANAGER_IFACE = "org.freedesktop.DBus.ObjectManager"
PROPERTIES_IFACE = "org.freedesktop.DBus.Properties"

found = {}


def record(path, name, address, rssi):
    if name is None:
        return
    found[path] = (name, address, rssi)
    print(f"seen: {name} ({address}) RSSI={rssi}")


def check_existing_objects(om, target_name):
    for path, interfaces in om.GetManagedObjects().items():
        props = interfaces.get(DEVICE_IFACE)
        if not props:
            continue
        name = props.get("Name") or props.get("Alias")
        if name == target_name:
            record(path, name, props.get("Address"), props.get("RSSI"))


def on_interfaces_added(path, interfaces, target_name):
    props = interfaces.get(DEVICE_IFACE)
    if not props:
        return
    name = props.get("Name") or props.get("Alias")
    if name == target_name:
        record(path, name, props.get("Address"), props.get("RSSI"))


def on_properties_changed(interface, changed, invalidated, path, target_name):
    if interface != DEVICE_IFACE or path not in found:
        return
    if "RSSI" in changed:
        name, address, _ = found[path]
        record(path, name, address, changed["RSSI"])


def main():
    parser = argparse.ArgumentParser(description="Scan for a BLE advertisement via BlueZ")
    parser.add_argument("--name", default="TH1-MO-BLETEST",
                         help="advertised device name to look for")
    parser.add_argument("--timeout", type=int, default=15,
                         help="scan duration in seconds")
    args = parser.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    adapter = dbus.Interface(bus.get_object(BLUEZ_SERVICE, ADAPTER_PATH), ADAPTER_IFACE)
    om = dbus.Interface(bus.get_object(BLUEZ_SERVICE, "/"), OBJECT_MANAGER_IFACE)

    check_existing_objects(om, args.name)

    om.connect_to_signal(
        "InterfacesAdded",
        lambda path, interfaces: on_interfaces_added(path, interfaces, args.name),
    )
    bus.add_signal_receiver(
        lambda interface, changed, invalidated, path: on_properties_changed(
            interface, changed, invalidated, path, args.name
        ),
        dbus_interface=PROPERTIES_IFACE,
        signal_name="PropertiesChanged",
        path_keyword="path",
    )

    print(f"Scanning for {args.timeout}s, looking for '{args.name}'...")
    adapter.StartDiscovery()

    loop = GLib.MainLoop()
    GLib.timeout_add_seconds(args.timeout, loop.quit)
    loop.run()

    adapter.StopDiscovery()

    if found:
        print(f"PASS: '{args.name}' seen during scan")
        for name, address, rssi in found.values():
            print(f"  {name} ({address}) last RSSI={rssi}")
        sys.exit(0)

    print(f"FAIL: '{args.name}' not seen during scan")
    sys.exit(1)


if __name__ == "__main__":
    main()

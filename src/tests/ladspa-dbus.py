#!/usr/bin/env python

USAGE = """
Usage:
    python ladspa-dbus.py <sinkname> [values]

The "sinkname" parameter is the name of the ladspa sink. The "values"
parameter is a comma-separated list of ladspa sink parameter values. A
value in the list can be either string "default" or a float.

Example usage:

    python ladspa-dbus.py ladspa_1 10.0,default,4.0,0.6,default

This command will configure sink "ladspa_1" by setting the first value
to 10.0, the second to the default value (specified in the ladspa
filter), the third to 4.0 and so on.
"""

import dbus
import os
import sys

def get_ladspa_property_interface(sinkname):

    # do some D-Bus stuff to get to the real ladspa property object
    session = dbus.SessionBus()

    # get the private D-Bus socket address from PulseAudio properties
    session_property_iface = dbus.Interface(session.get_object("org.PulseAudio1", "/org/pulseaudio/server_lookup1"), "org.freedesktop.DBus.Properties")
    socket = session_property_iface.Get("org.PulseAudio.ServerLookup1", "Address")

    # connect to the private PulseAudio D-Bus socket
    connection = dbus.connection.Connection(socket)

    # core object for listing the sinks
    core = connection.get_object(object_path="/org/pulseaudio/core1")

    # object path to the ladspa sink
    ladspa_sink_path = core.GetSinkByName(sinkname)

    # property interface proxy for the sink
    ladspa_sink_property_iface = dbus.Interface(connection.get_object(object_path=ladspa_sink_path), "org.freedesktop.DBus.Properties")

    return ladspa_sink_property_iface

def parse_arguments(args):

    sinkname = None
    arguments = []
    defaults = []

    if len(args) >= 2:
        sinkname = args[1]

        if len(args) == 3:
            tokens = args[2].split(",")

            for token in tokens:
                if token == "default":
                    arguments.append(0.0)
                    defaults.append(True)
                else:
                    arguments.append(float(token))
                    defaults.append(False)

    """
    print("Input arguments:")
    print("         sink: " + sink)
    print("    arguments: " + str(arguments))
    print("     defaults: " + str(defaults))
    """

    return sinkname, arguments, defaults

def print_arguments(arguments, defaults):
    for i in range(len(arguments)):
        default = ""
        if defaults[i]:
            default = "default"
        print(str(i) + " : " + str(arguments[i]) + " \t"  + default)


sinkname, arguments, defaults = parse_arguments(sys.argv)

if sinkname == None:
    print USAGE
    sys.exit(1)

# get the D-Bus property interface of the sink
ladspa = get_ladspa_property_interface(sinkname)

# read the current sink arguments from PulseAudio
oldarguments, olddefaults = ladspa.Get("org.PulseAudio.Ext.Ladspa1", "AlgorithmParameters")

print("Current LADSPA parameters for sink " + sinkname + ":")
print_arguments(oldarguments, olddefaults)

if len(arguments) != 0:
    # set new arguments if they were provided on the command line

    # Set the arguments ...
    ladspa.Set("org.PulseAudio.Ext.Ladspa1", "AlgorithmParameters", (dbus.Array(arguments), dbus.Array(defaults)))

    # ... and read them back.
    newarguments, newdefaults = ladspa.Get("org.PulseAudio.Ext.Ladspa1", "AlgorithmParameters")

    print("New LADSPA parameters for sink " + sinkname + ":")
    print_arguments(newarguments, newdefaults)

# test the GetAll functionality
# print(str(ladspa.GetAll("org.PulseAudio.Ext.Ladspa1")))

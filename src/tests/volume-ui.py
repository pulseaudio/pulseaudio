#!/usr/bin/python

import pygtk, gtk
from ctypes import *

try:
    libpulse = cdll.LoadLibrary("../.libs/libpulse.so")
except OSError:
    try:
        libpulse = cdll.LoadLibrary(".libs/libpulse.so")
    except OSError:
        libpulse = cdll.LoadLibrary("libpulse.so")

class ChannelMap(Structure):
    _fields_ = [("channels", c_ubyte),
                ("map", c_uint * 32)]

    _to_name = libpulse.pa_channel_map_to_name
    _to_name.restype = c_char_p
    _to_name.argtypes = [c_void_p]

    _to_pretty_name = libpulse.pa_channel_map_to_pretty_name
    _to_pretty_name.restype = c_char_p
    _to_pretty_name.argtypes = [c_void_p]

    _snprint = libpulse.pa_channel_map_snprint
    _snprint.restype = c_char_p
    _snprint.argtypes = [c_char_p, c_ulong, c_void_p]

    _position_to_string = libpulse.pa_channel_position_to_string
    _position_to_string.restype = c_char_p
    _position_to_string.argtypes = [c_uint]

    _position_to_pretty_string = libpulse.pa_channel_position_to_pretty_string
    _position_to_pretty_string.restype = c_char_p
    _position_to_pretty_string.argtypes = [c_uint]

    def to_name(this):
        return this._to_name(byref(this))

    def to_pretty_name(this):
        return this._to_pretty_name(byref(this))

    def snprint(this):
        s = create_string_buffer(336)
        r = this._snprint(s, len(s), byref(this))

        if r is None:
            return None
        else:
            return s.raw

    def position_to_string(this, pos):
        return this._position_to_string(pos)

    def position_to_pretty_string(this, pos):
        return this._position_to_pretty_string(pos)

class CVolume(Structure):
    _fields_ = [("channels", c_ubyte),
                ("values", c_uint32 * 32)]


    _snprint = libpulse.pa_cvolume_snprint
    _snprint.restype = c_char_p
    _snprint.argtypes = [c_char_p, c_ulong, c_void_p]

    _max = libpulse.pa_cvolume_max
    _max.restype = c_uint32
    _max.argtypes = [c_void_p]

    _scale = libpulse.pa_cvolume_scale
    _scale.restype = c_void_p
    _scale.argtypes = [c_void_p, c_uint32]

    _get_balance = libpulse.pa_cvolume_get_balance
    _get_balance.restype = c_float
    _get_balance.argtypes = [c_void_p, c_void_p]

    _get_fade = libpulse.pa_cvolume_get_fade
    _get_fade.restype = c_float
    _get_fade.argtypes = [c_void_p, c_void_p]

    _set_balance = libpulse.pa_cvolume_set_balance
    _set_balance.restype = c_void_p
    _set_balance.argtypes = [c_void_p, c_void_p, c_float]

    _set_fade = libpulse.pa_cvolume_set_fade
    _set_fade.restype = c_void_p
    _set_fade.argtypes = [c_void_p, c_void_p, c_float]

    def snprint(this):
        s = create_string_buffer(320)
        r = this._snprint(s, len(s), byref(this))

        if r is None:
            return None
        else:
            return s.raw

    def max(this):
        return this._max(byref(this))

    def scale(this, v):
        return this._scale(byref(this), v)

    def get_balance(this, cm):
        return this._get_balance(byref(this), byref(cm))

    def get_fade(this, cm):
        return this._get_fade(byref(this), byref(cm))

    def set_balance(this, cm, f):
        return this._set_balance(byref(this), byref(cm), f)

    def set_fade(this, cm, f):
        return this._set_fade(byref(this), byref(cm), f)



cm = ChannelMap()
cm.channels = 6
cm.map[0] = 1
cm.map[1] = 2
cm.map[2] = 3
cm.map[3] = 5
cm.map[4] = 6
cm.map[5] = 7

print "Channel map name: %s" % cm.to_name()
print "Channel map mapping: %s" % cm.snprint()

v = CVolume()
v.channels = cm.channels

for i in range(cm.channels):
    v.values[i] = 65536/2

print v.max()
print v.snprint()
print v.get_balance(cm)
print v.get_fade(cm)

window = gtk.Window(gtk.WINDOW_TOPLEVEL)
window.set_title(cm.to_pretty_name())
window.set_border_width(12)

vbox = gtk.VBox(spacing=6)

channel_labels = {}
channel_scales = {}

def update_volume(update_channels = True, update_fade = True, update_balance = True, update_scale = True):
    if update_channels:
        for i in range(cm.channels):
            channel_scales[i].set_value(v.values[i])

    if update_scale:
        value_scale.set_value(v.max())

    if update_balance:
        balance_scale.set_value(v.get_balance(cm))

    if update_fade:
        fade_scale.set_value(v.get_fade(cm))

def fade_value_changed(fs):
    v.set_fade(cm, fade_scale.get_value())
    update_volume(update_fade = False)

def balance_value_changed(fs):
    v.set_balance(cm, balance_scale.get_value())
    update_volume(update_balance = False)

def value_value_changed(fs):
    v.scale(int(value_scale.get_value()))
    update_volume(update_scale = False)

def channel_value_changed(fs, i):
    v.values[i] = int(channel_scales[i].get_value())
    update_volume(update_channels = False)

for i in range(cm.channels):
    channel_labels[i] = gtk.Label(cm.position_to_pretty_string(cm.map[i]))
    channel_labels[i].set_alignment(0, 1)
    vbox.pack_start(channel_labels[i], expand=False, fill=True)

    channel_scales[i] = gtk.HScale()
    channel_scales[i].set_range(0, 65536)
    channel_scales[i].set_digits(0)
    channel_scales[i].set_value_pos(gtk.POS_RIGHT)
    vbox.pack_start(channel_scales[i], expand=False, fill=True)

value_label = gtk.Label("Value")
value_label.set_alignment(0, .5)
vbox.pack_start(value_label, expand=False, fill=True)
value_scale = gtk.HScale()
value_scale.set_range(0, 65536)
value_scale.set_value_pos(gtk.POS_RIGHT)
value_scale.set_digits(0)
vbox.pack_start(value_scale, expand=False, fill=True)

balance_label = gtk.Label("Balance")
balance_label.set_alignment(0, .5)
vbox.pack_start(balance_label, expand=False, fill=True)
balance_scale = gtk.HScale()
balance_scale.set_range(-1.0, +1.0)
balance_scale.set_value_pos(gtk.POS_RIGHT)
balance_scale.set_digits(2)
vbox.pack_start(balance_scale, expand=False, fill=True)

fade_label = gtk.Label("Fade")
fade_label.set_alignment(0, .5)
vbox.pack_start(fade_label, expand=False, fill=True)
fade_scale = gtk.HScale()
fade_scale.set_range(-1.0, +1.0)
fade_scale.set_value_pos(gtk.POS_RIGHT)
fade_scale.set_digits(2)
vbox.pack_start(fade_scale, expand=False, fill=True)

window.add(vbox)
window.set_default_size(600, 400)

update_volume()

for i in range(cm.channels):
    channel_scales[i].connect("value_changed", channel_value_changed, i)
fade_scale.connect("value_changed", fade_value_changed)
balance_scale.connect("value_changed", balance_value_changed)
value_scale.connect("value_changed", value_value_changed)

window.show_all()
gtk.main()

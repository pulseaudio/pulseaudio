#!/usr/bin/python
# $Id$

import sys, os, string

exported_symbols = {}
imported_symbols = {}

for fn in sys.argv[1:]:
    f = os.popen("nm '%s'" % fn, "r")

    imported_symbols[fn] = []

    for line in f:
        sym_address = line[:7].strip()
        sym_type = line[9].strip()
        sym_name = line[11:].strip()

        if sym_name in ('_fini', '_init'):
            continue

        if sym_type in ('T', 'B', 'R', 'D' 'G', 'S', 'D'):
            if exported_symbols.has_key(sym_name):
                sys.stderr.write("CONFLICT: %s defined in both '%s' and '%s'.\n" % (sym_name, fn, exported_symbols[sym_name]))
            else:
                exported_symbols[sym_name] = fn
        elif sym_type in ('U',):
            if sym_name[:3] == 'pa_':
                imported_symbols[fn].append(sym_name)

    f.close()

dependencies = {}
unresolved_symbols = {}

for fn in imported_symbols:
    dependencies[fn] = []
    
    for sym in imported_symbols[fn]:
        if exported_symbols.has_key(sym):
            if exported_symbols[sym] not in dependencies[fn]:
                dependencies[fn].append(exported_symbols[sym])
        else:
            if unresolved_symbols.has_key(sym):
                unresolved_symbols[sym].append(fn)
            else:
                unresolved_symbols[sym] = [fn]

for sym, files in unresolved_symbols.iteritems():
    print "WARNING: Unresolved symbol '%s' in %s" % (sym, `files`)

k = dependencies.keys()
k.sort()
for fn in k:
    dependencies[fn].sort()
    print "%s: %s" % (fn, string.join(dependencies[fn], " "))

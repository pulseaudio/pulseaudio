#
# This is a makefile.am fragment to build Orc code.
#
# Define ORC_SOURCE and then include this file, such as:
#
#  ORC_SOURCE=gstadderorc
#  include $(top_srcdir)/common/orc.mak
#
# This fragment will create tmp-orc.c and gstadderorc.h from
# gstadderorc.orc.
#
# When 'make dist' is run at the top level, or 'make orc-update'
# in a directory including this fragment, the generated source
# files will be copied to $(ORC_SOURCE)-dist.[ch].  These files
# should be checked in to git, since they are used if Orc is
# disabled.
#
# Note that this file defines BUILT_SOURCES, so any later usage
# of BUILT_SOURCES in the Makefile.am that includes this file
# must use '+='.
#


EXTRA_DIST += $(ORC_SOURCE).orc

ORC_NODIST_SOURCES = tmp-orc.c $(ORC_SOURCE).h
BUILT_SOURCES += tmp-orc.c $(ORC_SOURCE).h


orc-update: tmp-orc.c $(ORC_SOURCE).h
	cp tmp-orc.c $(srcdir)/$(ORC_SOURCE)-dist.c
	cp $(ORC_SOURCE).h $(srcdir)/$(ORC_SOURCE)-dist.h

orcc_v_gen = $(orcc_v_gen_$(V))
orcc_v_gen_ = $(orcc_v_gen_$(AM_DEFAULT_VERBOSITY))
orcc_v_gen_0 = @echo "  ORCC   $@";

cp_v_gen = $(cp_v_gen_$(V))
cp_v_gen_ = $(cp_v_gen_$(AM_DEFAULT_VERBOSITY))
cp_v_gen_0 = @echo "  CP     $@";

if HAVE_ORC
tmp-orc.c: $(srcdir)/$(ORC_SOURCE).orc
	$(orcc_v_gen)$(ORCC) --implementation -o $(builddir)/tmp-orc.c $(srcdir)/$(ORC_SOURCE).orc

$(ORC_SOURCE).h: $(srcdir)/$(ORC_SOURCE).orc
	mkdir -p $$(dirname $(builddir)/$(ORC_SOURCE).h)
	$(orcc_v_gen)$(ORCC) --header -o $(builddir)/$(ORC_SOURCE).h $(srcdir)/$(ORC_SOURCE).orc
else
tmp-orc.c: $(srcdir)/$(ORC_SOURCE).orc
	$(cp_v_gen)cp $(srcdir)/$(ORC_SOURCE)-dist.c tmp-orc.c

$(ORC_SOURCE).h: $(srcdir)/$(ORC_SOURCE).orc
	$(cp_v_gen)cp $(srcdir)/$(ORC_SOURCE)-dist.h $(ORC_SOURCE).h
endif

clean-local: clean-orc
.PHONY: clean-orc
clean-orc:
	rm -f tmp-orc.c $(ORC_SOURCE).h

dist-hook: dist-hook-orc
.PHONY: dist-hook-orc
dist-hook-orc: tmp-orc.c $(ORC_SOURCE).h
	rm -f tmp-orc.c~
	cmp -s tmp-orc.c $(srcdir)/$(ORC_SOURCE)-dist.c || \
	  cp tmp-orc.c $(srcdir)/$(ORC_SOURCE)-dist.c
	cmp -s $(ORC_SOURCE).h $(srcdir)/$(ORC_SOURCE)-dist.h || \
	  cp $(ORC_SOURCE).h $(srcdir)/$(ORC_SOURCE)-dist.h
	mkdir -p $$(dirname $(ORC_SOURCE))
	cp -p $(srcdir)/$(ORC_SOURCE)-dist.c $(distdir)/$$(dirname $(ORC_SOURCE))
	cp -p $(srcdir)/$(ORC_SOURCE)-dist.h $(distdir)/$$(dirname $(ORC_SOURCE))

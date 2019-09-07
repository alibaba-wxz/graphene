include ../../../Makefile.configs

# SUBDIRS = regression native inline benchmark
SUBDIRS = native

.PHONY: all clean
all clean:
	for d in $(SUBDIRS); do \
		$(MAKE) -C $$d $@; \
	done

include ../../Makefile.configs
include ../../Makefile.rules

CFLAGS	+= -fno-builtin -nostdlib \
	  -I../include/pal -I../lib -I../src

preloads = \
	Preload1.so \
	Preload2.so

executables = \
	..Bootstrap \
	Attestation \
	AtomicMath \
	AvxDisable \
	Bootstrap \
	Bootstrap2 \
	Bootstrap3 \
	Bootstrap7 \
	Directory \
	Event \
	Exception \
	Exit \
	File \
	Hex \
	Ipc \
	Memory \
	Misc \
	Pipe \
	Process \
	Process2 \
	Process3 \
	Semaphore \
	SendHandle \
	Socket \
	Symbols \
	Thread \
	Thread2 \
	normalize_path

manifests   = manifest $(patsubst %.manifest.template,%.manifest,$(wildcard *.manifest.template))

target = $(executables) $(manifests)

include ../../Makefile.manifest
graphene_lib = .lib/graphene-lib.a
pal_lib = ../../Runtime/libpal-$(PAL_HOST).so

RUNTIME_DIR = $(CURDIR)/../../Runtime

.PHONY: all
ifeq ($(findstring x86_64,$(SYS))$(findstring linux,$(SYS)),x86_64linux)
all: $(target) $(call expand_target_to_sig,$(target)) $(call expand_target_to_sgx,$(target)) $(call expand_target_to_token,$(target)) $(preloads)
else
all:
endif

# To enable tests for remote attestation, please provide RA_CLIENT_SPID and RA_CLIENT_KEY
# in the environment variables.
# To obtain a SPID, register in the Intel API portal:
# https://api.portal.trustedservices.intel.com/EPID-attestation

manifest_rules = \
	-e 's:\$$(PAL):$(abspath ../../Runtime/pal_loader):g' \
	-e 's:\$$(PWD):$(shell pwd)/:g' \
	-e 's:\$$(RA_CLIENT_SPID):$(RA_CLIENT_SPID):g' \
	-e 's:\$$(RA_CLIENT_KEY):$(RA_CLIENT_KEY):g' \
	-e 's:\$$(RA_CLIENT_LINKABLE):$(if $(RA_CLIENT_LINKABLE),$(RA_CLIENT_LINKABLE),0):g' \

../csu/user_shared_start.o ../csu/user_start.o: ../csu/user_start.S
	$(MAKE) -C $(dir $@) $(notdir $@)

CFLAGS-AvxDisable += -mavx

# work around: File.manifest.template has strange reference
# to ../regression/File
../regression/File: | File
	@true

LDLIBS-preloads = ../csu/user_shared_start.o $(graphene_lib) $(pal_lib)
$(preloads): CFLAGS += -shared -fPIC
$(preloads): LDLIBS = $(LDLIBS-preloads)
$(preloads): %.so: %.c $(LDLIBS-preloads)
	$(call cmd,csingle)

LDLIBS-executables = ../csu/user_start.o $(graphene_lib) $(pal_lib) $(preloads)
$(executables): CFLAGS += -no-pie
$(executables): LDLIBS = $(LDLIBS-executables)
$(executables): %: %.c $(LDLIBS-executables)
	$(call cmd,csingle)

include $(wildcard *.d)

$(graphene_lib):
	$(MAKE) -C ../lib target=$(abspath .lib)/

export PAL_LOADER = $(RUNTIME_DIR)/pal-$(PAL_HOST)
export PYTHONPATH=../../Scripts

.PHONY: regression
regression:
	$(RM) pal-regression.xml
	$(MAKE) pal-regression.xml

pal-regression.xml: test_pal.py $(target) $(call expand_target_to_sig,$(target)) $(call expand_target_to_sgx,$(target)) $(call expand_target_to_token,$(target))
	python3 -m pytest --junit-xml $@ -v test_pal.py

.PHONY: clean
clean:
	rm -rf $(target) \
	$(call expand_target_to_sig,$(target)) \
	$(call expand_target_to_sgx,$(target)) \
	$(call expand_target_to_token,$(target)) \
	$(preloads) *.tmp .lib *.cached *.sig *.d

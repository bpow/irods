BASEDIRS = osauth \
           pam \
           native

######################################################################
# Configuration should occur above this line

include ../../iRODS/config/platform.mk
include ../../iRODS/config/config.mk

GCC = g++ -DRODS_SERVER -DZIP_EXEC_PATH=\"$(ZIP_EXEC_PATH)\" -DUNZIP_EXEC_PATH=\"$(UNZIP_EXEC_PATH)\"
GCC += `../../packaging/compiler_check.sh 4 2`

ifeq ($(IRODS_BUILD_COVERAGE), 1)
GCC += -fprofile-arcs -ftest-coverage -lgcov
endif

export OBJDIR = .objs
export IRODSTOPDIR = ../../iRODS
export SOTOPDIR = .

SUBS = ${BASEDIRS}

ifeq ($(IRODS_BUILD_DEBUG), 0)
#SUBS += ${HPSSDIRS}
endif

.PHONY: ${SUBS} clean

default: ${SUBS}

${SUBS}:
	@-mkdir -p $@/${OBJDIR} > /dev/null 2>&1
	@echo "Compile auth plugin `basename $@`..."
	@${MAKE} -C $@

clean:
	@-for dir in ${SUBS}; do \
	echo "Cleaning $$dir"; \
	rm -f $$dir/${OBJDIR}/*.o > /dev/null 2>&1; \
	rm -f $$dir/*.o > /dev/null 2>&1; \
	done
	@-rm -f ${SOTOPDIR}/*.so > /dev/null 2>&1


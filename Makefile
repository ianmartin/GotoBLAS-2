TOPDIR	= .
include ./Makefile.system

BLASDIRS = interface driver/level2 driver/level3 driver/others

ifndef DYNAMIC_ARCH
BLASDIRS += kernel 
endif

ifdef SANITY_CHECK
BLASDIRS += reference
endif

SUBDIRS	= $(BLASDIRS) lapack

SUBDIRS_ALL = $(SUBDIRS) test ctest exports benchmark ../laswp ../bench

.PHONY : all libs netlib test ctest shared
.NOTPARALLEL : all libs prof lapack-test

all :: libs netlib tests shared
	@echo
	@echo " GotoBLAS build complete."
	@echo
	@echo "  OS               ... $(OSNAME)             "
	@echo "  Architecture     ... $(ARCH)               "
ifndef BINARY64
	@echo "  BINARY           ... 32bit                 "
else
	@echo "  BINARY           ... 64bit                 "
endif
	@echo "  C compiler       ... $(C_COMPILER)  (command line : $(CC))"
	@echo "  Fortran compiler ... $(F_COMPILER)  (command line : $(FC))"
ifneq ($(OSNAME), AIX)
	@echo -n "  Library Name     ... $(LIBNAME)"
else
	@echo "  Library Name     ... $(LIBNAME)"
endif

ifndef SMP
	@echo " (Single threaded)  "
else
	@echo " (Multi threaded; Max num-threads is $(NUM_THREADS))"
endif
	@echo

shared :
ifeq ($(OSNAME), Linux)
	$(MAKE) -C exports so
	-ln -fs $(LIBSONAME) libgoto2.so
endif
ifeq ($(OSNAME), FreeBSD)
	$(MAKE) -C exports so
	-ln -fs $(LIBSONAME) libgoto2.so
endif
ifeq ($(OSNAME), NetBSD)
	$(MAKE) -C exports so
	-ln -fs $(LIBSONAME) libgoto2.so
endif
ifeq ($(OSNAME), Darwin)
	$(MAKE) -C exports dyn
	-ln -fs $(LIBDYNNAME) libgoto2.dylib
endif
ifeq ($(OSNAME), WINNT)
	$(MAKE) -C exports dll
#	-ln -fs $(LIBDLLNAME) libgoto2.dll
endif
ifeq ($(OSNAME), CYGWIN_NT)
	$(MAKE) -C exports dll
	-ln -fs $(LIBDLLNAME) libgoto2.dll
endif

tests :
ifndef NOFORTRAN
ifndef TARGET
ifndef CROSS
	touch $(LIBNAME)
ifndef NO_FBLAS
	$(MAKE) -C test all
endif
ifndef NO_CBLAS
	$(MAKE) -C ctest all
endif
endif
endif
endif

libs :
	-ln -fs $(LIBNAME) libgoto2.$(LIBSUFFIX)
	for d in $(SUBDIRS) ; \
	do if test -d $$d; then \
	  $(MAKE) -C $$d $(@F) || exit 1 ; \
	fi; \
	done
ifdef DYNAMIC_ARCH
	  $(MAKE) -C kernel commonlibs || exit 1
	for d in $(DYNAMIC_CORE) ; \
	do  $(MAKE) GOTOBLAS_MAKEFILE= -C kernel TARGET_CORE=$$d kernel || exit 1 ;\
	done
endif

prof : prof_blas prof_lapack

prof_blas :
	ln -fs $(LIBNAME_P) libgoto2_p.$(LIBSUFFIX)
	for d in $(SUBDIRS) ; \
	do if test -d $$d; then \
	  $(MAKE) -C $$d prof || exit 1 ; \
	fi; \
	done
ifdef DYNAMIC_ARCH
	  $(MAKE) -C kernel commonprof || exit 1
endif

blas :
	ln -fs $(LIBNAME) libgoto2.$(LIBSUFFIX)
	for d in $(BLASDIRS) ; \
	do if test -d $$d; then \
	  $(MAKE) -C $$d libs || exit 1 ; \
	fi; \
	done

hpl : 
	ln -fs $(LIBNAME) libgoto2.$(LIBSUFFIX)
	for d in $(BLASDIRS) ../laswp exports ; \
	do if test -d $$d; then \
	  $(MAKE) -C $$d $(@F) || exit 1 ; \
	fi; \
	done
ifdef DYNAMIC_ARCH
	  $(MAKE) -C kernel commonlibs || exit 1
	for d in $(DYNAMIC_CORE) ; \
	do  $(MAKE) GOTOBLAS_MAKEFILE= -C kernel TARGET_CORE=$$d kernel || exit 1 ;\
	done
endif

hpl_p :
	ln -fs $(LIBNAME_P) libgoto2_p.$(LIBSUFFIX)
	for d in $(SUBDIRS) ../laswp exports ; \
	do if test -d $$d; then \
	  $(MAKE) -C $$d $(@F) || exit 1 ; \
	fi; \
	done

netlib : lapack-3.1.1 patch.for_lapack-3.1.1 lapack-3.1.1/make.inc
ifndef NOFORTRAN
	-@$(MAKE) -C lapack-3.1.1 lapacklib
endif

prof_lapack : lapack-3.1.1 lapack-3.1.1/make.inc
	-@$(MAKE) -C lapack-3.1.1 lapack_prof

lapack-3.1.1/make.inc :
ifndef NOFORTRAN
	-@echo "FORTRAN   = $(FC)" > lapack-3.1.1/make.inc
	-@echo "OPTS      = $(FFLAGS)" >> lapack-3.1.1/make.inc
	-@echo "POPTS     = $(FPFLAGS)" >> lapack-3.1.1/make.inc
	-@echo "NOOPT     = $(FFLAGS) -O0" >> lapack-3.1.1/make.inc
	-@echo "PNOOPT     = $(FPFLAGS) -O0" >> lapack-3.1.1/make.inc
	-@echo "LOADOPTS  = $(FFLAGS) $(EXTRALIB)" >> lapack-3.1.1/make.inc
	-@echo "ARCH      = $(AR)" >> lapack-3.1.1/make.inc
	-@echo "RANLIB    = $(RANLIB)" >> lapack-3.1.1/make.inc
	-@echo "LAPACKLIB = ../$(LIBNAME)" >> lapack-3.1.1/make.inc
	-@echo "LAPACKLIB_P = ../$(LIBNAME_P)" >> lapack-3.1.1/make.inc
	-@echo "SUFFIX     = $(SUFFIX)" >> lapack-3.1.1/make.inc
	-@echo "PSUFFIX    = $(PSUFFIX)" >> lapack-3.1.1/make.inc
#	-@echo "CEXTRALIB  = $(CEXTRALIB)" >> lapack-3.1.1/make.inc
	-@cat  make.inc >> lapack-3.1.1/make.inc
endif

lapack-3.1.1 : lapack-3.1.1.tgz
ifndef NOFORTRAN
	@if test `$(MD5SUM) lapack-3.1.1.tgz | $(AWK) '{print $$1}'` = 00b21551a899bcfbaa7b8443e1faeef9; then \
		echo $(TAR) zxf $< ;\
		$(TAR) zxf $< && (cd lapack-3.1.1; $(PATCH) -p1 < ../patch.for_lapack-3.1.1) ;\
	else \
		echo "	lapack-3.1.1.tgz check sum is wrong (Please use orignal)." ;\
		rm -rf lapack-3.1.1 ;\
	fi
endif

lapack-3.1.1.tgz :
ifndef NOFORTRAN
	-wget http://www.netlib.org/lapack/lapack-3.1.1.tgz
endif

large.tgz : 
ifndef NOFORTRAN
	-wget http://www.netlib.org/lapack/timing/large.tgz
endif

timing.tgz :
ifndef NOFORTRAN
	-wget http://www.netlib.org/lapack/timing/timing.tgz
endif

lapack-timing : lapack-3.1.1 large.tgz timing.tgz
ifndef NOFORTRAN
	(cd lapack-3.1.1; $(TAR) zxf ../timing.tgz TIMING)
	(cd lapack-3.1.1/TIMING; $(TAR) zxf ../../large.tgz )
	make -C lapack-3.1.1 tmglib
	make -C lapack-3.1.1/TIMING
endif


lapack-test :
	$(MAKE) -C lapack-3.1.1 tmglib
	$(MAKE) -C lapack-3.1.1/TESTING xeigtstc xeigtstd xeigtsts xeigtstz xlintstc xlintstd xlintstds xlintsts xlintstz xlintstzc
	@rm	-f lapack-3.1.1/TESTING/*.out
	$(MAKE) -j 1 -C lapack-3.1.1/TESTING
	$(GREP) failed lapack-3.1.1/TESTING/*.out

dummy :

clean ::
	@for d in $(SUBDIRS_ALL) ; \
	do if test -d $$d; then \
	  $(MAKE) -C $$d $(@F) || exit 1 ; \
	fi; \
	done
ifdef DYNAMIC_ARCH
	@$(MAKE) -C kernel clean
endif
	@rm -f *.$(LIBSUFFIX) *.so *~ *.exe getarch getarch_2nd *.dll *.lib *.$(SUFFIX) *.dwf libgoto2.$(LIBSUFFIX) libgoto2_p.$(LIBSUFFIX) *.lnk myconfig.h
	@rm -f Makefile.conf config.h Makefile_kernel.conf config_kernel.h st* *.dylib
	@if test -d lapack-3.1.1; then \
	echo deleting lapack-3.1.1; \
	rm -rf lapack-3.1.1 ;\
	fi
	@echo Done.
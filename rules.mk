# rules.mk - GSURF Build Rules
# Pattern rules and common build recipes

# All library/main objects depend on the generated version header
$(LIB_OBJS) $(MAIN_OBJ): src/gsurf-version.h

# main.o (and its .d) depends on the generated embedded-config header
$(MAIN_OBJ) $(OBJDIR)/main.d: $(OUTDIR)/gsurf-default-config.h

# Generic object compilation (handles src/ and all subdirs incl. backend/)
$(OBJDIR)/%.o: src/%.c | $(BUILDDIR)/include/gsurf
	@$(MKDIR_P) $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# Header dependency generation
$(OBJDIR)/%.d: src/%.c | $(BUILDDIR)/include/gsurf
	@$(MKDIR_P) $(@D)
	@$(CC) $(CFLAGS) -MM -MT '$(@:.d=.o)' $< > $@

# Test compilation
$(OBJDIR)/tests/%.o: tests/%.c
	@$(MKDIR_P) $(@D)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

# Module compilation (generic rule: modules/<name>/*.c -> modules/<name>.so)
$(OUTDIR)/modules/%.so: modules/%/*.c | $(OUTDIR)/modules
	@$(MKDIR_P) $(dir $@)
	$(CC) $(MODULE_CFLAGS) $(MODULE_LDFLAGS) -o $@ $^ $(LDFLAGS) -L$(OUTDIR) -lgsurf

# yaml-glib dependency compilation
$(OBJDIR)/deps/yaml-glib/src/%.o: deps/yaml-glib/src/%.c
	@$(MKDIR_P) $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# crispy dependency compilation (wired in Phase 3)
$(OBJDIR)/deps/crispy/src/%.o: deps/crispy/src/%.c deps/crispy/src/crispy-version.h
	@$(MKDIR_P) $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# Static library creation (merge gsurf objects + crispy archive via ar MRI)
$(OUTDIR)/$(LIB_STATIC): $(LIB_OBJS) $(YAMLGLIB_OBJS) $(CRISPY_LIB) | $(OUTDIR)
	@$(MKDIR_P) $(dir $@)
	@rm -f $@
	@{ echo "create $@"; \
	   for o in $(LIB_OBJS) $(YAMLGLIB_OBJS); do echo "addmod $$o"; done; \
	   echo "addlib $(CRISPY_LIB)"; \
	   echo "save"; echo "end"; } | $(AR) -M

# Shared library creation (links only the referenced crispy objects)
$(OUTDIR)/$(LIB_SHARED_FULL): $(LIB_OBJS) $(YAMLGLIB_OBJS) $(CRISPY_LIB) | $(OUTDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(LDFLAGS_SHARED) -o $@ $(LIB_OBJS) $(YAMLGLIB_OBJS) $(CRISPY_LIB) $(LDFLAGS)
	cd $(OUTDIR) && ln -sf $(LIB_SHARED_FULL) $(LIB_SHARED_MAJOR)
	cd $(OUTDIR) && ln -sf $(LIB_SHARED_MAJOR) $(LIB_SHARED)

# Executable linking.
ifeq ($(STATIC),1)
# Static: link the library objects directly into the binary. The gsurf and
# yaml-glib objects are pulled in --whole-archive so every GType/boxed/enum
# registration and yaml_* symbol the runtime modules rely on is present even
# when main.c doesn't reference it; crispy is linked normally (only the
# config-compiler objects it actually needs, not its readline-based REPL).
# --export-dynamic exposes those symbols so loaded module .so files resolve
# gsurf_*/yaml_* against the executable — one library instance, no
# libgsurf.so runtime dependency.
$(OUTDIR)/gsurf: $(MAIN_OBJ) $(LIB_OBJS) $(YAMLGLIB_OBJS) $(CRISPY_LIB)
	$(CC) -o $@ $(MAIN_OBJ) \
		-Wl,--whole-archive $(LIB_OBJS) $(YAMLGLIB_OBJS) -Wl,--no-whole-archive \
		$(CRISPY_LIB) $(LDFLAGS) -Wl,--export-dynamic
else
# Shared: link libgsurf.so (rpath $ORIGIN so it is found beside the binary).
$(OUTDIR)/gsurf: $(MAIN_OBJ) $(OUTDIR)/$(LIB_SHARED_FULL)
	$(CC) -o $@ $(MAIN_OBJ) -L$(OUTDIR) -lgsurf $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'
endif

# GIR generation
$(OUTDIR)/$(GIR_FILE): $(LIB_SRCS) $(LIB_HDRS) | $(OUTDIR)/$(LIB_SHARED_FULL)
	$(GIR_SCANNER) \
		--namespace=$(GIR_NAMESPACE) \
		--nsversion=$(GIR_VERSION) \
		--library=gsurf \
		--library-path=$(OUTDIR) \
		--include=GLib-2.0 \
		--include=GObject-2.0 \
		--include=Gio-2.0 \
		--pkg=glib-2.0 \
		--pkg=gobject-2.0 \
		--pkg=gio-2.0 \
		--output=$@ \
		--warn-all \
		-Isrc \
		$(LIB_HDRS) $(LIB_SRCS)

# Typelib compilation
$(OUTDIR)/$(TYPELIB_FILE): $(OUTDIR)/$(GIR_FILE)
	$(GIR_COMPILER) --output=$@ $<

# Development include symlink so <gsurf/gsurf.h> resolves when crispy
# compiles a user C config against the in-tree headers.
$(BUILDDIR):
	@$(MKDIR_P) $(BUILDDIR)

$(BUILDDIR)/include/gsurf: | $(BUILDDIR)
	@$(MKDIR_P) $(BUILDDIR)/include
	@ln -sfn $(CURDIR)/src $(BUILDDIR)/include/gsurf

# Directory creation
$(OUTDIR):
	@$(MKDIR_P) $(OUTDIR)

$(OUTDIR)/modules:
	@$(MKDIR_P) $(OUTDIR)/modules

# pkg-config file generation
$(OUTDIR)/gsurf.pc: gsurf.pc.in | $(OUTDIR)
	sed \
		-e 's|@PREFIX@|$(PREFIX)|g' \
		-e 's|@LIBDIR@|$(LIBDIR)|g' \
		-e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' \
		-e 's|@VERSION@|$(VERSION)|g' \
		$< > $@

# Version header generation
src/gsurf-version.h: src/gsurf-version.h.in
	sed \
		-e 's|@GSURF_VERSION_MAJOR@|$(VERSION_MAJOR)|g' \
		-e 's|@GSURF_VERSION_MINOR@|$(VERSION_MINOR)|g' \
		-e 's|@GSURF_VERSION_MICRO@|$(VERSION_MICRO)|g' \
		-e 's|@GSURF_VERSION@|$(VERSION)|g' \
		$< > $@

# Crispy version header generation (for the vendored crispy sources)
deps/crispy/src/crispy-version.h: deps/crispy/src/crispy-version.h.in
	sed \
		-e 's|@CRISPY_VERSION_MAJOR@|0|g' \
		-e 's|@CRISPY_VERSION_MINOR@|1|g' \
		-e 's|@CRISPY_VERSION_MICRO@|0|g' \
		-e 's|@CRISPY_VERSION@|0.1.0|g' \
		$< > $@

# Default config header generation (embeds YAML + C config as string constants)
$(OUTDIR)/gsurf-default-config.h: data/default-config.yaml data/default-config.c | $(OUTDIR)
	@echo "  GEN     $@"
	@echo "static const gchar *default_yaml_config =" > $@
	@sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' data/default-config.yaml >> $@
	@echo ";" >> $@
	@echo "" >> $@
	@echo "static const gchar *default_c_config =" >> $@
	@sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' data/default-config.c >> $@
	@echo ";" >> $@

# Clean rules
.PHONY: clean clean-all
clean:
	rm -rf $(BUILDDIR)/$(BUILD_TYPE)
	rm -f src/gsurf-version.h
	rm -f deps/crispy/src/crispy-version.h

clean-all:
	rm -rf $(BUILDDIR)
	rm -f src/gsurf-version.h
	rm -f deps/crispy/src/crispy-version.h

# Installation rules
.PHONY: install install-lib install-bin install-headers install-pc install-gir install-modules install-desktop install-icon install-man install-gsurf-mcp
install: install-lib install-bin install-headers install-pc install-desktop install-icon install-man
ifeq ($(BUILD_GIR),1)
install: install-gir
endif
ifeq ($(BUILD_MODULES),1)
install: install-modules
endif
ifeq ($(MCP_AVAILABLE),1)
ifneq ($(wildcard tools/gsurf-mcp/Makefile),)
install: install-gsurf-mcp
endif
endif

ifeq ($(STATIC),1)
install-bin: $(MAIN_OBJ) $(LIB_OBJS) $(YAMLGLIB_OBJS) $(CRISPY_LIB)
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(CC) -o $(DESTDIR)$(BINDIR)/gsurf $(MAIN_OBJ) \
		-Wl,--whole-archive $(LIB_OBJS) $(YAMLGLIB_OBJS) -Wl,--no-whole-archive \
		$(CRISPY_LIB) $(LDFLAGS) -Wl,--export-dynamic
	chmod 755 $(DESTDIR)$(BINDIR)/gsurf
else
install-bin: $(MAIN_OBJ) $(OUTDIR)/$(LIB_SHARED_FULL)
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(CC) -o $(DESTDIR)$(BINDIR)/gsurf $(MAIN_OBJ) \
		-L$(OUTDIR) -lgsurf $(LDFLAGS) -Wl,-rpath,$(LIBDIR)
	chmod 755 $(DESTDIR)$(BINDIR)/gsurf
endif

install-lib: $(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_SHARED_FULL)
	$(MKDIR_P) $(DESTDIR)$(LIBDIR)
	$(INSTALL_DATA) $(OUTDIR)/$(LIB_STATIC) $(DESTDIR)$(LIBDIR)/
	$(INSTALL_PROGRAM) $(OUTDIR)/$(LIB_SHARED_FULL) $(DESTDIR)$(LIBDIR)/
	cd $(DESTDIR)$(LIBDIR) && ln -sf $(LIB_SHARED_FULL) $(LIB_SHARED_MAJOR)
	cd $(DESTDIR)$(LIBDIR) && ln -sf $(LIB_SHARED_MAJOR) $(LIB_SHARED)
	@if [ -z "$(DESTDIR)" ] && command -v ldconfig >/dev/null 2>&1; then \
		echo "Updating shared library cache..."; ldconfig; \
	fi

install-headers:
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gsurf
	@for h in $(LIB_HDRS); do \
		sub=$$(dirname $$h | sed 's|^src||; s|^/||'); \
		$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gsurf/$$sub; \
		$(INSTALL_DATA) $$h $(DESTDIR)$(INCLUDEDIR)/gsurf/$$sub/; \
	done

install-pc: $(OUTDIR)/gsurf.pc
	$(MKDIR_P) $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL_DATA) $(OUTDIR)/gsurf.pc $(DESTDIR)$(PKGCONFIGDIR)/

install-gir: $(OUTDIR)/$(GIR_FILE) $(OUTDIR)/$(TYPELIB_FILE)
	$(MKDIR_P) $(DESTDIR)$(GIRDIR)
	$(MKDIR_P) $(DESTDIR)$(TYPELIBDIR)
	$(INSTALL_DATA) $(OUTDIR)/$(GIR_FILE) $(DESTDIR)$(GIRDIR)/
	$(INSTALL_DATA) $(OUTDIR)/$(TYPELIB_FILE) $(DESTDIR)$(TYPELIBDIR)/

install-modules:
	$(MKDIR_P) $(DESTDIR)$(MODULEDIR)
	@for mod in $(OUTDIR)/modules/*.so; do \
		if [ -f "$$mod" ]; then $(INSTALL_DATA) "$$mod" $(DESTDIR)$(MODULEDIR)/; fi \
	done

install-gsurf-mcp: gsurf-mcp
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(OUTDIR)/gsurf-mcp $(DESTDIR)$(BINDIR)/

install-desktop:
	$(MKDIR_P) $(DESTDIR)$(DATADIR)/applications
	$(INSTALL_DATA) data/gsurf.desktop $(DESTDIR)$(DATADIR)/applications/

install-icon:
	$(MKDIR_P) $(DESTDIR)$(ICONDIR)/256x256/apps
	$(INSTALL_DATA) data/logo-256.png $(DESTDIR)$(ICONDIR)/256x256/apps/gsurf.png

install-man:
	$(MKDIR_P) $(DESTDIR)$(MANDIR)/man1
	sed 's|@DATADIR@|$(DATADIR)|g' data/gsurf.1 > $(DESTDIR)$(MANDIR)/man1/gsurf.1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/gsurf.1

# Uninstall
.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/gsurf
	rm -f $(DESTDIR)$(BINDIR)/gsurf-mcp
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_STATIC)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_SHARED_FULL)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_SHARED_MAJOR)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_SHARED)
	rm -rf $(DESTDIR)$(INCLUDEDIR)/gsurf
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/gsurf.pc
	rm -f $(DESTDIR)$(GIRDIR)/$(GIR_FILE)
	rm -f $(DESTDIR)$(TYPELIBDIR)/$(TYPELIB_FILE)
	rm -rf $(DESTDIR)$(MODULEDIR)
	rm -f $(DESTDIR)$(DATADIR)/applications/gsurf.desktop
	rm -f $(DESTDIR)$(ICONDIR)/256x256/apps/gsurf.png
	rm -f $(DESTDIR)$(MANDIR)/man1/gsurf.1

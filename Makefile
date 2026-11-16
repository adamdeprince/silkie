ifdef V
	VVAL=--verbose
endif
ifdef VERBOSE
	VVAL=--verbose
endif

ifdef FAIL_WARN
export FAIL_WARN
endif

all:
	python3 setup.py $(VVAL)

test:
	python3 setup.py $(VVAL) test

clean:
	python3 setup.py $(VVAL) clean

# A debug build
debug:
	python3 setup.py build $(VVAL) --debug

debug-event-loop:
	python3 setup.py build $(VVAL) --debug --extra-logging=event-loop

# Build with the ASAN and UBSAN sanitizers
asan:
	python3 setup.py build $(VVAL) --debug --sanitize

profile:
	python3 setup.py build $(VVAL) --profile

app:
	python3 setup.py kitty.app $(VVAL)

linux-package: FORCE
	rm -rf linux-package
	python3 setup.py linux-package

FORCE:

man:
	$(MAKE) -C docs man

.PHONY: html browser-assets wasm browser-guest browser-demo browser-serve browser-test
html: browser-assets
	$(MAKE) -C docs html

browser-assets:
	python3 web/build.py

wasm:
	python3 web/build-wasm.py

browser-guest:
	python3 web/build-guest.py $(BROWSER_GUEST_ARGS)

browser-demo: wasm browser-assets browser-guest
	$(MAKE) html

browser-serve:
	python3 web/serve.py $(BROWSER_SERVE_ARGS)

browser-test: wasm
	node --test web/*.test.mjs web/tests/*.test.mjs
	cargo test --locked --manifest-path web/guest/console-session/Cargo.toml
	python3 -m unittest discover -s web -p 'test_*.py'
	python3 -m unittest discover -s web/guest -p 'test_*.py'

dirhtml:
	$(MAKE) -C docs dirhtml

linkcheck:
	$(MAKE) -C docs linkcheck

website: html
	python3 packaging/publish_r2.py

docs: man html


develop-docs:
	$(MAKE) -C docs develop-docs


prepare-for-cross-compile: clean all
	python3 setup.py $(VVAL) clean --clean-for-cross-compile

cross-compile:
	python3 setup.py linux-package --skip-code-generation
	

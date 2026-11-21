# Convenience targets.  The real UI is ./l2 (gum) and ./l2install.
.PHONY: all build iso deps doctor test check l2pm install clean ram stability

all: build

build:
	./l2 build all

iso:
	./l2 iso

deps:
	./l2 deps install

doctor:
	./l2 doctor

l2pm:
	$(MAKE) -C l2pm

test: check
	./l2 pm selftest
	./l2fetch --no-color --no-logo
	./l2 doctor

check:
	bash -n l2
	bash -n lib/l2ui.sh
	sh -n lib/l2ui.sh
	sh -n l2fetch
	sh -n l2install
	sh -n overlay/bin/l2net
	sh -n overlay/bin/openssl
	sh -n overlay/etc/l2/udhcpc.script
	sh -n overlay/etc/l2/serial-getty
	python3 -m py_compile tools/patch-kernel-ram.py
	python3 tools/ramtest.py --selftest
	python3 -m unittest discover -s tools -p 'test_stability_harness.py'
	$(MAKE) -C rcl2 test
	python3 -m unittest discover -s tools -p 'test_rcl2_lifecycle.py'
	$(MAKE) -C l2pm
	python3 -m unittest discover -s tools -p 'test_l2pm_security.py'
	python3 -m unittest discover -s tools -p 'test_l2pm_pipeline.py'
	python3 -m unittest discover -s tools -p 'test_l2pm_recipe_validation.py'
	python3 -m unittest discover -s tools -p 'test_l2pm_features.py'
	python3 -m unittest discover -s tools -p 'test_l2pm_transactions.py'
	python3 -m unittest discover -s tools -p 'test_l2gpt.py'
	python3 -m unittest discover -s tools -p 'test_l2install.py'
	python3 -m unittest discover -s tools -p 'test_l2net.py'
	$(MAKE) -C l2wifi
	python3 -m unittest discover -s tools -p 'test_l2wifi.py'

install:
	./l2install shared

ram:
	./l2 ram

stability:
	python3 tools/test-stability.py $(STABILITY_ARGS)

clean:
	rm -rf build
	$(MAKE) -C l2pm clean
	$(MAKE) -C l2wifi clean

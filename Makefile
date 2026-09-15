# CSE-Host — The C ABI and the composition root.
# The gate runs in the umbrella (CSE), which merges every part into one view and runs the full engine gate;
# this Makefile delegates to it. UMBRELLA points at a CSE checkout that has this repo as a submodule.
UMBRELLA ?= ..
.PHONY: check
check:
	$(MAKE) -C $(UMBRELLA) check-part PART=CSE-Host

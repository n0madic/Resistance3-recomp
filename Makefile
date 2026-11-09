# Build and run Resistance 3 (macOS) from a source checkout. A thin wrapper over scripts/ and r3.py; see CLAUDE.md.
#
#   make              host, then the game module (the module is built against the host's SDK)
#   make host         full host build (toolkit clone/pin, patches, deps)
#   make host-quick   rebuild only host/*.cpp edits and copy r3_host into dist/host
#   make module       lift + compile the game module (steps whose inputs did not change are skipped)
#   make run          run the game       (FPS=1 prints the frame rate, LOG=1 writes logs/, GAME_LANG=ru|en)
#   make test         the launcher's and the host's unit tests
#   make package      dist/r3-<version>-macos-<arch>.zip for users
#   make kill         kill stale r3_host instances
#   make clean-cache  drop the FIOS disk cache (use if a data-path bug may have poisoned it)
#
# The checkout is its own data directory (R3_HOME): the game is its PS3_GAME/, saves and caches are gamedata/ and cache/.
# Another game folder or data directory: make run GAME=/path/to/PS3_GAME R3_HOME=/path/to/data

GAME      ?= $(CURDIR)/PS3_GAME
GAME_LANG ?=
FPS       ?=
LOG       ?=

export R3_HOME ?= $(CURDIR)

R3 = ./r3.py --game "$(GAME)"
RUN_ARGS = $(if $(FPS),--fps) $(if $(LOG),--log) $(if $(GAME_LANG),--lang $(GAME_LANG))

.PHONY: all build host host-quick module run kill clean-cache test package

all build: host module

host:
	scripts/build_host.sh

host-quick:
	cmake --build build/host
	cp build/host/r3_host dist/host/

module:
	$(R3) build --no-host-build

# r3_host ignores SIGTERM and stale instances skew results, hence -9 (r3.py kill).
kill:
	./r3.py kill

run: kill
	$(R3) $(RUN_ARGS)

clean-cache:
	./r3.py clean-cache

test:
	python3 tests/test_launcher.py
	cmake --build build/host --target test_r3_unself test_r3_controls test_r3_input test_r3_glyphs
	build/host/r3_host selftest
	build/host/test_r3_unself
	build/host/test_r3_controls
	build/host/test_r3_input
	R3_GAME_SETTINGS=0 build/host/test_r3_input
	build/host/test_r3_glyphs

package:
	scripts/package.sh

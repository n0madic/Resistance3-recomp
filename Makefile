# Build and run Resistance 3 (macOS). Thin wrapper over scripts/; see CLAUDE.md for details.
#
#   make              full build: host, then module (the module is built against the host's SDK)
#   make host         full host build (toolkit clone/pin, patches, deps)
#   make host-quick   rebuild only host/*.cpp edits and copy r3_host into dist/host
#   make module       lift + compile module/game.dylib (~6 min)
#   make recompile    recompile the module, reusing the existing lift
#   make run          run the game       (FPS=1 prints the frame rate, LOG=1 writes logs/, GAME_LANG=ru|en)
#   make kill         kill stale r3_host instances
#   make clean-cache  drop the FIOS disk cache (use if a data-path bug may have poisoned it)

PYTHON    ?= .venv/bin/python
GAME_LANG ?=
FPS       ?=
LOG       ?=

RUN_ARGS = $(if $(FPS),--fps) $(if $(LOG),--log) $(if $(GAME_LANG),--lang $(GAME_LANG))

.PHONY: all build host host-quick module recompile run kill clean-cache

all build: host module

host:
	scripts/build_host.sh

host-quick:
	cmake --build build/host
	cp build/host/r3_host dist/host/

module:
	$(PYTHON) scripts/build_module.py

recompile:
	$(PYTHON) scripts/build_module.py --skip-lift

# r3_host ignores SIGTERM and stale instances skew results, hence -9; pkill exits 1 when nothing matched.
kill:
	-pkill -9 -x r3_host

run: kill
	scripts/run_game.sh $(RUN_ARGS)

clean-cache:
	rm -rf cache/BCES01118

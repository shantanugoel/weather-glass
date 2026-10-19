# Host build of the firmware compositor: the same sources the device runs.
GFX   := ../firmware/components/glass_gfx
APP   := ../firmware/components/glass_app
SRCS  := sim_main.cc \
         $(GFX)/glass_canvas.cc \
         $(GFX)/glass_font.cc \
         $(GFX)/glass_astro.cc \
         $(GFX)/glass_sky.cc \
         $(GFX)/glass_glass.cc \
         $(GFX)/glass_plate.cc \
         $(GFX)/glass_scene.cc \
         $(GFX)/glass_card.cc \
         $(GFX)/generated/glass_assets.cc \
         $(GFX)/generated/glass_plates.cc \
         $(APP)/weather_model.cc \
         $(APP)/mini_json.cc
INC   := -I$(GFX)/include -I$(GFX)/generated -I$(APP)/include -I$(APP)/generated
# -B/usr/bin pins the host assembler: this machine's PATH puts ESP ULP binutils
# first, which cannot link host objects.
FLAGS := -B/usr/bin -O2 -g -std=c++17 -Wall -Wextra -Wno-unused-parameter $(INC)

out/glass-sim: $(SRCS) Makefile
	@mkdir -p out
	g++ $(FLAGS) $(SRCS) -o $@ -lm

.PHONY: run assets clean
run: out/glass-sim
	@mkdir -p out
	./out/glass-sim out

assets:
	cd .. && .venv/bin/python tools/gen_assets.py

clean:
	rm -rf out/glass-sim out/*.png out/*.bin

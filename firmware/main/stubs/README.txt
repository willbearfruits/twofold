Development-only stand-ins for components that other engineers were still writing (dsp, ui, gfx).
Used only with `idf.py -DTWOFOLD_STUBS=ON build`; a real component of the same name in
firmware/components always takes precedence. Release builds never use these (option default OFF).

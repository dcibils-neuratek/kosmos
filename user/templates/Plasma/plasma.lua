-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name Plasma
-- kosmos: image build/plasma.elf
--
-- Plasma: a C app with a window. Everything is in plasma.c - the window,
-- the drawing, the keys - through the Window Kit (kosmos_window.h); this
-- line starts it and prints what it says when it ends.
print(use("plasma.elf").main())

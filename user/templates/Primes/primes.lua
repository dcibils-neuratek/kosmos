-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: image build/primes.elf
-- Primes: a C app. Everything is in primes.c; this line starts it and prints
-- what it says.
print(use("primes.elf").main(args))

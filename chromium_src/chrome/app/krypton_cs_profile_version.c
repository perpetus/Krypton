// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

// Link-only CS-PGO supplies no frontend version symbol. Set the IR and CS
// bits so the runtime writes a profile the later CS-PGO link can use.
unsigned long long __llvm_profile_raw_version = 11ULL | (1ULL << 56) | (1ULL << 57);

#if defined(KRYPTON_CS_PROFILE_OUTPUT)
// Sandboxed children receive a filtered environment without
// LLVM_PROFILE_FILE. The profile runtime uses this pattern when that variable
// is absent, so those processes still write their profiles.
char __llvm_profile_filename[] = KRYPTON_CS_PROFILE_OUTPUT;
#endif

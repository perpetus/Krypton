// Copyright 2026 The Krypton Authors. GPL-3.0.
// CPU-only provider policy for the pinned streaming core. No provider loading,
// cache directories, environment overrides or network code is linked here.
#include "ort-utils.h"

OrtStatus* ort_append_execution_providers(
    const OrtApi*, OrtSessionOptions*, const std::vector<std::string>&,
    const OrtExecutionProviderOptions*) {
  return nullptr;
}

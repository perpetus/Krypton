// Copyright 2026 The Krypton Authors. GPL-3.0.
#ifndef CHROME_BROWSER_KRYPTON_TRANSLATE_PROVIDER_H_
#define CHROME_BROWSER_KRYPTON_TRANSLATE_PROVIDER_H_

namespace krypton::translation {
// Register before profile preferences are initialized. No file/network/engine
// work occurs here; document sessions start inference only on demand.
void RegisterProvider();
}  // namespace krypton::translation
#endif  // CHROME_BROWSER_KRYPTON_TRANSLATE_PROVIDER_H_

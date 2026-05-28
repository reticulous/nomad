# License

This repository, **reticulous-nomad** (Nomad Network page-client on spangap;
renders Micron pages in the browser SPA and on-device LVGL), is released
under the **Apache License, Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by reticulous project contributors.

## Third-party software

### Vendored in this repository

None.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml` and `browser/package.json`:

| Component / package | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |
| Browser peer deps (Vue, Quasar, Pinia, vue-router) | npm | MIT |

Nomad Network and its Micron markup are by Mark Qvist
(`markqvist/NomadNet`, MIT). This implementation is independent; no
Nomad Network source code is incorporated from the upstream reference.

/**
 * nomad_app.h — the on-device "Nomad" browser program as a boot-registered
 * Service.
 *
 * NomadApp is an LcdApp (hence a Service): the straddle's `services:` entry
 * points the generated boot code at this header, which constructs a NomadApp
 * and registers it. LcdApp::onInit installs its launcher tile; appInit() does
 * the boot-task wiring (Settings pane, the nomad.url_lcd open-page hook).
 *
 * The class is declared here (global, no namespace — the codebase disambiguates
 * by the nomad* symbol prefix) so the trampoline TU can `new NomadApp()`; the
 * methods are defined out-of-line in nomad_lcd.cpp, where the file-static
 * browser state (list/page widgets, renderer) lives. Compiled only under
 * conditional/spangap-lcd/, so it exists only when the lcd straddle is staged —
 * matching the entry's `when: spangap/spangap-lcd` gate.
 */
#pragma once

#include "lcd_app.h"   /* LcdApp (a Service) */
#include "lvgl.h"      /* lv_obj_t */

/** The Nomad browser. onCreate builds the browser; onClose nulls the widget
 *  handles so a storage change (subscriptions outlive the layer) early-returns
 *  instead of touching freed objects after eviction. appInit() (boot task)
 *  registers the Settings pane and the nomad.url_lcd open-page subscription. */
class NomadApp : public LcdApp {
public:
    NomadApp();
    void onCreate(lv_obj_t* root) override;
    void onClose() override;

protected:
    void appInit() override;
};

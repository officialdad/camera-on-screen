// Camera-on-Screen overlay teleport (#53), Wayland edition.
//
// On a Wayland session no client can observe global pointer clicks (the app-side
// Ctrl+click poll only sees clicks over X11 surfaces), but the compositor sees
// everything — so KWin itself moves the overlay: press the shortcut (default
// Ctrl+Alt+O, rebindable in System Settings > Shortcuts > KWin) and the overlay
// recenters on the cursor, clamped inside that screen's work area.
//
// Overlay identification: the app's XWayland windows share one resource class;
// the overlay is the one skipping the taskbar (ShowInTaskbar=false) or typed as
// a KDE critical notification (#40). The control panel is a normal taskbar window.
// On-screen feedback via Plasma's OSD (the volume-popup surface): journal output from
// KWin scripts is filtered out by default, so silent failures are undebuggable without it.
function osd(text) {
    callDBus("org.kde.plasmashell", "/org/kde/osdService", "org.kde.osdService",
        "showText", "camera-web", text);
}

function teleport() {
    const c = workspace.cursorPos;
    let hit = false;
    for (const w of workspace.stackingOrder) {
        if (!w.resourceClass.toLowerCase().includes("cameraonscreen")) continue;
        if (!w.skipTaskbar && !w.criticalNotification) continue;
        const g = w.frameGeometry;
        const area = workspace.clientArea(KWin.PlacementArea, w);
        const x = Math.max(area.x, Math.min(c.x - g.width / 2, area.x + area.width - g.width));
        const y = Math.max(area.y, Math.min(c.y - g.height / 2, area.y + area.height - g.height));
        w.frameGeometry = Qt.rect(x, y, g.width, g.height);
        hit = true;
        osd("Overlay → " + Math.round(x) + "," + Math.round(y));
    }
    if (!hit) osd("Camera overlay not found — start the camera first");
}

registerShortcut("CameraOverlayTeleport", "Camera overlay: teleport to cursor",
    "Ctrl+Alt+B", teleport);
